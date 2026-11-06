BEGIN;
CREATE TABLE IF NOT EXISTS page_checksum_history (
 page_id TEXT PRIMARY KEY, previous_sha256 TEXT, calculated_sha256 TEXT NOT NULL,
 calculated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
 method TEXT NOT NULL DEFAULT 'sha256_of_existing_stored_text');
PRAGMA user_version=6;
COMMIT;
