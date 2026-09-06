#pragma once

// SLO Fabric - canonical binary encoding for persistence.
//
// Deterministic little-endian encoding with bounded reads. Every read is
// bounds-checked and rejects out-of-range lengths before allocation. Used by
// the integrity-checked persistence layer.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace slofabric {

class ByteWriter {
 public:
  void write_u8(std::uint8_t v) { buf_.push_back(v); }
  void write_bool(bool v) { write_u8(v ? 1 : 0); }
  void write_u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v));
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void write_u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
  void write_i32(std::int32_t v) { write_u32(static_cast<std::uint32_t>(v)); }
  void write_u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
  }
  void write_i64(std::int64_t v) { write_u64(static_cast<std::uint64_t>(v)); }

  void write_string(const std::string& s) {
    write_u64(static_cast<std::uint64_t>(s.size()));
    for (char c : s) buf_.push_back(static_cast<std::uint8_t>(c));
  }

  void write_bytes(const std::uint8_t* data, std::size_t n) {
    buf_.insert(buf_.end(), data, data + n);
  }
  void write_raw(const std::vector<std::uint8_t>& b) {
    write_u64(static_cast<std::uint64_t>(b.size()));
    write_bytes(b.data(), b.size());
  }

  template <typename T>
  void write_pod(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    write_bytes(reinterpret_cast<const std::uint8_t*>(&v), sizeof(T));
  }

  const std::vector<std::uint8_t>& data() const noexcept { return buf_; }
  std::vector<std::uint8_t> take() { auto d = std::move(buf_); buf_.clear(); return d; }
  std::size_t size() const noexcept { return buf_.size(); }
  void reset() { buf_.clear(); }

 private:
  std::vector<std::uint8_t> buf_;
};

class ByteReader {
 public:
  ByteReader() = default;
  explicit ByteReader(const std::uint8_t* data, std::size_t n) : data_(data), size_(n) {}
  static ByteReader from(const std::vector<std::uint8_t>& v) { return ByteReader(v.data(), v.size()); }

  bool read_u8(std::uint8_t& out) {
    if (!need(1)) return false;
    out = data_[pos_++];
    return true;
  }
  bool read_bool(bool& out) {
    std::uint8_t v;
    if (!read_u8(v)) return false;
    out = (v != 0);
    return true;
  }
  bool read_u16(std::uint16_t& out) {
    std::uint8_t b[2];
    if (!read_bytes(b, 2)) return false;
    out = static_cast<std::uint16_t>(b[0]) | (static_cast<std::uint16_t>(b[1]) << 8);
    return true;
  }
  bool read_u32(std::uint32_t& out) {
    std::uint8_t b[4];
    if (!read_bytes(b, 4)) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) out |= static_cast<std::uint32_t>(b[i]) << (8 * i);
    return true;
  }
  bool read_i32(std::int32_t& out) {
    std::uint32_t u;
    if (!read_u32(u)) return false;
    out = static_cast<std::int32_t>(u);
    return true;
  }
  bool read_u64(std::uint64_t& out) {
    std::uint8_t b[8];
    if (!read_bytes(b, 8)) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<std::uint64_t>(b[i]) << (8 * i);
    return true;
  }
  bool read_i64(std::int64_t& out) {
    std::uint64_t u;
    if (!read_u64(u)) return false;
    out = static_cast<std::int64_t>(u);
    return true;
  }
  // Reads a length-prefixed string, bounded to max_len before allocating.
  bool read_string(std::string& out, std::uint64_t max_len = 1 << 20) {
    std::uint64_t len;
    if (!read_u64(len)) return false;
    if (len > max_len) return false;
    if (!need(len)) return false;
    out.assign(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return true;
  }
  bool read_raw(std::vector<std::uint8_t>& out, std::uint64_t max_len = 1 << 20) {
    std::uint64_t len;
    if (!read_u64(len)) return false;
    if (len > max_len) return false;
    if (!need(len)) return false;
    out.assign(data_ + pos_, data_ + pos_ + len);
    pos_ += len;
    return true;
  }
  template <typename T>
  bool read_pod(T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
    if (!need(sizeof(T))) return false;
    std::memcpy(&v, data_ + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  bool read_bytes(std::uint8_t* out, std::size_t n) {
    if (!need(n)) return false;
    std::memcpy(out, data_ + pos_, n);
    pos_ += n;
    return true;
  }
  bool need(std::size_t n) const { return pos_ + n <= size_ && pos_ <= size_ && n <= size_ - pos_; }
  std::size_t remaining() const { return size_ - pos_; }
  std::size_t position() const { return pos_; }
  bool exhausted() const { return pos_ == size_; }
  const std::uint8_t* data() const { return data_; }
  std::size_t size() const { return size_; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
};

}  // namespace slofabric
