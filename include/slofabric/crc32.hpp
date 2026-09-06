#pragma once

// SLO Fabric - deterministic CRC-32 (IEEE 802.3) checksum.
//
// Used by the integrity-checked persistence layer to detect corruption,
// truncation, and trailing garbage. Deterministic across platforms.

#include <cstddef>
#include <cstdint>

namespace slofabric {

namespace detail {

inline const std::uint32_t* crc32_table() {
  static std::uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    init = true;
  }
  return table;
}

}  // namespace detail

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t len,
                           std::uint32_t seed = 0xFFFFFFFFu) {
  std::uint32_t c = seed;
  const auto* table = detail::crc32_table();
  for (std::size_t i = 0; i < len; ++i) {
    c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

inline std::uint32_t crc32_bytes(const std::uint8_t* data, std::size_t len) {
  return crc32(data, len);
}

}  // namespace slofabric
