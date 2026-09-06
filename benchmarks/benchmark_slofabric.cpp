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
