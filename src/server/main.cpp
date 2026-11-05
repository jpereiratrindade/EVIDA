#include "evida/auth.hpp"
#include "evida/evidence.hpp"
#include "evida/ingest.hpp"
#include "evida/store.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

namespace {

std::string q(std::string_view s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\b') o += "\\b";
    else if (c == '\f') o += "\\f";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[7];
      std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
      o += buf;
    } else {
      o += c;
    }
  }
  o += "\"";
  return o;
}

std::string json_extract_string(const std::string& body, const std::string& key) {
  std::string pat = "\"" + key + "\"";
  auto pos = body.find(pat);
  if (pos == std::string::npos) return "";
  auto colon = body.find(':', pos + pat.size());
  if (colon == std::string::npos) return "";
  auto q1 = body.find('"', colon + 1);
  if (q1 == std::string::npos) return "";
  std::string value;
  for (std::size_t i = q1 + 1; i < body.size(); ++i) {
    char c = body[i];
    if (c == '"') return value;
    if (c != '\\') { value += c; continue; }
    if (++i >= body.size()) return "";
    char esc = body[i];
    if (esc == '"' || esc == '\\' || esc == '/') value += esc;
    else if (esc == 'b') value += '\b';
    else if (esc == 'f') value += '\f';
    else if (esc == 'n') value += '\n';
    else if (esc == 'r') value += '\r';
    else if (esc == 't') value += '\t';
  }
  return value;
}

int64_t json_extract_int64(const std::string& body, const std::string& key, int64_t def = 0) {
  std::string pat = "\"" + key + "\"";
  auto pos = body.find(pat);
  if (pos == std::string::npos) return def;
  auto colon = body.find(':', pos + pat.size());
  if (colon == std::string::npos) return def;
  auto start = body.find_first_of("-0123456789", colon + 1);
  if (start == std::string::npos) return def;
  auto end = body.find_first_not_of("-0123456789", start);
  std::string val = (end == std::string::npos) ? body.substr(start) : body.substr(start, end - start);
  try { return std::stoll(val); } catch (...) { return def; }
}

std::string file_contents(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw std::runtime_error("Arquivo web ausente: " + file.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string_view media_type(std::string_view route) {
  if (route == "/app.css") return "text/css; charset=utf-8";
  if (route == "/app.js") return "application/javascript; charset=utf-8";
  if (route == "/favicon.svg") return "image/svg+xml";
  return "text/html; charset=utf-8";
}

std::string stats_json(evida::Store& store) {
  const auto stats = store.stats();
  return "{\"documents\":" + std::to_string(stats.documents) +
         ",\"evidences\":" + std::to_string(stats.evidences) +
         ",\"users\":" + std::to_string(stats.users) + "}";
}

std::string extract_token(const http::request<http::string_body>& req) {
  if (auto it = req.find(http::field::authorization); it != req.end()) {
    std::string val(it->value());
    if (val.starts_with("Bearer ")) return val.substr(7);
    if (val.starts_with("bearer ")) return val.substr(7);
  }
  if (auto it = req.find(http::field::cookie); it != req.end()) {
    std::string cookies(it->value());
    std::string prefix = "evida_session=";
    auto pos = cookies.find(prefix);
    if (pos != std::string::npos) {
      auto start = pos + prefix.size();
      auto end = cookies.find(';', start);
      return cookies.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
  }
  return "";
}

std::string client_ip(const http::request<http::string_body>& req) {
  if (auto it = req.find("X-Forwarded-For"); it != req.end()) {
    std::string ip(it->value());
    auto c = ip.find(',');
    return (c != std::string::npos) ? ip.substr(0, c) : ip;
  }
  if (auto it = req.find("X-Client-IP"); it != req.end()) {
    return std::string(it->value());
  }
  return "127.0.0.1";
}

std::string documents_to_json(const std::vector<evida::ingest::DocumentRecord>& docs) {
  std::ostringstream ss;
  ss << "{\"documents\":[";
  for (std::size_t i = 0; i < docs.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& d = docs[i];
    ss << "{\"id\":" << d.id
       << ",\"sha256\":" << q(d.sha256)
       << ",\"title\":" << q(d.title)
       << ",\"publication_date\":" << q(d.publication_date)
       << ",\"source_uri\":" << q(d.source_uri)
       << ",\"total_pages\":" << d.total_pages
       << ",\"evidence_count\":" << d.evidence_count
       << ",\"created_at\":" << q(d.created_at) << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string evidences_to_json(const std::vector<evida::ingest::EvidenceRecord>& evs) {
  std::ostringstream ss;
  ss << "{\"evidences\":[";
  for (std::size_t i = 0; i < evs.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& e = evs[i];
    ss << "{\"id\":" << e.id
       << ",\"source_document_id\":" << e.source_document_id
       << ",\"doc_title\":" << q(e.doc_title)
       << ",\"doc_sha256\":" << q(e.doc_sha256)
       << ",\"page\":" << e.page
       << ",\"excerpt\":" << q(e.excerpt)
       << ",\"status\":" << q(e.status)
       << ",\"taxonomy_version\":" << q(e.taxonomy_version)
       << ",\"reviewer\":" << q(e.reviewer)
       << ",\"created_at\":" << q(e.created_at) << "}";
  }
  ss << "]}";
  return ss.str();
}

http::response<http::string_body> respond(http::status status, unsigned version, const std::string& body, std::string_view content_type = "application/json") {
  http::response<http::string_body> res{status, version};
  res.set(http::field::server, "evida/0.2.0");
  res.set(http::field::cache_control, "no-store");
  res.set("X-Content-Type-Options", "nosniff");
  res.set(http::field::content_type, std::string(content_type));
  res.body() = body;
  res.prepare_payload();
  return res;
}

http::response<http::string_body> handle_request(const http::request<http::string_body>& req,
                                                evida::Store& store,
                                                evida::auth::AuthSmtpService& auth_service) {
  const unsigned v = req.version();
  const std::string target{req.target()};
  const std::string path = target.substr(0, target.find('?'));
  const std::string query = (target.find('?') != std::string::npos) ? target.substr(target.find('?') + 1) : "";

  // 1. Static Web Assets
  if (req.method() == http::verb::get) {
    if (path == "/" || path == "/index.html" || path == "/app.css" ||
        path == "/app.js" || path == "/favicon.svg") {
      const std::string filename = (path == "/") ? "index.html" : path.substr(1);
      auto res = respond(http::status::ok, v, file_contents(std::filesystem::path(EVIDA_WEB_ROOT) / filename), media_type(path));
      res.set("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; script-src 'self' 'unsafe-inline'; img-src 'self' data:;");
      return res;
    }
  }

  // 2. Health & Capabilities
  if (req.method() == http::verb::get && path == "/api/health") {
    return respond(http::status::ok, v, "{\"status\":\"ok\",\"system\":\"evida\",\"version\":\"0.2.0\"}");
  }

  if (req.method() == http::verb::get && path == "/api/v1/capabilities") {
    return respond(http::status::ok, v,
      "{\"document_ingestion\":true,\"pdf_extraction\":true,"
      "\"graph_analysis\":false,\"episode_review\":true,"
      "\"sqlite_schema\":true,\"provenance_rules\":true,\"auth_smtp\":true}");
  }

  if (req.method() == http::verb::get && path == "/api/v1/stats") {
    return respond(http::status::ok, v, stats_json(store));
  }

  // 3. Authentication Routes (Padrão MORFO)
  if (req.method() == http::verb::get && path == "/api/v1/auth/me") {
    std::string token = extract_token(req);
    auto user = auth_service.validate_session(store.db(), token);
    if (!user) {
      return respond(http::status::ok, v, "{\"authenticated\":false}");
    }
    std::ostringstream ss;
    ss << "{\"authenticated\":true,\"user\":{"
       << "\"id\":" << q(user->id)
       << ",\"email\":" << q(user->email)
       << ",\"name\":" << q(user->name)
       << ",\"role\":" << q(user->role) << "}}";
    return respond(http::status::ok, v, ss.str());
  }

  if (req.method() == http::verb::get && path == "/api/v1/auth/providers") {
    std::ostringstream ss;
    ss << "{\"email_otp\":true,\"dev_mode\":" << (auth_service.config().dev_mode ? "true" : "false")
       << ",\"smtp_from\":" << q(auth_service.config().smtp_from) << "}";
    return respond(http::status::ok, v, ss.str());
  }

  if (req.method() == http::verb::post && path == "/api/v1/auth/request-code") {
    std::string email = evida::auth::normalize_email(json_extract_string(req.body(), "email"));
    std::string ip = client_ip(req);
    auto res = auth_service.request_code(store.db(), email, ip);
    std::ostringstream ss;
    ss << "{\"status\":" << (res.ok ? "\"ok\"" : "\"error\"")
       << ",\"message\":" << q(res.user_message);
    if (!res.dev_code.empty()) {
      ss << ",\"dev_code\":" << q(res.dev_code);
    }
    if (res.retry_after_seconds > 0) {
      ss << ",\"retry_after_seconds\":" << res.retry_after_seconds;
    }
    ss << "}";
    return respond(static_cast<http::status>(res.status_code), v, ss.str());
  }

  if (req.method() == http::verb::post && path == "/api/v1/auth/verify-code") {
    std::string email = evida::auth::normalize_email(json_extract_string(req.body(), "email"));
    std::string code = json_extract_string(req.body(), "code");
    std::string ip = client_ip(req);
    auto res = auth_service.verify_code(store.db(), email, code, ip);
    if (!res.ok) {
      std::ostringstream err;
      err << "{\"status\":\"error\",\"message\":" << q(res.user_message)
          << ",\"remaining_attempts\":" << res.remaining_attempts << "}";
      return respond(static_cast<http::status>(res.status_code), v, err.str());
    }
    std::ostringstream ss;
    ss << "{\"status\":\"ok\",\"session_token\":" << q(res.session_token)
       << ",\"user\":{"
       << "\"id\":" << q(res.user.id)
       << ",\"email\":" << q(res.user.email)
       << ",\"name\":" << q(res.user.name)
       << ",\"role\":" << q(res.user.role) << "}}";
    auto resp = respond(http::status::ok, v, ss.str());
    resp.set(http::field::set_cookie, "evida_session=" + res.session_token + "; Path=/; HttpOnly; SameSite=Lax");
    return resp;
  }

  if (req.method() == http::verb::post && path == "/api/v1/auth/logout") {
    std::string token = extract_token(req);
    if (!token.empty()) {
      auth_service.revoke_session(store.db(), token);
    }
    auto resp = respond(http::status::ok, v, "{\"status\":\"ok\"}");
    resp.set(http::field::set_cookie, "evida_session=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; HttpOnly; SameSite=Lax");
    return resp;
  }

  if (req.method() == http::verb::get && path == "/api/v1/auth/users") {
    std::string token = extract_token(req);
    auto user = auth_service.validate_session(store.db(), token);
    if (!user) return respond(http::status::unauthorized, v, "{\"error\":\"unauthorized\"}");

    auto users = auth_service.list_users(store.db());
    std::ostringstream ss;
    ss << "{\"users\":[";
    for (std::size_t i = 0; i < users.size(); ++i) {
      if (i > 0) ss << ",";
      ss << "{\"id\":" << q(users[i].id)
         << ",\"email\":" << q(users[i].email)
         << ",\"name\":" << q(users[i].name)
         << ",\"role\":" << q(users[i].role)
         << ",\"is_active\":" << (users[i].is_active ? "true" : "false")
         << ",\"created_at\":" << q(users[i].created_at)
         << ",\"last_login_at\":" << q(users[i].last_login_at) << "}";
    }
    ss << "]}";
    return respond(http::status::ok, v, ss.str());
  }

  // 4. Ingestion & Documents API
  if (req.method() == http::verb::get && path == "/api/v1/documents") {
    auto docs = evida::ingest::list_documents(store.db());
    return respond(http::status::ok, v, documents_to_json(docs));
  }

  if (req.method() == http::verb::get && path.starts_with("/api/v1/documents/")) {
    std::string sub = path.substr(std::string("/api/v1/documents/").size());
    auto slash = sub.find('/');
    if (slash == std::string::npos) {
      // Document detail: /api/v1/documents/<id>
      try {
        int64_t id = std::stoll(sub);
        auto doc = evida::ingest::get_document_by_id(store.db(), id);
        if (!doc) return respond(http::status::not_found, v, "{\"error\":\"document_not_found\"}");

        auto pages = evida::ingest::get_document_pages(store.db(), id);
        std::ostringstream ss;
        ss << "{\"document\":{"
           << "\"id\":" << doc->id
           << ",\"sha256\":" << q(doc->sha256)
           << ",\"title\":" << q(doc->title)
           << ",\"publication_date\":" << q(doc->publication_date)
           << ",\"source_uri\":" << q(doc->source_uri)
           << ",\"total_pages\":" << doc->total_pages
           << ",\"evidence_count\":" << doc->evidence_count
           << ",\"created_at\":" << q(doc->created_at)
           << "},\"pages\":[";
        for (std::size_t i = 0; i < pages.size(); ++i) {
          if (i > 0) ss << ",";
          ss << "{\"page\":" << pages[i].page_number
             << ",\"excerpt\":" << q(pages[i].text_content.substr(0, 180)) << "}";
        }
        ss << "]}";
        return respond(http::status::ok, v, ss.str());
      } catch (...) {
        return respond(http::status::bad_request, v, "{\"error\":\"invalid_id\"}");
      }
    } else {
      // Page detail: /api/v1/documents/<id>/pages/<page>
      std::string id_str = sub.substr(0, slash);
      std::string rest = sub.substr(slash + 1);
      if (rest.starts_with("pages/")) {
        std::string page_str = rest.substr(std::string("pages/").size());
        try {
          int64_t doc_id = std::stoll(id_str);
          int page_num = std::stoi(page_str);
          auto page = evida::ingest::get_document_page(store.db(), doc_id, page_num);
          if (!page) return respond(http::status::not_found, v, "{\"error\":\"page_not_found\"}");

          std::ostringstream ss;
          ss << "{\"page\":{"
             << "\"source_document_id\":" << page->source_document_id
             << ",\"page_number\":" << page->page_number
             << ",\"text\":" << q(page->text_content)
             << ",\"created_at\":" << q(page->created_at) << "}}";
          return respond(http::status::ok, v, ss.str());
        } catch (...) {
          return respond(http::status::bad_request, v, "{\"error\":\"invalid_parameters\"}");
        }
      }
    }
  }

  // Sample ingestion endpoint (Emater PDF)
  if (req.method() == http::verb::post && (path == "/api/v1/documents/ingest-sample" || path == "/api/v1/ingest/sample")) {
    std::filesystem::path sample_pdf = "/tmp/evida_sample_conj_08012026.pdf";
    if (!std::filesystem::exists(sample_pdf)) {
      // Unpack sample from evida-reference archive if available
      std::filesystem::path ref_zip = "/run/media/jpereiratrindade/labeco10T/dev/cpp/evida-reference/archive/analise_emater.zip";
      if (std::filesystem::exists(ref_zip)) {
        std::string cmd = "unzip -p " + ref_zip.string() + " analise_emater/webapp_data/jobs/5c878af68a23/input/conj_08012026.pdf > " + sample_pdf.string() + " 2>/dev/null";
        int rc = std::system(cmd.c_str());
        (void)rc;
      }
    }

    if (!std::filesystem::exists(sample_pdf)) {
      return respond(http::status::not_found, v, "{\"error\":\"sample_file_not_found\",\"details\":\"Informativo de referência não localizado em archive/\"}");
    }

    evida::ingest::IngestOptions opts;
    opts.title = "Informativo Conjuntural nº 1901 - Emater/RS-Ascar (08/01/2026)";
    opts.publication_date = "2026-01-08";
    opts.source_uri = "https://www.emater.tche.br/site/infoagro/informativo_conjuntural.php";
    opts.actor = "pesquisador@labeco.local";

    auto res = evida::ingest::ingest_pdf_document(store.db(), sample_pdf, opts);
    if (!res.ok) {
      return respond(http::status::internal_server_error, v, "{\"error\":" + q(res.error_message) + "}");
    }

    std::ostringstream ss;
    ss << "{\"status\":\"ok\",\"document_id\":" << res.document_id
       << ",\"sha256\":" << q(res.sha256)
       << ",\"title\":" << q(res.title)
       << ",\"total_pages\":" << res.total_pages
       << ",\"extracted_evidences\":" << res.extracted_evidences << "}";
    return respond(http::status::ok, v, ss.str());
  }

  // Generic document ingest from filepath
  if (req.method() == http::verb::post && path == "/api/v1/documents/ingest") {
    std::string file_path = json_extract_string(req.body(), "file_path");
    if (file_path.empty()) file_path = json_extract_string(req.body(), "path");
    if (file_path.empty()) {
      return respond(http::status::bad_request, v, "{\"error\":\"missing_file_path\"}");
    }

    evida::ingest::IngestOptions opts;
    opts.title = json_extract_string(req.body(), "title");
    opts.publication_date = json_extract_string(req.body(), "publication_date");
    opts.source_uri = json_extract_string(req.body(), "source_uri");

    std::string token = extract_token(req);
    auto user = auth_service.validate_session(store.db(), token);
    opts.actor = user ? user->email : "pesquisador@labeco.local";

    auto res = evida::ingest::ingest_pdf_document(store.db(), file_path, opts);
    if (!res.ok) {
      return respond(http::status::internal_server_error, v, "{\"error\":" + q(res.error_message) + "}");
    }

    std::ostringstream ss;
    ss << "{\"status\":\"ok\",\"document_id\":" << res.document_id
       << ",\"sha256\":" << q(res.sha256)
       << ",\"title\":" << q(res.title)
       << ",\"total_pages\":" << res.total_pages
       << ",\"extracted_evidences\":" << res.extracted_evidences << "}";
    return respond(http::status::ok, v, ss.str());
  }

  // 5. Evidences API
  if (req.method() == http::verb::get && path == "/api/v1/evidences") {
    int64_t doc_id = 0;
    std::string status_filter;
    if (!query.empty()) {
      auto p = query.find("doc_id=");
      if (p != std::string::npos) {
        auto end = query.find('&', p);
        try { doc_id = std::stoll(query.substr(p + 7, end == std::string::npos ? std::string::npos : end - (p + 7))); } catch (...) {}
      }
      auto s = query.find("status=");
      if (s != std::string::npos) {
        auto end = query.find('&', s);
        status_filter = query.substr(s + 7, end == std::string::npos ? std::string::npos : end - (s + 7));
      }
    }
    auto evs = evida::ingest::list_evidences(store.db(), doc_id, status_filter);
    return respond(http::status::ok, v, evidences_to_json(evs));
  }

  if (req.method() == http::verb::post && path == "/api/v1/evidences") {
    int64_t doc_id = json_extract_int64(req.body(), "source_document_id");
    int page = static_cast<int>(json_extract_int64(req.body(), "page"));
    std::string excerpt = json_extract_string(req.body(), "excerpt");

    std::string token = extract_token(req);
    auto user = auth_service.validate_session(store.db(), token);
    std::string actor = user ? user->email : "pesquisador@labeco.local";

    int64_t new_id = evida::ingest::add_evidence(store.db(), doc_id, page, excerpt, actor);
    if (new_id <= 0) {
      return respond(http::status::bad_request, v, "{\"error\":\"failed_to_create_evidence\"}");
    }
    return respond(http::status::ok, v, "{\"status\":\"ok\",\"evidence_id\":" + std::to_string(new_id) + "}");
  }

  if (req.method() == http::verb::post && path.starts_with("/api/v1/evidences/") && path.ends_with("/status")) {
    std::string mid = path.substr(std::string("/api/v1/evidences/").size());
    mid = mid.substr(0, mid.rfind("/status"));
    try {
      int64_t evidence_id = std::stoll(mid);
      std::string new_status = json_extract_string(req.body(), "status");
      std::string token = extract_token(req);
      auto user = auth_service.validate_session(store.db(), token);
      std::string actor = user ? user->email : "pesquisador@labeco.local";

      bool ok = evida::ingest::update_evidence_status(store.db(), evidence_id, new_status, actor);
      if (!ok) return respond(http::status::bad_request, v, "{\"error\":\"failed_to_update_status\"}");
      return respond(http::status::ok, v, "{\"status\":\"ok\",\"evidence_id\":" + std::to_string(evidence_id) + ",\"new_status\":" + q(new_status) + "}");
    } catch (...) {
      return respond(http::status::bad_request, v, "{\"error\":\"invalid_evidence_id\"}");
    }
  }

  return respond(http::status::not_found, v, "{\"error\":\"not_found\"}");
}

void session(tcp::socket& socket, evida::Store& store, evida::auth::AuthSmtpService& auth_service) {
  beast::flat_buffer buffer;
  http::request_parser<http::string_body> parser;
  parser.body_limit(32 * 1024 * 1024); // Support up to 32MB uploads
  parser.header_limit(16 * 1024);
  beast::error_code ec;
  http::read(socket, buffer, parser, ec);
  if (ec) return;
  auto res = handle_request(parser.get(), store, auth_service);
  http::write(socket, res, ec);
  socket.shutdown(tcp::socket::shutdown_both, ec);
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    unsigned long port = 18142;
    std::string dbfile = "var/evida.sqlite";
    bool dev_mode = true;
    for (int i = 1; i < argc; ++i) {
      if (std::string_view(argv[i]) == "--port" && i + 1 < argc) {
        port = std::stoul(argv[++i]);
      } else if (std::string_view(argv[i]) == "--db" && i + 1 < argc) {
        dbfile = argv[++i];
      } else if (std::string_view(argv[i]) == "--dev") {
        dev_mode = true;
      } else if (std::string_view(argv[i]) == "--prod") {
        dev_mode = false;
      } else if (std::string_view(argv[i]) == "--add-user" && i + 3 < argc) {
        std::string email = argv[++i];
        std::string name = argv[++i];
        std::string role = argv[++i];
        evida::Store s(dbfile);
        evida::auth::AuthSmtpService as(evida::auth::AuthSmtpConfig::from_environment());
        if (as.add_user(s.db(), email, name, role)) {
          std::cout << "Usuário adicionado com sucesso: " << email << " (" << role << ")\n";
        } else {
          std::cerr << "Falha ao adicionar usuário: " << email << "\n";
        }
        return 0;
      } else if (std::string_view(argv[i]) == "--help") {
        std::cout << "EVIDA v0.2.0 — Observatório de Evidências Científicas\n";
        std::cout << "Uso: evida-server [--port 18142] [--db var/evida.sqlite] [--dev|--prod]\n";
        std::cout << "                  [--add-user <email> <nome> <role>]\n";
        return 0;
      } else {
        throw std::runtime_error("Argumento desconhecido ou incompleto: " + std::string(argv[i]));
      }
    }
    if (port == 0 || port > 65535) throw std::runtime_error("Porta inválida");

    evida::Store store(dbfile);
    auto auth_config = evida::auth::AuthSmtpConfig::from_environment();
    auth_config.dev_mode = dev_mode;
    evida::auth::AuthSmtpService auth_service(auth_config);

    net::io_context io{1};
    tcp::acceptor acceptor(io, {net::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
    std::cout << "EVIDA v0.2.0 em http://127.0.0.1:" << port << " [DevMode=" << (dev_mode ? "ON" : "OFF") << "]" << std::endl;

    for (;;) {
      tcp::socket socket(io);
      acceptor.accept(socket);
      try { session(socket, store, auth_service); }
      catch (const std::exception& e) { std::cerr << "Falha na requisição: " << e.what() << '\n'; }
    }
  } catch (const std::exception& e) {
    std::cerr << "EVIDA: " << e.what() << '\n';
    return 1;
  }
}
