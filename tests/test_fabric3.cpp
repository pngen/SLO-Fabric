#include "test_framework.hpp"

#include <filesystem>

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"

using namespace slofabric;

static int g_id3 = 0;

static void add_policy3(SloFabric& f) {
  SloPolicy p; p.id = PolicyId(1); p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft; p.tie_break = TieBreak::ByName;
  p.name = "p3"; p.description = "desc"; f.set_policy(p);
}

TEST("fabric3: multi-objective latency vs cost -> deterministic binding") {
  SloFabric f; add_policy3(f);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active; c.name = "c"; c.provenance = "t";
  Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
  lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
  lat.target = milliseconds(100); lat.min_evidence = Count(8);
  lat.window_type = WindowType::Sliding; lat.window_duration = seconds(60); lat.window_count = Count(16);
  lat.priority = Priority(1);
  c.objectives.push_back(lat);
  Objective cost; cost.id = ObjectiveId(2); cost.gen = ObjectiveGen(1);
  cost.name = "cost"; cost.dim = Dimension::Cost; cost.kind = ObjectiveKind::CostWindowBudget;
  cost.comparison = Comparison::AtMost; cost.hard = HardSoft::Hard;
  cost.target = CostMicros(1000); cost.window_type = WindowType::Sliding; cost.window_duration = seconds(60);
  cost.priority = Priority(0);   // higher priority than latency
  c.objectives.push_back(cost);
  REQUIRE(f.add_contract(c).ok());

  // Latency violating (130ms).
  for (int i = 0; i < 8; ++i) {
    EvidenceRecord e; e.id = EvidenceId(++g_id3); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i * 100); e.value = Duration(130000000LL);
    REQUIRE(f.ingest(e, e.time).ok());
  }
  // Cost violating.
  for (int i = 0; i < 4; ++i) {
    EvidenceRecord e; e.id = EvidenceId(++g_id3); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Cost; e.objective_id = ObjectiveId(2); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i * 100); e.value = CostMicros(600);
    REQUIRE(f.ingest(e, e.time).ok());
  }
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), milliseconds(1000));
  REQUIRE(r.ok());
  // Cost has higher priority (0), so it is the binding constraint.
  CHECK(r.value_unchecked().explanation.binding_objective == ObjectiveId(2));
  CHECK(r.value_unchecked().explanation.binding_dimension == Dimension::Cost);
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Violating);
  // Latency is a secondary constraint.
  bool secondary = false;
  for (auto id : r.value_unchecked().explanation.secondary_constraints) if (id == ObjectiveId(1)) secondary = true;
  CHECK(secondary);
  CHECK(r.value_unchecked().intent.action == EnforcementAction::ManualInterventionRequired);
}

TEST("fabric3: determinism identical inputs -> identical decision") {
  SloFabric f1, f2;
  for (auto* f : {&f1, &f2}) {
    add_policy3(*f);
    SloContract c;
    c.id = SloContractId(1); c.gen = SloContractGen(1);
    c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
    c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
    c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
    c.lifecycle = ContractLifecycle::Active; c.name = "c"; c.provenance = "t";
    Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
    lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
    lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
    lat.target = milliseconds(100); lat.min_evidence = Count(8);
    lat.window_type = WindowType::Sliding; lat.window_duration = seconds(60); lat.window_count = Count(16);
    c.objectives.push_back(lat);
    REQUIRE(f->add_contract(c).ok());
    for (int i = 0; i < 8; ++i) {
      EvidenceRecord e; e.id = EvidenceId(100 + i); e.gen = EvidenceGen(1);
      e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
      e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
      e.time = milliseconds(i * 100); e.value = Duration(50000000LL);
      REQUIRE(f->ingest(e, e.time).ok());
    }
  }
  auto r1 = f1.evaluate(ServiceId(100), WorkloadId(200), milliseconds(900));
  auto r2 = f2.evaluate(ServiceId(100), WorkloadId(200), milliseconds(900));
  REQUIRE(r1.ok()); REQUIRE(r2.ok());
  CHECK(r1.value_unchecked().explanation.compliance == r2.value_unchecked().explanation.compliance);
  CHECK(r1.value_unchecked().explanation.binding_objective == r2.value_unchecked().explanation.binding_objective);
  CHECK(r1.value_unchecked().intent.action == r2.value_unchecked().intent.action);
  // Digests equal.
  auto e1 = f1.get_evaluation(r1.value_unchecked().explanation.evaluation_id);
  auto e2 = f2.get_evaluation(r2.value_unchecked().explanation.evaluation_id);
  REQUIRE(e1.ok()); REQUIRE(e2.ok());
  CHECK_EQ(e1.value_unchecked().digest, e2.value_unchecked().digest);
}

TEST("fabric3: cooldown suppresses repeated enforcement") {
  SloFabric f; add_policy3(f);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active; c.name = "c"; c.provenance = "t";
  Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
  lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
  lat.target = milliseconds(100); lat.min_evidence = Count(8);
  lat.window_type = WindowType::Sliding; lat.window_duration = seconds(60); lat.window_count = Count(16);
  c.objectives.push_back(lat);
  REQUIRE(f.add_contract(c).ok());
  for (int i = 0; i < 8; ++i) {
    EvidenceRecord e; e.id = EvidenceId(++g_id3); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i * 100); e.value = Duration(140000000LL);
    f.ingest(e, e.time);
  }
  auto r1 = f.evaluate(ServiceId(100), WorkloadId(200), milliseconds(900));
  REQUIRE(r1.ok());
  CHECK(r1.value_unchecked().intent.action == EnforcementAction::IncreaseCapacity);
  // Authorize the action -> sets cooldown.
  auto rc = f.authorize(r1.value_unchecked().intent, milliseconds(900));
  REQUIRE(rc.ok());
  // Re-evaluate immediately -> cooldown suppresses the action.
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), milliseconds(950));
  REQUIRE(r2.ok());
  CHECK(r2.value_unchecked().intent.action == EnforcementAction::NoAction);
}

TEST("fabric3: superseded contract cannot authorize") {
  SloFabric f; add_policy3(f);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active; c.name = "c"; c.provenance = "t";
  Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
  lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
  lat.target = milliseconds(100); lat.min_evidence = Count(8);
  lat.window_type = WindowType::Sliding; lat.window_duration = seconds(60); lat.window_count = Count(16);
  c.objectives.push_back(lat);
  REQUIRE(f.add_contract(c).ok());
  for (int i = 0; i < 8; ++i) {
    EvidenceRecord e; e.id = EvidenceId(++g_id3); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i * 100); e.value = Duration(140000000LL);
    f.ingest(e, e.time);
  }
  auto r1 = f.evaluate(ServiceId(100), WorkloadId(200), milliseconds(900));
  REQUIRE(r1.ok());
  // Supersede the contract with gen 2.
  SloContract c2 = c; c2.gen = SloContractGen(2);
  REQUIRE(f.supersede_contract(c2, milliseconds(900)).ok());
  CHECK(f.find_contract(SloContractId(1)).has_value());
  // Old-generation intent must be rejected on authorize.
  auto rc = f.authorize(r1.value_unchecked().intent, milliseconds(900));
  CHECK(!rc.ok());
  CHECK(rc.status().code == StatusCode::StaleAuthority);
}

TEST("fabric3: error budget consume/exhaust/replenish") {
  SloFabric f; add_policy3(f);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active; c.name = "c"; c.provenance = "t";
  Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
  lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
  lat.target = milliseconds(100); lat.min_evidence = Count(1);
  lat.window_type = WindowType::Sliding; lat.window_duration = seconds(10); lat.window_count = Count(16);
  c.objectives.push_back(lat);
  REQUIRE(f.add_contract(c).ok());
  REQUIRE(f.set_objective_budget(ObjectiveId(1), Count(100), seconds(60)).ok());
  for (int i = 0; i < 95; ++i) {
    EvidenceRecord e; e.id = EvidenceId(++g_id3); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i * 10); e.value = Duration(50000000LL);
    f.ingest(e, e.time);
    f.ingest_violation_event(ObjectiveId(1), e.time);
  }
  auto r1 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(1));
  REQUIRE(r1.ok());
  CHECK(r1.value_unchecked().explanation.objectives[0].budget_state.has_value());
  CHECK(*r1.value_unchecked().explanation.objectives[0].budget_state == BudgetState::NearLimit);
  // Exhaust.
  for (int i = 0; i < 6; ++i) f.ingest_violation_event(ObjectiveId(1), seconds(1));
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(1));
  REQUIRE(r2.ok());
  CHECK(*r2.value_unchecked().explanation.objectives[0].budget_state == BudgetState::Exhausted);
  // Advance past the window -> budget replenishes (window rollover).
  auto r3 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  REQUIRE(r3.ok());
  CHECK(*r3.value_unchecked().explanation.objectives[0].budget_state == BudgetState::Healthy);
}

TEST("fabric3: save/load restart advances epoch and revalidates dynamic evidence") {
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "slofab_restart";
  fs::create_directories(dir);
  auto path = dir / "state.bin";
  {
    SloFabric f; add_policy3(f);
    SloContract c;
    c.id = SloContractId(1); c.gen = SloContractGen(1);
    c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
    c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
    c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
    c.lifecycle = ContractLifecycle::Active; c.name = "c"; c.provenance = "t";
    Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
    lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
    lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
    lat.target = milliseconds(100); lat.min_evidence = Count(8);
    lat.window_type = WindowType::Sliding; lat.window_duration = seconds(60); lat.window_count = Count(16);
    c.objectives.push_back(lat);
    REQUIRE(f.add_contract(c).ok());
    CHECK(f.current_epoch().value == 1u);
    REQUIRE(f.save(path).ok());
  }
  {
    SloFabric f; add_policy3(f);
    REQUIRE(f.load(path).ok());
    CHECK(f.current_epoch().value == 2u);   // epoch advanced on restart
    // Dynamic evidence was NOT persisted; revalidation required.
    auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(1));
    REQUIRE(r.ok());
    CHECK(r.value_unchecked().explanation.compliance == ComplianceState::RevalidationRequired ||
          r.value_unchecked().explanation.compliance == ComplianceState::InsufficientEvidence);
    CHECK(r.value_unchecked().intent.action == EnforcementAction::NoAction);
  }
  fs::remove_all(dir);
}
