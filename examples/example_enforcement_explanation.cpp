// example_enforcement_explanation.cpp - SLO Fabric enforcement explanation.
//
// Builds a SloFabric, sets a policy, adds an active contract with a p99 latency
// objective, ingests a violating sample set, evaluates, and prints the full
// inspectable Explanation: overall compliance, binding objective/dimension, the
// generation-bound EnforcementIntent (action, class, reason, authority
// generations, evaluation id/gen, cooldown), the policy rationale, per-objective
// detail (state, value, target, breach/recovery thresholds, freshness, predicted
// risk, detail), the ranked intents, and the what-would-change analysis.

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
  const EnforcementIntent& intent = er.intent;

  std::printf("%s: compliance=%s binding=%s/%s\n",
              label,
              std::string(compliance_state_name(ex.compliance)).c_str(),
              std::string(dimension_name(ex.binding_dimension)).c_str(),
              ex.binding_objective.to_string().c_str());

  std::printf("  intent: action=%s class=%s dimension=%s objective=%s\n",
              std::string(enforcement_action_name(intent.action)).c_str(),
              std::string(enforcement_class_name(intent.cls)).c_str(),
              std::string(dimension_name(intent.dimension)).c_str(),
              intent.objective_id.to_string().c_str());
  std::printf("  intent.generations: contract=%s policy=%s evidence=%s epoch=%s evaluation_id=%s evaluation_gen=%s\n",
              intent.contract_gen.to_string().c_str(),
              intent.policy_gen.to_string().c_str(),
              intent.evidence_gen.to_string().c_str(),
              intent.epoch.to_string().c_str(),
              intent.evaluation_id.to_string().c_str(),
              intent.evaluation_gen.to_string().c_str());
  std::printf("  intent.cooldown=%lldns reason=%s\n",
              static_cast<long long>(intent.cooldown.as_ns()),
              intent.reason.c_str());

  std::printf("  policy_rationale: %s\n", ex.policy_rationale.c_str());

  std::printf("  secondary_constraints:");
  for (const auto& sid : ex.secondary_constraints) std::printf(" %s", sid.to_string().c_str());
  std::printf("\n");

  for (const auto& os : ex.objectives) {
    std::printf("  obj[%s] %s: state=%s value=%s target=%s\n",
                os.id.to_string().c_str(), os.name.c_str(),
                std::string(compliance_state_name(os.state)).c_str(),
                optstr(os.current_value).c_str(),
                slofabric::to_string(os.target).c_str());
    std::printf("      breach=%s recovery=%s comparison=%s hard=%s samples=%lld sufficient=%s freshness=%s risk=%s\n",
                os.breach_threshold ? slofabric::to_string(*os.breach_threshold).c_str() : "n/a",
                os.recovery_threshold ? slofabric::to_string(*os.recovery_threshold).c_str() : "n/a",
                std::string(comparison_name(os.comparison)).c_str(),
                std::string(hardsoft_name(os.hard)).c_str(),
                static_cast<long long>(os.sample_count.value()),
                os.sufficient ? "yes" : "no",
                std::string(freshness_name(os.freshness)).c_str(),
                std::string(predicted_risk_name(os.predicted)).c_str());
    std::printf("      detail: %s\n", os.detail.c_str());
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
  policy.name = "enforcement-policy";
  policy.description = "violations authorize a typed runtime action";
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
  contract.name = "enforcement-contract";
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

  // A clearly violated p99: 64 samples at 130ms (target 100ms).
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
    e.value = Duration(130000000LL);
    Status s = fabric.ingest(e, e.time);
    if (!s.ok()) { std::printf("ingest failed: %s\n", s.message.c_str()); return 1; }
  }

  Result<EvaluationResult> res = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  print_full("enforcement", res);
  return 0;
}
