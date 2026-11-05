// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.

#include "evida/corpus.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace evida::corpus {

namespace {

std::string col_text(sqlite3_stmt* stmt, int col) {
  const auto* txt = sqlite3_column_text(stmt, col);
  return txt ? reinterpret_cast<const char*>(txt) : "";
}

int col_int(sqlite3_stmt* stmt, int col) {
  return sqlite3_column_int(stmt, col);
}

bool check_in_vault(const std::string& sha256) {
  if (sha256.empty()) return false;
  fs::path p = fs::path("storage/documents") / (sha256 + ".pdf");
  return fs::exists(p);
}

std::string get_page_text_for_sha(sqlite3* db, const std::string& sha256, int page) {
  if (sha256.empty() || page <= 0) return "";
  const char* sql = "SELECT dp.text_content FROM document_page dp "
                    "JOIN source_document sd ON dp.source_document_id = sd.id "
                    "WHERE sd.sha256 = ? AND dp.page_number = ? LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
  sqlite3_bind_text(stmt, 1, sha256.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, page);

  std::string result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    result = col_text(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return result;
}

} // namespace

std::vector<EventItem> list_events(sqlite3* db, const std::string& classe,
                                   const std::string& regiao,
                                   const std::string& atividade,
                                   int limit, int offset) {
  std::vector<EventItem> out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  if (offset < 0) offset = 0;

  std::string sql = "SELECT evento_id, documento_id, arquivo_origem, numero_informativo, "
                    "data_publicacao, regiao_emater, municipio, atividade, tipo_evento, "
                    "classe_evento, descricao_evento, efeito_produtivo, efeito_economico, "
                    "pagina, secao, evidencia_textual, confianca FROM eventos WHERE 1=1 ";

  if (!classe.empty()) sql += " AND classe_evento = ? ";
  if (!regiao.empty()) sql += " AND regiao_emater = ? ";
  if (!atividade.empty()) sql += " AND atividade = ? ";
  sql += " ORDER BY data_publicacao DESC, evento_id ASC LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;

  int bidx = 1;
  if (!classe.empty()) sqlite3_bind_text(stmt, bidx++, classe.c_str(), -1, SQLITE_STATIC);
  if (!regiao.empty()) sqlite3_bind_text(stmt, bidx++, regiao.c_str(), -1, SQLITE_STATIC);
  if (!atividade.empty()) sqlite3_bind_text(stmt, bidx++, atividade.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, bidx++, limit);
  sqlite3_bind_int(stmt, bidx++, offset);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    EventItem it;
    it.evento_id = col_text(stmt, 0);
    it.documento_id = col_text(stmt, 1);
    it.arquivo_origem = col_text(stmt, 2);
    it.numero_informativo = col_int(stmt, 3);
    it.data_publicacao = col_text(stmt, 4);
    it.regiao_emater = col_text(stmt, 5);
    it.municipio = col_text(stmt, 6);
    it.atividade = col_text(stmt, 7);
    it.tipo_evento = col_text(stmt, 8);
    it.classe_evento = col_text(stmt, 9);
    it.descricao_evento = col_text(stmt, 10);
    it.efeito_produtivo = col_text(stmt, 11);
    it.efeito_economico = col_text(stmt, 12);
    it.pagina = col_int(stmt, 13);
    it.secao = col_text(stmt, 14);
    it.evidencia_textual = col_text(stmt, 15);
    it.confianca = col_text(stmt, 16);
    out.push_back(std::move(it));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ObservationItem> list_observations(sqlite3* db, const std::string& atividade,
                                               const std::string& variavel,
                                               const std::string& regiao,
                                               int limit, int offset) {
  std::vector<ObservationItem> out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;
  if (offset < 0) offset = 0;

  std::string sql = "SELECT observacao_id, documento_id, arquivo_origem, numero_informativo, "
                    "data_publicacao, dimensao, categoria, atividade, cultura_criacao, "
                    "variavel, valor, unidade, regiao_emater, municipio, "
                    "pagina, secao, evidencia_textual, confianca FROM observacoes WHERE 1=1 ";

  if (!atividade.empty()) sql += " AND atividade = ? ";
  if (!variavel.empty()) sql += " AND variavel = ? ";
  if (!regiao.empty()) sql += " AND regiao_emater = ? ";
  sql += " ORDER BY data_publicacao DESC, observacao_id ASC LIMIT ? OFFSET ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;

  int bidx = 1;
  if (!atividade.empty()) sqlite3_bind_text(stmt, bidx++, atividade.c_str(), -1, SQLITE_STATIC);
  if (!variavel.empty()) sqlite3_bind_text(stmt, bidx++, variavel.c_str(), -1, SQLITE_STATIC);
  if (!regiao.empty()) sqlite3_bind_text(stmt, bidx++, regiao.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, bidx++, limit);
  sqlite3_bind_int(stmt, bidx++, offset);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ObservationItem it;
    it.observacao_id = col_text(stmt, 0);
    it.documento_id = col_text(stmt, 1);
    it.arquivo_origem = col_text(stmt, 2);
    it.numero_informativo = col_int(stmt, 3);
    it.data_publicacao = col_text(stmt, 4);
    it.dimensao = col_text(stmt, 5);
    it.categoria = col_text(stmt, 6);
    it.atividade = col_text(stmt, 7);
    it.cultura_criacao = col_text(stmt, 8);
    it.variavel = col_text(stmt, 9);
    it.valor = col_text(stmt, 10);
    it.unidade = col_text(stmt, 11);
    it.regiao_emater = col_text(stmt, 12);
    it.municipio = col_text(stmt, 13);
    it.pagina = col_int(stmt, 14);
    it.secao = col_text(stmt, 15);
    it.evidencia_textual = col_text(stmt, 16);
    it.confianca = col_text(stmt, 17);
    out.push_back(std::move(it));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<MunicipalityItem> list_municipalities(sqlite3* db, const std::string& query,
                                                   int limit) {
  std::vector<MunicipalityItem> out;
  if (limit <= 0) limit = 100;
  if (limit > 500) limit = 500;

  std::string sql = "SELECT m.codigo_ibge, m.municipio, m.uf, "
                    "COUNT(o.ocorrencia_id) as total_ocs, "
                    "GROUP_CONCAT(DISTINCT o.classe_ocorrencia) as top_classes "
                    "FROM municipios_ibge_rs m "
                    "LEFT JOIN ocorrencias_municipais o ON m.codigo_ibge = o.codigo_ibge ";

  if (!query.empty()) {
    sql += " WHERE m.municipio LIKE ? OR m.codigo_ibge LIKE ? ";
  }
  sql += " GROUP BY m.codigo_ibge ORDER BY total_ocs DESC, m.municipio ASC LIMIT ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;

  int bidx = 1;
  if (!query.empty()) {
    std::string wildcard = "%" + query + "%";
    sqlite3_bind_text(stmt, bidx++, wildcard.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, bidx++, wildcard.c_str(), -1, SQLITE_TRANSIENT);
  }
  sqlite3_bind_int(stmt, bidx++, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MunicipalityItem it;
    it.codigo_ibge = col_text(stmt, 0);
    it.municipio = col_text(stmt, 1);
    it.uf = col_text(stmt, 2);
    it.occurrences_count = col_int(stmt, 3);
    it.top_classes = col_text(stmt, 4);
    out.push_back(std::move(it));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<MunicipalOccurrenceItem> list_municipal_occurrences(sqlite3* db,
                                                               const std::string& codigo_ibge,
                                                               const std::string& classe,
                                                               int limit) {
  std::vector<MunicipalOccurrenceItem> out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;

  std::string sql = "SELECT ocorrencia_id, codigo_ibge, municipio, data_publicacao, "
                    "data_ocorrencia, regiao_emater, classe_ocorrencia, subclasse_ocorrencia, "
                    "atividade, descricao, documento, numero_informativo, pagina, "
                    "evidencia_textual, confianca_extracao FROM ocorrencias_municipais WHERE 1=1 ";

  if (!codigo_ibge.empty()) sql += " AND codigo_ibge = ? ";
  if (!classe.empty()) sql += " AND classe_ocorrencia = ? ";
  sql += " ORDER BY data_publicacao DESC, ocorrencia_id ASC LIMIT ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;

  int bidx = 1;
  if (!codigo_ibge.empty()) sqlite3_bind_text(stmt, bidx++, codigo_ibge.c_str(), -1, SQLITE_STATIC);
  if (!classe.empty()) sqlite3_bind_text(stmt, bidx++, classe.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, bidx++, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    MunicipalOccurrenceItem it;
    it.ocorrencia_id = col_text(stmt, 0);
    it.codigo_ibge = col_text(stmt, 1);
    it.municipio = col_text(stmt, 2);
    it.data_publicacao = col_text(stmt, 3);
    it.data_ocorrencia = col_text(stmt, 4);
    it.regiao_emater = col_text(stmt, 5);
    it.classe_ocorrencia = col_text(stmt, 6);
    it.subclasse_ocorrencia = col_text(stmt, 7);
    it.atividade = col_text(stmt, 8);
    it.descricao = col_text(stmt, 9);
    it.documento = col_text(stmt, 10);
    it.numero_informativo = col_int(stmt, 11);
    it.pagina = col_int(stmt, 12);
    it.evidencia_textual = col_text(stmt, 13);
    it.confianca = col_text(stmt, 14);
    out.push_back(std::move(it));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<PriceItem> list_prices(sqlite3* db, const std::string& produto,
                                   const std::string& regiao,
                                   int limit) {
  std::vector<PriceItem> out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;

  std::string sql = "SELECT documento_id, data_publicacao, produto, regiao, "
                    "preco_medio, preco_minimo, preco_maximo, unidade, pagina, evidencia_textual "
                    "FROM precos WHERE 1=1 ";

  if (!produto.empty()) sql += " AND produto LIKE ? ";
  if (!regiao.empty()) sql += " AND regiao = ? ";
  sql += " ORDER BY data_publicacao DESC, produto ASC LIMIT ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return out;

  int bidx = 1;
  if (!produto.empty()) {
    std::string w = "%" + produto + "%";
    sqlite3_bind_text(stmt, bidx++, w.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (!regiao.empty()) sqlite3_bind_text(stmt, bidx++, regiao.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, bidx++, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PriceItem it;
    it.documento_id = col_text(stmt, 0);
    it.data_publicacao = col_text(stmt, 1);
    it.produto = col_text(stmt, 2);
    it.regiao = col_text(stmt, 3);
    it.preco_medio = col_text(stmt, 4);
    it.preco_minimo = col_text(stmt, 5);
    it.preco_maximo = col_text(stmt, 6);
    it.unidade = col_text(stmt, 7);
    it.pagina = col_int(stmt, 8);
    it.evidencia_textual = col_text(stmt, 9);
    out.push_back(std::move(it));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<EpisodeItem> list_episodes(sqlite3* db, int limit) {
  std::vector<EpisodeItem> out;
  if (limit <= 0) limit = 50;
  if (limit > 200) limit = 200;

  const char* sql = "SELECT episodio_id, territorio, atividade, data_inicio, data_fim, "
                    "perturbacao, impactos_observados, respostas_observadas, "
                    "nivel_confianca, numero_documentos FROM episodios "
                    "ORDER BY data_inicio DESC, episodio_id ASC LIMIT ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
  sqlite3_bind_int(stmt, 1, limit);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    EpisodeItem it;
    it.episodio_id = col_text(stmt, 0);
    it.territorio = col_text(stmt, 1);
    it.atividade = col_text(stmt, 2);
    it.data_inicio = col_text(stmt, 3);
    it.data_fim = col_text(stmt, 4);
    it.perturbacao = col_text(stmt, 5);
    it.impactos_observados = col_text(stmt, 6);
    it.respostas_observadas = col_text(stmt, 7);
    it.nivel_confianca = col_text(stmt, 8);
    it.numero_documentos = col_int(stmt, 9);
    out.push_back(std::move(it));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<VaultDocumentItem> list_vault_documents(sqlite3* db) {
  std::vector<VaultDocumentItem> out;
  const char* sql = "SELECT documento_id, arquivo, numero_informativo, data_publicacao, "
                    "ano, numero_paginas, caracteres_extraidos, sha256, sha256_texto_normalizado "
                    "FROM documentos ORDER BY numero_informativo ASC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    VaultDocumentItem it;
    it.documento_id = col_text(stmt, 0);
    it.arquivo = col_text(stmt, 1);
    it.numero_informativo = col_int(stmt, 2);
    it.data_publicacao = col_text(stmt, 3);
    it.ano = col_int(stmt, 4);
    it.numero_paginas = col_int(stmt, 5);
    it.caracteres_extraidos = col_int(stmt, 6);
    it.sha256 = col_text(stmt, 7);
    it.sha256_texto_normalizado = col_text(stmt, 8);
    it.in_vault = check_in_vault(it.sha256);
    out.push_back(std::move(it));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::optional<AuditProofItem> get_audit_proof(sqlite3* db, const std::string& entity_id) {
  if (entity_id.empty()) return std::nullopt;

  AuditProofItem proof;
  proof.entity_id = entity_id;

  // 1. Check eventos
  if (entity_id.starts_with("EVT-")) {
    const char* sql = "SELECT e.evento_id, e.documento_id, e.pagina, e.secao, e.evidencia_textual, "
                      "d.arquivo, d.numero_informativo, d.data_publicacao, d.sha256, d.sha256_texto_normalizado "
                      "FROM eventos e LEFT JOIN documentos d ON e.documento_id = d.documento_id "
                      "WHERE e.evento_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, entity_id.c_str(), -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        proof.entity_type = "evento";
        proof.documento_id = col_text(stmt, 1);
        proof.pagina = col_int(stmt, 2);
        proof.secao = col_text(stmt, 3);
        proof.evidencia_textual = col_text(stmt, 4);
        proof.arquivo = col_text(stmt, 5);
        proof.numero_informativo = col_int(stmt, 6);
        proof.data_publicacao = col_text(stmt, 7);
        proof.sha256 = col_text(stmt, 8);
        proof.sha256_texto_normalizado = col_text(stmt, 9);
        proof.in_vault = check_in_vault(proof.sha256);
        proof.page_text = get_page_text_for_sha(db, proof.sha256, proof.pagina);
        sqlite3_finalize(stmt);
        return proof;
      }
      sqlite3_finalize(stmt);
    }
  }

  // 2. Check observacoes
  if (entity_id.starts_with("OBS-")) {
    const char* sql = "SELECT o.observacao_id, o.documento_id, o.pagina, o.secao, o.evidencia_textual, "
                      "d.arquivo, d.numero_informativo, d.data_publicacao, d.sha256, d.sha256_texto_normalizado "
                      "FROM observacoes o LEFT JOIN documentos d ON o.documento_id = d.documento_id "
                      "WHERE o.observacao_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, entity_id.c_str(), -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        proof.entity_type = "observacao";
        proof.documento_id = col_text(stmt, 1);
        proof.pagina = col_int(stmt, 2);
        proof.secao = col_text(stmt, 3);
        proof.evidencia_textual = col_text(stmt, 4);
        proof.arquivo = col_text(stmt, 5);
        proof.numero_informativo = col_int(stmt, 6);
        proof.data_publicacao = col_text(stmt, 7);
        proof.sha256 = col_text(stmt, 8);
        proof.sha256_texto_normalizado = col_text(stmt, 9);
        proof.in_vault = check_in_vault(proof.sha256);
        proof.page_text = get_page_text_for_sha(db, proof.sha256, proof.pagina);
        sqlite3_finalize(stmt);
        return proof;
      }
      sqlite3_finalize(stmt);
    }
  }

  // 3. Check ocorrencias_municipais
  if (entity_id.starts_with("OM-")) {
    const char* sql = "SELECT om.ocorrencia_id, om.documento, om.pagina, om.secao, om.evidencia_textual, "
                      "d.documento_id, d.arquivo, d.numero_informativo, d.data_publicacao, d.sha256, d.sha256_texto_normalizado "
                      "FROM ocorrencias_municipais om "
                      "LEFT JOIN documentos d ON (om.documento = d.arquivo OR om.numero_informativo = d.numero_informativo) "
                      "WHERE om.ocorrencia_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, entity_id.c_str(), -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        proof.entity_type = "ocorrencia_municipal";
        proof.pagina = col_int(stmt, 2);
        proof.secao = col_text(stmt, 3);
        proof.evidencia_textual = col_text(stmt, 4);
        proof.documento_id = col_text(stmt, 5);
        proof.arquivo = col_text(stmt, 6);
        proof.numero_informativo = col_int(stmt, 7);
        proof.data_publicacao = col_text(stmt, 8);
        proof.sha256 = col_text(stmt, 9);
        proof.sha256_texto_normalizado = col_text(stmt, 10);
        proof.in_vault = check_in_vault(proof.sha256);
        proof.page_text = get_page_text_for_sha(db, proof.sha256, proof.pagina);
        sqlite3_finalize(stmt);
        return proof;
      }
      sqlite3_finalize(stmt);
    }
  }

  // 4. Check legacy evidence table by ID
  try {
    int64_t id_num = std::stoll(entity_id);
    const char* sql = "SELECT ev.id, ev.page, ev.excerpt, sd.id, sd.title, sd.publication_date, sd.sha256 "
                      "FROM evidence ev JOIN source_document sd ON ev.source_document_id = sd.id "
                      "WHERE ev.id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_int64(stmt, 1, id_num);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        proof.entity_type = "evidencia";
        proof.pagina = col_int(stmt, 1);
        proof.evidencia_textual = col_text(stmt, 2);
        proof.documento_id = std::to_string(sqlite3_column_int64(stmt, 3));
        proof.arquivo = col_text(stmt, 4);
        proof.data_publicacao = col_text(stmt, 5);
        proof.sha256 = col_text(stmt, 6);
        proof.in_vault = check_in_vault(proof.sha256);
        proof.page_text = get_page_text_for_sha(db, proof.sha256, proof.pagina);
        sqlite3_finalize(stmt);
        return proof;
      }
      sqlite3_finalize(stmt);
    }
  } catch (...) {}

  return std::nullopt;
}

} // namespace evida::corpus
