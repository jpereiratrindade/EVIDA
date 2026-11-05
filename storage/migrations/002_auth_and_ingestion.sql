-- EVIDA migration v0.2.0: Autenticação padrão MORFO, ingestão documental e proveniência auditável
BEGIN TRANSACTION;

-- Autenticação padrão MORFO
CREATE TABLE IF NOT EXISTS users (
    id TEXT PRIMARY KEY,
    email TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'researcher', -- 'admin', 'researcher', 'user'
    is_active INTEGER NOT NULL DEFAULT 1,
    created_at TEXT NOT NULL,
    last_login_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS auth_codes (
    id TEXT PRIMARY KEY,
    email TEXT NOT NULL,
    code TEXT NOT NULL,
    code_hash TEXT NOT NULL,
    code_hmac TEXT NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    max_attempts INTEGER NOT NULL DEFAULT 5,
    expires_at TEXT NOT NULL,
    used_at TEXT,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_auth_codes_lookup ON auth_codes(email, expires_at, used_at);

CREATE TABLE IF NOT EXISTS sessions (
    token TEXT PRIMARY KEY,
    user_id TEXT NOT NULL REFERENCES users(id),
    email TEXT NOT NULL,
    created_at TEXT NOT NULL,
    expires_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_token ON sessions(token);

CREATE TABLE IF NOT EXISTS auth_audit_events (
    id TEXT PRIMARY KEY,
    email TEXT NOT NULL,
    ip_address TEXT NOT NULL,
    action TEXT NOT NULL,
    decision TEXT NOT NULL,
    details TEXT NOT NULL,
    occurred_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_auth_audit_email ON auth_audit_events(email, occurred_at);

-- Ingestão Documental e Páginas
CREATE TABLE IF NOT EXISTS document_page (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_document_id INTEGER NOT NULL REFERENCES source_document(id) ON DELETE CASCADE,
    page_number INTEGER NOT NULL CHECK (page_number > 0),
    text_content TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(source_document_id, page_number)
);
CREATE INDEX IF NOT EXISTS idx_document_page_lookup ON document_page(source_document_id, page_number);

-- Registro de proveniência auditável de transformações
CREATE TABLE IF NOT EXISTS provenance_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    entity_type TEXT NOT NULL,
    entity_id TEXT NOT NULL,
    action TEXT NOT NULL,
    details TEXT NOT NULL,
    actor TEXT NOT NULL DEFAULT 'system',
    recorded_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_provenance_entity ON provenance_log(entity_type, entity_id);

-- Usuários iniciais de desenvolvimento e pesquisa
INSERT OR IGNORE INTO users(id, email, name, role, is_active, created_at, last_login_at)
VALUES
  ('usr-admin-001', 'admin@labeco.local', 'Administrador LABECO', 'admin', 1, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP),
  ('usr-pesq-001', 'pesquisador@labeco.local', 'Pesquisador SisTer', 'researcher', 1, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);

PRAGMA user_version = 2;
COMMIT;
