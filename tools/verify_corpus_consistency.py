#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 EVIDA Contributors
"""
EVIDA v0.3.2 — Auditoria Completa de Consistência Probatória do Corpus Real.
Verifica a cadeia epistêmica:
Dado científico -> Vínculo -> Trecho & Página -> Execução -> PDF com SHA-256 verificado.
"""
import sys
import hashlib
import sqlite3
from pathlib import Path

def main():
    root = Path(__file__).resolve().parent.parent
    db_path = root / "var" / "evida.sqlite"
    vault_dir = root / "storage" / "documents"

    if not db_path.exists():
        print(f"ERRO: Banco {db_path} não encontrado.", file=sys.stderr)
        return 1

    con = sqlite3.connect(str(db_path))
    cur = con.cursor()

    print("=" * 68)
    print(" EVIDA v0.3.2 — RELATÓRIO DE AUDITORIA E CONSISTÊNCIA PROBATÓRIA")
    print("=" * 68)

    # 1. Auditoria Física dos Blobs Documentais
    docs = cur.execute("SELECT documento_id, sha256, numero_informativo, data_publicacao FROM documentos ORDER BY numero_informativo ASC;").fetchall()
    print(f"\n[1/5] Custódia Física de Documentos: {len(docs)} informativos registrados")
    
    corrupted = 0
    missing = 0
    verified = 0

    for doc_id, sha256, issue, pub_date in docs:
        pdf_path = vault_dir / f"{sha256}.pdf"
        if not pdf_path.exists():
            print(f"  [AUSENTE] {doc_id} (nº {issue}): {pdf_path.name}")
            missing += 1
            continue
        
        hasher = hashlib.sha256()
        with open(pdf_path, "rb") as f:
            while chunk := f.read(65536):
                hasher.update(chunk)
        calc_sha = hasher.hexdigest()

        if calc_sha == sha256:
            verified += 1
        else:
            print(f"  [CORROMPIDO] {doc_id} (nº {issue}): hash recalculado difere!")
            corrupted += 1

    print(f"  -> Concluído: {verified} íntegros, {corrupted} corrompidos, {missing} ausentes.")
    assert verified == 34, f"Esperado 34 documentos íntegros, obtido {verified}"
    assert corrupted == 0
    assert missing == 0

    # 2. Auditoria da Camada de Páginas
    page_count = cur.execute("SELECT count(*) FROM document_page_provenance;").fetchone()[0]
    print(f"\n[2/5] Proveniência Textual de Páginas: {page_count} páginas indexadas")
    assert page_count >= 1272, f"Páginas indexadas insuficientes: {page_count}"

    # 3. Auditoria de Vínculos de Eventos
    evts_total = cur.execute("SELECT count(*) FROM eventos;").fetchone()[0]
    evts_linked = cur.execute("SELECT count(DISTINCT record_id) FROM record_evidence_link WHERE record_type = 'evento';").fetchone()[0]
    print(f"\n[3/5] Eventos: {evts_total} no banco | {evts_linked} com vínculo M:N comprovado")
    assert evts_total == 2354
    assert evts_linked == 2354

    # 4. Auditoria de Vínculos de Observações
    obs_total = cur.execute("SELECT count(*) FROM observacoes;").fetchone()[0]
    obs_linked = cur.execute("SELECT count(DISTINCT record_id) FROM record_evidence_link WHERE record_type = 'observacao';").fetchone()[0]
    print(f"\n[4/5] Observações: {obs_total} no banco | {obs_linked} com vínculo M:N comprovado")
    assert obs_total == 5230
    assert obs_linked == 5230

    # 5. Auditoria de Ocorrências Municipais
    oc_total = cur.execute("SELECT count(*) FROM ocorrencias_municipais;").fetchone()[0]
    oc_linked = cur.execute("SELECT count(DISTINCT record_id) FROM record_evidence_link WHERE record_type = 'ocorrencia_municipal';").fetchone()[0]
    print(f"\n[5/5] Ocorrências Municipais: {oc_total} no banco | {oc_linked} com vínculo M:N comprovado")
    assert oc_total == 356
    assert oc_linked == 356

    # 6. Total de Evidências Spans
    total_spans = cur.execute("SELECT count(*) FROM evidence_span;").fetchone()[0]
    total_links = cur.execute("SELECT count(*) FROM record_evidence_link;").fetchone()[0]
    print(f"\nTotal Geral de Spans de Evidência : {total_spans}")
    print(f"Total Geral de Vínculos Explícitos: {total_links}")
    assert total_spans == 7940
    assert total_links == 7940

    # 7. Verificação de integridade referencial estrita (chaves estrangeiras órfãs)
    orphan_links = cur.execute("""
        SELECT count(*) FROM record_evidence_link rel
        LEFT JOIN evidence_span es ON rel.evidence_id = es.evidence_id
        WHERE es.evidence_id IS NULL;
    """).fetchone()[0]

    orphan_spans = cur.execute("""
        SELECT count(*) FROM evidence_span es
        LEFT JOIN document_acquisition da ON es.document_id = da.document_id
        WHERE da.document_id IS NULL;
    """).fetchone()[0]

    print(f"Vínculos órfãos: {orphan_links} | Spans órfãos: {orphan_spans}")
    assert orphan_links == 0
    assert orphan_spans == 0

    print("\n" + "=" * 68)
    print(" HOMOLOGAÇÃO EVIDA v0.3.2: CONSISTÊNCIA PROBATÓRIA 100% COMPROVADA")
    print("=" * 68)
    return 0

if __name__ == "__main__":
    sys.exit(main())
