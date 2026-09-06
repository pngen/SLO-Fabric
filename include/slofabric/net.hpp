#pragma once

// SLO Fabric - bounded framed TCP protocol.
//
// Frames are checksummed, length-bounded, and validated before any allocation.
// Partial reads/writes are handled; oversized, truncated, corrupt, and
// malformed frames are rejected. Used by the distributed proof (coordinator and
// workers) and available to adjacent runtimes as a narrow typed transport.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "slofabric/status.hpp"

namespace slofabric {

inline constexpr std::uint32_t kFrameMagic = 0x534C4631u;
inline constexpr std::size_t kMaxFramePayload = 4 * 1024 * 1024;

enum class FrameKind : std::uint8_t {
  Hello = 0,
  RegisterAck = 1,
  Evidence = 2,
  Heartbeat = 3,
  EvaluateRequest = 4,
  EvaluateReply = 5,
  Enforcement = 6,
  EnforcementAck = 7,
  Kill = 8,
  Shutdown = 9,
  Unknown = 255,
};

constexpr std::string_view frame_kind_name(FrameKind k) noexcept {
  using F = FrameKind;
  switch (k) {
    case F::Hello: return "hello";
    case F::RegisterAck: return "register_ack";
    case F::Evidence: return "evidence";
    case F::Heartbeat: return "heartbeat";
    case F::EvaluateRequest: return "evaluate_request";
    case F::EvaluateReply: return "evaluate_reply";
    case F::Enforcement: return "enforcement";
    case F::EnforcementAck: return "enforcement_ack";
    case F::Kill: return "kill";
    case F::Shutdown: return "shutdown";
    case F::Unknown: return "unknown";
  }
  return "unknown";
}

struct Frame {
  FrameKind kind = FrameKind::Unknown;
  std::uint8_t flags = 0;
  std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encode_frame(const Frame& f);
Result<Frame> decode_frame(const std::uint8_t* data, std::size_t n);
inline Result<Frame> decode_frame(const std::vector<std::uint8_t>& v) {
  return decode_frame(v.data(), v.size());
}

#if defined(_WIN32)
using socket_handle = std::uintptr_t;
constexpr socket_handle kInvalidSocket = static_cast<socket_handle>(~socket_handle(0));
#else
using socket_handle = int;
constexpr socket_handle kInvalidSocket = -1;
#endif

Status send_frame(socket_handle sock, const Frame& f);
Status recv_frame(socket_handle sock, Frame& out, std::size_t max_payload = kMaxFramePayload);

bool net_initialize();
void net_cleanup();
socket_handle net_connect(const std::string& host, std::uint16_t port, Status& out);
socket_handle net_listen(const std::string& host, std::uint16_t port, Status& out);
socket_handle net_accept(socket_handle listener, Status& out);
void net_close(socket_handle sock);
std::string net_last_error();

}  // namespace slofabric
