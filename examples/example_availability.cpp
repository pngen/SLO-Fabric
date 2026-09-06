// example_availability.cpp - SLO Fabric request-availability SLO demonstration.
//
// Builds a SloFabric, sets a policy, adds an active contract with a
// request-availability objective (target 99.90%), ingests availability samples
// with explicit times and sample counts, evaluates twice (compliant then
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
  policy.name = "availability-policy";
  policy.description = "serve at least 99.90% of eligible requests";
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
  contract.name = "availability-contract";
  contract.provenance = "example";

  Objective avail;
  avail.id = ObjectiveId(1);
  avail.gen = ObjectiveGen(1);
  avail.name = "request-availability";
  avail.dim = Dimension::Availability;
  avail.kind = ObjectiveKind::AvailabilityRequest;
  avail.comparison = Comparison::AtLeast;
  avail.hard = HardSoft::Hard;
  avail.enforcement = EnforcementClass::Authorized;
  avail.target = BasisPoints(9990);
  avail.availability_mode = AvailabilityMode::Requests;
  avail.min_evidence = Count(1);
  contract.objectives.push_back(avail);

  if (!fabric.add_contract(contract).ok()) {
    std::printf("add_contract failed\n");
    return 1;
  }

  int eid = 1;

  // Window 1: 100% available -> compliant.
  EvidenceRecord e1;
  e1.id = EvidenceId(eid++);
  e1.gen = EvidenceGen(1);
  e1.dimension = Dimension::Availability;
  e1.objective_id = avail.id;
  e1.objective_gen = ObjectiveGen(1);
  e1.source = SourceBootId(1);
  e1.source_gen = SourceBootGen(1);
  e1.epoch = CoordinatorEpoch(1);
  e1.time = seconds(1);
  e1.value = BasisPoints(10000);
  e1.sample_count = Count(1000);
  fabric.ingest(e1, e1.time);

  Result<EvaluationResult> r1 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  print_result("availability[1] all-100%", r1);

  // Window 2: add a 99.0% window; cumulative drops below the 99.90% target.
  EvidenceRecord e2;
  e2.id = EvidenceId(eid++);
  e2.gen = EvidenceGen(1);
  e2.dimension = Dimension::Availability;
  e2.objective_id = avail.id;
  e2.objective_gen = ObjectiveGen(1);
  e2.source = SourceBootId(1);
  e2.source_gen = SourceBootGen(1);
  e2.epoch = CoordinatorEpoch(1);
  e2.time = seconds(2);
  e2.value = BasisPoints(9900);
  e2.sample_count = Count(1000);
  fabric.ingest(e2, e2.time);

  Result<EvaluationResult> r2 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(3));
  print_result("availability[2] single-99%", r2);

  return 0;
}
