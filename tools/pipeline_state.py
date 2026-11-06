"""Persistent collection and extraction bookkeeping; no network side effects."""
import json
import hashlib
import sqlite3
from pathlib import Path


def connect(db):
    con = sqlite3.connect(str(db), timeout=30)
    con.execute('PRAGMA foreign_keys=ON')
    if con.execute('PRAGMA user_version').fetchone()[0] < 4:
        raise RuntimeError('Inicialize o banco com o servidor EVIDA antes de sincronizar')
    if con.execute('PRAGMA user_version').fetchone()[0] < 5:
        con.executescript((Path(__file__).resolve().parent.parent / 'storage/migrations/005_collection_runs.sql').read_text())
    if con.execute('PRAGMA user_version').fetchone()[0] < 6:
        con.executescript((Path(__file__).resolve().parent.parent / 'storage/migrations/006_page_checksum_history.sql').read_text())
    # Missing legacy checksums are calculated from stored text, never presented as
    # a historical verification of the PDF. Existing nonempty hashes are untouched.
    for page, text, previous in con.execute("SELECT page_id,text_content,text_sha256 FROM document_page_provenance WHERE text_sha256='' OR text_sha256 IS NULL").fetchall():
        digest = hashlib.sha256(text.encode()).hexdigest()
        con.execute('INSERT OR IGNORE INTO page_checksum_history(page_id,previous_sha256,calculated_sha256) VALUES(?,?,?)',(page,previous,digest))
        con.execute('UPDATE document_page_provenance SET text_sha256=? WHERE page_id=?',(digest,page))
    con.commit()
    return con


def snapshot(con, doc_id, sha):
    data = {}
    for table, column in [('documentos','documento_id'), ('document_acquisition','document_id'),
                          ('document_page_provenance','document_id'), ('extraction_run','document_id')]:
        cur = con.execute(f'SELECT * FROM {table} WHERE {column}=?', (doc_id,))
        keys = [c[0] for c in cur.description]
        data[table] = [dict(zip(keys, row)) for row in cur]
    con.execute('INSERT OR IGNORE INTO document_revision(revision_id,document_id,sha256,snapshot_json) VALUES(?,?,?,?)',
                (doc_id + ':' + sha, doc_id, sha, json.dumps(data, ensure_ascii=False)))
