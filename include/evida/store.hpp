#pragma once
#include <string>

struct sqlite3;

namespace evida {

struct Stats {
  int documents{};
  int evidences{};
  int users{};
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
