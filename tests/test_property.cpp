#include "test_framework.hpp"

#include <filesystem>

#include "slofabric/budget.hpp"
#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"

using namespace slofabric;

// Deterministic PRNG (xorshift64) with a fixed seed for reproducible property tests.
static std::uint64_t xs = 0x9E3779B97F4A7C15ull;
static std::uint64_t rnd() { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }
static void reset_seed() { xs = 0x9E3779B97F4A7C15ull; }

static SloContract make_latency_contract(const char* name, Duration target = milliseconds(100)) {
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active; c.name = name; c.provenance = "test";
  Objective o; o.id = ObjectiveId(1); o.gen = ObjectiveGen(1);
  o.name = "lat"; o.dim = Dimension::Latency; o.kind = ObjectiveKind::LatencyPercentile;
  o.percentile_rank = 0.99; o.comparison = Comparison::AtMost; o.hard = HardSoft::Hard;
  o.target = target; o.min_evidence = Count(32);
  o.window_type = WindowType::Sliding; o.window_duration = seconds(60); o.window_count = Count(64);
  c.objectives.push_back(o);
  return c;
}

static void add_policy_prop(SloFabric& f) {
  SloPolicy p; p.id = PolicyId(1); p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft; p.tie_break = TieBreak::ByName;
  p.name = "prop"; p.description = "desc"; f.set_policy(p);
}

TEST("property: percentile exact nearest-rank on known distribution") {
  // Feed 100 samples 0..99ns. p99 ≈ 98ns (nearest-rank: ceil(0.99*100)-1 = 98).
  SloFabric f; add_policy_prop(f);
  // Target well above the sample range so the p99 value is comfortably compliant.
  REQUIRE(f.add_contract(make_latency_contract("c", seconds(1))).ok());
  for (int i = 0; i < 100; ++i) {
    EvidenceRecord e; e.id = EvidenceId(i + 1); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i); e.value = Duration(i * 1000000LL);  // 0..99 ms, ns
    f.ingest(e, e.time);
  }
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), milliseconds(200));
  REQUIRE(r.ok());
  auto cur = r.value_unchecked().explanation.objectives[0].current_value;
  REQUIRE(cur.has_value());
  // p99 over the in-window (bounded) samples must be >= 98ms (nearest-rank).
  CHECK(numeric_of(*cur) >= 98000000.0);
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Compliant);
}

TEST("property: deterministic replay gives identical digest for identical input") {
  reset_seed();
  // Precompute identical sample values and feed the SAME values to both fabrics.
  std::vector<Duration> samples;
  for (int i = 0; i < 64; ++i) {
    double v = 40e6 + static_cast<double>(rnd() % 20000000);
    samples.push_back(Duration(static_cast<std::int64_t>(v)));
  }
  SloFabric f1, f2;
  for (auto* f : {&f1, &f2}) {
    add_policy_prop(*f);
    REQUIRE(f->add_contract(make_latency_contract("c")).ok());
    for (int i = 0; i < 64; ++i) {
      EvidenceRecord e; e.id = EvidenceId(i + 1); e.gen = EvidenceGen(1);
      e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
      e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
      e.time = milliseconds(i * 100); e.value = samples[i];
      REQUIRE(f->ingest(e, e.time).ok());
    }
  }
  auto r1 = f1.evaluate(ServiceId(100), WorkloadId(200), milliseconds(7000));
  auto r2 = f2.evaluate(ServiceId(100), WorkloadId(200), milliseconds(7000));
  REQUIRE(r1.ok()); REQUIRE(r2.ok());
  CHECK(r1.value_unchecked().explanation.compliance == r2.value_unchecked().explanation.compliance);
  CHECK(r1.value_unchecked().explanation.binding_objective == r2.value_unchecked().explanation.binding_objective);
  auto e1 = f1.get_evaluation(r1.value_unchecked().explanation.evaluation_id);
  auto e2 = f2.get_evaluation(r2.value_unchecked().explanation.evaluation_id);
  REQUIRE(e1.ok()); REQUIRE(e2.ok());
  CHECK_EQ(e1.value_unchecked().digest, e2.value_unchecked().digest);
}

TEST("property: remaining budget never exceeds total after random consumes") {
  for (int trial = 0; trial < 50; ++trial) {
    std::int64_t total = static_cast<std::int64_t>(rnd() % 1000) + 1;
    ErrorBudget b(Count(total), seconds(60));
    int consume = static_cast<int>(rnd() % 3000);
    for (int i = 0; i < consume; ++i) b.consume();
    CHECK(b.remaining().value() >= 0);
    CHECK(b.remaining().value() <= total);
    CHECK(b.overrun().value() >= 0);
  }
}

TEST("property: stale evidence cannot produce current enforcement authority") {
  SloFabric f; add_policy_prop(f);
  REQUIRE(f.add_contract(make_latency_contract("c")).ok());
  for (int i = 0; i < 32; ++i) {
    EvidenceRecord e; e.id = EvidenceId(i + 1); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i * 100); e.value = Duration(140000000LL);
    f.ingest(e, e.time);
  }
  // Evaluate NOW -> violating (current).
  auto r1 = f.evaluate(ServiceId(100), WorkloadId(200), milliseconds(3200));
  REQUIRE(r1.ok());
  CHECK(r1.value_unchecked().explanation.compliance == ComplianceState::Violating);
  CHECK(r1.value_unchecked().intent.action == EnforcementAction::IncreaseCapacity);
  // Evaluate far in the future -> evidence stale -> RevalidationRequired, NoAction.
  auto r2 = f.evaluate(ServiceId(100), WorkloadId(200), seconds(5000));
  REQUIRE(r2.ok());
  CHECK(r2.value_unchecked().explanation.compliance == ComplianceState::RevalidationRequired);
  CHECK(r2.value_unchecked().intent.action == EnforcementAction::NoAction);
}

TEST("property: all hard objectives impossible -> violation, not compliant") {
  SloFabric f; add_policy_prop(f);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active; c.name = "c"; c.provenance = "t";
  // Two hard, mutually-impossible objectives: latency <= 1ns and throughput >= 1e9.
  Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
  lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
  lat.target = nanoseconds(1); lat.min_evidence = Count(4);
  lat.window_type = WindowType::Sliding; lat.window_duration = seconds(60); lat.window_count = Count(16);
  c.objectives.push_back(lat);
  Objective thr; thr.id = ObjectiveId(2); thr.gen = ObjectiveGen(1);
  thr.name = "thr"; thr.dim = Dimension::Throughput; thr.kind = ObjectiveKind::ThroughputRate;
  thr.comparison = Comparison::AtLeast; thr.hard = HardSoft::Hard;
  thr.target = RequestsPerSecond(1e9); thr.rate_tag = RateTag::Requests;
  thr.min_evidence = Count(4);
  thr.window_type = WindowType::Sliding; thr.window_duration = seconds(60); thr.window_count = Count(16);
  c.objectives.push_back(thr);
  REQUIRE(f.add_contract(c).ok());
  for (int i = 0; i < 4; ++i) {
    EvidenceRecord e; e.id = EvidenceId(100 + i); e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency; e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1); e.source_gen = SourceBootGen(1); e.epoch = CoordinatorEpoch(1);
    e.time = milliseconds(i * 10); e.value = Duration(1000000LL);  // 1ms >> 1ns
    f.ingest(e, e.time);
    EvidenceRecord t; t.id = EvidenceId(200 + i); t.gen = EvidenceGen(1);
    t.dimension = Dimension::Throughput; t.objective_id = ObjectiveId(2); t.objective_gen = ObjectiveGen(1);
    t.source = SourceBootId(1); t.source_gen = SourceBootGen(1); t.epoch = CoordinatorEpoch(1);
    t.time = milliseconds(i * 10); t.value = RequestsPerSecond(100);   // 100 << 1e9
    f.ingest(t, t.time);
  }
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), milliseconds(100));
  REQUIRE(r.ok());
  CHECK(r.value_unchecked().explanation.compliance == ComplianceState::Violating);
  CHECK(r.value_unchecked().explanation.binding_dimension == Dimension::Latency ||
        r.value_unchecked().explanation.binding_dimension == Dimension::Throughput);  // worst offender binds
}
