#pragma once
#include <cstdint>
#include <string_view>

namespace slofabric {

// Explicit freshness of evidence (or of an evaluation result derived from it).
enum class Freshness : std::uint8_t {
  Current = 0,
  Stale = 1,
  Expired = 2,
  RevalidationRequired = 3,
  Unknown = 4,
};

constexpr std::string_view freshness_name(Freshness f) noexcept {
  using F = Freshness;
  switch (f) {
    case F::Current: return "current";
    case F::Stale: return "stale";
    case F::Expired: return "expired";
    case F::RevalidationRequired: return "revalidation_required";
    case F::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace slofabric
