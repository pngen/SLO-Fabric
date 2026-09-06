// example_recovery.cpp - SLO Fabric recovery-time SLO demonstration.
//
// Builds a SloFabric, sets a policy, adds an active contract with a
// recovery-max-duration objective (RTO <= 5s), drives the full recovery
// lifecycle through record_recovery_event (Observed, Authoritative, Restored,
// Verified) at explicit times, evaluates (compliant in-progress then
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
  policy.name = "recovery-policy";
  policy.description = "recover within the 5 second RTO";
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
  contract.name = "recovery-contract";
  contract.provenance = "example";

  Objective rto;
  rto.id = ObjectiveId(2);
  rto.gen = ObjectiveGen(1);
  rto.name = "recovery-max-duration";
  rto.dim = Dimension::RecoveryTime;
  rto.kind = ObjectiveKind::RecoveryMaxDuration;
  rto.comparison = Comparison::AtMost;
  rto.hard = HardSoft::Hard;
  rto.enforcement = EnforcementClass::Authorized;
  rto.target = seconds(5);
  rto.min_evidence = Count(1);
  contract.objectives.push_back(rto);

  if (!fabric.add_contract(contract).ok()) {
    std::printf("add_contract failed\n");
    return 1;
  }

  // A failure is observed and confirmed authoritative at t = 0s.
  fabric.record_recovery_event(ObjectiveId(2), RecoveryEventPhase::Observed, seconds(0));
  fabric.record_recovery_event(ObjectiveId(2), RecoveryEventPhase::Authoritative, seconds(0));

  // At t = 3s recovery is still in progress and within the 5s target.
  Result<EvaluationResult> r1 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(3));
  print_result("recovery[1] in-progress-3s", r1);

  // Recovery completes (Restored + Verified) at t = 6s, missing the 5s RTO.
  fabric.record_recovery_event(ObjectiveId(2), RecoveryEventPhase::Restored, seconds(6));
  fabric.record_recovery_event(ObjectiveId(2), RecoveryEventPhase::Verified, seconds(6));

  // At t = 7s the measured duration is 6s (> 5s target) -> violation.
  Result<EvaluationResult> r2 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(7));
  print_result("recovery[2] restored-6s", r2);

  return 0;
}
