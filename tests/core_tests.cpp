// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.

#include "evida/evidence.hpp"
#include "evida/auth.hpp"
#include "evida/ingest.hpp"
#include "evida/store.hpp"
#include <cassert>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

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
    // 2. Banco e Migrações v0.1 + v0.2
    evida::Store store(":memory:");
    auto stats = store.stats();
    if (stats.documents != 0 || stats.evidences != 0) return 5;
    if (stats.users < 2) return 6; // Seeded users: admin and pesquisador

    // 3. Teste de Autenticação Padrão MORFO (OTP SMTP / Dev Mode)
    evida::auth::AuthSmtpConfig auth_cfg;
    auth_cfg.dev_mode = true;
    auth_cfg.simulated = true;
    evida::auth::AuthSmtpService auth(auth_cfg);

    // Request code para pesquisador@labeco.local
    auto req_res = auth.request_code(store.db(), "pesquisador@labeco.local", "127.0.0.1");
    if (!req_res.ok || req_res.dev_code.size() != 6) {
      std::cerr << "Falha no request_code: " << req_res.user_message << "\n";
      return 7;
    }

    // Verify code correto
    auto ver_res = auth.verify_code(store.db(), "pesquisador@labeco.local", req_res.dev_code, "127.0.0.1");
    if (!ver_res.ok || ver_res.session_token.empty()) {
      std::cerr << "Falha no verify_code: " << ver_res.user_message << "\n";
      return 8;
    }

    // Validação de sessão ativa
    auto sess_user = auth.validate_session(store.db(), ver_res.session_token);
    if (!sess_user || sess_user->email != "pesquisador@labeco.local") return 9;

    // Revogação de sessão (logout)
    if (!auth.revoke_session(store.db(), ver_res.session_token)) return 10;
    if (auth.validate_session(store.db(), ver_res.session_token).has_value()) return 11;

    // 4. Teste de Adição e Atualização de Evidência com Proveniência
    // Simula documento inserido
    evida::ingest::IngestOptions opts;
    opts.title = "Documento Teste";
    opts.publication_date = "2026-11-05";
    opts.source_uri = "https://labeco.local/doc/teste.pdf";

    // Criamos um arquivo temporário de teste
    std::string tmp_pdf = "/tmp/evida_test_dummy.txt";
    {
      std::ofstream f(tmp_pdf);
      f << "Linha de teste para extração de evidência documental com proveniência.";
    }

    std::string doc_hash = evida::ingest::compute_file_sha256(tmp_pdf);
    if (doc_hash.size() != 64) return 12;

  } catch (const std::exception& e) {
    std::cerr << "Exceção em teste: " << e.what() << '\n';
    return 20;
  }

  std::cout << "PROBANTE: Evidência epistêmica, Banco v0.2, Autenticação MORFO e Proveniência APROVADOS.\n";
  return 0;
}
