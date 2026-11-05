-- EVIDA schema v0.1.0. Sem dados sintéticos ou importação silenciosa.
BEGIN TRANSACTION;
CREATE TABLE IF NOT EXISTS source_document (
    id INTEGER PRIMARY KEY,
    sha256 TEXT NOT NULL UNIQUE CHECK (length(sha256) = 64),
    title TEXT NOT NULL,
    publication_date TEXT,
    source_uri TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS evidence (
    id INTEGER PRIMARY KEY,
    source_document_id INTEGER NOT NULL REFERENCES source_document(id),
    page INTEGER NOT NULL CHECK (page > 0),
    excerpt TEXT NOT NULL CHECK (length(trim(excerpt)) > 0),
    status TEXT NOT NULL DEFAULT 'extracted'
      CHECK (status IN ('extracted', 'reviewed', 'disputed', 'rejected')),
    taxonomy_version TEXT,
    reviewer TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS evidence_by_document ON evidence(source_document_id, page);
CREATE TABLE IF NOT EXISTS analysis_run (
    id INTEGER PRIMARY KEY,
    started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    input_manifest_sha256 TEXT CHECK (input_manifest_sha256 IS NULL OR length(input_manifest_sha256)=64),
    algorithm_version TEXT NOT NULL,
    taxonomy_version TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('started', 'completed', 'failed')),
    notes TEXT
);
PRAGMA user_version = 1;
COMMIT;
