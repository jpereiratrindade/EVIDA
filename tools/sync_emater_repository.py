#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
"""
EVIDA — Sincronizador e Ingestor do Repositório Oficial da Emater/RS.
Monitora e consome os Informativos Conjunturais diretamente de:
https://www.emater.tche.br/site/info-agro/informativo_conjuntural.php

Preserva a custódia epistêmica estrita do EVIDA:
1. Download do PDF original oficial.
2. Cálculo de SHA-256 imediato sobre os bytes físicos.
3. Armazenamento no cofre frio (storage/documents/<sha256>.pdf).
4. Registro de proveniência de aquisição (document_acquisition) e blob (document_blob).
5. Extração e indexação de páginas (document_page_provenance).
6. Geração de spans de evidência e vínculos estruturados (evidence_span, record_evidence_link).
"""

import argparse
import json
import uuid
import fcntl
from pipeline_state import connect, snapshot
import hashlib
import os
import re
import sqlite3
import subprocess
import sys
import urllib.parse
import urllib.request
from datetime import datetime
from zoneinfo import ZoneInfo
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

BASE_URL = "https://www.emater.tche.br/site/info-agro/informativo_conjuntural.php"
USER_AGENT = "EVIDA-Scientific-Custody/0.3.2 (LABECO; +https://github.com/jpereiratrindade/EVIDA)"

MONTHS_PT = {
    "jan": 1, "janeiro": 1, "fev": 2, "fevereiro": 2, "mar": 3, "março": 3,
    "abr": 4, "abril": 4, "mai": 5, "maio": 5, "jun": 6, "junho": 6,
    "jul": 7, "julho": 7, "ago": 8, "agosto": 8, "set": 9, "setembro": 9,
    "out": 10, "outubro": 10, "nov": 11, "novembro": 11, "dez": 12, "dezembro": 12
}

def discover_emater_documents(year: int, report=None):
    """Varre os 12 meses do ano no portal da Emater/RS e retorna metadados das edições encontradas."""
    found = {}
    for m in range(1, 13 if year < datetime.now(ZoneInfo("America/Sao_Paulo")).year else datetime.now(ZoneInfo("America/Sao_Paulo")).month + 1):
        mes_str = f"{m:02d}"
        data = urllib.parse.urlencode({"ano": str(year), "mes": mes_str}).encode("utf-8")
        req = urllib.request.Request(BASE_URL, data=data, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                html = resp.read().decode("utf-8", errors="ignore")
                matches = re.findall(
                    r'href=[\"\'](https://www.emater.tche.br/site/arquivos_pdf/conjuntural/conj_(\d{2})(\d{2})(\d{4})\.pdf)[\"\'][^>]*>([^<]*)',
                    html
                )
                if not matches and not re.search(r'informativo|conjuntural', html, re.I):
                    raise RuntimeError('Resposta do portal não reconhecida')
                month_count = 0
                for pdf_url, day, month, yr, text in matches:
                    if int(yr) != year or int(month) != m:
                        continue
                    month_count += 1
                    issue_match = re.search(r'Informativo Conjuntural\s*(\d+)', text)
                    issue_num = int(issue_match.group(1)) if issue_match else 0
                    date_iso = f"{yr}-{month}-{day}"
                    found[pdf_url] = {
                        "url": pdf_url,
                        "day": day,
                        "month": month,
                        "year": yr,
                        "date_iso": date_iso,
                        "issue_number": issue_num,
                        "title": f"Informativo Conjuntural nº {issue_num}" if issue_num else f"Informativo Conjuntural ({date_iso})"
                    }
                if report is not None:
                    report.append((m, 'consultado' if month_count else 'sem_edicoes', month_count, None))
        except Exception as e:
            if report is not None:
                report.append((m, 'falha', 0, str(e)))
            print(f"[crawler] Aviso: falha ao consultar mês {mes_str}/{year}: {e}", file=sys.stderr)
    return found

def get_existing_issues_and_hashes(con: sqlite3.Connection):
    cur = con.cursor()
    # Verifica em document_acquisition e em documentos
    existing_hashes = set()
    existing_issues = set()
    for row in cur.execute("SELECT sha256, issue_number FROM document_acquisition;"):
        if row[0]: existing_hashes.add(row[0].lower())
        if row[1]: existing_issues.add(row[1])
    for row in cur.execute("SELECT sha256, numero_informativo FROM documentos;"):
        if row[0]: existing_hashes.add(row[0].lower())
        if row[1]: existing_issues.add(row[1])
    return existing_hashes, existing_issues

def extract_pdf_pages_text(pdf_path: Path):
    """Extrai o texto por página de um PDF usando pdftotext."""
    pages = []
    # Obtém contagem total de páginas via pdfinfo
    try:
        info_out = subprocess.check_output(["pdfinfo", str(pdf_path)], stderr=subprocess.DEVNULL).decode("utf-8", errors="ignore")
        page_match = re.search(r"Pages:\s*(\d+)", info_out)
        total_pages = int(page_match.group(1)) if page_match else 1
    except Exception as e:
        raise RuntimeError("Falha ao inspecionar PDF") from e

    for p in range(1, total_pages + 1):
        try:
            txt = subprocess.check_output(
                ["pdftotext", "-layout", "-f", str(p), "-l", str(p), str(pdf_path), "-"],
                stderr=subprocess.DEVNULL
            ).decode("utf-8", errors="ignore")
        except Exception as e:
            raise RuntimeError(f"Falha ao extrair página {p}") from e
        pages.append(txt)
    return pages

def ingest_document(con: sqlite3.Connection, vault_dir: Path, item: dict):
    pdf_url = item["url"]
    date_iso = item["date_iso"]
    issue_num = item["issue_number"]
    title = item["title"]

    print(f"\n[ingest] Baixando: {title} ({date_iso}) <- {pdf_url}")
    req = urllib.request.Request(pdf_url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=45) as r:
        pdf_bytes = r.read()

    if not pdf_bytes.startswith(b'%PDF-'):
        raise RuntimeError('Resposta não contém um PDF válido')
    byte_size = len(pdf_bytes)
    sha256_hash = hashlib.sha256(pdf_bytes).hexdigest().lower()
    print(f"  -> Concluído: {byte_size} bytes | SHA-256: {sha256_hash}")

    # Salva no cofre físico de documentos
    vault_file = vault_dir / f"{sha256_hash}.pdf"
    if vault_file.exists():
        if hashlib.sha256(vault_file.read_bytes()).hexdigest() != sha256_hash:
            raise RuntimeError(f'PDF existente no cofre está corrompido: {vault_file.name}')
    else:
        temporary = vault_file.with_suffix('.pdf.tmp-' + uuid.uuid4().hex)
        try:
            temporary.write_bytes(pdf_bytes)
            temporary.replace(vault_file)
        finally:
            temporary.unlink(missing_ok=True)

    existing = con.execute("SELECT sha256 FROM documentos WHERE documento_id=?", (f"DOC-{date_iso}",)).fetchone()
    if existing and existing[0] == sha256_hash:
        con.execute("UPDATE document_blob SET verified_at=CURRENT_TIMESTAMP,status='verified' WHERE sha256=?",(sha256_hash,))
        con.commit()
        return False

    # Extrai texto de todas as páginas
    pages_text = extract_pdf_pages_text(vault_file)
    if not pages_text or not any(p.strip() for p in pages_text):
        raise RuntimeError('PDF sem texto extraível; requer análise/OCR')
    total_pages = len(pages_text)
    total_chars = sum(len(p) for p in pages_text)

    # Normalização de texto completo para hash de texto normalizado
    norm_full = " ".join(" ".join(p.split()) for p in pages_text)
    norm_text_sha = hashlib.sha256(norm_full.encode("utf-8")).hexdigest()

    doc_id = f"DOC-{date_iso}"
    run_id = "pdf-" + uuid.uuid4().hex

    cur = con.cursor()
    cur.execute("BEGIN IMMEDIATE TRANSACTION;")

    if existing:
        snapshot(con, doc_id, existing[0])
        for table, kind, key in [('observacoes','observacao','observacao_id'), ('eventos','evento','evento_id')]:
            cur.execute(f"INSERT OR IGNORE INTO record_status SELECT ?, {key}, 'superseded', ? FROM {table} WHERE documento_id=?", (kind,run_id,doc_id))

    # 1. Registra document_blob
    cur.execute("""
        INSERT INTO document_blob (sha256, byte_size, storage_path, verified_at, status)
        VALUES (?, ?, ?, CURRENT_TIMESTAMP, 'verified') ON CONFLICT(sha256) DO UPDATE SET verified_at=CURRENT_TIMESTAMP,status='verified';
    """, (sha256_hash, byte_size, f"storage/documents/{sha256_hash}.pdf"))

    # 2. Registra document_acquisition
    cur.execute("""
        INSERT INTO document_acquisition (document_id, sha256, source_title, publisher, publication_date, issue_number, source_url, acquired_by, acquired_at)
        VALUES (?, ?, ?, 'Emater/RS-Ascar', ?, ?, ?, 'crawler-emater-oficial', CURRENT_TIMESTAMP) ON CONFLICT(document_id) DO UPDATE SET sha256=excluded.sha256, source_title=excluded.source_title, publication_date=excluded.publication_date, issue_number=excluded.issue_number, source_url=excluded.source_url, acquired_at=excluded.acquired_at;
    """, (doc_id, sha256_hash, title, date_iso, issue_num, pdf_url))

    # 3. Registra extraction_run
    cur.execute("""
        INSERT OR REPLACE INTO extraction_run (run_id, document_id, extractor_name, extractor_version, parameters_json, page_count, record_count, checksum)
        VALUES (?, ?, 'evida-pdf-extractor', '0.3.2', '{"engine":"poppler","layout":true,"source":"emater_crawler"}', ?, 0, ?);
    """, (run_id, doc_id, total_pages, norm_text_sha))

    # 4. Registra tabela legada documentos
    cur.execute("""
        INSERT INTO documentos (documento_id, sha256, numero_informativo, ano, data_publicacao, arquivo, numero_paginas, caracteres_extraidos, sha256_texto_normalizado)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(documento_id) DO UPDATE SET sha256=excluded.sha256, numero_informativo=excluded.numero_informativo, ano=excluded.ano, data_publicacao=excluded.data_publicacao, arquivo=excluded.arquivo, numero_paginas=excluded.numero_paginas, caracteres_extraidos=excluded.caracteres_extraidos, sha256_texto_normalizado=excluded.sha256_texto_normalizado;
    """, (doc_id, sha256_hash, issue_num, int(date_iso[:4]), date_iso, f"conj_{item['day']}{item['month']}{item['year']}.pdf", total_pages, total_chars, norm_text_sha))

    # 5. Registra source_document legado
    cur.execute("""
        INSERT OR IGNORE INTO source_document (sha256, title, publication_date, source_uri)
        VALUES (?, ?, ?, ?);
    """, (sha256_hash, title, date_iso, pdf_url))
    
    # Obtém id de source_document para páginas legadas
    sd_id = cur.execute("SELECT id FROM source_document WHERE sha256 = ?;", (sha256_hash,)).fetchone()[0]

    # 6. Grava páginas textuais em document_page_provenance e document_page
    for page_idx, page_content in enumerate(pages_text, start=1):
        p_sha = hashlib.sha256(page_content.encode("utf-8")).hexdigest()
        page_id = f"page-{doc_id}-p{page_idx}"
        
        cur.execute("""
            INSERT OR REPLACE INTO document_page_provenance (page_id, extraction_run_id, document_id, page_number, text_content, text_sha256)
            VALUES (?, ?, ?, ?, ?, ?);
        """, (page_id, run_id, doc_id, page_idx, page_content, p_sha))

        cur.execute("""
            INSERT OR REPLACE INTO document_page (source_document_id, page_number, text_content)
            VALUES (?, ?, ?);
        """, (sd_id, page_idx, page_content))

    con.commit()
    print(f"  -> Ingestão concluída com sucesso: {total_pages} páginas, {total_chars} caracteres.")
    return True

def main():
    parser = argparse.ArgumentParser(description="EVIDA — Sincronizador do Repositório Emater/RS")
    parser.add_argument("--year", type=int, default=datetime.now(ZoneInfo("America/Sao_Paulo")).year, help="Ano dos informativos (padrão: ano corrente)")
    parser.add_argument("--db", type=str, default=os.environ.get("EVIDA_DB", "var/evida.sqlite"), help="Caminho do banco SQLite")
    parser.add_argument("--storage", type=str, default=os.environ.get("EVIDA_STORAGE", "storage/documents"), help="Diretório do cofre de PDFs")
    parser.add_argument("--limit", type=int, default=0, help="Limita edições verificadas, inclusive conhecidas (0 para todas)")
    parser.add_argument("--cron", action="store_true", help="Modo cron/automatizado (saída silenciosa se nada novo)")
    args = parser.parse_args()
    if args.year < 2015 or args.year > datetime.now(ZoneInfo("America/Sao_Paulo")).year or args.limit < 0:
        parser.error('Ano fora do intervalo 2015–ano corrente ou limite negativo')

    root = Path(__file__).resolve().parent.parent
    db_path = root / args.db
    vault_dir = root / args.storage
    vault_dir.mkdir(parents=True, exist_ok=True)

    if not db_path.exists():
        print(f"ERRO: Banco {db_path} não encontrado.", file=sys.stderr)
        return 1

    lock_file = open(str(db_path) + '.sync.lock', 'a')
    try:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('Outra sincronização está em execução', file=sys.stderr)
        return 1
    con = connect(db_path)

    con.execute("UPDATE collection_run SET status='interrupted',finished_at=CURRENT_TIMESTAMP WHERE status='running'")
    run_id = 'collection-' + uuid.uuid4().hex
    con.execute('INSERT INTO collection_run(run_id,year) VALUES(?,?)', (run_id,args.year))
    con.commit()
    report = []
    available = discover_emater_documents(args.year, report)
    con.executemany('INSERT INTO collection_month(run_id,month,status,documents,error) VALUES(?,?,?,?,?)',
                    [(run_id,*row) for row in report])
    con.commit()
    errors = [f'Mês {m}: {error}' for m,status,n,error in report if status == 'falha']
    count = 0
    items = sorted(available.values(), key=lambda i:i['date_iso'])
    # Reobtém também edições conhecidas para detectar mudança de bytes na origem.
    if args.limit:
        items = items[:args.limit]
    for item in items:
        try:
            if ingest_document(con, vault_dir, item):
                count += 1
        except Exception as e:
            con.rollback()
            errors.append(f"{item['url']}: {e}")
    try:
        from extract_corpus_scientific import run_extraction
        run_extraction(db_path, year=args.year)
    except Exception as e:
        errors.append(f'Extração: {e}')
    status = 'partial' if errors or args.limit else 'completed'
    con.execute("UPDATE collection_run SET finished_at=CURRENT_TIMESTAMP,status=?,discovered=?,incorporated=?,errors_json=? WHERE run_id=?",
                (status,len(available),count,json.dumps(errors,ensure_ascii=False),run_id))
    con.commit()
    con.close()
    print(json.dumps(dict(run_id=run_id,status=status,discovered=len(available),incorporated=count,errors=errors),ensure_ascii=False))
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())
