#pragma once
#include <cstdint>
#include <string_view>

namespace slofabric {

// Deterministic compliance outcomes. "at risk" (predicted risk) and
// "violating" (current violation) are distinct and must not be collapsed.
enum class ComplianceState : std::uint8_t {
  Compliant = 0,
  NearLimit = 1,
  AtRisk = 2,             // predicted risk within the evaluation horizon
  Violating = 3,          // current violation
  Recovering = 4,         // violation detected; recovery in progress
  InsufficientEvidence = 5,
  RevalidationRequired = 6,
  Suspended = 7,
};

constexpr std::string_view compliance_state_name(ComplianceState s) noexcept {
  using C = ComplianceState;
  switch (s) {
    case C::Compliant: return "compliant";
    case C::NearLimit: return "near_limit";
    case C::AtRisk: return "at_risk";
    case C::Violating: return "violating";
    case C::Recovering: return "recovering";
    case C::InsufficientEvidence: return "insufficient_evidence";
    case C::RevalidationRequired: return "revalidation_required";
    case C::Suspended: return "suspended";
  }
  return "unknown";
}

}  // namespace slofabric
