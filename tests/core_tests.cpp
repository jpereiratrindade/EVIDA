// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.

#include "evida/evidence.hpp"
#include "evida/auth.hpp"
#include "evida/corpus.hpp"
#include "evida/ingest.hpp"
#include "evida/store.hpp"
#include <sqlite3.h>
#include <cassert>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
  // 1. Validação de evidência e integridade epistêmica
  const std::string hash(64, 'a');
  evida::Evidence item{hash, "informativo-1901.pdf", 5, "Trecho literal", evida::EvidenceStatus::extracted};
  if (!evida::valid_evidence(item)) return 1;
  item.page = 0;
  if (evida::valid_evidence(item)) return 2;
  item.page = 5;
  item.source_sha256 = "hash-falso";
  if (evida::valid_evidence(item)) return 3;
  if (std::string(evida::status_name(evida::EvidenceStatus::disputed)) != "disputed") return 4;

  try {
    // 2. Banco e Migrações v0.1 + v0.2 + v0.3 + v0.4
    evida::Store store(":memory:");
    auto stats = store.stats();
    if (stats.users < 2) return 5; // Seeded users: admin and pesquisador

    // 3. Teste de Autenticação Padrão MORFO (OTP SMTP / Dev Mode)
    evida::auth::AuthSmtpConfig auth_cfg;
    auth_cfg.dev_mode = true;
    auth_cfg.simulated = true;
    evida::auth::AuthSmtpService auth(auth_cfg);

    auto req_res = auth.request_code(store.db(), "pesquisador@labeco.local", "127.0.0.1");
    if (!req_res.ok || req_res.dev_code.size() != 6) return 6;

    auto ver_res = auth.verify_code(store.db(), "pesquisador@labeco.local", req_res.dev_code, "127.0.0.1");
    if (!ver_res.ok || ver_res.session_token.empty()) return 7;

    auto sess_user = auth.validate_session(store.db(), ver_res.session_token);
    if (!sess_user || sess_user->email != "pesquisador@labeco.local") return 8;

    if (!auth.revoke_session(store.db(), ver_res.session_token)) return 9;
    if (auth.validate_session(store.db(), ver_res.session_token).has_value()) return 10;

    // 4. Teste de Conferência Dinâmica de Bytes (Custódia & Adulteração)
    fs::path test_storage = "storage/documents";
    fs::create_directories(test_storage);

    std::string test_content = "CONTEUDO ORIGINAL DO DOCUMENTO TESTE DE AUDITORIA.";
    std::string test_sha = evida::ingest::compute_bytes_sha256(test_content);
    fs::path test_file = test_storage / (test_sha + ".pdf");

    {
      std::ofstream out(test_file, std::ios::binary);
      out << test_content;
    }

    // Registra blob prévio no banco para conferir persistência do status e verified_at
    sqlite3_exec(store.db(), ("INSERT OR REPLACE INTO document_blob (sha256, byte_size, storage_path, status) VALUES ('" + test_sha + "', 0, 'storage/documents/" + test_sha + ".pdf', 'missing');").c_str(), nullptr, nullptr, nullptr);

    // 4.1 Verificação de bytes íntegros
    auto int_ok = evida::corpus::verify_blob_integrity(store.db(), test_sha);
    if (!int_ok.exists || !int_ok.matches_sha256 || int_ok.status != "verified_intact") {
      std::cerr << "Falha na verificação de integridade positiva: " << int_ok.status << "\n";
      return 11;
    }

    // Valida persistência real no SQLite (status='verified' e verified_at NOT NULL)
    sqlite3_stmt* check_db = nullptr;
    if (sqlite3_prepare_v2(store.db(), "SELECT status, verified_at, byte_size FROM document_blob WHERE sha256 = ?;", -1, &check_db, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(check_db, 1, test_sha.c_str(), -1, SQLITE_STATIC);
      if (sqlite3_step(check_db) != SQLITE_ROW) {
        std::cerr << "Blob não encontrado no banco após verificação!\n";
        return 110;
      }
      std::string db_st = (const char*)sqlite3_column_text(check_db, 0);
      const char* v_at = (const char*)sqlite3_column_text(check_db, 1);
      int64_t b_sz = sqlite3_column_int64(check_db, 2);
      if (db_st != "verified" || v_at == nullptr || b_sz <= 0) {
        std::cerr << "Persistência no banco inválida: status=" << db_st << ", size=" << b_sz << "\n";
        return 111;
      }
      sqlite3_finalize(check_db);
    }

    // 4.2 Verificação de arquivo adulterado (bit-flip / corrupção)
    {
      std::ofstream out(test_file, std::ios::binary | std::ios::trunc);
      out << test_content << " BYTE ADULTERADO FRAUDULENTO.";
    }
    auto int_tampered = evida::corpus::verify_blob_integrity(store.db(), test_sha);
    if (!int_tampered.exists || int_tampered.matches_sha256 || int_tampered.status != "corrupted_tampered") {
      std::cerr << "Falha na detecção de arquivo adulterado!\n";
      return 12;
    }

    // Valida persistência de corrupção no banco
    if (sqlite3_prepare_v2(store.db(), "SELECT status FROM document_blob WHERE sha256 = ?;", -1, &check_db, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(check_db, 1, test_sha.c_str(), -1, SQLITE_STATIC);
      if (sqlite3_step(check_db) == SQLITE_ROW) {
        std::string db_st = (const char*)sqlite3_column_text(check_db, 0);
        if (db_st != "corrupted") {
          std::cerr << "Persistência de status corrompido no banco falhou: status=" << db_st << "\n";
          return 121;
        }
      }
      sqlite3_finalize(check_db);
    }

    // 4.3 Verificação de arquivo ausente
    std::string fake_sha(64, 'f');
    auto int_missing = evida::corpus::verify_blob_integrity(store.db(), fake_sha);
    if (int_missing.exists || int_missing.status != "missing") {
      std::cerr << "Falha na detecção de arquivo ausente!\n";
      return 13;
    }

    // Limpa arquivo de teste
    fs::remove(test_file);

    // 5. Teste da Camada Unificada de Proveniência (Múltiplas Evidências para um Registro)
    // 5.1 Registra blob e acquisition
    std::string doc_id = "DOC-TEST-001";
    std::string valid_sha(64, 'e');
    char* err = nullptr;
    std::string sql_setup =
        "INSERT INTO document_blob (sha256, byte_size, storage_path, status) VALUES ('" + valid_sha + "', 1024, 'storage/documents/" + valid_sha + ".pdf', 'verified');"
        "INSERT INTO document_acquisition (document_id, sha256, source_title, publication_date, issue_number) VALUES ('" + doc_id + "', '" + valid_sha + "', 'Informativo Teste nº 999', '2026-11-05', 999);"
        "INSERT INTO extraction_run (run_id, document_id, extractor_name, extractor_version, parameters_json) VALUES ('run-001', '" + doc_id + "', 'evida-extractor', '0.3.1', '{\"mode\":\"strict\"}');"
        // Evidência 1: Suporte inicial
        "INSERT INTO evidence_span (evidence_id, extraction_run_id, document_id, page_number, verbatim_text, text_sha256) VALUES ('span-001', 'run-001', '" + doc_id + "', 12, 'Precipitação acumulada foi de 42 mm.', 'sha-span-1');"
        // Evidência 2: Revisão posterior em outra página
        "INSERT INTO evidence_span (evidence_id, extraction_run_id, document_id, page_number, verbatim_text, text_sha256) VALUES ('span-002', 'run-001', '" + doc_id + "', 14, 'Correção técnica: precipitação em Ibirubá atingiu 45 mm.', 'sha-span-2');"
        // Vínculo M:N entre registro analítico OBS-TEST e ambas as evidências
        "INSERT INTO record_evidence_link (link_id, record_type, record_id, evidence_id, link_type, notes) VALUES ('link-1', 'observacao', 'OBS-TEST-001', 'span-001', 'supports', 'Dado inicial de campo');"
        "INSERT INTO record_evidence_link (link_id, record_type, record_id, evidence_id, link_type, notes) VALUES ('link-2', 'observacao', 'OBS-TEST-001', 'span-002', 'revises', 'Retificação de medição posterior');";

    if (sqlite3_exec(store.db(), sql_setup.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string m = err ? err : "erro sql";
      sqlite3_free(err);
      std::cerr << "Falha no setup de links: " << m << "\n";
      return 14;
    }

    // 5.2 Consulta via get_audit_proof e valida múltiplos links
    auto proof = evida::corpus::get_audit_proof(store.db(), "OBS-TEST-001");
    if (!proof.has_value()) {
      std::cerr << "Audit proof não localizou registro OBS-TEST-001!\n";
      return 15;
    }
    if (proof->links.size() != 2) {
      std::cerr << "Esperado 2 vínculos de evidência, obtido: " << proof->links.size() << "\n";
      return 16;
    }
    if (proof->links[0].link_type != "supports" || proof->links[1].link_type != "revises") {
      std::cerr << "Tipos de vínculo incorretos!\n";
      return 17;
    }
    if (proof->extractor_name != "evida-extractor" || proof->extractor_version != "0.3.1") {
      std::cerr << "Execução de extração não recuperada corretamente!\n";
      return 18;
    }

    // 6. Teste de Backfill de Proveniência Histórica (Eventos, Observações, Ocorrências)
    // Instancia uma store isolada simulando banco pré-existente com dados legados
    evida::Store backfill_store(":memory:");
    std::string sql_legacy =
        "INSERT OR IGNORE INTO documentos (documento_id, sha256, numero_informativo, ano, data_publicacao, arquivo, numero_paginas, caracteres_extraidos, sha256_texto_normalizado) "
        "VALUES ('DOC-LEGACY-01', '" + valid_sha + "', 9999, 2026, '2026-11-05', 'legacy.pdf', 10, 500, 'normsha');"
        "INSERT OR IGNORE INTO eventos (evento_id, documento_id, pagina, secao, evidencia_textual, confianca) "
        "VALUES ('EVT-LEG-01', 'DOC-LEGACY-01', 2, 'Clima', 'Chuva torrencial de 80mm em Passo Fundo.', 'ALTA');"
        "INSERT OR IGNORE INTO observacoes (observacao_id, documento_id, pagina, secao, evidencia_textual, confianca) "
        "VALUES ('OBS-LEG-01', 'DOC-LEGACY-01', 3, 'Precipitação', 'Precipitação pluviométrica 80 mm.', 'ALTA');"
        "INSERT OR IGNORE INTO ocorrencias_municipais (ocorrencia_id, numero_informativo, pagina, secao, evidencia_textual, confianca_extracao) "
        "VALUES ('OC-LEG-01', 9999, 4, 'Relato', 'Alagamentos no centro.', 'ALTA');";

    if (sqlite3_exec(backfill_store.db(), sql_legacy.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string m = err ? err : "erro sql legacy";
      sqlite3_free(err);
      std::cerr << "Falha ao preparar dados legados: " << m << "\n";
      return 19;
    }

    // Executa backfill
    evida::corpus::backfill_provenance_if_needed(backfill_store.db());

    // Valida que spans e links foram criados e text_sha256 foi gerado com 64 caracteres
    sqlite3_stmt* bf_stmt = nullptr;
    if (sqlite3_prepare_v2(backfill_store.db(), "SELECT es.evidence_id, es.text_sha256, rel.link_id FROM evidence_span es JOIN record_evidence_link rel ON es.evidence_id = rel.evidence_id WHERE rel.record_id = 'EVT-LEG-01';", -1, &bf_stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(bf_stmt) != SQLITE_ROW) {
        std::cerr << "Falha no backfill de eventos: span ou link não encontrados!\n";
        return 20;
      }
      std::string t_sha = (const char*)sqlite3_column_text(bf_stmt, 1);
      if (t_sha.size() != 64) {
        std::cerr << "Falha no cálculo criptográfico de text_sha256: " << t_sha << "\n";
        return 21;
      }
      sqlite3_finalize(bf_stmt);
    } else {
      std::cerr << "Erro ao preparar consulta de backfill: " << sqlite3_errmsg(backfill_store.db()) << "\n";
      return 22;
    }

    // Valida ocorrência municipal no backfill
    if (sqlite3_prepare_v2(backfill_store.db(), "SELECT es.evidence_id, es.text_sha256 FROM evidence_span es JOIN record_evidence_link rel ON es.evidence_id = rel.evidence_id WHERE rel.record_id = 'OC-LEG-01';", -1, &bf_stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(bf_stmt) != SQLITE_ROW) {
        std::cerr << "Falha no backfill de ocorrencia_municipal: span ou link não encontrados!\n";
        return 23;
      }
      sqlite3_finalize(bf_stmt);
    } else {
      std::cerr << "Erro ao consultar ocorrencia_municipal backfill\n";
      return 24;
    }

  } catch (const std::exception& e) {
    std::cerr << "Exceção em teste: " << e.what() << '\n';
    return 20;
  }

  std::cout << "PROBANTE: Evidência epistêmica, Banco v0.4, Autenticação MORFO, Verificação Dinâmica de Bytes e Vínculos M:N APROVADOS.\n";
  return 0;
}
