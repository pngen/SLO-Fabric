#pragma once

// SLO Fabric - structured what-would-change analysis.
//
// Computed only after binding constraints are resolved. Returns the reason
// codes and the concrete changes that would move the system toward compliance.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slofabric {

enum class ChangeReason : std::uint8_t {
  LatencyBelowRecovery,
  CapacityBecomesAvailable,
  WorkloadPriorityChanges,
  BudgetReplenishes,
  DeadlineRelaxes,
  MemoryPressureFalls,
  CostBudgetIncreases,
  WarmReplicaBecomesReady,
  RecoveryCandidateImproves,
  ThroughputRises,
  EvidenceBecomesFresh,
  PolicyGenerationChanges,
  ConfigurationChanges,
};

constexpr std::string_view change_reason_name(ChangeReason r) noexcept {
  using C = ChangeReason;
  switch (r) {
    case C::LatencyBelowRecovery: return "latency_below_recovery";
    case C::CapacityBecomesAvailable: return "capacity_becomes_available";
    case C::WorkloadPriorityChanges: return "workload_priority_changes";
    case C::BudgetReplenishes: return "budget_replenishes";
    case C::DeadlineRelaxes: return "deadline_relaxes";
    case C::MemoryPressureFalls: return "memory_pressure_falls";
    case C::CostBudgetIncreases: return "cost_budget_increases";
    case C::WarmReplicaBecomesReady: return "warm_replica_becomes_ready";
    case C::RecoveryCandidateImproves: return "recovery_candidate_improves";
    case C::ThroughputRises: return "throughput_rises";
    case C::EvidenceBecomesFresh: return "evidence_becomes_fresh";
    case C::PolicyGenerationChanges: return "policy_generation_changes";
    case C::ConfigurationChanges: return "configuration_changes";
  }
  return "unknown_change_reason";
}

struct RequiredChange {
  ChangeReason reason;
  std::string description;
};

struct WhatWouldChange {
  std::vector<ChangeReason> reasons;
  std::vector<RequiredChange> required_changes;

  bool empty() const noexcept { return reasons.empty(); }
};

}  // namespace slofabric
