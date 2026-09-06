#include "test_framework.hpp"

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"

using namespace slofabric;

static SloPolicy make_policy(const char* name = "default") {
  SloPolicy p;
  p.id = PolicyId(1);
  p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft;
  p.tie_break = TieBreak::ByName;
  p.name = name;
  p.description = "test policy";
  return p;
}

static SloContract make_latency_contract(ObjectiveId oid, ObjectiveGen ogen, Duration target = milliseconds(100)) {
  SloContract c;
  c.id = SloContractId(1);
  c.gen = SloContractGen(1);
  c.service = ServiceId(100);
  c.workload = WorkloadId(200);
  c.tenant = TenantId(1);
  c.policy_id = PolicyId(1);
  c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0);
  c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active;
  c.name = "latency-contract";
  c.provenance = "test";
  Objective o;
  o.id = oid; o.gen = ogen;
  o.name = "p99-latency";
  o.dim = Dimension::Latency;
  o.kind = ObjectiveKind::LatencyPercentile;
  o.percentile_rank = 0.99;
  o.comparison = Comparison::AtMost;
  o.hard = HardSoft::Hard;
  o.target = target;
  o.min_evidence = Count(64);
  o.window_type = WindowType::Sliding;
  o.window_duration = seconds(120);
  o.window_count = Count(64);
  c.objectives.push_back(o);
  return c;
}

static void ingest_thread(SloFabric& f, ObjectiveId oid, ObjectiveGen ogen, SourceBootId src,
                          CoordinatorEpoch ep, Duration start, int n, double ns_value,
                          int& id_floor) {
  for (int i = 0; i < n; ++i) {
    EvidenceRecord e;
    e.id = EvidenceId(++id_floor);
    e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency;
    e.objective_id = oid;
    e.objective_gen = ogen;
    e.source = src;
    e.source_gen = SourceBootGen(1);
    e.epoch = ep;
    e.time = Duration(start.as_ns() + i * 1000000000LL);  // 1s apart
    e.value = Duration(static_cast<std::int64_t>(ns_value));
    Status s = f.ingest(e, e.time);
    if (!s.ok()) { (void)0; }
  }
}

TEST("fabric: latency compliant") {
  SloFabric f;
  f.set_policy(make_policy());
  auto c = make_latency_contract(ObjectiveId(7), ObjectiveGen(1));
  REQUIRE(f.add_contract(c).ok());
  int floor = 0;
  ingest_thread(f, ObjectiveId(7), ObjectiveGen(1), SourceBootId(1), CoordinatorEpoch(1), seconds(0), 64, 50000000LL, floor);
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Compliant);
  CHECK(r.value_unchecked().intent.action == EnforcementAction::NoAction);
}

TEST("fabric: latency violating") {
  SloFabric f;
  f.set_policy(make_policy());
  auto c = make_latency_contract(ObjectiveId(7), ObjectiveGen(1));
  REQUIRE(f.add_contract(c).ok());
  int floor = 0;
  ingest_thread(f, ObjectiveId(7), ObjectiveGen(1), SourceBootId(1), CoordinatorEpoch(1), seconds(0), 64, 130000000LL, floor);
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Violating);
  CHECK(r.value_unchecked().intent.action == EnforcementAction::IncreaseCapacity);
  CHECK(r.value_unchecked().explanation.binding_objective == ObjectiveId(7));
  CHECK(r.value_unchecked().explanation.binding_dimension == Dimension::Latency);
}

TEST("fabric: latency near-limit") {
  SloFabric f;
  f.set_policy(make_policy());
  auto c = make_latency_contract(ObjectiveId(7), ObjectiveGen(1), milliseconds(100));
  REQUIRE(f.add_contract(c).ok());
  int floor = 0;
  // 99ms is within margin of 100ms breach but not violating.
  ingest_thread(f, ObjectiveId(7), ObjectiveGen(1), SourceBootId(1), CoordinatorEpoch(1), seconds(0), 64, 99000000LL, floor);
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance != ComplianceState::Violating);
}

TEST("fabric: insufficient percentile evidence") {
  SloFabric f;
  f.set_policy(make_policy());
  auto c = make_latency_contract(ObjectiveId(7), ObjectiveGen(1));
  REQUIRE(f.add_contract(c).ok());
  int floor = 0;
  // Only 10 samples, below min_evidence=64.
  ingest_thread(f, ObjectiveId(7), ObjectiveGen(1), SourceBootId(1), CoordinatorEpoch(1), seconds(0), 10, 50000000LL, floor);
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(12));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::InsufficientEvidence);
  CHECK(r.value_unchecked().intent.action == EnforcementAction::NoAction);
}

TEST("fabric: stale evidence rejected / revalidation") {
  SloFabric f;
  f.set_policy(make_policy());
  auto c = make_latency_contract(ObjectiveId(7), ObjectiveGen(1));
  REQUIRE(f.add_contract(c).ok());
  int floor = 0;
  // fresh at t=0..63
  ingest_thread(f, ObjectiveId(7), ObjectiveGen(1), SourceBootId(1), CoordinatorEpoch(1), seconds(0), 64, 50000000LL, floor);
  // Evaluate at a time far beyond freshness (window 120s but we set freshness ttl not set).
  // Default freshness: current if within window_duration (120s). Evaluate at t=1000s -> stale.
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(1000));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::RevalidationRequired);
  CHECK(r.value_unchecked().intent.action == EnforcementAction::NoAction);
}

TEST("fabric: stale authority rejection (epoch roll)") {
  SloFabric f;
  f.set_policy(make_policy());
  auto c = make_latency_contract(ObjectiveId(7), ObjectiveGen(1));
  REQUIRE(f.add_contract(c).ok());
  int floor = 0;
  // Evidence published under epoch 1.
  ingest_thread(f, ObjectiveId(7), ObjectiveGen(1), SourceBootId(1), CoordinatorEpoch(1), seconds(0), 64, 50000000LL, floor);
  // Ingest evidence published under a mismatched (future) coordinator epoch
  // -> rejected as stale authority. Note epoch 2 != current epoch 1.
  EvidenceRecord stale;
  stale.id = EvidenceId(99999);
  stale.gen = EvidenceGen(1);
  stale.objective_id = ObjectiveId(7);
  stale.objective_gen = ObjectiveGen(1);
  stale.epoch = CoordinatorEpoch(2);
  stale.time = seconds(100);
  stale.value = Duration(50000000LL);
  Status s = f.ingest(stale, seconds(100));
  CHECK(!s.ok());
  CHECK(s.code == StatusCode::StaleAuthority);
}
