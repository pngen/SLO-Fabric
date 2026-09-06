#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "slofabric/fabric.hpp"
#include "slofabric/net.hpp"

#include "distributed_protocol.hpp"

using namespace slofabric;
using namespace slofabric_proof;

namespace {
constexpr std::uint64_t kLatencyObjId = 1;
constexpr std::uint64_t kRecoveryObjId = 2;
constexpr std::uint64_t kAvailObjId = 3;

struct WorkerInfo {
  std::string name;
  std::uint64_t boot_id = 0;
  std::int64_t last_hb_ms = 0;
  std::atomic<bool> alive{true};
};

SloFabric g_fabric;
CoordinatorEpoch g_epoch{1};
std::mutex g_mu;
std::map<socket_handle, WorkerInfo*> g_workers;
std::map<std::string, std::vector<std::uint64_t>> g_seen_boots;
std::string g_state_file;
std::atomic<bool> g_down_a{false};
std::atomic<bool> g_recovery_auth{false};
std::atomic<bool> g_recovery_verified{false};
std::uint64_t g_next_evidence = 1000000;

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
Duration now_dur() { return Duration(now_ms() * 1000000LL); }

void set_scenario() {
  SloPolicy p;
  p.id = PolicyId(1); p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft; p.tie_break = TieBreak::ByName;
  p.name = "distributed"; p.description = "distributed proof policy";
  g_fabric.set_policy(p);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = p.id; c.policy_gen = p.gen;
  c.effective_from = Duration(0);
  c.epoch = g_epoch; c.lifecycle = ContractLifecycle::Active;
  c.name = "dist-contract"; c.provenance = "test";
  Objective o;
  o.id = ObjectiveId(kLatencyObjId); o.gen = ObjectiveGen(1);
  o.name = "p99-latency"; o.dim = Dimension::Latency;
  o.kind = ObjectiveKind::LatencyPercentile; o.percentile_rank = 0.99;
  o.comparison = Comparison::AtMost; o.hard = HardSoft::Hard;
  o.target = milliseconds(120);
  o.min_evidence = Count(16);
  o.window_type = WindowType::Sliding;
  o.window_duration = seconds(5);
  o.window_count = Count(64);
  c.objectives.push_back(o);
  Objective o2;
  o2.id = ObjectiveId(kRecoveryObjId); o2.gen = ObjectiveGen(1);
  o2.name = "rto"; o2.dim = Dimension::RecoveryTime;
  o2.kind = ObjectiveKind::RecoveryMaxDuration;
  o2.comparison = Comparison::AtMost; o2.hard = HardSoft::Hard;
  o2.target = seconds(4);
  c.objectives.push_back(o2);
  Objective o3;
  o3.id = ObjectiveId(kAvailObjId); o3.gen = ObjectiveGen(1);
  o3.name = "avail"; o3.dim = Dimension::Availability;
  o3.kind = ObjectiveKind::AvailabilityRequest;
  o3.comparison = Comparison::AtLeast; o3.hard = HardSoft::Hard;
  o3.target = BasisPoints(9990);
  c.objectives.push_back(o3);
  g_fabric.add_contract(c);
  g_fabric.activate_contract(SloContractId(1), now_dur());
}

void ingest_latency(std::uint64_t boot, double latency_ns) {
  EvidenceRecord e;
  e.id = EvidenceId(g_next_evidence++);
  e.gen = EvidenceGen(1);
  e.dimension = Dimension::Latency;
  e.objective_id = ObjectiveId(kLatencyObjId);
  e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(boot);
  e.source_gen = SourceBootGen(1);
  e.epoch = g_epoch;
  e.time = now_dur();
  e.provenance = Provenance::Measured;
  e.value = Duration(static_cast<std::int64_t>(latency_ns));
  g_fabric.ingest(e, e.time);
}

void ingest_availability(std::uint64_t boot, double bp, std::int64_t sample_count) {
  EvidenceRecord e;
  e.id = EvidenceId(g_next_evidence++);
  e.gen = EvidenceGen(1);
  e.dimension = Dimension::Availability;
  e.objective_id = ObjectiveId(kAvailObjId);
  e.objective_gen = ObjectiveGen(1);
  e.source = SourceBootId(boot);
  e.source_gen = SourceBootGen(1);
  e.epoch = g_epoch;
  e.time = now_dur();
  e.provenance = Provenance::Measured;
  e.value = BasisPoints(static_cast<std::int64_t>(bp));
  e.sample_count = Count(sample_count);
  g_fabric.ingest(e, e.time);
}

void monitor_loop() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> lk(g_mu);
    std::int64_t now = now_ms();
    for (auto& kv : g_workers) {
      WorkerInfo* w = kv.second;
      if (!w->alive.load()) continue;
      if (w->name == "A" && !g_down_a.load() && now - w->last_hb_ms > 2000) {
        g_down_a.store(true);
        g_recovery_auth.store(true);
        g_fabric.record_recovery_event(ObjectiveId(kRecoveryObjId), RecoveryEventPhase::Observed, now_dur());
        g_fabric.record_recovery_event(ObjectiveId(kRecoveryObjId), RecoveryEventPhase::Authoritative, now_dur());
        ingest_availability(w->boot_id, 0.0, 1);
      }
    }
  }
}

void handle_client(socket_handle sock) {
  while (true) {
    Frame f;
    Status s = recv_frame(sock, f);
    if (!s.ok()) break;
    if (f.kind == FrameKind::Hello) {
      std::string name; std::uint64_t boot, epoch;
      if (!decode_hello(f.payload, name, boot, epoch)) {
        Frame bad; bad.kind = FrameKind::RegisterAck; bad.payload = encode_ack(0, boot, 99, "malformed hello");
        send_frame(sock, bad); net_close(sock); return;
      }
      if (epoch != g_epoch.raw()) {
        Frame ack; ack.kind = FrameKind::RegisterAck; ack.payload = encode_ack(g_epoch.raw(), boot, 1, "stale epoch");
        send_frame(sock, ack); net_close(sock); return;
      }
      {
        std::lock_guard<std::mutex> lk(g_mu);
        // Boot-id fencing: a boot id already used for this name is stale.
        auto& seen = g_seen_boots[name];
        for (auto b : seen) if (b == boot) {
          Frame ack; ack.kind = FrameKind::RegisterAck; ack.payload = encode_ack(g_epoch.raw(), boot, 2, "stale boot id");
          send_frame(sock, ack); net_close(sock); return;
        }
        seen.push_back(boot);
        WorkerInfo* w = new WorkerInfo{name, boot, now_ms(), true};
        g_workers[sock] = w;
      }
      Frame ack; ack.kind = FrameKind::RegisterAck; ack.payload = encode_ack(g_epoch.raw(), boot, 0, "ok");
      send_frame(sock, ack);
    } else if (f.kind == FrameKind::Evidence) {
      std::uint64_t oid, boot, epoch; std::uint8_t dim; double value; std::int64_t sc, tns;
      if (decode_evidence(f.payload, oid, dim, value, sc, tns, boot, epoch)) {
        if (epoch != g_epoch.raw()) { /* stale */ }
        else if (dim == 0) {
          ingest_latency(boot, value);
          // A serving worker after a failure marks recovery restored + verified (once).
          if (g_recovery_auth.load() && !g_recovery_verified.load() && boot != 1001u) {
            g_fabric.record_recovery_event(ObjectiveId(kRecoveryObjId), RecoveryEventPhase::Restored, now_dur());
            g_fabric.record_recovery_event(ObjectiveId(kRecoveryObjId), RecoveryEventPhase::Verified, now_dur());
            g_recovery_verified.store(true);
          }
        }
        else if (dim == 2) ingest_availability(boot, value, sc);
      }
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_workers.find(sock);
      if (it != g_workers.end()) it->second->last_hb_ms = now_ms();
    } else if (f.kind == FrameKind::Heartbeat) {
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_workers.find(sock);
      if (it != g_workers.end()) it->second->last_hb_ms = now_ms();
    } else if (f.kind == FrameKind::EvaluateRequest) {
      Duration now = now_dur();
      auto res = g_fabric.evaluate(ServiceId(100), WorkloadId(200), now);
      Frame reply; reply.kind = FrameKind::EvaluateReply;
      if (!res.ok()) {
        reply.payload = encode_reply(g_epoch.raw(), 0, 0, 5, 0, 0, 0, 0.0, res.status().message);
      } else {
        const EvaluationResult& er = res.value_unchecked();
        std::string text = compliance_state_name(er.explanation.compliance).data();
        text += ";";
        text += dimension_name(er.explanation.binding_dimension).data();
        double measured = 0.0;
        if (!er.explanation.objectives.empty() && er.explanation.objectives[0].current_value)
          measured = numeric_of(*er.explanation.objectives[0].current_value);
        reply.payload = encode_reply(g_epoch.raw(), er.explanation.contract_gen.raw(),
                                     er.explanation.policy_gen.raw(),
                                     static_cast<std::uint8_t>(er.explanation.compliance),
                                     static_cast<std::uint8_t>(er.explanation.binding_dimension),
                                     static_cast<std::uint8_t>(er.intent.action),
                                     er.explanation.binding_objective.value, measured, text);
      }
      send_frame(sock, reply);
    } else if (f.kind == FrameKind::Kill) {
      if (!g_state_file.empty()) g_fabric.save(g_state_file);
      std::printf("coordinator: SAVED\n"); std::fflush(stdout);
      std::_Exit(0);
    } else if (f.kind == FrameKind::Shutdown) {
      if (!g_state_file.empty()) g_fabric.save(g_state_file);
      break;
    }
  }
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_workers.find(sock);
  if (it != g_workers.end()) { it->second->alive = false; delete it->second; g_workers.erase(it); }
  net_close(sock);
}
}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = kDefaultPort;
  std::string state_file;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
    else if (a == "--state" && i + 1 < argc) state_file = argv[++i];
  }
  g_state_file = state_file;
  net_initialize();
  set_scenario();
  if (!state_file.empty()) {
    Status s = g_fabric.load(state_file);
    if (s.ok()) g_epoch = g_fabric.current_epoch();
    else std::printf("coordinator: load state failed (%s)\n", s.message.c_str());
  }
  Status st;
  socket_handle listener = net_listen("127.0.0.1", port, st);
  if (!st.ok()) { std::printf("coordinator: listen failed: %s\n", st.message.c_str()); return 1; }
  std::printf("coordinator: READY port=%u epoch=%llu\n", port, (unsigned long long)g_epoch.raw());
  std::fflush(stdout);
  std::thread mon(monitor_loop);
  while (true) {
    Status sa;
    socket_handle sock = net_accept(listener, sa);
    if (!sa.ok()) break;
    std::thread(handle_client, sock).detach();
  }
  mon.join();
  net_close(listener);
  net_cleanup();
  return 0;
}
