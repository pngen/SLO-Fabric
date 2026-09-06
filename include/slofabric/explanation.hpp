#pragma once

// SLO Fabric - typed, inspectable evaluation results.
//
// Explanations are structured data, never constructed from log strings. Every
// evaluation exposes the active contract, objective states, current values,
// targets, evidence freshness, sample sufficiency, budget state, binding
// constraint, secondary constraints, predicted risk, the selected enforcement
// intent, rejected alternatives, policy rationale, and what-would-change.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "slofabric/budget.hpp"
#include "slofabric/compliance.hpp"
#include "slofabric/enforcement.hpp"
#include "slofabric/evidence.hpp"
#include "slofabric/freshness.hpp"
#include "slofabric/identities.hpp"
#include "slofabric/objective.hpp"
#include "slofabric/objective_value.hpp"
#include "slofabric/what_would_change.hpp"

namespace slofabric {

// Predicted (future) risk, distinct from current violation.
enum class PredictedRisk : std::uint8_t {
  None,
  Low,
  Elevated,
  High,
};

inline std::string_view predicted_risk_name(PredictedRisk p) noexcept {
  using P = PredictedRisk;
  switch (p) {
    case P::None: return "none";
    case P::Low: return "low";
    case P::Elevated: return "elevated";
    case P::High: return "high";
  }
  return "unknown";
}

// Per-objective evaluation detail carried into the explanation.
struct ObjectiveState {
  ObjectiveId id;
  ObjectiveGen gen;
  std::string name;
  Dimension dim = Dimension::Latency;
  ObjectiveKind kind = ObjectiveKind::LatencyMean;
  ComplianceState state = ComplianceState::InsufficientEvidence;
  std::optional<ObjectiveValue> current_value;
  ObjectiveValue target;
  std::optional<ObjectiveValue> breach_threshold;
  std::optional<ObjectiveValue> recovery_threshold;
  Comparison comparison = Comparison::AtMost;
  HardSoft hard = HardSoft::Hard;
  Freshness freshness = Freshness::Unknown;
  Provenance provenance = Provenance::Unknown;
  Count sample_count{0};
  bool sufficient = false;
  std::optional<double> confidence;
  std::optional<BudgetState> budget_state;
  std::optional<BurnRate> burn;
  PredictedRisk predicted = PredictedRisk::None;
  std::string detail;
};

struct RankedIntent {
  EnforcementAction action;
  bool selected = false;
  std::string rationale;
};

struct Explanation {
  SloContractId contract_id;
  SloContractGen contract_gen;
  PolicyId policy_id;
  PolicyGen policy_gen;
  ServiceId service;
  WorkloadId workload;
  CoordinatorEpoch epoch;
  WorkloadGen workload_gen;
  EvidenceGen evidence_gen;
  EvaluationId evaluation_id;
  EvaluationGen evaluation_gen;

  std::vector<ObjectiveState> objectives;

  ComplianceState compliance = ComplianceState::InsufficientEvidence;
  Dimension binding_dimension = Dimension::Latency;
  ObjectiveId binding_objective;
  std::vector<ObjectiveId> secondary_constraints;
  std::vector<PredictedRisk> predicted_risks;

  EnforcementAction selected_action = EnforcementAction::NoAction;
  std::vector<RankedIntent> ranked_intents;
  std::string policy_rationale;

  WhatWouldChange what_would_change;
};

}  // namespace slofabric
