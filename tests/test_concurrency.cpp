#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"

using namespace slofabric;

static void add_conc_policy(SloFabric& f) {
  SloPolicy p; p.id = PolicyId(1); p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft; p.tie_break = TieBreak::ByName;
  p.name = "conc"; p.description = "concurrency test"; f.set_policy(p);
}

static void add_conc_contract(SloFabric& f) {
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = PolicyId(1); c.policy_gen = PolicyGen(1);
  c.effective_from = seconds(0); c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active; c.name = "conc"; c.provenance = "t";
  Objective lat; lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
  lat.name = "lat"; lat.dim = Dimension::Latency; lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99; lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
  lat.target = milliseconds(200); lat.min_evidence = Count(1);
  lat.window_type = WindowType::RollingCount; lat.window_count = Count(512);
  c.objectives.push_back(lat);
  REQUIRE(f.add_contract(c).ok());
  REQUIRE(f.set_objective_budget(ObjectiveId(1), Count(1000000), seconds(3600)).ok());
}

TEST("concurrency: public SloFabric operations are internally thread-safe") {
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "slofab_conc";
  fs::create_directories(dir);
  auto path = dir / "state.bin";

  SloFabric f;
  add_conc_policy(f);
  add_conc_contract(f);

  const int T = 8;
  const int K = 2000;
  std::atomic<int> thread_err{0};
  std::atomic<int> g_err_code{0};
  std::atomic<int> g_err_op{0};
  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> g_time{0};
  std::atomic<long> g_consume_count{0};
  std::vector<std::thread> threads;

  // Saver thread: concurrently persists while writers run (exercises save's
  // "I/O outside the lock" discipline and proves it does not deadlock).
  threads.emplace_back([&]() {
    int n = 0;
    while (!stop.load() && n < 20) {
      Status s = f.save(path);
      if (!s.ok()) thread_err.store(1);
      ++n;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  // Writer/reader threads: concurrent evidence ingestion, budget consumption,
  // and evaluation.
  for (int t = 0; t < T; ++t) {
    threads.emplace_back([&, t]() {
      for (int k = 0; k < K; ++k) {
        EvidenceRecord e;
        e.id = EvidenceId(static_cast<std::uint64_t>(t) * 1000000u + k + 1);
        e.gen = EvidenceGen(1);
        e.dimension = Dimension::Latency;
        e.objective_id = ObjectiveId(1);
        e.objective_gen = ObjectiveGen(1);
        e.source = SourceBootId(1);
        e.source_gen = SourceBootGen(1);
        e.epoch = CoordinatorEpoch(1);
        e.time = Duration(g_time.fetch_add(1) * 1000000LL);
        e.provenance = Provenance::Measured;
        e.value = Duration(50000000LL);
        Status si = f.ingest(e, e.time);
        // Concurrent asynchronous writers may be legitimately rejected for
        // out-of-order (clock-rollback) arrival; that is correct window
        // semantics, not a thread-safety defect. Only unexpected failures count.
        if (!si.ok() && si.code != StatusCode::StaleEvidence) { thread_err.store(1); g_err_op.store(1); g_err_code.store((int)si.code); }
        Status sv = f.ingest_violation_event(ObjectiveId(1), e.time);
        if (!sv.ok()) { thread_err.store(1); g_err_op.store(2); g_err_code.store((int)sv.code); }
        else { g_consume_count.fetch_add(1); }
        if ((k % 200) == 0) {
          // Evaluate within the budget window so it never triggers a rollover
          // reset; this keeps the consumed count cumulative and exact.
          auto r = f.evaluate(ServiceId(100), WorkloadId(200), e.time);
          if (!r.ok()) { thread_err.store(1); g_err_op.store(3); g_err_code.store((int)r.status().code); }
          if (f.contract_count() != 1u) { thread_err.store(1); g_err_op.store(4); }
        }
      }
    });
  }

  for (int i = 1; i < threads.size(); ++i) threads[i].join();
  stop.exchange(true);
  threads[0].join();  // saver

  CHECK(thread_err.load() == 0);

  // Exact accounting: budget consumption must be exactly T*K (no lost updates).
  auto statuses = f.objective_statuses(SloContractId(1));
  bool found = false;
  for (auto& s : statuses) {
    if (s.objective.id == ObjectiveId(1) && s.budget_consumed.has_value()) {
      CHECK_EQ(s.budget_consumed->value(), static_cast<std::int64_t>(T) * K);
      found = true;
    }
  }
  CHECK(found);

  // Final evaluation is valid and the persisted state loads into a fresh fabric.
  auto r = f.evaluate(ServiceId(100), WorkloadId(200), seconds(10));
  REQUIRE(r.ok());
  REQUIRE(f.save(path).ok());
  SloFabric f2;
  add_conc_policy(f2);
  REQUIRE(f2.load(path).ok());
  CHECK(f2.current_epoch().value == 2u);   // restart advances epoch
  CHECK(f2.contract_count() == 1u);

  fs::remove_all(dir);
}
