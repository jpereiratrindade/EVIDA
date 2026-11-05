// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.

#include "evida/evidence.hpp"
#include <algorithm>

namespace evida {

bool valid_sha256(std::string_view hash) noexcept {
  return hash.size() == 64 &&
      std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
      });
}

bool valid_evidence(const Evidence& item) noexcept {
  return valid_sha256(item.source_sha256) &&
         !item.source_document.empty() && item.page > 0 &&
         !item.excerpt.empty();
}

const char* status_name(EvidenceStatus status) noexcept {
  switch (status) {
    case EvidenceStatus::extracted: return "extracted";
    case EvidenceStatus::reviewed: return "reviewed";
    case EvidenceStatus::disputed: return "disputed";
    case EvidenceStatus::rejected: return "rejected";
  }
  return "unknown";
}

} // namespace evida
