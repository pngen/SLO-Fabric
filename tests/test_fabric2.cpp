#include "test_framework.hpp"

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"

using namespace slofabric;

static int g_id = 0;

static void add_policy(SloFabric& f, PolicyGen gen = PolicyGen(1)) {
  SloPolicy p;
  p.id = PolicyId(1);
  p.gen = gen;
  p.mode = PolicyMode::HardThenSoft;
  p.tie_break = TieBreak::ByName;
  p.name = "p";
  p.description = "desc";
  f.set_policy(p);
}

static SloContract base_contract(ServiceId svc = ServiceId(100), PolicyGen pgen = PolicyGen(1)) {
  SloContract c;
  c.id = SloContractId(1);
  c.gen = SloContractGen(1);
  c.service = svc;
  c.workload = WorkloadId(200);
  c.tenant = TenantId(1);
  c.policy_id = PolicyId(1);
  c.policy_gen = pgen;
  c.effective_from = seconds(0);
  c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active;
  c.name = "c";
  c.provenance = "test";
  return c;
}

TEST("fabric: availability compliant / near / violating") {
  SloFabric f;
  add_policy(f);
  auto c = base_contract();
  Objective o;
  o.id = ObjectiveId(1); o.gen = ObjectiveGen(1);
  o.name = "avail"; o.dim = Dimension::Availability;
  o.kind = ObjectiveKind::AvailabilityRequest;
  o.comparison = Comparison::AtLeast; o.hard = HardSoft::Hard;
  o.target = BasisPoints(9990);
  o.availability_mode = AvailabilityMode::Requests;
  c.objectives.push_back(o);
  REQUIRE(f.add_contract(c).ok());

  // 100% available -> compliant.
  EvidenceRecord e;
  e.id = EvidenceId(++g_id); e.gen = EvidenceGen(1);
  e.dimension = Dimension::Availability;
  e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(5); e.source_gen = SourceBootGen(1);
  e.epoch = CoordinatorEpoch(1); e.time = seconds(1);
  e.value = BasisPoints(10000); e.sample_count = Count(1000);
  REQUIRE(f.ingest(e, seconds(1)).ok());
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Compliant);
  CHECK(r.value_unchecked().intent.action == EnforcementAction::NoAction);

  // 99.0% (below 99.9% target) -> violating.
  EvidenceRecord e2;
  e2.id = EvidenceId(++g_id); e2.gen = EvidenceGen(1);
  e2.dimension = Dimension::Availability;
  e2.objective_id = ObjectiveId(1); e2.objective_gen = ObjectiveGen(1);
  e2.source = SourceBootId(5); e2.source_gen = SourceBootGen(1);
  e2.epoch = CoordinatorEpoch(1); e2.time = seconds(2);
  e2.value = BasisPoints(9900); e2.sample_count = Count(1000);
  REQUIRE(f.ingest(e2, seconds(2)).ok());
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(3));
  REQUIRE(r2.ok());
  CHECK(r2.value_unchecked().explanation.compliance == ComplianceState::Violating);
  CHECK(r2.value_unchecked().intent.action == EnforcementAction::Replicate);
}

TEST("fabric: throughput compliant / violating") {
  SloFabric f;
  add_policy(f);
  auto c = base_contract();
  Objective o;
  o.id = ObjectiveId(2); o.gen = ObjectiveGen(1);
  o.name = "tput"; o.dim = Dimension::Throughput;
  o.kind = ObjectiveKind::ThroughputRate;
  o.comparison = Comparison::AtLeast; o.hard = HardSoft::Hard;
  o.target = RequestsPerSecond(100);
  o.rate_tag = RateTag::Requests;
  o.min_evidence = Count(1);
  c.objectives.push_back(o);
  REQUIRE(f.add_contract(c).ok());

  EvidenceRecord e;
  e.id = EvidenceId(++g_id); e.gen = EvidenceGen(1);
  e.dimension = Dimension::Throughput; e.objective_id = ObjectiveId(2); e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(6); e.epoch = CoordinatorEpoch(1); e.time = seconds(1);
  e.value = RequestsPerSecond(150);
  REQUIRE(f.ingest(e, seconds(1)).ok());
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Compliant);

  EvidenceRecord e2;
  e2.id = EvidenceId(++g_id); e2.gen = EvidenceGen(1);
  e2.dimension = Dimension::Throughput; e2.objective_id = ObjectiveId(2); e2.objective_gen = ObjectiveGen(1);
  e2.source = SourceBootId(6); e2.epoch = CoordinatorEpoch(1); e2.time = seconds(2);
  e2.value = RequestsPerSecond(40);
  REQUIRE(f.ingest(e2, seconds(2)).ok());
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(3));
  REQUIRE(r2.ok());
  CHECK(r2.value_unchecked().explanation.compliance == ComplianceState::Violating);
}

TEST("fabric: recovery time target met / missed") {
  SloFabric f;
  add_policy(f);
  auto c = base_contract();
  Objective o;
  o.id = ObjectiveId(3); o.gen = ObjectiveGen(1);
  o.name = "rto"; o.dim = Dimension::RecoveryTime;
  o.kind = ObjectiveKind::RecoveryMaxDuration;
  o.comparison = Comparison::AtMost; o.hard = HardSoft::Hard;
  o.target = seconds(5);
  c.objectives.push_back(o);
  REQUIRE(f.add_contract(c).ok());

  REQUIRE(f.record_recovery_event(ObjectiveId(3), RecoveryEventPhase::Observed, seconds(0)).ok());
  REQUIRE(f.record_recovery_event(ObjectiveId(3), RecoveryEventPhase::Authoritative, seconds(0)).ok());
  // At t=2s, recovery duration 2s < 5s -> compliant.
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Compliant);

  // At t=10s, recovery duration 10s > 5s -> violating.
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(10));
  REQUIRE(r2.ok());
  CHECK(r2.value_unchecked().explanation.compliance == ComplianceState::Violating);
  CHECK(r2.value_unchecked().intent.action == EnforcementAction::Failover);

  // Restored + verified stops the clock; at t=12s duration = verified_at(11s)-auth(0s)=11s still >5s.
  REQUIRE(f.record_recovery_event(ObjectiveId(3), RecoveryEventPhase::Restored, seconds(11)).ok());
  REQUIRE(f.record_recovery_event(ObjectiveId(3), RecoveryEventPhase::Verified, seconds(11)).ok());
  auto r3 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(12));
  REQUIRE(r3.ok());
  CHECK(r3.value_unchecked().explanation.compliance == ComplianceState::Violating);  // 11s > 5s
}

TEST("fabric: memory pressure + hysteresis") {
  SloFabric f;
  add_policy(f);
  auto c = base_contract();
  Objective o;
  o.id = ObjectiveId(4); o.gen = ObjectiveGen(1);
  o.name = "mem"; o.dim = Dimension::MemoryPressure;
  o.kind = ObjectiveKind::MemoryPressureMax;
  o.comparison = Comparison::AtMost; o.hard = HardSoft::Hard;
  o.target = PressureRatio(0.90);
  o.breach_threshold = PressureRatio(0.90);
  o.recovery_threshold = PressureRatio(0.80);
  o.min_evidence = Count(1);
  o.window_type = WindowType::Sliding;
  o.window_duration = seconds(2);   // bounded so a single high blip ages out
  c.objectives.push_back(o);
  REQUIRE(f.add_contract(c).ok());

  auto ingestP = [&](EvidenceId id, double p, Duration t) {
    EvidenceRecord e;
    e.id = id; e.gen = EvidenceGen(1);
    e.dimension = Dimension::MemoryPressure; e.objective_id = ObjectiveId(4); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(7); e.epoch = CoordinatorEpoch(1); e.time = t;
    e.value = PressureRatio(p);
    return f.ingest(e, t);
  };

  REQUIRE(ingestP(EvidenceId(++g_id), 0.95, seconds(1)).ok());   // violation zone
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Violating);

  // Ingest 0.85 at t=4: the 0.95 sample (t=1, age 3s >= 2s) ages out of the window.
  REQUIRE(ingestP(EvidenceId(++g_id), 0.85, seconds(4)).ok());
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(5));
  REQUIRE(r2.ok());
  // 0.85 is in the band [0.80, 0.90) : previous Violating state holds (hysteresis).
  CHECK(r2.value_unchecked().explanation.compliance == ComplianceState::Violating);

  // Ingest 0.75 at t=7 (0.85 sample ages out); below recovery -> compliant.
  REQUIRE(ingestP(EvidenceId(++g_id), 0.75, seconds(7)).ok());
  auto r3 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(8));
  REQUIRE(r3.ok());
  CHECK(r3.value_unchecked().explanation.compliance == ComplianceState::Compliant);
}

TEST("fabric: cost window budget under / over") {
  SloFabric f;
  add_policy(f);
  auto c = base_contract();
  Objective o;
  o.id = ObjectiveId(5); o.gen = ObjectiveGen(1);
  o.name = "cost"; o.dim = Dimension::Cost;
  o.kind = ObjectiveKind::CostWindowBudget;
  o.comparison = Comparison::AtMost; o.hard = HardSoft::Hard;
  o.target = CostMicros(1000000);
  o.window_type = WindowType::Sliding;
  o.window_duration = seconds(60);
  c.objectives.push_back(o);
  REQUIRE(f.add_contract(c).ok());

  auto ingestC = [&](EvidenceId id, std::int64_t micros, Duration t) {
    EvidenceRecord e;
    e.id = id; e.gen = EvidenceGen(1);
    e.dimension = Dimension::Cost; e.objective_id = ObjectiveId(5); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(8); e.epoch = CoordinatorEpoch(1); e.time = t;
    e.value = CostMicros(micros);
    return f.ingest(e, t);
  };
  REQUIRE(ingestC(EvidenceId(++g_id), 300000, seconds(1)).ok());  // 300k < 1M
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(2));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Compliant);

  REQUIRE(ingestC(EvidenceId(++g_id), 800000, seconds(3)).ok());  // total 1.1M > 1M
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(4));
  REQUIRE(r2.ok());
  CHECK(r2.value_unchecked().explanation.compliance == ComplianceState::Violating);
  CHECK(r2.value_unchecked().intent.action == EnforcementAction::ManualInterventionRequired);
}
