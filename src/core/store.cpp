// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.

#include "evida/store.hpp"
#include "evida/corpus.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace evida {
namespace {

void execute(sqlite3* db, const std::string& sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::string message = error ? error : "Erro SQL";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

int get_user_version(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) return 0;
  int v = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return v;
}

int count_rows(sqlite3* db, const char* table) {
  const std::string sql = std::string("SELECT count(*) FROM ") + table;
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
    return 0;
  int value = 0;
  if (sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  return value;
}

void apply_file(sqlite3* db, const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) throw std::runtime_error("Migração SQL ausente: " + path.string());
  const std::string sql((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  execute(db, sql);
}

} // namespace

Store::Store(const std::string& filename) {
  if (filename != ":memory:") {
    const auto parent = std::filesystem::path(filename).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
  }
  if (sqlite3_open(filename.c_str(), &db_) != SQLITE_OK) {
    const std::string why = db_ ? sqlite3_errmsg(db_) : "sqlite3_open";
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("Não foi possível abrir o banco: " + why);
  }
  sqlite3_busy_timeout(db_, 30000);
  try { migrate(); } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}

Store::~Store() { if (db_) sqlite3_close(db_); }

void Store::migrate() {
  execute(db_, "PRAGMA foreign_keys=ON;");
  auto mig_dir = std::filesystem::path(EVIDA_MIGRATIONS_DIR);
  const auto installed = std::filesystem::read_symlink("/proc/self/exe").parent_path().parent_path() / "storage/migrations";
  if (std::filesystem::exists(installed)) mig_dir = installed;
  int v = get_user_version(db_);

  if (v < 1) {
    apply_file(db_, mig_dir / "001_initial.sql");
  }
  if (v < 2) {
    apply_file(db_, mig_dir / "002_auth_and_ingestion.sql");
  }
  if (v < 3) {
    apply_file(db_, mig_dir / "003_scientific_corpus.sql");
  }
  if (v < 4) {
    apply_file(db_, mig_dir / "004_document_custody_provenance.sql");
  }
  if (v < 5) apply_file(db_, mig_dir / "005_collection_runs.sql");
  if (v < 6) apply_file(db_, mig_dir / "006_page_checksum_history.sql");
  evida::corpus::backfill_provenance_if_needed(db_);
}

Stats Store::stats() const {
  Stats s;
  s.documents = count_rows(db_, "documentos");
  if (s.documents == 0) {
    s.documents = count_rows(db_, "source_document");
  }
  s.evidences = count_rows(db_, "evidence");
  s.users = count_rows(db_, "users");
  s.events = count_rows(db_, "eventos WHERE NOT EXISTS (SELECT 1 FROM record_status WHERE record_type='evento' AND record_id=evento_id AND status='superseded')");
  s.observations = count_rows(db_, "observacoes WHERE NOT EXISTS (SELECT 1 FROM record_status WHERE record_type='observacao' AND record_id=observacao_id AND status='superseded')");
  s.responses = count_rows(db_, "respostas");
  s.prices = count_rows(db_, "precos");
  s.episodes = count_rows(db_, "episodios");
  s.municipalities = count_rows(db_, "municipios_ibge_rs");
  s.municipal_occurrences = count_rows(db_, "ocorrencias_municipais");
  return s;
}

} // namespace evida
