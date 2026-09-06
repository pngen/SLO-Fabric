// SLO Fabric micro-benchmarks.
//
// Compiles with:
//   cl /std:c++20 /W4 /WX /permissive- /EHsc /utf-8 /I include
//      /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /c benchmarks/benchmark_slofabric.cpp
//
// Prints one line per scenario: "name N ops  X ms  Y ops/s".
// Optional argument: --iters N  (number of operations per scenario, default 2000).

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"
#include "slofabric/persistence.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace slofabric;
namespace fs = std::filesystem;

namespace {

// Sink so optimization cannot elide pure (read-only) query operations.
volatile std::uint64_t g_sink = 0;

SloPolicy bench_policy() {
  SloPolicy p;
  p.id = PolicyId(1);
  p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft;
  p.tie_break = TieBreak::ByName;
  p.name = "bench";
  p.description = "benchmark policy";
  return p;
}

// Run op(it) `iters` times and report ops/sec + milliseconds.
template <typename Fn>
void run_scenario(const char* name, std::size_t iters, Fn&& op) {
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iters; ++i) op(i);
  const auto stop = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(stop - start).count();
  const double ops = static_cast<double>(iters);
  const double ops_per_s = ops / (ms / 1000.0);
  std::printf("%s %llu ops  %.3f ms  %.1f ops/s\n", name,
              static_cast<unsigned long long>(iters), ms, ops_per_s);
}

void seed_latency(SloFabric& f, ObjectiveId oid, std::int64_t ns_value, int n,
                  Duration start, std::uint64_t& ev_id) {
  for (int i = 0; i < n; ++i) {
    EvidenceRecord e;
    e.id = EvidenceId(++ev_id);
    e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency;
    e.objective_id = oid;
    e.objective_gen = ObjectiveGen(1);
    e.source = SourceBootId(1);
    e.source_gen = SourceBootGen(1);
    e.epoch = CoordinatorEpoch(1);
    e.time = Duration(start.as_ns() + static_cast<std::int64_t>(i) * 1000000000LL);
    e.value = Duration(ns_value);
    f.ingest(e, e.time);
  }
}

void seed_availability(SloFabric& f, ObjectiveId oid, std::int64_t bp, std::int64_t count,
                       Duration t, std::uint64_t& ev_id) {
  EvidenceRecord e;
  e.id = EvidenceId(++ev_id);
  e.gen = EvidenceGen(1);
  e.dimension = Dimension::Availability;
  e.objective_id = oid;
  e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(3);
  e.source_gen = SourceBootGen(1);
  e.epoch = CoordinatorEpoch(1);
  e.time = t;
  e.value = BasisPoints(bp);
  e.sample_count = Count(count);
  f.ingest(e, t);
}

void seed_throughput(SloFabric& f, ObjectiveId oid, double rps, Duration t, std::uint64_t& ev_id) {
  EvidenceRecord e;
  e.id = EvidenceId(++ev_id);
  e.gen = EvidenceGen(1);
  e.dimension = Dimension::Throughput;
  e.objective_id = oid;
  e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(5);
  e.source_gen = SourceBootGen(1);
  e.epoch = CoordinatorEpoch(1);
  e.time = t;
  e.value = RequestsPerSecond(rps);
  f.ingest(e, t);
}

void seed_pressure(SloFabric& f, ObjectiveId oid, double p, Duration t, std::uint64_t& ev_id) {
  EvidenceRecord e;
  e.id = EvidenceId(++ev_id);
  e.gen = EvidenceGen(1);
  e.dimension = Dimension::MemoryPressure;
  e.objective_id = oid;
  e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(7);
  e.source_gen = SourceBootGen(1);
  e.epoch = CoordinatorEpoch(1);
  e.time = t;
  e.value = PressureRatio(p);
  f.ingest(e, t);
}

void seed_cost(SloFabric& f, ObjectiveId oid, std::int64_t micros, Duration t, std::uint64_t& ev_id) {
  EvidenceRecord e;
  e.id = EvidenceId(++ev_id);
  e.gen = EvidenceGen(1);
  e.dimension = Dimension::Cost;
  e.objective_id = oid;
  e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(9);
  e.source_gen = SourceBootGen(1);
  e.epoch = CoordinatorEpoch(1);
  e.time = t;
  e.value = CostMicros(micros);
  f.ingest(e, t);
}

// ---- Contract builders -----------------------------------------------------

SloContract latency_mean_contract(ObjectiveId oid) {
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
  c.name = "bench-contract";
  c.provenance = "bench";
  Objective o;
  o.id = oid;
  o.gen = ObjectiveGen(1);
  o.name = "latency-mean";
  o.dim = Dimension::Latency;
  o.kind = ObjectiveKind::LatencyMean;
  o.comparison = Comparison::AtMost;
  o.hard = HardSoft::Hard;
  o.target = milliseconds(100);
  o.min_evidence = Count(1);
  o.window_type = WindowType::Instantaneous;
  c.objectives.push_back(o);
  return c;
}

SloContract latency_p99_contract(ObjectiveId oid) {
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
  c.name = "bench-contract";
  c.provenance = "bench";
  Objective o;
  o.id = oid;
  o.gen = ObjectiveGen(1);
  o.name = "p99-latency";
  o.dim = Dimension::Latency;
  o.kind = ObjectiveKind::LatencyPercentile;
  o.percentile_rank = 0.99;
  o.comparison = Comparison::AtMost;
  o.hard = HardSoft::Hard;
  o.target = milliseconds(100);
  o.min_evidence = Count(64);
  o.window_type = WindowType::Sliding;
  o.window_duration = seconds(120);
  o.window_count = Count(64);
  c.objectives.push_back(o);
  return c;
}

SloContract latency_sliding_contract(ObjectiveId oid) {
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
  c.name = "bench-contract";
  c.provenance = "bench";
  Objective o;
  o.id = oid;
  o.gen = ObjectiveGen(1);
  o.name = "latency-sliding";
  o.dim = Dimension::Latency;
  o.kind = ObjectiveKind::LatencyMean;
  o.comparison = Comparison::AtMost;
  o.hard = HardSoft::Hard;
  o.target = milliseconds(100);
  o.min_evidence = Count(1);
  o.window_type = WindowType::Sliding;
  o.window_duration = seconds(120);
  o.window_count = Count(64);
  c.objectives.push_back(o);
  return c;
}

SloContract multi8_contract() {
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
  c.name = "multi8";
  c.provenance = "bench";
  c.objectives.reserve(8);

  {
    Objective o;
    o.id = ObjectiveId(1);
    o.gen = ObjectiveGen(1);
    o.name = "lat-mean";
    o.dim = Dimension::Latency;
    o.kind = ObjectiveKind::LatencyMean;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = milliseconds(100);
    o.min_evidence = Count(1);
    o.window_type = WindowType::Instantaneous;
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(2);
    o.gen = ObjectiveGen(1);
    o.name = "p99";
    o.dim = Dimension::Latency;
    o.kind = ObjectiveKind::LatencyPercentile;
    o.percentile_rank = 0.99;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = milliseconds(100);
    o.min_evidence = Count(64);
    o.window_type = WindowType::Sliding;
    o.window_duration = seconds(120);
    o.window_count = Count(64);
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(3);
    o.gen = ObjectiveGen(1);
    o.name = "avail";
    o.dim = Dimension::Availability;
    o.kind = ObjectiveKind::AvailabilityRequest;
    o.comparison = Comparison::AtLeast;
    o.hard = HardSoft::Hard;
    o.target = BasisPoints(9990);
    o.availability_mode = AvailabilityMode::Requests;
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(4);
    o.gen = ObjectiveGen(1);
    o.name = "tput";
    o.dim = Dimension::Throughput;
    o.kind = ObjectiveKind::ThroughputRate;
    o.comparison = Comparison::AtLeast;
    o.hard = HardSoft::Hard;
    o.target = RequestsPerSecond(100.0);
    o.rate_tag = RateTag::Requests;
    o.min_evidence = Count(1);
    o.window_type = WindowType::Instantaneous;
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(5);
    o.gen = ObjectiveGen(1);
    o.name = "rto";
    o.dim = Dimension::RecoveryTime;
    o.kind = ObjectiveKind::RecoveryMaxDuration;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = seconds(5);
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(6);
    o.gen = ObjectiveGen(1);
    o.name = "mem";
    o.dim = Dimension::MemoryPressure;
    o.kind = ObjectiveKind::MemoryPressureMax;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = PressureRatio(0.90);
    o.min_evidence = Count(1);
    o.window_type = WindowType::Instantaneous;
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(7);
    o.gen = ObjectiveGen(1);
    o.name = "lat-deadline";
    o.dim = Dimension::Latency;
    o.kind = ObjectiveKind::LatencyMaxDeadline;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = milliseconds(100);
    o.min_evidence = Count(1);
    o.window_type = WindowType::Instantaneous;
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(8);
    o.gen = ObjectiveGen(1);
    o.name = "cost-win";
    o.dim = Dimension::Cost;
    o.kind = ObjectiveKind::CostWindowBudget;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = CostMicros(1000000);
    o.window_type = WindowType::Sliding;
    o.window_duration = seconds(60);
    c.objectives.push_back(o);
  }
  return c;
}

SloContract binding_contract() {
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
  c.name = "binding";
  c.provenance = "bench";
  c.objectives.reserve(4);

  {
    Objective o;
    o.id = ObjectiveId(1);
    o.gen = ObjectiveGen(1);
    o.name = "violated-p99";
    o.dim = Dimension::Latency;
    o.kind = ObjectiveKind::LatencyPercentile;
    o.percentile_rank = 0.99;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = milliseconds(100);
    o.min_evidence = Count(64);
    o.window_type = WindowType::Sliding;
    o.window_duration = seconds(120);
    o.window_count = Count(64);
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(2);
    o.gen = ObjectiveGen(1);
    o.name = "lat-mean-ok";
    o.dim = Dimension::Latency;
    o.kind = ObjectiveKind::LatencyMean;
    o.comparison = Comparison::AtMost;
    o.hard = HardSoft::Hard;
    o.target = milliseconds(100);
    o.min_evidence = Count(1);
    o.window_type = WindowType::Instantaneous;
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(3);
    o.gen = ObjectiveGen(1);
    o.name = "avail-ok";
    o.dim = Dimension::Availability;
    o.kind = ObjectiveKind::AvailabilityRequest;
    o.comparison = Comparison::AtLeast;
    o.hard = HardSoft::Hard;
    o.target = BasisPoints(9990);
    o.availability_mode = AvailabilityMode::Requests;
    c.objectives.push_back(o);
  }
  {
    Objective o;
    o.id = ObjectiveId(4);
    o.gen = ObjectiveGen(1);
    o.name = "tput-ok";
    o.dim = Dimension::Throughput;
    o.kind = ObjectiveKind::ThroughputRate;
    o.comparison = Comparison::AtLeast;
    o.hard = HardSoft::Hard;
    o.target = RequestsPerSecond(100.0);
    o.rate_tag = RateTag::Requests;
    o.min_evidence = Count(1);
    o.window_type = WindowType::Instantaneous;
    c.objectives.push_back(o);
  }
  return c;
}

DurableState make_durable_state() {
  DurableState st;
  st.epoch = CoordinatorEpoch(7);

  SloPolicy p;
  p.id = PolicyId(1);
  p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft;
  p.tie_break = TieBreak::ByName;
  p.name = "persist-policy";
  p.description = "persist policy";
  st.policies.push_back(p);

  SloContract con = latency_p99_contract(ObjectiveId(1));
  st.contracts.push_back(con);

  PersistedObjectiveState ps;
  ps.objective_id = ObjectiveId(1);
  ps.objective_gen = ObjectiveGen(1);
  ps.budget_total = Count(1000);
  ps.budget_consumed = Count(42);
  ps.budget_window = seconds(60);
  ps.budget_window_start = seconds(0);
  ps.last_state = ComplianceState::Compliant;
  ps.last_state_time = seconds(10);
  ps.cooldown_until = seconds(0);
  st.objective_states.push_back(ps);

  EnforcementReceipt rc;
  rc.id = EnforcementId(1);
  rc.gen = EnforcementGen(1);
  rc.action = EnforcementAction::IncreaseCapacity;
  rc.lifecycle = EnforcementLifecycle::Authorized;
  rc.epoch = st.epoch;
  rc.contract_gen = con.gen;
  rc.policy_gen = p.gen;
  rc.evaluation_id = EvaluationId(1);
  rc.evaluation_gen = EvaluationGen(1);
  rc.workload_gen = WorkloadGen(1);
  rc.service = con.service;
  rc.workload = con.workload;
  rc.objective_id = ObjectiveId(1);
  rc.objective_gen = ObjectiveGen(1);
  rc.reason = "bench receipt";
  st.enforcement_history.push_back(rc);

  EvaluationRecord er;
  er.evaluation_id = EvaluationId(1);
  er.evaluation_gen = EvaluationGen(1);
  er.contract_gen = con.gen;
  er.policy_gen = p.gen;
  er.evidence_gen = EvidenceGen(1);
  er.epoch = st.epoch;
  er.time = seconds(10);
  er.compliance = ComplianceState::Compliant;
  er.binding_dimension = Dimension::Latency;
  er.binding_objective = ObjectiveId(1);
  er.selected_action = EnforcementAction::NoAction;
  er.digest = 0x1234567890ABCDEFULL;
  st.evaluation_history.push_back(er);

  return st;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t iters = 2000;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--iters" && i + 1 < argc) {
      iters = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    }
  }
  if (iters == 0) iters = 1;

  // 1) Evidence ingestion into a latency SloFabric.
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(latency_sliding_contract(ObjectiveId(1)));
    const Duration base(0);
    run_scenario("ingest", iters, [&](std::size_t i) {
      EvidenceRecord e;
      e.id = EvidenceId(static_cast<std::uint64_t>(i + 1));
      e.gen = EvidenceGen(1);
      e.dimension = Dimension::Latency;
      e.objective_id = ObjectiveId(1);
      e.objective_gen = ObjectiveGen(1);
      e.source = SourceBootId(1);
      e.source_gen = SourceBootGen(1);
      e.epoch = CoordinatorEpoch(1);
      e.time = Duration(base.as_ns() + static_cast<std::int64_t>(i) * 1000000000LL);
      e.value = Duration(50000000LL + static_cast<std::int64_t>(i % 1000));
      if (f.ingest(e, e.time).ok()) ++g_sink;
    });
  }

  // 2) Single-objective evaluate().
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(latency_mean_contract(ObjectiveId(1)));
    std::uint64_t ev_id = 0;
    seed_latency(f, ObjectiveId(1), 50000000LL, 1, seconds(0), ev_id);
    run_scenario("evaluate-single", iters, [&](std::size_t) {
      auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
      if (r.ok()) ++g_sink;
    });
  }

  // 3) Multi-objective evaluate() with a contract of 8 objectives.
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(multi8_contract());
    std::uint64_t ev_id = 0;
    seed_latency(f, ObjectiveId(1), 50000000LL, 3, seconds(0), ev_id);
    seed_latency(f, ObjectiveId(2), 50000000LL, 64, seconds(0), ev_id);
    seed_availability(f, ObjectiveId(3), 10000, 1000, seconds(1), ev_id);
    seed_throughput(f, ObjectiveId(4), 150.0, seconds(1), ev_id);
    seed_pressure(f, ObjectiveId(6), 0.50, seconds(1), ev_id);
    seed_latency(f, ObjectiveId(7), 60000000LL, 1, seconds(1), ev_id);
    seed_cost(f, ObjectiveId(8), 300000, seconds(1), ev_id);
    run_scenario("evaluate-multi8", iters, [&](std::size_t) {
      auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
      if (r.ok()) ++g_sink;
    });
  }

  // 4) p99 percentile evaluate().
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(latency_p99_contract(ObjectiveId(1)));
    std::uint64_t ev_id = 0;
    seed_latency(f, ObjectiveId(1), 50000000LL, 64, seconds(0), ev_id);
    run_scenario("evaluate-p99", iters, [&](std::size_t) {
      auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
      if (r.ok()) ++g_sink;
    });
  }

  // 5) Budget consume (ingest_violation_event).
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(latency_mean_contract(ObjectiveId(1)));
    f.set_objective_budget(ObjectiveId(1), Count(100000), seconds(60));
    run_scenario("budget-consume", iters, [&](std::size_t) {
      if (f.ingest_violation_event(ObjectiveId(1), seconds(70)).ok()) ++g_sink;
    });
  }

  // 6) Budget update (set_objective_budget).
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(latency_mean_contract(ObjectiveId(1)));
    run_scenario("budget-update", iters, [&](std::size_t) {
      if (f.set_objective_budget(ObjectiveId(1), Count(100000), seconds(60)).ok()) ++g_sink;
    });
  }

  // 7) Binding-constraint selection (multi-objective with a violated objective).
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(binding_contract());
    std::uint64_t ev_id = 0;
    seed_latency(f, ObjectiveId(1), 130000000LL, 64, seconds(0), ev_id);
    seed_latency(f, ObjectiveId(2), 50000000LL, 1, seconds(1), ev_id);
    seed_availability(f, ObjectiveId(3), 10000, 1000, seconds(1), ev_id);
    seed_throughput(f, ObjectiveId(4), 150.0, seconds(1), ev_id);
    run_scenario("binding-selection", iters, [&](std::size_t) {
      auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
      if (r.ok()) g_sink += r.value_unchecked().explanation.binding_objective.value;
    });
  }

  // 8) Explanation generation (read the full explanation structure).
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(binding_contract());
    std::uint64_t ev_id = 0;
    seed_latency(f, ObjectiveId(1), 130000000LL, 64, seconds(0), ev_id);
    seed_latency(f, ObjectiveId(2), 50000000LL, 1, seconds(1), ev_id);
    seed_availability(f, ObjectiveId(3), 10000, 1000, seconds(1), ev_id);
    seed_throughput(f, ObjectiveId(4), 150.0, seconds(1), ev_id);
    run_scenario("explanation", iters, [&](std::size_t) {
      auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
      if (r.ok()) {
        const Explanation& ex = r.value_unchecked().explanation;
        g_sink += static_cast<std::uint64_t>(ex.objectives.size());
        g_sink += static_cast<std::uint64_t>(ex.ranked_intents.size());
        g_sink += static_cast<std::uint64_t>(ex.secondary_constraints.size());
        g_sink += static_cast<std::uint64_t>(ex.predicted_risks.size());
        g_sink += static_cast<std::uint64_t>(ex.what_would_change.reasons.size());
        g_sink += static_cast<std::uint64_t>(static_cast<int>(ex.compliance));
        g_sink += static_cast<std::uint64_t>(static_cast<int>(ex.selected_action));
        g_sink += ex.binding_objective.value;
      }
    });
  }

  // 9) Contract lookup by id.
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(latency_mean_contract(ObjectiveId(1)));
    run_scenario("contract-lookup", iters, [&](std::size_t) {
      auto c = f.find_contract(SloContractId(1));
      if (c.has_value()) g_sink += static_cast<std::uint64_t>(c->gen.value);
    });
  }

  // 10) Historical replay via get_evaluation(id).
  {
    SloFabric f;
    f.set_policy(bench_policy());
    f.add_contract(latency_p99_contract(ObjectiveId(1)));
    std::uint64_t ev_id = 0;
    seed_latency(f, ObjectiveId(1), 50000000LL, 64, seconds(0), ev_id);
    // Populate the evaluation history.
    for (int k = 0; k < 100; ++k) {
      (void)f.evaluate(ServiceId(100), WorkloadId(200), seconds(70));
    }
    const std::uint64_t base_id = 1;
    run_scenario("replay", iters, [&](std::size_t i) {
      EvaluationId id(static_cast<std::uint64_t>(base_id + i % 100));
      auto r = f.get_evaluation(id);
      if (r.ok()) g_sink += r.value_unchecked().evaluation_id.value;
    });
  }

  // 11) Persistence save (save_state to temp dir).
  {
    const fs::path dir = fs::temp_directory_path() / "slofabric_bench_state";
    fs::create_directories(dir);
    const fs::path path = dir / "state.bin";
    const DurableState state = make_durable_state();
    run_scenario("persistence-save", iters, [&](std::size_t) {
      if (save_state(path, state).ok()) ++g_sink;
    });
  }

  // 12) Persistence load (load_state from temp dir).
  {
    const fs::path dir = fs::temp_directory_path() / "slofabric_bench_state";
    fs::create_directories(dir);
    const fs::path path = dir / "state.bin";
    const DurableState state = make_durable_state();
    (void)save_state(path, state);
    DurableState out;
    run_scenario("persistence-load", iters, [&](std::size_t) {
      if (load_state(path, out).ok()) ++g_sink;
    });
    fs::remove_all(dir);
  }

  std::printf("sink=%llu\n", static_cast<unsigned long long>(g_sink));
  return 0;
}

