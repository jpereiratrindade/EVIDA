BEGIN;
CREATE TABLE IF NOT EXISTS collection_run (
 run_id TEXT PRIMARY KEY, year INTEGER NOT NULL, started_at TEXT DEFAULT CURRENT_TIMESTAMP,
 finished_at TEXT, status TEXT NOT NULL DEFAULT 'running', discovered INTEGER DEFAULT 0,
 incorporated INTEGER DEFAULT 0, errors_json TEXT NOT NULL DEFAULT '[]');
CREATE TABLE IF NOT EXISTS collection_month (
 run_id TEXT NOT NULL REFERENCES collection_run(run_id), month INTEGER NOT NULL,
 status TEXT NOT NULL, documents INTEGER NOT NULL DEFAULT 0, error TEXT,
 PRIMARY KEY(run_id, month));
CREATE TABLE IF NOT EXISTS document_revision (
 revision_id TEXT PRIMARY KEY, document_id TEXT NOT NULL, sha256 TEXT NOT NULL,
 captured_at TEXT DEFAULT CURRENT_TIMESTAMP, snapshot_json TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS scientific_processing (
 run_id TEXT PRIMARY KEY, document_id TEXT NOT NULL, source_sha256 TEXT NOT NULL,
 extractor_version TEXT NOT NULL, completed_at TEXT DEFAULT CURRENT_TIMESTAMP,
 observation_count INTEGER NOT NULL, event_count INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS record_status (
 record_type TEXT NOT NULL, record_id TEXT NOT NULL, status TEXT NOT NULL,
 processing_run TEXT, PRIMARY KEY(record_type, record_id));
PRAGMA user_version=5;
COMMIT;
