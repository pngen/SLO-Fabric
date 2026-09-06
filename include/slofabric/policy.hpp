#pragma once

// SLO Fabric - multi-objective policy.
//
// A policy is first-class, versioned state. It determines how conflicting
// objectives are resolved into a deterministic binding constraint and an
// enforcement intent. It is NOT an unexplained weighted scalar; every factor
// used in ranking is explicit.

#include <cstdint>
#include <string>
#include <string_view>

#include "slofabric/identities.hpp"

namespace slofabric {

enum class PolicyMode : std::uint8_t {
  Lexicographic,        // resolve by (hard, priority, severity, tie-break)
  HardThenSoft,         // hard constraints first, then soft ranking
  Pareto,               // maintain Pareto frontier, deterministic selection
};

constexpr std::string_view policy_mode_name(PolicyMode m) noexcept {
  using M = PolicyMode;
  switch (m) {
    case M::Lexicographic: return "lexicographic";
    case M::HardThenSoft: return "hard_then_soft";
    case M::Pareto: return "pareto";
  }
  return "unknown_policy_mode";
}

// Deterministic tie-break rule used when two objectives are otherwise equal.
enum class TieBreak : std::uint8_t {
  ByPriority,       // lower priority number wins
  ByName,           // lexicographic name wins (fully deterministic)
  ByObjectiveId,    // lower objective id wins
};

constexpr std::string_view tie_break_name(TieBreak t) noexcept {
  using T = TieBreak;
  switch (t) {
    case T::ByPriority: return "by_priority";
    case T::ByName: return "by_name";
    case T::ByObjectiveId: return "by_objective_id";
  }
  return "unknown_tie_break";
}

struct SloPolicy {
  PolicyId id;
  PolicyGen gen;
  PolicyMode mode = PolicyMode::HardThenSoft;
  TieBreak tie_break = TieBreak::ByName;
  std::string name;
  std::string description;
};

}  // namespace slofabric
