// example_memory.cpp - SLO Fabric memory-pressure SLO demonstration.
//
// Builds a SloFabric, sets a policy, adds an active contract with a
// memory-pressure objective (max sustained pressure ratio 0.90, breach 0.90,
// recovery 0.80, sliding 2s window), ingests pressure samples at explicit
// times, evaluates across a hysteresis cycle, and prints the compliance state,
// binding objective/dimension, selected enforcement action, and explanation.

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
  policy.name = "memory-policy";
  policy.description = "keep sustained memory pressure under 0.90";
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
  contract.name = "memory-contract";
  contract.provenance = "example";

  Objective mem;
  mem.id = ObjectiveId(1);
  mem.gen = ObjectiveGen(1);
  mem.name = "memory-pressure-max";
  mem.dim = Dimension::MemoryPressure;
  mem.kind = ObjectiveKind::MemoryPressureMax;
  mem.comparison = Comparison::AtMost;
  mem.hard = HardSoft::Hard;
  mem.enforcement = EnforcementClass::Authorized;
  mem.target = PressureRatio(0.90);
  mem.breach_threshold = PressureRatio(0.90);
  mem.recovery_threshold = PressureRatio(0.80);
  mem.min_evidence = Count(1);
  mem.window_type = WindowType::Sliding;
  mem.window_duration = seconds(2);
  contract.objectives.push_back(mem);

  if (!fabric.add_contract(contract).ok()) {
    std::printf("add_contract failed\n");
    return 1;
  }

  int eid = 1;

  // t = 1s: pressure 0.95, above the 0.90 breach threshold -> violation.
  EvidenceRecord e1;
  e1.id = EvidenceId(eid++);
  e1.gen = EvidenceGen(1);
  e1.dimension = Dimension::MemoryPressure;
  e1.objective_id = mem.id;
  e1.objective_gen = ObjectiveGen(1);
  e1.source = SourceBootId(1);
  e1.source_gen = SourceBootGen(1);
  e1.epoch = CoordinatorEpoch(1);
  e1.time = seconds(1);
  e1.value = PressureRatio(0.95);
  fabric.ingest(e1, e1.time);

  Result<EvaluationResult> r1 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  print_result("memory[1] pressure-0.95", r1);

  // t = 4s: pressure 0.85. The 0.95 sample (age 3s >= 2s window) has aged out.
  // 0.85 is inside the hysteresis band [0.80, 0.90); the prior violation holds.
  EvidenceRecord e2;
  e2.id = EvidenceId(eid++);
  e2.gen = EvidenceGen(1);
  e2.dimension = Dimension::MemoryPressure;
  e2.objective_id = mem.id;
  e2.objective_gen = ObjectiveGen(1);
  e2.source = SourceBootId(1);
  e2.source_gen = SourceBootGen(1);
  e2.epoch = CoordinatorEpoch(1);
  e2.time = seconds(4);
  e2.value = PressureRatio(0.85);
  fabric.ingest(e2, e2.time);

  Result<EvaluationResult> r2 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(5));
  print_result("memory[2] pressure-0.85", r2);

  // t = 7s: pressure 0.75, below the 0.80 recovery threshold -> compliant.
  EvidenceRecord e3;
  e3.id = EvidenceId(eid++);
  e3.gen = EvidenceGen(1);
  e3.dimension = Dimension::MemoryPressure;
  e3.objective_id = mem.id;
  e3.objective_gen = ObjectiveGen(1);
  e3.source = SourceBootId(1);
  e3.source_gen = SourceBootGen(1);
  e3.epoch = CoordinatorEpoch(1);
  e3.time = seconds(7);
  e3.value = PressureRatio(0.75);
  fabric.ingest(e3, e3.time);

  Result<EvaluationResult> r3 = fabric.evaluate(ServiceId(100), WorkloadId(200), seconds(8));
  print_result("memory[3] pressure-0.75", r3);

  return 0;
}
