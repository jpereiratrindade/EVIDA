#!/usr/bin/env python3
"""Audit custody and links. This does not certify semantic extraction accuracy."""
import argparse
import hashlib
import json
import sqlite3
from pathlib import Path


def audit(db, storage, allow_missing=False):
    con = sqlite3.connect(f'file:{db}?mode=ro', uri=True, timeout=30)
    errors = []
    warnings = []
    for doc, sha in con.execute("SELECT documento_id,sha256 FROM documentos UNION SELECT 'blob:' || sha256,sha256 FROM document_blob"):
        pdf = storage / f'{sha}.pdf'
        if not pdf.is_file():
            errors.append(f'PDF ausente: {doc}')
        elif hashlib.sha256(pdf.read_bytes()).hexdigest() != sha:
            errors.append(f'Hash divergente: {doc}')
    checks = {
        'vinculos_sem_trecho': '''SELECT count(*) FROM record_evidence_link l LEFT JOIN evidence_span s ON s.evidence_id=l.evidence_id WHERE s.evidence_id IS NULL''',
        'trechos_sem_aquisicao': '''SELECT count(*) FROM evidence_span s LEFT JOIN document_acquisition a ON a.document_id=s.document_id WHERE a.document_id IS NULL''',
        'trechos_sem_execucao': '''SELECT count(*) FROM evidence_span s LEFT JOIN extraction_run r ON r.run_id=s.extraction_run_id WHERE r.run_id IS NULL''',
        'documentos_sem_paginas': '''SELECT count(*) FROM documentos d WHERE NOT EXISTS(SELECT 1 FROM document_page_provenance p WHERE p.document_id=d.documento_id)''',
    }
    counts = {}
    for name, query in checks.items():
        counts[name] = con.execute(query).fetchone()[0]
        if counts[name]:
            errors.append(f'{name}: {counts[name]}')
    for table, kind, key in [('observacoes','observacao','observacao_id'), ('eventos','evento','evento_id'), ('ocorrencias_municipais','ocorrencia_municipal','ocorrencia_id')]:
        source = 't.documento_id' if table != 'ocorrencias_municipais' else '(SELECT d.documento_id FROM documentos d WHERE d.numero_informativo=t.numero_informativo LIMIT 1)'
        n = con.execute(f'''SELECT count(*) FROM {table} t WHERE NOT EXISTS
          (SELECT 1 FROM record_evidence_link l JOIN evidence_span s ON s.evidence_id=l.evidence_id
           WHERE l.record_type=? AND l.record_id=t.{key} AND s.document_id={source} AND s.page_number=t.pagina)''',(kind,)).fetchone()[0]
        counts[f'{table}_sem_vinculo_compativel'] = n
        if n:
            errors.append(f'{table}: {n} registros sem vínculo compatível com documento/página')
    for evidence, text, sha in con.execute('SELECT evidence_id,verbatim_text,text_sha256 FROM evidence_span'):
        if hashlib.sha256((text or '').encode()).hexdigest() != sha:
            errors.append(f'Hash de trecho divergente: {evidence}')
    for page, text, sha in con.execute('SELECT page_id,text_content,text_sha256 FROM document_page_provenance'):
        if not sha and allow_missing:
            warnings.append(f'Página legada sem hash: {page}')
        elif hashlib.sha256((text or '').encode()).hexdigest() != sha:
            errors.append(f'Hash de página divergente: {page}')
    fk = con.execute('PRAGMA foreign_key_check').fetchall()
    if fk:
        errors.append(f'Violações de chaves estrangeiras: {len(fk)}')
    integrity = con.execute('PRAGMA integrity_check').fetchone()[0]
    if integrity != 'ok':
        errors.append(integrity)
    result = dict(status='failed' if errors else ('passed_with_warnings' if warnings else 'passed'), checks=counts, errors=errors, warnings=warnings,
                  scope='Integridade física e vínculos. Correção semântica e completude histórica não certificadas.')
    con.close()
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--db', type=Path, default=Path(__file__).resolve().parents[1]/'var/evida.sqlite')
    parser.add_argument('--storage', type=Path, default=Path(__file__).resolve().parents[1]/'storage/documents')
    parser.add_argument('--allow-missing-legacy-checksums', action='store_true')
    args = parser.parse_args()
    result = audit(args.db,args.storage,args.allow_missing_legacy_checksums)
    print(json.dumps(result,ensure_ascii=False,indent=2))
    raise SystemExit(1 if result['errors'] else 0)
