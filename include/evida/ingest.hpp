#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace evida::ingest {

struct IngestOptions {
    std::string title;
    std::string publication_date;
    std::string source_uri;
    std::string actor = "pesquisador@labeco.local";
    bool extract_evidences = true;
};

struct IngestResult {
    bool ok = false;
    int64_t document_id = 0;
    std::string sha256;
    std::string title;
    int total_pages = 0;
    int extracted_evidences = 0;
    std::string error_message;
};

struct DocumentRecord {
    int64_t id = 0;
    std::string sha256;
    std::string title;
    std::string publication_date;
    std::string source_uri;
    std::string file_path;
    int total_pages = 0;
    int64_t file_size_bytes = 0;
    std::string created_at;
    int evidence_count = 0;
};

struct PageRecord {
    int64_t id = 0;
    int64_t source_document_id = 0;
    int page_number = 0;
    std::string text_content;
    std::string created_at;
};

struct EvidenceRecord {
    int64_t id = 0;
    int64_t source_document_id = 0;
    std::string doc_title;
    std::string doc_sha256;
    int page = 0;
    std::string excerpt;
    std::string status; // 'extracted', 'reviewed', 'disputed', 'rejected'
    std::string taxonomy_version;
    std::string reviewer;
    std::string created_at;
};

std::string compute_file_sha256(const std::filesystem::path& file_path);
int get_pdf_total_pages(const std::filesystem::path& pdf_path);
std::string extract_pdf_page_text(const std::filesystem::path& pdf_path, int page_number);

IngestResult ingest_pdf_document(sqlite3* db, const std::filesystem::path& pdf_path, const IngestOptions& opts);

std::vector<DocumentRecord> list_documents(sqlite3* db);
std::optional<DocumentRecord> get_document_by_id(sqlite3* db, int64_t doc_id);
std::optional<DocumentRecord> get_document_by_sha256(sqlite3* db, const std::string& sha256);
std::vector<PageRecord> get_document_pages(sqlite3* db, int64_t doc_id);
std::optional<PageRecord> get_document_page(sqlite3* db, int64_t doc_id, int page_number);

std::vector<EvidenceRecord> list_evidences(sqlite3* db, int64_t doc_id = 0, const std::string& status_filter = "");
bool update_evidence_status(sqlite3* db, int64_t evidence_id, const std::string& new_status, const std::string& reviewer);
int64_t add_evidence(sqlite3* db, int64_t doc_id, int page, const std::string& excerpt, const std::string& reviewer);

} // namespace evida::ingest
