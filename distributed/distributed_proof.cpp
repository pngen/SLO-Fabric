// Distributed proof orchestrator. Spawns the coordinator and worker as real
// OS processes over framed TCP, kills worker A as a real process, verifies
// failover/recovery, stale boot-id and stale epoch rejection, and coordinator
// restart with epoch advance.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "slofabric/net.hpp"

#include "distributed_protocol.hpp"
#include "process.hpp"

using namespace slofabric;
using namespace slofabric_proof;

static int g_fail = 0;
static void chk(bool c, const char* m) {
  std::printf("%s %s\n", c ? "[PASS]" : "[FAIL]", m);
  if (!c) ++g_fail;
}

struct Eval {
  std::uint64_t epoch = 0, cgen = 0, pgen = 0, oid = 0;
  std::uint8_t compliance = 0, binding = 0, action = 0;
  double measured = 0.0;
  std::string text;
};

static socket_handle ctrl_connect(std::uint16_t port) {
  for (int i = 0; i < 80; ++i) {
    Status s;
    socket_handle c = net_connect("127.0.0.1", port, s);
    if (s.ok()) return c;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return kInvalidSocket;
}

static bool ctrl_eval(socket_handle c, Eval& out) {
  Frame req; req.kind = FrameKind::EvaluateRequest;
  if (!send_frame(c, req).ok()) return false;
  Frame rep;
  if (!recv_frame(c, rep).ok()) return false;
  if (rep.kind != FrameKind::EvaluateReply) return false;
  return decode_reply(rep.payload, out.epoch, out.cgen, out.pgen, out.compliance,
                      out.binding, out.action, out.oid, out.measured, out.text);
}

static bool poll(socket_handle c, int max, bool (*pred)(const Eval&), Eval& out) {
  for (int i = 0; i < max; ++i) {
    if (ctrl_eval(c, out) && pred(out)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

static bool is_compliant(const Eval& e) { return e.compliance == 0u; }
static bool is_failure_transition(const Eval& e) {
  // After worker A death: latency evidence aged out -> revalidation/insufficient,
  // or recovery in-flight/violating, or availability down.
  return e.compliance == 3u /*violating*/ || e.compliance == 6u /*revalidation*/ ||
         e.compliance == 4u /*recovering*/ || e.compliance == 5u /*insufficient*/;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::printf("usage: distributed_proof <coordinator_exe> <worker_exe> [port]\n"); return 2; }
  std::string coord_exe = argv[1];
  std::string worker_exe = argv[2];
  std::uint16_t port = argc >= 4 ? static_cast<std::uint16_t>(std::stoi(argv[3])) : 45701;
  std::string state_file = std::string("coord_state_") + std::to_string(port) + ".bin";

  net_initialize();

  // ---- Phase 1: start coordinator ----
  Process coord = spawn(coord_exe, {"--port", std::to_string(port), "--state", state_file});
  chk(coord.valid(), "coordinator started");
  socket_handle ctl = ctrl_connect(port);
  chk(ctl != kInvalidSocket, "coordinator accept connections");

  // ---- Phase 2: worker A serves -> compliant ----
  Process workerA = spawn(worker_exe, {"--port", std::to_string(port), "--name", "A",
                                       "--boot", "1001", "--epoch", "1", "--latency", "50000000", "--mode", "serve"});
  chk(workerA.valid(), "worker A started");
  Eval ev;
  bool ok = poll(ctl, 120, is_compliant, ev);
  chk(ok, "worker A serving yields compliant");
  bool a_epoch_ok = poll(ctl, 5, [](const Eval& e){ return e.text.find("latency") != std::string::npos; }, ev);
  (void)a_epoch_ok;

  // ---- Phase 3: kill worker A (real OS process) ----
  kill(workerA);
  std::printf("worker A killed (pid)\n");
  bool failed = poll(ctl, 120, is_failure_transition, ev);
  chk(failed, "worker A death produces a failure transition (revalidation/insufficient/recovering)");

  // ---- Phase 4: worker B fails over -> recovery ----
  Process workerB = spawn(worker_exe, {"--port", std::to_string(port), "--name", "B",
                                       "--boot", "1002", "--epoch", "1", "--latency", "60000000", "--mode", "serve"});
  chk(workerB.valid(), "worker B started");
  bool recovered = poll(ctl, 120, is_compliant, ev);
  chk(recovered, "worker B failover restores compliance");

  // ---- Phase 5: stale boot id rejected ----
  Process stale = spawn(worker_exe, {"--port", std::to_string(port), "--name", "A",
                                     "--boot", "1001", "--epoch", "1", "--latency", "50000000", "--mode", "serve"});
  bool sex = false; int scode = 0;
  wait_exit(stale, 4000, sex, scode);
  chk(sex && scode == 2, "stale boot id (reused 1001) rejected; worker exits with error");

  // ---- Phase 6: coordinator restart -> epoch advances ----
  // Send Kill frame -> coordinator saves state then exits.
  Frame killf; killf.kind = FrameKind::Kill;
  send_frame(ctl, killf);
  bool cexit = false; int ccode = 0;
  wait_exit(coord, 6000, cexit, ccode);
  chk(cexit, "coordinator stopped");
  net_close(ctl);

  Process coord2 = spawn(coord_exe, {"--port", std::to_string(port), "--state", state_file});
  chk(coord2.valid(), "coordinator restarted");
  socket_handle ctl2 = ctrl_connect(port);
  chk(ctl2 != kInvalidSocket, "restarted coordinator accepts connections");
  Eval e2;
  if (ctrl_eval(ctl2, e2)) chk(e2.epoch == 2, "coordinator epoch advanced to 2 after restart");
  else chk(false, "restart eval reply");

  // Old-epoch worker (epoch 1) must be rejected.
  Process oldw = spawn(worker_exe, {"--port", std::to_string(port), "--name", "A",
                                    "--boot", "2001", "--epoch", "1", "--latency", "50000000", "--mode", "serve"});
  bool owx = false; int owc = 0;
  wait_exit(oldw, 4000, owx, owc);
  chk(owx && owc == 2, "stale epoch (1 after restart) worker rejected");

  // New-epoch worker accepted.
  Process neww = spawn(worker_exe, {"--port", std::to_string(port), "--name", "A",
                                    "--boot", "2002", "--epoch", "2", "--latency", "50000000", "--mode", "serve"});
  bool nwx = false; int nwc = 0;
  wait_exit(neww, 1500, nwx, nwc);
  chk(!nwx, "new-epoch worker accepted (still running)");

  // Cleanup.
  kill(workerB); kill(neww); kill(coord2);
  net_close(ctl2);
  net_cleanup();
  std::remove(state_file.c_str());

  std::printf("\n%s\n", g_fail == 0 ? "DISTRIBUTED PROOF: ALL PASS" : "DISTRIBUTED PROOF: FAILURES");
  return g_fail == 0 ? 0 : 1;
}
