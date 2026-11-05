// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace evida::corpus {

struct EventItem {
  std::string evento_id;
  std::string documento_id;
  std::string arquivo_origem;
  int numero_informativo{0};
  std::string data_publicacao;
  std::string regiao_emater;
  std::string municipio;
  std::string atividade;
  std::string tipo_evento;
  std::string classe_evento;
  std::string descricao_evento;
  std::string efeito_produtivo;
  std::string efeito_economico;
  int pagina{0};
  std::string secao;
  std::string evidencia_textual;
  std::string confianca;
};

struct ObservationItem {
  std::string observacao_id;
  std::string documento_id;
  std::string arquivo_origem;
  int numero_informativo{0};
  std::string data_publicacao;
  std::string dimensao;
  std::string categoria;
  std::string atividade;
  std::string cultura_criacao;
  std::string variavel;
  std::string valor;
  std::string unidade;
  std::string regiao_emater;
  std::string municipio;
  int pagina{0};
  std::string secao;
  std::string evidencia_textual;
  std::string confianca;
};

struct MunicipalityItem {
  std::string codigo_ibge;
  std::string municipio;
  std::string uf;
  int occurrences_count{0};
  std::string top_classes;
};

struct MunicipalOccurrenceItem {
  std::string ocorrencia_id;
  std::string codigo_ibge;
  std::string municipio;
  std::string data_publicacao;
  std::string data_ocorrencia;
  std::string regiao_emater;
  std::string classe_ocorrencia;
  std::string subclasse_ocorrencia;
  std::string atividade;
  std::string descricao;
  std::string documento;
  int numero_informativo{0};
  int pagina{0};
  std::string evidencia_textual;
  std::string confianca;
};

struct PriceItem {
  std::string documento_id;
  std::string data_publicacao;
  std::string produto;
  std::string regiao;
  std::string preco_medio;
  std::string preco_minimo;
  std::string preco_maximo;
  std::string unidade;
  int pagina{0};
  std::string evidencia_textual;
};

struct EpisodeItem {
  std::string episodio_id;
  std::string territorio;
  std::string atividade;
  std::string data_inicio;
  std::string data_fim;
  std::string perturbacao;
  std::string impactos_observados;
  std::string respostas_observadas;
  std::string nivel_confianca;
  int numero_documentos{0};
};

struct VaultDocumentItem {
  std::string documento_id;
  std::string arquivo;
  int numero_informativo{0};
  std::string data_publicacao;
  int ano{0};
  int numero_paginas{0};
  int caracteres_extraidos{0};
  std::string sha256;
  std::string sha256_texto_normalizado;
  bool in_vault{false};
  uintmax_t byte_size{0};
  std::string integrity_status; // "verified_intact", "corrupted_tampered", "missing"
};

struct EvidenceLinkInfo {
  std::string link_id;
  std::string evidence_id;
  std::string link_type; // "supports", "revises", "disputes", "neutral"
  std::string document_id;
  int page_number{0};
  std::string verbatim_text;
  std::string notes;
};

struct BlobIntegrity {
  bool exists{false};
  bool matches_sha256{false};
  uintmax_t byte_size{0};
  std::string recalculated_sha256;
  std::string status; // "verified_intact", "corrupted_tampered", "missing"
};

struct AuditProofItem {
  std::string entity_id;
  std::string entity_type; // "evento", "observacao", "ocorrencia_municipal", "preco"
  std::string documento_id;
  std::string arquivo;
  int numero_informativo{0};
  std::string data_publicacao;
  int pagina{0};
  std::string secao;
  std::string evidencia_textual;
  std::string sha256;
  std::string sha256_texto_normalizado;
  
  // Proveniência Unificada v0.3.1
  std::string evidence_span_id;
  std::string text_sha256;
  std::string extraction_run_id;
  std::string extractor_name;
  std::string extractor_version;
  std::string extraction_params;
  std::string executed_at;
  
  // Integridade Física Recalculada Dinamicamente
  BlobIntegrity blob_integrity;
  
  std::string page_text;
  std::vector<EvidenceLinkInfo> links;
};

// API query functions
std::vector<EventItem> list_events(sqlite3* db, const std::string& classe = "",
                                   const std::string& regiao = "",
                                   const std::string& atividade = "",
                                   int limit = 50, int offset = 0);

std::vector<ObservationItem> list_observations(sqlite3* db, const std::string& atividade = "",
                                               const std::string& variavel = "",
                                               const std::string& regiao = "",
                                               int limit = 50, int offset = 0);

std::vector<MunicipalityItem> list_municipalities(sqlite3* db, const std::string& query = "",
                                                   int limit = 100);

std::vector<MunicipalOccurrenceItem> list_municipal_occurrences(sqlite3* db,
                                                               const std::string& codigo_ibge = "",
                                                               const std::string& classe = "",
                                                               int limit = 50);

std::vector<PriceItem> list_prices(sqlite3* db, const std::string& produto = "",
                                   const std::string& regiao = "",
                                   int limit = 50);

std::vector<EpisodeItem> list_episodes(sqlite3* db, int limit = 50);

std::vector<VaultDocumentItem> list_vault_documents(sqlite3* db);

BlobIntegrity verify_blob_integrity(sqlite3* db, const std::string& sha256);

std::optional<AuditProofItem> get_audit_proof(sqlite3* db, const std::string& entity_id);

void backfill_provenance_if_needed(sqlite3* db);

} // namespace evida::corpus
