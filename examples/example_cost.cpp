// example_cost.cpp - SLO Fabric cost-window-budget SLO demonstration.
//
// Builds a SloFabric, sets a policy, adds an active contract with a cost
// window-budget objective (<= 1,000,000 micros over a 60s sliding window),
// ingests cost samples at explicit times, evaluates (compliant then
// violating), and prints the compliance state, binding objective/dimension,
// selected enforcement action, and explanation detail.

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
    std::printf("  obj[%s] %s: state=%s value=%s target=%s samples=%lld detail=%s\n",
                os.id.to_string().c_str(), os.name.c_str(),
                std::string(compliance_state_name(os.state)).c_str(),
                optstr(os.current_value).c_str(),
                slofabric::to_string(os.target).c_str(),
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
  policy.name = "cost-policy";
  policy.description = "stay within the 60s cost budget";
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
  contract.name = "cost-contract";
  contract.provenance = "example";

  Objective cost;
  cost.id = ObjectiveId(1);
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

  // t = 1s: 300,000 micros, within the 1,000,000 budget -> compliant.
  EvidenceRecord e1;
  e1.id = EvidenceId(eid++);
  e1.gen = EvidenceGen(1);
  e1.dimension = Dimension::Cost;
  e1.objective_id = cost.id;
  e1.objective_gen = ObjectiveGen(1);
  e1.source = SourceBootId(1);
  e1.source_gen = SourceBootGen(1);
  e1.epoch = CoordinatorEpoch(1);
  e1.time = seconds(1);
  e1.value = CostMicros(300000);
  fabric.ingest(e1, e1.time);

  Result<EvaluationResult> r1 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  print_result("cost[1] 300k-micros", r1);

  // t = 3s: +800,000 micros -> cumulative 1,100,000 > 1,000,000 -> violation.
  EvidenceRecord e2;
  e2.id = EvidenceId(eid++);
  e2.gen = EvidenceGen(1);
  e2.dimension = Dimension::Cost;
  e2.objective_id = cost.id;
  e2.objective_gen = ObjectiveGen(1);
  e2.source = SourceBootId(1);
  e2.source_gen = SourceBootGen(1);
  e2.epoch = CoordinatorEpoch(1);
  e2.time = seconds(3);
  e2.value = CostMicros(800000);
  fabric.ingest(e2, e2.time);

  Result<EvaluationResult> r2 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(4));
  print_result("cost[2] 1.1M-micros", r2);

  return 0;
}
