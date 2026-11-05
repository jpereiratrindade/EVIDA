-- EVIDA migration v0.3.1: Custódia documental e proveniência científica unificada
-- Licença: GNU General Public License v3.0 (GPLv3)
BEGIN TRANSACTION;

-- 1. Acervo Documental Imutável: Blobs de bytes originais
CREATE TABLE IF NOT EXISTS "document_blob" (
  "sha256" TEXT PRIMARY KEY CHECK(length(sha256) = 64),
  "byte_size" INTEGER NOT NULL CHECK(byte_size >= 0),
  "storage_path" TEXT NOT NULL,
  "registered_at" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  "verified_at" TEXT,
  "status" TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending', 'verified', 'corrupted', 'missing', 'verified_intact', 'corrupted_tampered'))
);
CREATE INDEX IF NOT EXISTS idx_blob_status ON document_blob(status);

-- 2. Aquisição Documental e Origem
CREATE TABLE IF NOT EXISTS "document_acquisition" (
  "document_id" TEXT PRIMARY KEY,
  "sha256" TEXT NOT NULL REFERENCES document_blob(sha256),
  "source_title" TEXT NOT NULL,
  "publisher" TEXT NOT NULL DEFAULT 'Emater/RS-Ascar',
  "publication_date" TEXT,
  "issue_number" INTEGER,
  "source_url" TEXT,
  "acquired_by" TEXT NOT NULL DEFAULT 'sistema',
  "acquired_at" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_acq_sha256 ON document_acquisition(sha256);
CREATE INDEX IF NOT EXISTS idx_acq_issue ON document_acquisition(issue_number);

-- 3. Execução de Extração Verificável
CREATE TABLE IF NOT EXISTS "extraction_run" (
  "run_id" TEXT PRIMARY KEY,
  "document_id" TEXT NOT NULL REFERENCES document_acquisition(document_id),
  "extractor_name" TEXT NOT NULL,
  "extractor_version" TEXT NOT NULL,
  "parameters_json" TEXT NOT NULL,
  "executed_at" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  "page_count" INTEGER NOT NULL DEFAULT 0,
  "record_count" INTEGER NOT NULL DEFAULT 0,
  "checksum" TEXT
);
CREATE INDEX IF NOT EXISTS idx_ext_run_doc ON extraction_run(document_id);

-- 4. Trechos e Posições das Evidências (Spans rastreáveis)
CREATE TABLE IF NOT EXISTS "evidence_span" (
  "evidence_id" TEXT PRIMARY KEY,
  "extraction_run_id" TEXT REFERENCES extraction_run(run_id),
  "document_id" TEXT NOT NULL REFERENCES document_acquisition(document_id),
  "page_number" INTEGER NOT NULL CHECK(page_number > 0),
  "start_char" INTEGER,
  "end_char" INTEGER,
  "verbatim_text" TEXT NOT NULL,
  "text_sha256" TEXT NOT NULL,
  "section_name" TEXT,
  "confidence" TEXT NOT NULL DEFAULT 'MÉDIA',
  "created_at" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_span_doc_page ON evidence_span(document_id, page_number);
CREATE INDEX IF NOT EXISTS idx_span_text_sha ON evidence_span(text_sha256);

-- 5. Vínculo Explícito entre Dados Estruturados e Evidências (M:N)
CREATE TABLE IF NOT EXISTS "record_evidence_link" (
  "link_id" TEXT PRIMARY KEY,
  "record_type" TEXT NOT NULL CHECK(record_type IN ('evento', 'observacao', 'ocorrencia_municipal', 'preco', 'episodio')),
  "record_id" TEXT NOT NULL,
  "evidence_id" TEXT NOT NULL REFERENCES evidence_span(evidence_id),
  "link_type" TEXT NOT NULL DEFAULT 'supports' CHECK(link_type IN ('supports', 'revises', 'disputes', 'neutral')),
  "notes" TEXT,
  "created_at" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_rel_rec ON record_evidence_link(record_type, record_id);
CREATE INDEX IF NOT EXISTS idx_rel_evi ON record_evidence_link(evidence_id);

-- 6. Decisão de Revisão Humana sem alteração do extrato bruto
CREATE TABLE IF NOT EXISTS "review_decision" (
  "decision_id" TEXT PRIMARY KEY,
  "record_type" TEXT NOT NULL,
  "record_id" TEXT NOT NULL,
  "reviewer" TEXT NOT NULL,
  "status" TEXT NOT NULL CHECK(status IN ('extracted', 'reviewed', 'disputed', 'rejected')),
  "justification" TEXT,
  "decided_at" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_rev_rec ON review_decision(record_type, record_id);

-- 7. Páginas Textuais Rastreáveis vinculadas à execução
CREATE TABLE IF NOT EXISTS "document_page_provenance" (
  "page_id" TEXT PRIMARY KEY,
  "extraction_run_id" TEXT REFERENCES extraction_run(run_id),
  "document_id" TEXT NOT NULL REFERENCES document_acquisition(document_id),
  "page_number" INTEGER NOT NULL CHECK(page_number > 0),
  "text_content" TEXT NOT NULL,
  "text_sha256" TEXT NOT NULL,
  "created_at" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE(document_id, page_number)
);
CREATE INDEX IF NOT EXISTS idx_page_prov_lookup ON document_page_provenance(document_id, page_number);

PRAGMA user_version = 4;
COMMIT;
