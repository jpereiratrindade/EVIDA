// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 EVIDA Contributors
// This file is part of EVIDA - Scientific Evidence Intelligence System
// Licensed under the GNU General Public License v3.0 or later.
#pragma once
#include <string>

struct sqlite3;

namespace evida {

struct Stats {
  int documents{};
  int evidences{};
  int users{};
  int events{};
  int observations{};
  int responses{};
  int prices{};
  int episodes{};
  int municipalities{};
  int municipal_occurrences{};
};

class Store {
 public:
  explicit Store(const std::string& filename);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  sqlite3* db() const noexcept { return db_; }
  Stats stats() const;

 private:
  sqlite3* db_{};
  void migrate();
};

} // namespace evida
