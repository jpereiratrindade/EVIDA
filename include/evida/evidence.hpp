#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace evida {

// Os estados são epistêmicos, não graus arbitrários de certeza.
enum class EvidenceStatus { extracted, reviewed, disputed, rejected };

struct Evidence {
  std::string source_sha256;
  std::string source_document;
  std::int32_t page{};
  std::string excerpt;
  EvidenceStatus status{EvidenceStatus::extracted};
};

bool valid_sha256(std::string_view hash) noexcept;
bool valid_evidence(const Evidence& evidence) noexcept;
const char* status_name(EvidenceStatus status) noexcept;

} // namespace evida
