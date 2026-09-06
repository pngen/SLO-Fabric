#pragma once

// SLO Fabric - enforcement model.
//
// SLO Fabric determines WHAT obligation is binding and WHAT class of runtime
// response is authorized or required. Adjacent runtimes decide HOW that
// response executes. Enforcement intents are typed, generation-bound, and
// carry a lifecycle. A stale or superseded intent must be rejected before it
// mutates any runtime state.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "slofabric/identities.hpp"
#include "slofabric/objective.hpp"
#include "slofabric/status.hpp"
#include "slofabric/units.hpp"

namespace slofabric {

// Typed runtime response classes.
enum class EnforcementAction : std::uint8_t {
  NoAction,
  Admit,
  Defer,
  Reject,
  Throttle,
  ReserveCapacity,
  IncreaseCapacity,
  ReduceBatchSize,
  IncreaseBatchSize,
  ChangeQueuePriority,
  PreemptLowerPriority,
  PromoteWarmCapacity,
  Replicate,
  Drain,
  Failover,
  Recover,
  ReplanRecovery,
  ReclaimMemory,
  ReduceResidency,
  LimitPower,
  ChangePlacement,
  Escalate,
  ManualInterventionRequired,
};

constexpr std::string_view enforcement_action_name(EnforcementAction a) noexcept {
  using A = EnforcementAction;
  switch (a) {
    case A::NoAction: return "no_action";
    case A::Admit: return "admit";
    case A::Defer: return "defer";
    case A::Reject: return "reject";
    case A::Throttle: return "throttle";
    case A::ReserveCapacity: return "reserve_capacity";
    case A::IncreaseCapacity: return "increase_capacity";
    case A::ReduceBatchSize: return "reduce_batch_size";
    case A::IncreaseBatchSize: return "increase_batch_size";
    case A::ChangeQueuePriority: return "change_queue_priority";
    case A::PreemptLowerPriority: return "preempt_lower_priority";
    case A::PromoteWarmCapacity: return "promote_warm_capacity";
    case A::Replicate: return "replicate";
    case A::Drain: return "drain";
    case A::Failover: return "failover";
    case A::Recover: return "recover";
    case A::ReplanRecovery: return "replan_recovery";
    case A::ReclaimMemory: return "reclaim_memory";
    case A::ReduceResidency: return "reduce_residency";
    case A::LimitPower: return "limit_power";
    case A::ChangePlacement: return "change_placement";
    case A::Escalate: return "escalate";
    case A::ManualInterventionRequired: return "manual_intervention_required";
  }
  return "unknown_action";
}

// Enforcement lifecycle.
enum class EnforcementLifecycle : std::uint8_t {
  Proposed,
  Authorized,
  Dispatched,
  Acknowledged,
  Effective,
  Failed,
  Cancelled,
  Superseded,
  Expired,
  OutcomeUnknown,
};

constexpr std::string_view enforcement_lifecycle_name(EnforcementLifecycle l) noexcept {
  using L = EnforcementLifecycle;
  switch (l) {
    case L::Proposed: return "proposed";
    case L::Authorized: return "authorized";
    case L::Dispatched: return "dispatched";
    case L::Acknowledged: return "acknowledged";
    case L::Effective: return "effective";
    case L::Failed: return "failed";
    case L::Cancelled: return "cancelled";
    case L::Superseded: return "superseded";
    case L::Expired: return "expired";
    case L::OutcomeUnknown: return "outcome_unknown";
  }
  return "unknown_lifecycle";
}

// Legal lifecycle transition table. A cancelled or superseded enforcement must
// not become authoritative success unless it crossed an explicitly defined
// irreversible commit boundary (Effective).
inline bool enforcement_transition_legal(EnforcementLifecycle from, EnforcementLifecycle to) noexcept {
  using L = EnforcementLifecycle;
  switch (from) {
    case L::Proposed:
      return to == L::Authorized || to == L::Cancelled || to == L::Superseded || to == L::Expired;
    case L::Authorized:
      return to == L::Dispatched || to == L::Cancelled || to == L::Superseded || to == L::Expired;
    case L::Dispatched:
      return to == L::Acknowledged || to == L::Failed || to == L::Cancelled || to == L::Superseded ||
             to == L::Expired || to == L::OutcomeUnknown;
    case L::Acknowledged:
      return to == L::Effective || to == L::Failed || to == L::Cancelled || to == L::Superseded ||
             to == L::Expired || to == L::OutcomeUnknown;
    case L::Effective:
      return to == L::Expired || to == L::Superseded;
    case L::Failed:
      return to == L::Expired;
    case L::Cancelled:
      return to == L::Expired;
    case L::Superseded:
      return to == L::Expired;
    case L::Expired:
      return false;
    case L::OutcomeUnknown:
      return to == L::Expired || to == L::Failed;
  }
  return false;
}

// A typed enforcement intent. Generation-bound so a stale intent rejects.
struct EnforcementIntent {
  EnforcementAction action = EnforcementAction::NoAction;
  EnforcementClass cls = EnforcementClass::None;
  Dimension dimension{};
  ObjectiveId objective_id;
  ObjectiveGen objective_gen;
  ServiceId service;
  WorkloadId workload;

  // Authority / generation fence.
  CoordinatorEpoch epoch;
  SloContractGen contract_gen;
  PolicyGen policy_gen;
  EvidenceGen evidence_gen;
  EvaluationId evaluation_id;
  EvaluationGen evaluation_gen;
  WorkloadGen workload_gen;

  Duration cooldown{0};
  std::string reason;
};

// An enforcement receipt tracks the lifecycle of an authorized action and the
// authority generations it was bound to when created.
struct EnforcementReceipt {
  EnforcementId id;
  EnforcementGen gen;
  EnforcementAction action = EnforcementAction::NoAction;
  EnforcementLifecycle lifecycle = EnforcementLifecycle::Proposed;
  CoordinatorEpoch epoch;
  SloContractGen contract_gen;
  PolicyGen policy_gen;
  EvaluationId evaluation_id;
  EvaluationGen evaluation_gen;
  WorkloadGen workload_gen;
  ServiceId service;
  WorkloadId workload;
  ObjectiveId objective_id;
  ObjectiveGen objective_gen;
  std::string reason;
};

}  // namespace slofabric
