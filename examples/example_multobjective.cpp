// example_multobjective.cpp - SLO Fabric multi-objective policy demonstration.
//
// Builds a SloFabric, sets a policy, adds an active contract carrying two hard
// objectives (p99 latency and a cost window budget), ingests evidence for both
// at explicit times, evaluates, and prints the overall compliance state, the
// binding objective/dimension, the selected enforcement action, the secondary
// constraints, the ranked intents, and the what-would-change explanation.

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

void print_full(const char* label, const Result<EvaluationResult>& res) {
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

  std::printf("  secondary_constraints:");
  for (const auto& sid : ex.secondary_constraints) std::printf(" %s", sid.to_string().c_str());
  std::printf("\n");

  for (const auto& os : ex.objectives) {
    std::printf("  obj[%s] %s: state=%s value=%s target=%s samples=%lld detail=%s\n",
                os.id.to_string().c_str(), os.name.c_str(),
                std::string(compliance_state_name(os.state)).c_str(),
                optstr(os.current_value).c_str(),
                slofabric::to_string(os.target).c_str(),
                static_cast<long long>(os.sample_count.value()),
                os.detail.c_str());
  }

  for (const auto& ri : ex.ranked_intents) {
    std::printf("  ranked_intent: action=%s selected=%s rationale=%s\n",
                std::string(enforcement_action_name(ri.action)).c_str(),
                ri.selected ? "yes" : "no",
                ri.rationale.c_str());
  }

  for (const auto& rc : ex.what_would_change.required_changes) {
    std::printf("  what_would_change: reason=%s desc=%s\n",
                std::string(change_reason_name(rc.reason)).c_str(),
                rc.description.c_str());
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
  policy.name = "multobjective-policy";
  policy.description = "honour the most-severely-violated hard objective first";
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
  contract.name = "multobjective-contract";
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

  Objective cost;
  cost.id = ObjectiveId(2);
  cost.gen = ObjectiveGen(1);
  cost.name = "cost-window-budget";
  cost.dim = Dimension::Cost;
  cost.kind = ObjectiveKind::CostWindowBudget;
  cost.comparison = Comparison::AtMost;
  cost.hard = HardSoft::Hard;
  cost.enforcement = EnforcementClass::Authorized;
  cost.target = CostMicros(1000000);
  cost.window_type = WindowType::Sliding;
  cost.window_duration = seconds(60);
  contract.objectives.push_back(cost);

  if (!fabric.add_contract(contract).ok()) {
    std::printf("add_contract failed\n");
    return 1;
  }

  int eid = 1;

  // p99 latency: 64 samples @ 130ms, above the 100ms target -> violation.
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
    e.value = Duration(130000000LL);
    Status s = fabric.ingest(e, e.time);
    if (!s.ok()) { std::printf("ingest latency failed: %s\n", s.message.c_str()); return 1; }
  }

  // Cost: 300k @ t=1s and 800k @ t=3s -> cumulative 1.1M > 1M -> violation.
  EvidenceRecord c1;
  c1.id = EvidenceId(eid++);
  c1.gen = EvidenceGen(1);
  c1.dimension = Dimension::Cost;
  c1.objective_id = cost.id;
  c1.objective_gen = ObjectiveGen(1);
  c1.source = SourceBootId(1);
  c1.source_gen = SourceBootGen(1);
  c1.epoch = CoordinatorEpoch(1);
  c1.time = seconds(1);
  c1.value = CostMicros(300000);
  fabric.ingest(c1, c1.time);

  EvidenceRecord c2;
  c2.id = EvidenceId(eid++);
  c2.gen = EvidenceGen(1);
  c2.dimension = Dimension::Cost;
  c2.objective_id = cost.id;
  c2.objective_gen = ObjectiveGen(1);
  c2.source = SourceBootId(1);
  c2.source_gen = SourceBootGen(1);
  c2.epoch = CoordinatorEpoch(1);
  c2.time = seconds(3);
  c2.value = CostMicros(800000);
  fabric.ingest(c2, c2.time);

  Result<EvaluationResult> res = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  print_full("multobjective", res);
  return 0;
}
