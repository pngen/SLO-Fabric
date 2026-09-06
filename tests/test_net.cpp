#include "test_framework.hpp"

#include "slofabric/net.hpp"

using namespace slofabric;

TEST("net: frame encode/decode round trip") {
  Frame f;
  f.kind = FrameKind::Evidence;
  f.flags = 3;
  f.payload = {1, 2, 3, 4, 5};
  auto bytes = encode_frame(f);
  auto d = decode_frame(bytes);
  REQUIRE(d.ok());
  CHECK(d.value_unchecked().kind == FrameKind::Evidence);
  CHECK_EQ(d.value_unchecked().flags, 3u);
  CHECK_EQ(d.value_unchecked().payload.size(), std::size_t(5));
  CHECK_EQ(d.value_unchecked().payload[4], 5u);
}

TEST("net: malformed frame rejected (bad magic)") {
  Frame f; f.kind = FrameKind::Hello; f.payload = {0xAA};
  auto bytes = encode_frame(f);
  bytes[0] ^= 0xFF;
  auto d = decode_frame(bytes);
  CHECK(!d.ok());
  CHECK(d.status().code == StatusCode::ProtocolError);
}

TEST("net: corrupted frame rejected (crc mismatch)") {
  Frame f; f.kind = FrameKind::Heartbeat; f.payload = {0x11, 0x22, 0x33};
  auto bytes = encode_frame(f);
  bytes[6] ^= 0xFF;  // flip a payload byte
  auto d = decode_frame(bytes);
  CHECK(!d.ok());
}

TEST("net: truncated frame rejected") {
  Frame f; f.kind = FrameKind::Kill; f.payload = {0x01, 0x02, 0x03, 0x04};
  auto bytes = encode_frame(f);
  auto d = decode_frame(bytes.data(), bytes.size() - 3);
  CHECK(!d.ok());
}

TEST("net: trailing garbage rejected") {
  Frame f; f.kind = FrameKind::Evidence; f.payload = {0x42};
  auto bytes = encode_frame(f);
  std::vector<std::uint8_t> ext = bytes;
  ext.push_back(0x00);
  auto d = decode_frame(ext);
  CHECK(!d.ok());
}

TEST("net: oversized frame rejected") {
  // Header claims a huge length (provide >= 14 bytes so header parses).
  std::uint8_t hdr[14] = {};
  hdr[0] = 0x31; hdr[1] = 0x46; hdr[2] = 0x4C; hdr[3] = 0x53;  // magic LE "SLF1"
  hdr[4] = 2; hdr[5] = 0;
  std::uint32_t big = 0x7FFFFFFF;
  for (int i = 0; i < 4; ++i) hdr[6 + i] = static_cast<std::uint8_t>((big >> (8 * i)) & 0xFFu);
  auto d = decode_frame(hdr, 14);
  CHECK(!d.ok());
  CHECK(d.status().code == StatusCode::ProtocolError);
}

TEST("net: loopback send/recv round trip") {
  REQUIRE(net_initialize());
  Status s2;
  socket_handle listener = net_listen("127.0.0.1", 45999, s2);
  REQUIRE(s2.ok());
  socket_handle cli = net_connect("127.0.0.1", 45999, s2);
  REQUIRE(s2.ok());
  Status sa; socket_handle sr = net_accept(listener, sa);
  REQUIRE(sa.ok());

  Frame out; out.kind = FrameKind::Enforcement; out.flags = 7;
  out.payload = {0xDE, 0xAD, 0xBE, 0xEF};
  REQUIRE(send_frame(cli, out).ok());
  Frame in;
  REQUIRE(recv_frame(sr, in).ok());
  CHECK(in.kind == FrameKind::Enforcement);
  CHECK_EQ(in.flags, 7u);
  CHECK_EQ(in.payload.size(), std::size_t(4));
  CHECK_EQ(in.payload[3], 0xEFu);

  net_close(cli); net_close(sr); net_close(listener);
  net_cleanup();
}
