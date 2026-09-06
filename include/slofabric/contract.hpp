#pragma once

// SLO Fabric - SLO contract model.
//
// A contract is a generation-bound, lifecycle-managed bundle of objectives
// plus the policy that governs them, scoped to a service/workload/tenant. An
// expired or superseded contract cannot authorize new enforcement.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "slofabric/identities.hpp"
#include "slofabric/objective.hpp"
#include "slofabric/policy.hpp"
#include "slofabric/units.hpp"

namespace slofabric {

enum class ContractLifecycle : std::uint8_t {
  Draft,
  Active,
  Superseded,
  Suspended,
  Expired,
  Retired,
};

constexpr std::string_view contract_lifecycle_name(ContractLifecycle l) noexcept {
  using C = ContractLifecycle;
  switch (l) {
    case C::Draft: return "draft";
    case C::Active: return "active";
    case C::Superseded: return "superseded";
    case C::Suspended: return "suspended";
    case C::Expired: return "expired";
    case C::Retired: return "retired";
  }
  return "unknown_lifecycle";
}

struct SloContract {
  SloContractId id;
  SloContractGen gen;
  ServiceId service;
  WorkloadId workload;
  TenantId tenant;
  std::vector<Objective> objectives;
  PolicyId policy_id;
  PolicyGen policy_gen;
  Duration effective_from{0};
  std::optional<Duration> expiration;
  CoordinatorEpoch epoch;
  ContractLifecycle lifecycle = ContractLifecycle::Draft;
  std::string name;
  std::string provenance;

  [[nodiscard]] bool is_active() const noexcept { return lifecycle == ContractLifecycle::Active; }
  // A contract can currently authorize enforcement only when active and within
  // its time window (checked against a supplied clock value).
  [[nodiscard]] bool can_authorize(Duration now) const noexcept {
    if (!is_active()) return false;
    if (now < effective_from) return false;
    if (expiration.has_value() && now >= *expiration) return false;
    return true;
  }
};

}  // namespace slofabric
