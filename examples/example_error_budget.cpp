// example_error_budget.cpp - SLO Fabric explicit error-budget demonstration.
//
// Builds a SloFabric, sets a policy, adds an active contract with a p99 latency
// objective, attaches an explicit error budget (100 units over a 60s window),
// ingests compliant latency evidence, consumes violation events across the
// budget, evaluates at each stage, and prints the compliance state, binding
// objective/dimension, selected enforcement action, budget state, and burn rate.

#include <cstdio>
#include <optional>
#include <string>

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"

using namespace slofabric;

namespace {

std::string optstr(const std::optional<ObjectiveValue>& v) {
  return v ? slofabric::to_string(*v) : std::string("n/a");
}

void print_result(const char* label, const Result<EvaluationResult>& res) {
  if (!res.ok()) {
    std::printf("%s: evaluate FAILED: %s\n", label, res.status().message.c_str());
    return;
  }
  const EvaluationResult& er = res.value_unchecked();
  const Explanation& ex = er.explanation;

  std::printf("%s: compliance=%s binding=%s/%s action=%s\n",
              label,
              std::string(compliance_state_name(ex.compliance)).c_str(),
              std::string(dimension_name(ex.binding_dimension)).c_str(),
              ex.binding_objective.to_string().c_str(),
              std::string(enforcement_action_name(er.intent.action)).c_str());
  std::printf("  policy_rationale: %s\n", ex.policy_rationale.c_str());
  std::printf("  intent.reason: %s\n", er.intent.reason.c_str());
  for (const auto& os : ex.objectives) {
    std::printf("  obj[%s] %s: state=%s value=%s target=%s budget=%s burn=%s sample_count=%lld detail=%s\n",
                os.id.to_string().c_str(), os.name.c_str(),
                std::string(compliance_state_name(os.state)).c_str(),
                optstr(os.current_value).c_str(),
                slofabric::to_string(os.target).c_str(),
                os.budget_state ? std::string(budget_state_name(*os.budget_state)).c_str() : "n/a",
                os.burn ? std::string(burn_rate_name(*os.burn)).c_str() : "n/a",
                static_cast<long long>(os.sample_count.value()),
                os.detail.c_str());
  }
}

}  // namespace

int main() {
  SloFabric fabric;

  SloPolicy policy;
  policy.id = PolicyId(1);
  policy.gen = PolicyGen(1);
  policy.mode = PolicyMode::HardThenSoft;
  policy.tie_break = TieBreak::ByName;
  policy.name = "budget-policy";
  policy.description = "an exhausted error budget pushes the objective to the limit";
  fabric.set_policy(policy);

  SloContract contract;
  contract.id = SloContractId(1);
  contract.gen = SloContractGen(1);
  contract.service = ServiceId(100);
  contract.workload = WorkloadId(200);
  contract.tenant = TenantId(1);
  contract.policy_id = policy.id;
  contract.policy_gen = policy.gen;
  contract.effective_from = seconds(0);
  contract.epoch = CoordinatorEpoch(1);
  contract.lifecycle = ContractLifecycle::Active;
  contract.name = "budget-contract";
  contract.provenance = "example";

  Objective lat;
  lat.id = ObjectiveId(1);
  lat.gen = ObjectiveGen(1);
  lat.name = "p99-latency";
  lat.dim = Dimension::Latency;
  lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99;
  lat.comparison = Comparison::AtMost;
  lat.hard = HardSoft::Hard;
  lat.enforcement = EnforcementClass::Authorized;
  lat.target = milliseconds(100);
  lat.min_evidence = Count(64);
  lat.window_type = WindowType::Sliding;
  lat.window_duration = seconds(120);
  lat.window_count = Count(64);
  contract.objectives.push_back(lat);

  if (!fabric.add_contract(contract).ok()) {
    std::printf("add_contract failed\n");
    return 1;
  }

  // Attach an explicit error budget: 100 violations allowed per 60s window.
  fabric.set_objective_budget(ObjectiveId(1), Count(100), seconds(60));

  // Compliant latency evidence, t = 0s .. 63s.
  int eid = 1;
  for (int i = 0; i < 64; ++i) {
    EvidenceRecord e;
    e.id = EvidenceId(eid++);
    e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency;
    e.objective_id = lat.id;
    e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1);
    e.source_gen = SourceBootGen(1);
    e.epoch = CoordinatorEpoch(1);
    e.time = seconds(i);
    e.value = Duration(50000000LL);  // 50ms, compliant
    Status s = fabric.ingest(e, e.time);
    if (!s.ok()) { std::printf("ingest failed: %s\n", s.message.c_str()); return 1; }
  }

  // Stage 1: no violations consumed -> budget healthy.
  Result<EvaluationResult> r1 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  print_result("budget[1] healthy", r1);

  // Stage 2: consume 90 of 100 -> 10 remaining = within the 10% near-limit band.
  for (int i = 0; i < 90; ++i) fabric.ingest_violation_event(ObjectiveId(1), seconds(70));
  Result<EvaluationResult> r2 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  print_result("budget[2] near-limit", r2);

  // Stage 3: consume 11 more -> 101 consumed, budget exhausted with overrun.
  for (int i = 0; i < 11; ++i) fabric.ingest_violation_event(ObjectiveId(1), seconds(70));
  Result<EvaluationResult> r3 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  print_result("budget[3] exhausted", r3);

  return 0;
}
