#include "slofabric/net.hpp"

#include <cstring>

#include "slofabric/binary.hpp"
#include "slofabric/crc32.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace slofabric {

namespace {
bool g_net_init = false;

bool recv_exact(socket_handle sock, std::uint8_t* buf, std::size_t n) {
  std::size_t got = 0;
  while (got < n) {
    int r = 0;
#  if defined(_WIN32)
    r = ::recv(static_cast<SOCKET>(sock), reinterpret_cast<char*>(buf + got), static_cast<int>(n - got), 0);
#  else
    r = ::recv(sock, buf + got, n - got, 0);
#  endif
    if (r == 0) return false;
    if (r < 0) return false;
    got += static_cast<std::size_t>(r);
  }
  return true;
}

bool send_exact(socket_handle sock, const std::uint8_t* buf, std::size_t n) {
  std::size_t sent = 0;
  while (sent < n) {
    int r = 0;
#  if defined(_WIN32)
    r = ::send(static_cast<SOCKET>(sock), reinterpret_cast<const char*>(buf + sent), static_cast<int>(n - sent), 0);
#  else
    r = ::send(sock, buf + sent, n - sent, 0);
#  endif
    if (r <= 0) return false;
    sent += static_cast<std::size_t>(r);
  }
  return true;
}

void append_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFFu));
}
}  // namespace

std::vector<std::uint8_t> encode_frame(const Frame& f) {
  std::vector<std::uint8_t> out;
  append_u32(out, kFrameMagic);
  out.push_back(static_cast<std::uint8_t>(f.kind));
  out.push_back(f.flags);
  std::uint32_t len = static_cast<std::uint32_t>(f.payload.size());
  append_u32(out, len);
  out.insert(out.end(), f.payload.begin(), f.payload.end());
  std::uint32_t crc = crc32(out.data(), out.size());
  append_u32(out, crc);
  return out;
}

Result<Frame> decode_frame(const std::uint8_t* data, std::size_t n) {
  if (!data || n < 14) return persistence_corrupt("frame too short");
  std::uint32_t magic = 0;
  {
    ByteReader r(data, n);
    if (!r.read_u32(magic) || magic != kFrameMagic) return protocol_error("bad frame magic");
  }
  Frame f;
  f.kind = static_cast<FrameKind>(data[4]);
  f.flags = data[5];
  std::uint32_t len = 0;
  len = static_cast<std::uint32_t>(data[6]) | (static_cast<std::uint32_t>(data[7]) << 8) |
        (static_cast<std::uint32_t>(data[8]) << 16) | (static_cast<std::uint32_t>(data[9]) << 24);
  if (len > kMaxFramePayload) return protocol_error("oversized frame");
  std::size_t total = 10 + static_cast<std::size_t>(len) + 4;
  if (n < total) return protocol_error("truncated frame");
  if (n > total) return protocol_error("trailing garbage");
  std::uint32_t stored = 0;
  const std::uint8_t* crcb = data + 10 + len;
  stored = static_cast<std::uint32_t>(crcb[0]) | (static_cast<std::uint32_t>(crcb[1]) << 8) |
           (static_cast<std::uint32_t>(crcb[2]) << 16) | (static_cast<std::uint32_t>(crcb[3]) << 24);
  std::uint32_t computed = crc32(data, 10 + len);
  if (computed != stored) return protocol_error("frame checksum mismatch");
  f.payload.assign(data + 10, data + 10 + len);
  return f;
}

Status send_frame(socket_handle sock, const Frame& f) {
  std::vector<std::uint8_t> bytes = encode_frame(f);
  if (bytes.size() > kMaxFramePayload + 14) return protocol_error("oversized frame to send");
  return send_exact(sock, bytes.data(), bytes.size()) ? ok() : protocol_error("send failed");
}

Status recv_frame(socket_handle sock, Frame& out, std::size_t max_payload) {
  std::uint8_t hdr[10];
  if (!recv_exact(sock, hdr, 10)) return protocol_error("recv header failed");
  std::uint32_t magic = static_cast<std::uint32_t>(hdr[0]) | (static_cast<std::uint32_t>(hdr[1]) << 8) |
                        (static_cast<std::uint32_t>(hdr[2]) << 16) | (static_cast<std::uint32_t>(hdr[3]) << 24);
  if (magic != kFrameMagic) return protocol_error("bad frame magic");
  out.kind = static_cast<FrameKind>(hdr[4]);
  out.flags = hdr[5];
  std::uint32_t len = static_cast<std::uint32_t>(hdr[6]) | (static_cast<std::uint32_t>(hdr[7]) << 8) |
                      (static_cast<std::uint32_t>(hdr[8]) << 16) | (static_cast<std::uint32_t>(hdr[9]) << 24);
  if (len > std::min<std::uint32_t>(static_cast<std::uint32_t>(max_payload), static_cast<std::uint32_t>(kMaxFramePayload)))
    return protocol_error("oversized frame");
  out.payload.resize(len);
  if (!recv_exact(sock, out.payload.data(), len)) return protocol_error("recv payload failed");
  std::uint8_t crcb[4];
  if (!recv_exact(sock, crcb, 4)) return protocol_error("recv checksum failed");
  std::uint32_t stored = static_cast<std::uint32_t>(crcb[0]) | (static_cast<std::uint32_t>(crcb[1]) << 8) |
                         (static_cast<std::uint32_t>(crcb[2]) << 16) | (static_cast<std::uint32_t>(crcb[3]) << 24);
  std::vector<std::uint8_t> body(hdr, hdr + 10);
  body.insert(body.end(), out.payload.begin(), out.payload.end());
  std::uint32_t computed = crc32(body.data(), body.size());
  if (computed != stored) return protocol_error("frame checksum mismatch");
  return ok();
}

bool net_initialize() {
#if defined(_WIN32)
  if (g_net_init) return true;
  WSADATA d;
  if (::WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
  g_net_init = true;
  return true;
#else
  g_net_init = true;
  return true;
#endif
}

void net_cleanup() {
#if defined(_WIN32)
  if (g_net_init) { ::WSACleanup(); g_net_init = false; }
#endif
}

socket_handle net_connect(const std::string& host, std::uint16_t port, Status& out) {
  net_initialize();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* res = nullptr;
  std::string pstr = std::to_string(port);
#if defined(_WIN32)
  int rc = ::getaddrinfo(host.c_str(), pstr.c_str(), &hints, &res);
#else
  int rc = ::getaddrinfo(host.c_str(), pstr.c_str(), &hints, &res);
#endif
  if (rc != 0) { out = protocol_error("getaddrinfo failed"); return kInvalidSocket; }
  socket_handle sock = kInvalidSocket;
  for (addrinfo* rp = res; rp; rp = rp->ai_next) {
#if defined(_WIN32)
    SOCKET s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    if (::connect(s, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) { sock = static_cast<socket_handle>(s); break; }
    ::closesocket(s);
#else
    int s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s < 0) continue;
    if (::connect(s, rp->ai_addr, rp->ai_addrlen) == 0) { sock = s; break; }
    ::close(s);
#endif
  }
  ::freeaddrinfo(res);
  if (sock == kInvalidSocket) { out = protocol_error("connect failed"); return kInvalidSocket; }
  out = ok();
  return sock;
}

socket_handle net_listen(const std::string& host, std::uint16_t port, Status& out) {
  net_initialize();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* res = nullptr;
  std::string pstr = std::to_string(port);
  if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), pstr.c_str(), &hints, &res) != 0) {
    out = protocol_error("getaddrinfo failed");
    return kInvalidSocket;
  }
  socket_handle sock = kInvalidSocket;
  for (addrinfo* rp = res; rp; rp = rp->ai_next) {
#if defined(_WIN32)
    SOCKET s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s == INVALID_SOCKET) continue;
    BOOL one = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    if (::bind(s, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0 && ::listen(s, 32) == 0) {
      sock = static_cast<socket_handle>(s); break;
    }
    ::closesocket(s);
#else
    int s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (s < 0) continue;
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(s, rp->ai_addr, rp->ai_addrlen) == 0 && ::listen(s, 32) == 0) { sock = s; break; }
    ::close(s);
#endif
  }
  ::freeaddrinfo(res);
  if (sock == kInvalidSocket) { out = protocol_error("bind/listen failed"); return kInvalidSocket; }
  out = ok();
  return sock;
}

socket_handle net_accept(socket_handle listener, Status& out) {
#if defined(_WIN32)
  SOCKET s = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
  if (s == INVALID_SOCKET) { out = protocol_error("accept failed"); return kInvalidSocket; }
  out = ok();
  return static_cast<socket_handle>(s);
#else
  int s = ::accept(listener, nullptr, nullptr);
  if (s < 0) { out = protocol_error("accept failed"); return kInvalidSocket; }
  out = ok();
  return s;
#endif
}

void net_close(socket_handle sock) {
#if defined(_WIN32)
  if (sock != kInvalidSocket) ::closesocket(static_cast<SOCKET>(sock));
#else
  if (sock != kInvalidSocket) ::close(sock);
#endif
}

std::string net_last_error() {
#if defined(_WIN32)
  int e = ::WSAGetLastError();
  return std::string("winsock error ") + std::to_string(e);
#else
  return std::string("socket errno ") + std::to_string(errno);
#endif
}

}  // namespace slofabric
