// example_supersession.cpp - SLO Fabric contract supersession demonstration.
//
// Builds a SloFabric, sets a policy, adds an active contract (gen 1), ingests
// evidence for it, evaluates a binding violation, then supersedes the contract
// with a replacement (gen 2) carrying a looser objective and evaluates again.
// The output shows the contract generation used and that authority switches to
// the superseding contract.

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

  std::printf("%s: contract_gen=%s compliance=%s binding=%s/%s action=%s\n",
              label,
              ex.contract_gen.to_string().c_str(),
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

SloContract make_contract(SloContractGen gen, Duration target) {
  SloContract c;
  c.id = SloContractId(1);
  c.gen = gen;
  c.service = ServiceId(100);
  c.workload = WorkloadId(200);
  c.tenant = TenantId(1);
  c.policy_id = PolicyId(1);
  c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0);
  c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active;
  c.name = "latency-contract";
  c.provenance = "example";

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
  lat.target = target;
  lat.min_evidence = Count(64);
  lat.window_type = WindowType::Sliding;
  lat.window_duration = seconds(120);
  lat.window_count = Count(64);
  c.objectives.push_back(lat);
  return c;
}

}  // namespace

int main() {
  SloFabric fabric;

  SloPolicy policy;
  policy.id = PolicyId(1);
  policy.gen = PolicyGen(1);
  policy.mode = PolicyMode::HardThenSoft;
  policy.tie_break = TieBreak::ByName;
  policy.name = "supersession-policy";
  policy.description = "the newest active contract governs";
  fabric.set_policy(policy);

  // gen 1 contract: strict 100ms p99 target.
  fabric.add_contract(make_contract(SloContractGen(1), milliseconds(100)));

  int eid = 1;
  for (int i = 0; i < 64; ++i) {
    EvidenceRecord e;
    e.id = EvidenceId(eid++);
    e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency;
    e.objective_id = ObjectiveId(1);
    e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1);
    e.source_gen = SourceBootGen(1);
    e.epoch = CoordinatorEpoch(1);
    e.time = seconds(i);
    e.value = Duration(130000000LL);  // 130ms
    Status s = fabric.ingest(e, e.time);
    if (!s.ok()) { std::printf("ingest failed: %s\n", s.message.c_str()); return 1; }
  }

  // Under gen 1 (target 100ms) 130ms is a violation.
  Result<EvaluationResult> r1 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  print_result("supersession[before]", r1);

  // Supersede with gen 2: same objective, loosened to a 200ms target.
  fabric.supersede_contract(make_contract(SloContractGen(2), milliseconds(200)), seconds(71));

  // Under gen 2 (target 200ms) the same 130ms evidence is now compliant.
  Result<EvaluationResult> r2 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(72));
  print_result("supersession[after]", r2);

  return 0;
}
