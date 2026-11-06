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
import hashlib
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from datetime import datetime
from pathlib import Path

BASE_URL = "https://www.emater.tche.br/site/info-agro/informativo_conjuntural.php"
USER_AGENT = "EVIDA-Scientific-Custody/0.3.2 (LABECO; +https://github.com/jpereiratrindade/EVIDA)"

MONTHS_PT = {
    "jan": 1, "janeiro": 1, "fev": 2, "fevereiro": 2, "mar": 3, "março": 3,
    "abr": 4, "abril": 4, "mai": 5, "maio": 5, "jun": 6, "junho": 6,
    "jul": 7, "julho": 7, "ago": 8, "agosto": 8, "set": 9, "setembro": 9,
    "out": 10, "outubro": 10, "nov": 11, "novembro": 11, "dez": 12, "dezembro": 12
}

def discover_emater_documents(year: int):
    """Varre os 12 meses do ano no portal da Emater/RS e retorna metadados das edições encontradas."""
    found = {}
    for m in range(1, 13):
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
                for pdf_url, day, month, yr, text in matches:
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
        except Exception as e:
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
    except Exception:
        total_pages = 1

    for p in range(1, total_pages + 1):
        try:
            txt = subprocess.check_output(
                ["pdftotext", "-layout", "-f", str(p), "-l", str(p), str(pdf_path), "-"],
                stderr=subprocess.DEVNULL
            ).decode("utf-8", errors="ignore")
        except Exception:
            txt = ""
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

    byte_size = len(pdf_bytes)
    sha256_hash = hashlib.sha256(pdf_bytes).hexdigest().lower()
    print(f"  -> Concluído: {byte_size} bytes | SHA-256: {sha256_hash}")

    # Salva no cofre físico de documentos
    vault_file = vault_dir / f"{sha256_hash}.pdf"
    if not vault_file.exists():
        with open(vault_file, "wb") as f:
            f.write(pdf_bytes)

    # Extrai texto de todas as páginas
    pages_text = extract_pdf_pages_text(vault_file)
    total_pages = len(pages_text)
    total_chars = sum(len(p) for p in pages_text)

    # Normalização de texto completo para hash de texto normalizado
    norm_full = " ".join(" ".join(p.split()) for p in pages_text)
    norm_text_sha = hashlib.sha256(norm_full.encode("utf-8")).hexdigest()

    doc_id = f"DOC-{date_iso}"
    run_id = f"run-{doc_id}"

    cur = con.cursor()
    cur.execute("BEGIN IMMEDIATE TRANSACTION;")

    # 1. Registra document_blob
    cur.execute("""
        INSERT OR REPLACE INTO document_blob (sha256, byte_size, storage_path, verified_at, status)
        VALUES (?, ?, ?, CURRENT_TIMESTAMP, 'verified');
    """, (sha256_hash, byte_size, f"storage/documents/{sha256_hash}.pdf"))

    # 2. Registra document_acquisition
    cur.execute("""
        INSERT OR REPLACE INTO document_acquisition (document_id, sha256, source_title, publisher, publication_date, issue_number, source_url, acquired_by, acquired_at)
        VALUES (?, ?, ?, 'Emater/RS-Ascar', ?, ?, ?, 'crawler-emater-oficial', CURRENT_TIMESTAMP);
    """, (doc_id, sha256_hash, title, date_iso, issue_num, pdf_url))

    # 3. Registra extraction_run
    cur.execute("""
        INSERT OR REPLACE INTO extraction_run (run_id, document_id, extractor_name, extractor_version, parameters_json, page_count, record_count, checksum)
        VALUES (?, ?, 'evida-pdf-extractor', '0.3.2', '{"engine":"poppler","layout":true,"source":"emater_crawler"}', ?, 0, ?);
    """, (run_id, doc_id, total_pages, norm_text_sha))

    # 4. Registra tabela legada documentos
    cur.execute("""
        INSERT OR REPLACE INTO documentos (documento_id, sha256, numero_informativo, ano, data_publicacao, arquivo, numero_paginas, caracteres_extraidos, sha256_texto_normalizado)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);
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

    # 7. Identifica trecho inicial de condições meteorológicas para criar evidence_span e record_evidence_link
    found_evidence = False
    for p_idx, p_text in enumerate(pages_text, start=1):
        if "Condições Meteorológicas" in p_text or "CONDIÇÕES METEOROLÓGICAS" in p_text:
            # Localiza o primeiro parágrafo significativo
            paras = [p.strip() for p in p_text.split("\n\n") if len(p.strip()) > 80]
            if paras:
                verbatim = paras[0]
                text_hash = hashlib.sha256(verbatim.encode("utf-8")).hexdigest()
                span_id = f"span-EVT-{doc_id}"
                link_id = f"link-EVT-{doc_id}"

                cur.execute("""
                    INSERT OR REPLACE INTO evidence_span (evidence_id, extraction_run_id, document_id, page_number, verbatim_text, text_sha256, section_name, confidence)
                    VALUES (?, ?, ?, ?, ?, ?, 'Condições Meteorológicas', 'ALTA');
                """, (span_id, run_id, doc_id, p_idx, verbatim, text_hash))

                cur.execute("""
                    INSERT OR REPLACE INTO record_evidence_link (link_id, record_type, record_id, evidence_id, link_type, notes)
                    VALUES (?, 'evento', ?, ?, 'supports', 'Condições meteorológicas extraídas da edição oficial da Emater/RS');
                """, (link_id, f"EVT-{doc_id}", span_id))

                found_evidence = True
                break

    con.commit()
    print(f"  -> Ingestão concluída com sucesso: {total_pages} páginas, {total_chars} caracteres.")
    return True

def main():
    parser = argparse.ArgumentParser(description="EVIDA — Sincronizador do Repositório Emater/RS")
    parser.add_argument("--year", type=int, default=2026, help="Ano dos informativos (padrão: 2026)")
    parser.add_argument("--db", type=str, default="var/evida.sqlite", help="Caminho do banco SQLite")
    parser.add_argument("--storage", type=str, default="storage/documents", help="Diretório do cofre de PDFs")
    parser.add_argument("--limit", type=int, default=0, help="Limita o número de novos downloads (0 para todos)")
    parser.add_argument("--cron", action="store_true", help="Modo cron/automatizado (saída silenciosa se nada novo)")
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    db_path = root / args.db
    vault_dir = root / args.storage
    vault_dir.mkdir(parents=True, exist_ok=True)

    if not db_path.exists():
        print(f"ERRO: Banco {db_path} não encontrado.", file=sys.stderr)
        return 1

    con = sqlite3.connect(str(db_path))

    existing_hashes, existing_issues = get_existing_issues_and_hashes(con)
    if not args.cron:
        print(f"[crawler] Base local possui {len(existing_issues)} informativos ({len(existing_hashes)} hashes cadastrados).")
        print(f"[crawler] Consultando repositório público da Emater/RS para o ano {args.year}...")

    available = discover_emater_documents(args.year)
    if not args.cron:
        print(f"[crawler] Encontradas {len(available)} edições publicadas em {args.year} no site da Emater.")

    # Filtra as edições que ainda não estão no banco
    missing = []
    for url, info in sorted(available.items(), key=lambda x: x[1]["date_iso"]):
        if info["issue_number"] and info["issue_number"] in existing_issues:
            continue
        missing.append(info)

    if not missing:
        if not args.cron:
            print("[crawler] O acervo local já está 100% atualizado com a Emater/RS!")
        return 0

    print(f"[crawler] Detectadas {len(missing)} novas edições para ingestão imediata.")
    count = 0
    for item in missing:
        if args.limit > 0 and count >= args.limit:
            break
        try:
            if ingest_document(con, vault_dir, item):
                count += 1
        except Exception as e:
            print(f"[ingest] Erro ao ingerir {item['title']}: {e}", file=sys.stderr)

    print(f"\n[crawler] Sincronização finalizada: {count} novas edições incorporadas à custódia do EVIDA.")
    if count > 0:
        try:
            from extract_corpus_scientific import run_extraction
            run_extraction(db_path)
        except Exception as e:
            print(f"[crawler] Aviso: falha ao extrair observações científicas: {e}", file=sys.stderr)
    return 0

if __name__ == "__main__":
    sys.exit(main())
