#pragma once
#include <cstdint>
#include <string_view>

namespace slofabric {

// How an evidence value came to exist. Synthetic evidence must never be
// reported as measured.
enum class Provenance : std::uint8_t {
  Measured = 0,
  Derived = 1,
  Estimated = 2,
  Reported = 3,
  Synthetic = 4,
  Unknown = 5,
};

constexpr std::string_view provenance_name(Provenance p) noexcept {
  using P = Provenance;
  switch (p) {
    case P::Measured: return "measured";
    case P::Derived: return "derived";
    case P::Estimated: return "estimated";
    case P::Reported: return "reported";
    case P::Synthetic: return "synthetic";
    case P::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace slofabric
