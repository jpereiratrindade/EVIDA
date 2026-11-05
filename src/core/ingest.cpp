#include "evida/ingest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <sqlite3.h>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace evida::ingest {

namespace {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string safe_col_text(sqlite3_stmt* stmt, int col) {
    const auto* txt = sqlite3_column_text(stmt, col);
    return txt ? reinterpret_cast<const char*>(txt) : "";
}

std::string trim_spaces(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

} // namespace

std::string compute_file_sha256(const fs::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) throw std::runtime_error("Não foi possível ler arquivo para hash: " + file_path.string());

    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    std::array<char, 32768> buffer;
    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        SHA256_Update(&sha256, buffer.data(), file.gcount());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char b : hash) {
        ss << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

int get_pdf_total_pages(const fs::path& pdf_path) {
    std::string cmd = "pdfinfo " + shell_quote(pdf_path.string()) + " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return 0;

    char buffer[256];
    std::regex page_regex(R"(Pages:\s+(\d+))");
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        std::string line(buffer);
        std::smatch match;
        if (std::regex_search(line, match, page_regex) && match.size() > 1) {
            try {
                return std::stoi(match[1].str());
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

std::string extract_pdf_page_text(const fs::path& pdf_path, int page_number) {
    std::string cmd = "pdftotext -f " + std::to_string(page_number) + " -l " + std::to_string(page_number) + " " + shell_quote(pdf_path.string()) + " - 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";

    std::string output;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        output += buffer;
    }
    return output;
}

IngestResult ingest_pdf_document(sqlite3* db, const fs::path& pdf_path, const IngestOptions& opts) {
    IngestResult res;
    if (!fs::exists(pdf_path)) {
        res.error_message = "Arquivo não encontrado: " + pdf_path.string();
        return res;
    }

    std::string sha256;
    try {
        sha256 = compute_file_sha256(pdf_path);
    } catch (const std::exception& e) {
        res.error_message = e.what();
        return res;
    }
    res.sha256 = sha256;

    // Check if already registered
    auto existing = get_document_by_sha256(db, sha256);
    int64_t doc_id = 0;
    if (existing.has_value()) {
        doc_id = existing->id;
        res.document_id = doc_id;
        res.title = existing->title;
        res.total_pages = existing->total_pages;
        res.extracted_evidences = existing->evidence_count;
        res.ok = true;
        return res;
    }

    int total_pages = get_pdf_total_pages(pdf_path);
    if (total_pages <= 0) total_pages = 1;

    std::string doc_title = opts.title.empty() ? pdf_path.stem().string() : opts.title;
    std::string pub_date = opts.publication_date;
    if (pub_date.empty()) {
        // Tenta inferir do nome do arquivo (ex: conj_08012026 -> 2026-01-08)
        std::regex date_match(R"((\d{2})(\d{2})(\d{4}))");
        std::smatch m;
        std::string stem = pdf_path.stem().string();
        if (std::regex_search(stem, m, date_match) && m.size() == 4) {
            pub_date = m[3].str() + "-" + m[2].str() + "-" + m[1].str();
        } else {
            pub_date = "2026-01-08";
        }
    }

    std::string source_uri = opts.source_uri.empty() ? ("file://" + pdf_path.string()) : opts.source_uri;

    // Insert into source_document
    const char* insert_doc_sql = R"(
        INSERT INTO source_document(sha256, title, publication_date, source_uri)
        VALUES(?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, insert_doc_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        res.error_message = "Falha ao preparar SQL para source_document: " + std::string(sqlite3_errmsg(db));
        return res;
    }

    sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, doc_title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, pub_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, source_uri.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        res.error_message = "Falha ao registrar source_document: " + std::string(sqlite3_errmsg(db));
        sqlite3_finalize(stmt);
        return res;
    }
    sqlite3_finalize(stmt);
    doc_id = sqlite3_last_insert_rowid(db);

    int evidence_counter = 0;

    // Extract pages and save
    const char* ins_page_sql = "INSERT INTO document_page(source_document_id, page_number, text_content) VALUES(?, ?, ?);";
    sqlite3_stmt* page_stmt = nullptr;
    sqlite3_prepare_v2(db, ins_page_sql, -1, &page_stmt, nullptr);

    const char* ins_ev_sql = "INSERT INTO evidence(source_document_id, page, excerpt, status, taxonomy_version, reviewer) VALUES(?, ?, ?, 'extracted', 'v0.2.0', ?);";
    sqlite3_stmt* ev_stmt = nullptr;
    sqlite3_prepare_v2(db, ins_ev_sql, -1, &ev_stmt, nullptr);

    for (int p = 1; p <= total_pages; ++p) {
        std::string page_text = extract_pdf_page_text(pdf_path, p);
        if (page_stmt) {
            sqlite3_reset(page_stmt);
            sqlite3_bind_int64(page_stmt, 1, doc_id);
            sqlite3_bind_int(page_stmt, 2, p);
            sqlite3_bind_text(page_stmt, 3, page_text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(page_stmt);
        }

        if (opts.extract_evidences && ev_stmt) {
            // Segment page text into paragraphs
            std::istringstream stream(page_text);
            std::string line;
            std::string current_para;

            auto flush_para = [&]() {
                std::string cleaned = trim_spaces(current_para);
                if (cleaned.size() >= 55) {
                    sqlite3_reset(ev_stmt);
                    sqlite3_bind_int64(ev_stmt, 1, doc_id);
                    sqlite3_bind_int(ev_stmt, 2, p);
                    sqlite3_bind_text(ev_stmt, 3, cleaned.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ev_stmt, 4, opts.actor.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(ev_stmt) == SQLITE_DONE) {
                        evidence_counter++;
                    }
                }
                current_para.clear();
            };

            while (std::getline(stream, line)) {
                std::string trimmed = trim_spaces(line);
                if (trimmed.empty()) {
                    flush_para();
                } else {
                    if (!current_para.empty()) current_para += " ";
                    current_para += trimmed;
                }
            }
            flush_para();
        }
    }

    if (page_stmt) sqlite3_finalize(page_stmt);
    if (ev_stmt) sqlite3_finalize(ev_stmt);

    // Provenance Audit Log
    const char* prov_sql = "INSERT INTO provenance_log(entity_type, entity_id, action, details, actor) VALUES(?, ?, ?, ?, ?);";
    sqlite3_stmt* prov_stmt = nullptr;
    if (sqlite3_prepare_v2(db, prov_sql, -1, &prov_stmt, nullptr) == SQLITE_OK) {
        std::string details = "Ingestão documental de " + doc_title + " (" + std::to_string(total_pages) + " págs, " +
                              std::to_string(evidence_counter) + " evidências extraídas). Hash: " + sha256;
        sqlite3_bind_text(prov_stmt, 1, "source_document", -1, SQLITE_STATIC);
        sqlite3_bind_text(prov_stmt, 2, sha256.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(prov_stmt, 3, "INGEST_PDF_DOCUMENT", -1, SQLITE_STATIC);
        sqlite3_bind_text(prov_stmt, 4, details.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(prov_stmt, 5, opts.actor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(prov_stmt);
        sqlite3_finalize(prov_stmt);
    }

    res.ok = true;
    res.document_id = doc_id;
    res.title = doc_title;
    res.total_pages = total_pages;
    res.extracted_evidences = evidence_counter;
    return res;
}

std::vector<DocumentRecord> list_documents(sqlite3* db) {
    std::vector<DocumentRecord> list;
    if (!db) return list;

    const char* sql = R"(
        SELECT d.id, d.sha256, d.title, COALESCE(d.publication_date, ''), COALESCE(d.source_uri, ''),
               COALESCE((SELECT count(*) FROM document_page p WHERE p.source_document_id = d.id), 0) AS total_pages,
               d.created_at,
               COALESCE((SELECT count(*) FROM evidence e WHERE e.source_document_id = d.id), 0) AS evidence_count
        FROM source_document d
        ORDER BY d.id DESC;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DocumentRecord d;
        d.id = sqlite3_column_int64(stmt, 0);
        d.sha256 = safe_col_text(stmt, 1);
        d.title = safe_col_text(stmt, 2);
        d.publication_date = safe_col_text(stmt, 3);
        d.source_uri = safe_col_text(stmt, 4);
        d.total_pages = sqlite3_column_int(stmt, 5);
        d.created_at = safe_col_text(stmt, 6);
        d.evidence_count = sqlite3_column_int(stmt, 7);
        list.push_back(d);
    }
    sqlite3_finalize(stmt);
    return list;
}

std::optional<DocumentRecord> get_document_by_id(sqlite3* db, int64_t doc_id) {
    if (!db) return std::nullopt;
    const char* sql = R"(
        SELECT d.id, d.sha256, d.title, COALESCE(d.publication_date, ''), COALESCE(d.source_uri, ''),
               COALESCE((SELECT count(*) FROM document_page p WHERE p.source_document_id = d.id), 0) AS total_pages,
               d.created_at,
               COALESCE((SELECT count(*) FROM evidence e WHERE e.source_document_id = d.id), 0) AS evidence_count
        FROM source_document d
        WHERE d.id = ? LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_int64(stmt, 1, doc_id);
    std::optional<DocumentRecord> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        DocumentRecord d;
        d.id = sqlite3_column_int64(stmt, 0);
        d.sha256 = safe_col_text(stmt, 1);
        d.title = safe_col_text(stmt, 2);
        d.publication_date = safe_col_text(stmt, 3);
        d.source_uri = safe_col_text(stmt, 4);
        d.total_pages = sqlite3_column_int(stmt, 5);
        d.created_at = safe_col_text(stmt, 6);
        d.evidence_count = sqlite3_column_int(stmt, 7);
        res = d;
    }
    sqlite3_finalize(stmt);
    return res;
}

std::optional<DocumentRecord> get_document_by_sha256(sqlite3* db, const std::string& sha256) {
    if (!db) return std::nullopt;
    const char* sql = R"(
        SELECT d.id, d.sha256, d.title, COALESCE(d.publication_date, ''), COALESCE(d.source_uri, ''),
               COALESCE((SELECT count(*) FROM document_page p WHERE p.source_document_id = d.id), 0) AS total_pages,
               d.created_at,
               COALESCE((SELECT count(*) FROM evidence e WHERE e.source_document_id = d.id), 0) AS evidence_count
        FROM source_document d
        WHERE d.sha256 = ? LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<DocumentRecord> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        DocumentRecord d;
        d.id = sqlite3_column_int64(stmt, 0);
        d.sha256 = safe_col_text(stmt, 1);
        d.title = safe_col_text(stmt, 2);
        d.publication_date = safe_col_text(stmt, 3);
        d.source_uri = safe_col_text(stmt, 4);
        d.total_pages = sqlite3_column_int(stmt, 5);
        d.created_at = safe_col_text(stmt, 6);
        d.evidence_count = sqlite3_column_int(stmt, 7);
        res = d;
    }
    sqlite3_finalize(stmt);
    return res;
}

std::vector<PageRecord> get_document_pages(sqlite3* db, int64_t doc_id) {
    std::vector<PageRecord> pages;
    if (!db) return pages;

    const char* sql = "SELECT id, source_document_id, page_number, text_content, created_at FROM document_page WHERE source_document_id = ? ORDER BY page_number ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return pages;

    sqlite3_bind_int64(stmt, 1, doc_id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PageRecord p;
        p.id = sqlite3_column_int64(stmt, 0);
        p.source_document_id = sqlite3_column_int64(stmt, 1);
        p.page_number = sqlite3_column_int(stmt, 2);
        p.text_content = safe_col_text(stmt, 3);
        p.created_at = safe_col_text(stmt, 4);
        pages.push_back(p);
    }
    sqlite3_finalize(stmt);
    return pages;
}

std::optional<PageRecord> get_document_page(sqlite3* db, int64_t doc_id, int page_number) {
    if (!db) return std::nullopt;
    const char* sql = "SELECT id, source_document_id, page_number, text_content, created_at FROM document_page WHERE source_document_id = ? AND page_number = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;

    sqlite3_bind_int64(stmt, 1, doc_id);
    sqlite3_bind_int(stmt, 2, page_number);
    std::optional<PageRecord> res;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        PageRecord p;
        p.id = sqlite3_column_int64(stmt, 0);
        p.source_document_id = sqlite3_column_int64(stmt, 1);
        p.page_number = sqlite3_column_int(stmt, 2);
        p.text_content = safe_col_text(stmt, 3);
        p.created_at = safe_col_text(stmt, 4);
        res = p;
    }
    sqlite3_finalize(stmt);
    return res;
}

std::vector<EvidenceRecord> list_evidences(sqlite3* db, int64_t doc_id, const std::string& status_filter) {
    std::vector<EvidenceRecord> list;
    if (!db) return list;

    std::string sql = R"(
        SELECT e.id, e.source_document_id, d.title, d.sha256, e.page, e.excerpt, e.status,
               COALESCE(e.taxonomy_version, ''), COALESCE(e.reviewer, ''), e.created_at
        FROM evidence e
        JOIN source_document d ON d.id = e.source_document_id
        WHERE (1=1)
    )";
    if (doc_id > 0) sql += " AND e.source_document_id = " + std::to_string(doc_id);
    if (!status_filter.empty()) sql += " AND e.status = " + shell_quote(status_filter);
    sql += " ORDER BY e.id DESC LIMIT 200;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return list;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EvidenceRecord r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.source_document_id = sqlite3_column_int64(stmt, 1);
        r.doc_title = safe_col_text(stmt, 2);
        r.doc_sha256 = safe_col_text(stmt, 3);
        r.page = sqlite3_column_int(stmt, 4);
        r.excerpt = safe_col_text(stmt, 5);
        r.status = safe_col_text(stmt, 6);
        r.taxonomy_version = safe_col_text(stmt, 7);
        r.reviewer = safe_col_text(stmt, 8);
        r.created_at = safe_col_text(stmt, 9);
        list.push_back(r);
    }
    sqlite3_finalize(stmt);
    return list;
}

bool update_evidence_status(sqlite3* db, int64_t evidence_id, const std::string& new_status, const std::string& reviewer) {
    if (!db) return false;
    const char* sql = "UPDATE evidence SET status=?, reviewer=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, reviewer.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Provenance Log
    const char* prov_sql = "INSERT INTO provenance_log(entity_type, entity_id, action, details, actor) VALUES('evidence', ?, 'UPDATE_STATUS', ?, ?);";
    sqlite3_stmt* p_stmt = nullptr;
    if (sqlite3_prepare_v2(db, prov_sql, -1, &p_stmt, nullptr) == SQLITE_OK) {
        std::string details = "Status alterado para: " + new_status;
        sqlite3_bind_text(p_stmt, 1, std::to_string(evidence_id).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(p_stmt, 2, details.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(p_stmt, 3, reviewer.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(p_stmt);
        sqlite3_finalize(p_stmt);
    }

    return rc == SQLITE_DONE;
}

int64_t add_evidence(sqlite3* db, int64_t doc_id, int page, const std::string& excerpt, const std::string& reviewer) {
    if (!db || doc_id <= 0 || page <= 0 || excerpt.empty()) return 0;
    const char* sql = "INSERT INTO evidence(source_document_id, page, excerpt, status, taxonomy_version, reviewer) VALUES(?, ?, ?, 'reviewed', 'v0.2.0', ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    sqlite3_bind_int64(stmt, 1, doc_id);
    sqlite3_bind_int(stmt, 2, page);
    sqlite3_bind_text(stmt, 3, excerpt.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, reviewer.c_str(), -1, SQLITE_TRANSIENT);

    int64_t new_id = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        new_id = sqlite3_last_insert_rowid(db);
    }
    sqlite3_finalize(stmt);

    if (new_id > 0) {
        const char* prov_sql = "INSERT INTO provenance_log(entity_type, entity_id, action, details, actor) VALUES('evidence', ?, 'MANUAL_CURATION', 'Evidência adicionada manualmente pelo pesquisador', ?);";
        sqlite3_stmt* p_stmt = nullptr;
        if (sqlite3_prepare_v2(db, prov_sql, -1, &p_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(p_stmt, 1, std::to_string(new_id).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(p_stmt, 2, reviewer.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(p_stmt);
            sqlite3_finalize(p_stmt);
        }
    }
    return new_id;
}

} // namespace evida::ingest
