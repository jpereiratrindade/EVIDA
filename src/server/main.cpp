// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.

#include "evida/auth.hpp"
#include "evida/corpus.hpp"
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

std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int ch = 0;
      std::istringstream hex(std::string(s.substr(i + 1, 2)));
      if (hex >> std::hex >> ch) {
        out += static_cast<char>(ch);
        i += 2;
        continue;
      }
    }
    if (s[i] == '+') out += ' ';
    else out += s[i];
  }
  return out;
}

std::string get_query_param(std::string_view query, const std::string& key) {
  if (query.empty()) return "";
  std::string target = key + "=";
  auto pos = query.find(target);
  if (pos == std::string_view::npos) return "";
  auto val_start = pos + target.size();
  auto end = query.find('&', val_start);
  return url_decode(end == std::string_view::npos ? query.substr(val_start) : query.substr(val_start, end - val_start));
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
         ",\"users\":" + std::to_string(stats.users) +
         ",\"events\":" + std::to_string(stats.events) +
         ",\"observations\":" + std::to_string(stats.observations) +
         ",\"responses\":" + std::to_string(stats.responses) +
         ",\"prices\":" + std::to_string(stats.prices) +
         ",\"episodes\":" + std::to_string(stats.episodes) +
         ",\"municipalities\":" + std::to_string(stats.municipalities) +
         ",\"municipal_occurrences\":" + std::to_string(stats.municipal_occurrences) + "}";
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

std::string events_to_json(const std::vector<evida::corpus::EventItem>& events) {
  std::ostringstream ss;
  ss << "{\"events\":[";
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& e = events[i];
    ss << "{\"evento_id\":" << q(e.evento_id)
       << ",\"documento_id\":" << q(e.documento_id)
       << ",\"arquivo_origem\":" << q(e.arquivo_origem)
       << ",\"numero_informativo\":" << e.numero_informativo
       << ",\"data_publicacao\":" << q(e.data_publicacao)
       << ",\"regiao_emater\":" << q(e.regiao_emater)
       << ",\"municipio\":" << q(e.municipio)
       << ",\"atividade\":" << q(e.atividade)
       << ",\"tipo_evento\":" << q(e.tipo_evento)
       << ",\"classe_evento\":" << q(e.classe_evento)
       << ",\"descricao_evento\":" << q(e.descricao_evento)
       << ",\"efeito_produtivo\":" << q(e.efeito_produtivo)
       << ",\"efeito_economico\":" << q(e.efeito_economico)
       << ",\"pagina\":" << e.pagina
       << ",\"secao\":" << q(e.secao)
       << ",\"evidencia_textual\":" << q(e.evidencia_textual)
       << ",\"confianca\":" << q(e.confianca) << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string observations_to_json(const std::vector<evida::corpus::ObservationItem>& obs) {
  std::ostringstream ss;
  ss << "{\"observations\":[";
  for (std::size_t i = 0; i < obs.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& o = obs[i];
    ss << "{\"observacao_id\":" << q(o.observacao_id)
       << ",\"documento_id\":" << q(o.documento_id)
       << ",\"arquivo_origem\":" << q(o.arquivo_origem)
       << ",\"numero_informativo\":" << o.numero_informativo
       << ",\"data_publicacao\":" << q(o.data_publicacao)
       << ",\"dimensao\":" << q(o.dimensao)
       << ",\"categoria\":" << q(o.categoria)
       << ",\"atividade\":" << q(o.atividade)
       << ",\"cultura_criacao\":" << q(o.cultura_criacao)
       << ",\"variavel\":" << q(o.variavel)
       << ",\"valor\":" << q(o.valor)
       << ",\"unidade\":" << q(o.unidade)
       << ",\"regiao_emater\":" << q(o.regiao_emater)
       << ",\"municipio\":" << q(o.municipio)
       << ",\"pagina\":" << o.pagina
       << ",\"secao\":" << q(o.secao)
       << ",\"evidencia_textual\":" << q(o.evidencia_textual)
       << ",\"confianca\":" << q(o.confianca) << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string municipalities_to_json(const std::vector<evida::corpus::MunicipalityItem>& muns) {
  std::ostringstream ss;
  ss << "{\"municipalities\":[";
  for (std::size_t i = 0; i < muns.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& m = muns[i];
    ss << "{\"codigo_ibge\":" << q(m.codigo_ibge)
       << ",\"municipio\":" << q(m.municipio)
       << ",\"uf\":" << q(m.uf)
       << ",\"occurrences_count\":" << m.occurrences_count
       << ",\"top_classes\":" << q(m.top_classes) << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string municipal_occurrences_to_json(const std::vector<evida::corpus::MunicipalOccurrenceItem>& ocs) {
  std::ostringstream ss;
  ss << "{\"occurrences\":[";
  for (std::size_t i = 0; i < ocs.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& o = ocs[i];
    ss << "{\"ocorrencia_id\":" << q(o.ocorrencia_id)
       << ",\"codigo_ibge\":" << q(o.codigo_ibge)
       << ",\"municipio\":" << q(o.municipio)
       << ",\"data_publicacao\":" << q(o.data_publicacao)
       << ",\"data_ocorrencia\":" << q(o.data_ocorrencia)
       << ",\"regiao_emater\":" << q(o.regiao_emater)
       << ",\"classe_ocorrencia\":" << q(o.classe_ocorrencia)
       << ",\"subclasse_ocorrencia\":" << q(o.subclasse_ocorrencia)
       << ",\"atividade\":" << q(o.atividade)
       << ",\"descricao\":" << q(o.descricao)
       << ",\"documento\":" << q(o.documento)
       << ",\"numero_informativo\":" << o.numero_informativo
       << ",\"pagina\":" << o.pagina
       << ",\"evidencia_textual\":" << q(o.evidencia_textual)
       << ",\"confianca\":" << q(o.confianca) << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string prices_to_json(const std::vector<evida::corpus::PriceItem>& prices) {
  std::ostringstream ss;
  ss << "{\"prices\":[";
  for (std::size_t i = 0; i < prices.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& p = prices[i];
    ss << "{\"documento_id\":" << q(p.documento_id)
       << ",\"data_publicacao\":" << q(p.data_publicacao)
       << ",\"produto\":" << q(p.produto)
       << ",\"regiao\":" << q(p.regiao)
       << ",\"preco_medio\":" << q(p.preco_medio)
       << ",\"preco_minimo\":" << q(p.preco_minimo)
       << ",\"preco_maximo\":" << q(p.preco_maximo)
       << ",\"unidade\":" << q(p.unidade)
       << ",\"pagina\":" << p.pagina
       << ",\"evidencia_textual\":" << q(p.evidencia_textual) << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string episodes_to_json(const std::vector<evida::corpus::EpisodeItem>& episodes) {
  std::ostringstream ss;
  ss << "{\"episodes\":[";
  for (std::size_t i = 0; i < episodes.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& ep = episodes[i];
    ss << "{\"episodio_id\":" << q(ep.episodio_id)
       << ",\"territorio\":" << q(ep.territorio)
       << ",\"atividade\":" << q(ep.atividade)
       << ",\"data_inicio\":" << q(ep.data_inicio)
       << ",\"data_fim\":" << q(ep.data_fim)
       << ",\"perturbacao\":" << q(ep.perturbacao)
       << ",\"impactos_observados\":" << q(ep.impactos_observados)
       << ",\"respostas_observadas\":" << q(ep.respostas_observadas)
       << ",\"nivel_confianca\":" << q(ep.nivel_confianca)
       << ",\"numero_documentos\":" << ep.numero_documentos << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string vault_documents_to_json(const std::vector<evida::corpus::VaultDocumentItem>& docs) {
  std::ostringstream ss;
  ss << "{\"vault_documents\":[";
  for (std::size_t i = 0; i < docs.size(); ++i) {
    if (i > 0) ss << ",";
    const auto& d = docs[i];
    ss << "{\"documento_id\":" << q(d.documento_id)
       << ",\"arquivo\":" << q(d.arquivo)
       << ",\"numero_informativo\":" << d.numero_informativo
       << ",\"data_publicacao\":" << q(d.data_publicacao)
       << ",\"ano\":" << d.ano
       << ",\"numero_paginas\":" << d.numero_paginas
       << ",\"caracteres_extraidos\":" << d.caracteres_extraidos
       << ",\"sha256\":" << q(d.sha256)
       << ",\"sha256_texto_normalizado\":" << q(d.sha256_texto_normalizado)
       << ",\"in_vault\":" << (d.in_vault ? "true" : "false") << "}";
  }
  ss << "]}";
  return ss.str();
}

std::string audit_proof_to_json(const evida::corpus::AuditProofItem& p) {
  std::ostringstream ss;
  ss << "{\"proof\":{"
     << "\"entity_id\":" << q(p.entity_id)
     << ",\"entity_type\":" << q(p.entity_type)
     << ",\"documento_id\":" << q(p.documento_id)
     << ",\"arquivo\":" << q(p.arquivo)
     << ",\"numero_informativo\":" << p.numero_informativo
     << ",\"data_publicacao\":" << q(p.data_publicacao)
     << ",\"pagina\":" << p.pagina
     << ",\"secao\":" << q(p.secao)
     << ",\"evidencia_textual\":" << q(p.evidencia_textual)
     << ",\"sha256\":" << q(p.sha256)
     << ",\"sha256_texto_normalizado\":" << q(p.sha256_texto_normalizado)
     << ",\"in_vault\":" << (p.in_vault ? "true" : "false")
     << ",\"status_custodia\":" << (p.in_vault ? "\"preservado_no_cofre\"" : "\"ausente_no_cofre\"")
     << ",\"integridade_criptografica\":\"SHA-256 verificado\""
     << ",\"metodologia\":\"Evidência documental de reporte (não comprovação causal automática)\""
     << ",\"page_text\":" << q(p.page_text)
     << "}}";
  return ss.str();
}

http::response<http::string_body> respond(http::status status, unsigned version, const std::string& body, std::string_view content_type = "application/json") {
  http::response<http::string_body> res{status, version};
  res.set(http::field::server, "evida/0.3.0");
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
  if (req.method() == http::verb::get || req.method() == http::verb::head) {
    if (path == "/" || path == "/index.html" || path == "/app.css" ||
        path == "/app.js" || path == "/favicon.svg") {
      const std::string filename = (path == "/") ? "index.html" : path.substr(1);
      auto body = (req.method() == http::verb::head) ? "" : file_contents(std::filesystem::path(EVIDA_WEB_ROOT) / filename);
      auto res = respond(http::status::ok, v, body, media_type(path));
      res.set("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; script-src 'self' 'unsafe-inline'; img-src 'self' data:;");
      return res;
    }
  }

  // 2. Health & Capabilities
  if (req.method() == http::verb::get && path == "/api/health") {
    return respond(http::status::ok, v, "{\"status\":\"ok\",\"system\":\"evida\",\"version\":\"0.3.0\"}");
  }

  if (req.method() == http::verb::get && path == "/api/v1/capabilities") {
    return respond(http::status::ok, v,
      "{\"document_ingestion\":true,\"pdf_extraction\":true,"
      "\"graph_analysis\":true,\"episode_review\":true,"
      "\"sqlite_schema\":true,\"provenance_rules\":true,\"auth_smtp\":true,"
      "\"scientific_corpus\":true,\"audit_vault\":true}");
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

  // Binary File Upload (Padrão de envio contínuo como MORFO)
  if (req.method() == http::verb::post && path == "/api/v1/documents/upload") {
    std::string filename = get_query_param(query, "filename");
    if (filename.empty()) filename = "informativo_emater.pdf";
    std::string title = get_query_param(query, "title");
    std::string pub_date = get_query_param(query, "publication_date");

    if (req.body().empty()) {
      return respond(http::status::bad_request, v, "{\"error\":\"Arquivo vazio para upload\"}");
    }

    evida::ingest::IngestOptions opts;
    opts.title = title.empty() ? filename : title;
    opts.publication_date = pub_date;
    std::string token = extract_token(req);
    auto user = auth_service.validate_session(store.db(), token);
    opts.actor = user ? user->email : "pesquisador@labeco.local";

    auto res = evida::ingest::ingest_pdf_bytes(store.db(), req.body(), filename, opts);
    if (!res.ok) {
      return respond(http::status::unprocessable_entity, v, "{\"error\":" + q(res.error_message) + "}");
    }

    std::ostringstream ss;
    ss << "{\"status\":\"ok\",\"document_id\":" << res.document_id
       << ",\"sha256\":" << q(res.sha256)
       << ",\"title\":" << q(res.title)
       << ",\"total_pages\":" << res.total_pages
       << ",\"extracted_evidences\":" << res.extracted_evidences << "}";
    return respond(http::status::created, v, ss.str());
  }

  // Generic document ingest from filepath (JSON)
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

  // Document and Page detail endpoints
  if (req.method() == http::verb::get && path.starts_with("/api/v1/documents/")) {
    std::string sub = path.substr(std::string("/api/v1/documents/").size());
    auto slash = sub.find('/');
    if (slash == std::string::npos) {
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

  // 5. Scientific Corpus APIs
  // 5.1 Events API
  if (req.method() == http::verb::get && path == "/api/v1/events") {
    std::string classe = get_query_param(query, "classe");
    if (classe.empty()) classe = get_query_param(query, "classe_evento");
    std::string regiao = get_query_param(query, "regiao");
    if (regiao.empty()) regiao = get_query_param(query, "regiao_emater");
    std::string atividade = get_query_param(query, "atividade");
    int limit = 50;
    std::string limit_str = get_query_param(query, "limit");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
    int offset = 0;
    std::string offset_str = get_query_param(query, "offset");
    if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}

    auto events = evida::corpus::list_events(store.db(), classe, regiao, atividade, limit, offset);
    return respond(http::status::ok, v, events_to_json(events));
  }

  // 5.2 Observations API
  if (req.method() == http::verb::get && path == "/api/v1/observations") {
    std::string atividade = get_query_param(query, "atividade");
    std::string variavel = get_query_param(query, "variavel");
    std::string regiao = get_query_param(query, "regiao");
    if (regiao.empty()) regiao = get_query_param(query, "regiao_emater");
    int limit = 50;
    std::string limit_str = get_query_param(query, "limit");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}
    int offset = 0;
    std::string offset_str = get_query_param(query, "offset");
    if (!offset_str.empty()) try { offset = std::stoi(offset_str); } catch (...) {}

    auto obs = evida::corpus::list_observations(store.db(), atividade, variavel, regiao, limit, offset);
    return respond(http::status::ok, v, observations_to_json(obs));
  }

  // 5.3 Municipalities API
  if (req.method() == http::verb::get && path == "/api/v1/municipalities") {
    std::string q_param = get_query_param(query, "q");
    if (q_param.empty()) q_param = get_query_param(query, "search");
    int limit = 100;
    std::string limit_str = get_query_param(query, "limit");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}

    auto muns = evida::corpus::list_municipalities(store.db(), q_param, limit);
    return respond(http::status::ok, v, municipalities_to_json(muns));
  }

  // 5.4 Municipal Occurrences API
  if (req.method() == http::verb::get && (path == "/api/v1/municipalities/occurrences" || path == "/api/v1/occurrences")) {
    std::string codigo_ibge = get_query_param(query, "codigo_ibge");
    std::string classe = get_query_param(query, "classe");
    int limit = 50;
    std::string limit_str = get_query_param(query, "limit");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}

    auto ocs = evida::corpus::list_municipal_occurrences(store.db(), codigo_ibge, classe, limit);
    return respond(http::status::ok, v, municipal_occurrences_to_json(ocs));
  }

  // 5.5 Prices API
  if (req.method() == http::verb::get && path == "/api/v1/prices") {
    std::string produto = get_query_param(query, "produto");
    std::string regiao = get_query_param(query, "regiao");
    int limit = 50;
    std::string limit_str = get_query_param(query, "limit");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}

    auto prices = evida::corpus::list_prices(store.db(), produto, regiao, limit);
    return respond(http::status::ok, v, prices_to_json(prices));
  }

  // 5.6 Episodes API
  if (req.method() == http::verb::get && path == "/api/v1/episodes") {
    int limit = 50;
    std::string limit_str = get_query_param(query, "limit");
    if (!limit_str.empty()) try { limit = std::stoi(limit_str); } catch (...) {}

    auto episodes = evida::corpus::list_episodes(store.db(), limit);
    return respond(http::status::ok, v, episodes_to_json(episodes));
  }

  // 5.7 Vault Custody Documents
  if (req.method() == http::verb::get && path == "/api/v1/vault/documents") {
    auto docs = evida::corpus::list_vault_documents(store.db());
    return respond(http::status::ok, v, vault_documents_to_json(docs));
  }

  // 5.8 Documentary Proof / Audit Drawer
  if (req.method() == http::verb::get && path.starts_with("/api/v1/evidence/") && path.ends_with("/audit")) {
    std::string id = path.substr(std::string("/api/v1/evidence/").size());
    id = id.substr(0, id.rfind("/audit"));
    auto proof = evida::corpus::get_audit_proof(store.db(), id);
    if (!proof) {
      return respond(http::status::not_found, v, "{\"error\":\"audit_proof_not_found\"}");
    }
    return respond(http::status::ok, v, audit_proof_to_json(*proof));
  }

  if (req.method() == http::verb::get && path.starts_with("/api/v1/audit/")) {
    std::string id = path.substr(std::string("/api/v1/audit/").size());
    auto proof = evida::corpus::get_audit_proof(store.db(), id);
    if (!proof) {
      return respond(http::status::not_found, v, "{\"error\":\"audit_proof_not_found\"}");
    }
    return respond(http::status::ok, v, audit_proof_to_json(*proof));
  }

  // 6. Evidences legacy API
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
  parser.body_limit(64 * 1024 * 1024); // Support up to 64MB PDF uploads
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
        std::cout << "EVIDA v0.3.0 — Observatório de Evidências Científicas\n";
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
    tcp::acceptor acceptor(io, {net::ip::make_address("0.0.0.0"), static_cast<unsigned short>(port)});
    std::cout << "EVIDA v0.3.0 em http://127.0.0.1:" << port << " [DevMode=" << (dev_mode ? "ON" : "OFF") << "]" << std::endl;

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
