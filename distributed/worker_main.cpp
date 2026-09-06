#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>

#include "slofabric/net.hpp"

#include "distributed_protocol.hpp"

using namespace slofabric;
using namespace slofabric_proof;

static std::uint64_t fresh_boot_id() {
  std::random_device rd;
  auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
  return (static_cast<std::uint64_t>(t) ^ r) | 1u;
}

int main(int argc, char** argv) {
  std::uint16_t port = kDefaultPort;
  std::string name = "A";
  double latency_ns = 50000000.0;   // 50 ms
  bool serve = true;
  std::uint64_t forced_boot = 0;
  std::uint64_t epoch = 1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
    else if (a == "--name" && i + 1 < argc) name = argv[++i];
    else if (a == "--latency" && i + 1 < argc) latency_ns = std::stod(argv[++i]);
    else if (a == "--mode" && i + 1 < argc) serve = (std::string(argv[++i]) == "serve");
    else if (a == "--boot" && i + 1 < argc) forced_boot = std::stoull(argv[++i]);
    else if (a == "--epoch" && i + 1 < argc) epoch = std::stoull(argv[++i]);
  }
  if (forced_boot == 0) forced_boot = fresh_boot_id();

  net_initialize();
  Status st;
  socket_handle sock = net_connect("127.0.0.1", port, st);
  if (!st.ok()) { std::printf("worker: connect failed: %s\n", st.message.c_str()); return 1; }
  Frame hello; hello.kind = FrameKind::Hello;
  hello.payload = encode_hello(name, forced_boot, epoch);
  if (!send_frame(sock, hello).ok()) return 1;
  Frame ack;
  if (!recv_frame(sock, ack).ok()) return 1;
  std::uint64_t ack_epoch, granted_boot; int ack_code; std::string ack_msg;
  decode_ack(ack.payload, ack_epoch, granted_boot, ack_code, ack_msg);
  std::printf("worker: registered name=%s boot=%llu code=%d msg=%s\n", name.c_str(),
              (unsigned long long)forced_boot, ack_code, ack_msg.c_str());
  std::fflush(stdout);
  if (ack_code != 0) { net_close(sock); net_cleanup(); return 2; }

  while (true) {
    if (serve) {
      Frame ev; ev.kind = FrameKind::Evidence;
      ev.payload = encode_evidence(1, 0, latency_ns, 1, 0, forced_boot, epoch);
      send_frame(sock, ev);
      Frame av; av.kind = FrameKind::Evidence;
      av.payload = encode_evidence(3, 2, 10000.0, 1, 0, forced_boot, epoch);
      send_frame(sock, av);
    }
    Frame hb; hb.kind = FrameKind::Heartbeat;
    send_frame(sock, hb);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  net_close(sock);
  net_cleanup();
  return 0;
}
