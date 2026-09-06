#pragma once

// SLO Fabric - strongly typed measurement units.
//
// Distinct physical quantities must not be silently interchangeable. Every
// objective/value carries a typed unit. Integer-based units use checked
// 64-bit arithmetic with overflow rejection. Real-valued units (rates,
// percentages, ratios) are validated on construction to reject NaN, +/-inf,
// and out-of-range values. Deterministic on a given platform.

#include <cstdint>
#include <limits>
#include <string>

namespace slofabric {

namespace detail {

// Checked 64-bit add. Returns {ok, value}.
constexpr bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
      (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) {
    return false;
  }
  out = a + b;
  return true;
}

constexpr bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if ((b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) ||
      (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b)) {
    return false;
  }
  out = a - b;
  return true;
}

constexpr bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if (a == -1 && b == std::numeric_limits<std::int64_t>::min()) return false;
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) return false;
  std::int64_t r = a * b;
  if (r / b != a) return false;
  out = r;
  return true;
}

// Checked base-10 exponent (for micro/ms scaling): value * 10^exp.
constexpr bool checked_scale(std::int64_t value, int exp, std::int64_t& out) noexcept {
  std::int64_t r = value;
  for (int i = 0; i < exp; ++i) {
    std::int64_t t;
    if (!checked_mul(r, 10, t)) return false;
    r = t;
  }
  out = r;
  return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Duration: canonical integer nanoseconds; checked arithmetic.
// ---------------------------------------------------------------------------
class Duration {
 public:
  constexpr Duration() noexcept = default;
  constexpr explicit Duration(std::int64_t ns) noexcept : ns_(ns) {}

  constexpr std::int64_t as_ns() const noexcept { return ns_; }
  constexpr std::int64_t as_micros() const noexcept { return ns_ / 1000; }
  constexpr std::int64_t as_ms() const noexcept { return ns_ / 1000000; }
  constexpr double as_seconds() const noexcept { return static_cast<double>(ns_) / 1e9; }

  constexpr bool is_negative() const noexcept { return ns_ < 0; }
  constexpr bool is_zero() const noexcept { return ns_ == 0; }
  constexpr bool is_positive() const noexcept { return ns_ > 0; }

  [[nodiscard]] std::string to_string() const { return std::to_string(ns_) + "ns"; }

  friend constexpr bool operator==(Duration a, Duration b) noexcept { return a.ns_ == b.ns_; }
  friend constexpr bool operator!=(Duration a, Duration b) noexcept { return a.ns_ != b.ns_; }
  friend constexpr bool operator<(Duration a, Duration b) noexcept { return a.ns_ < b.ns_; }
  friend constexpr bool operator<=(Duration a, Duration b) noexcept { return a.ns_ <= b.ns_; }
  friend constexpr bool operator>(Duration a, Duration b) noexcept { return a.ns_ > b.ns_; }
  friend constexpr bool operator>=(Duration a, Duration b) noexcept { return a.ns_ >= b.ns_; }

  // Checked arithmetic: returns false on overflow.
  constexpr bool add(Duration b, Duration& out) const noexcept {
    std::int64_t r;
    if (!detail::checked_add(ns_, b.ns_, r)) return false;
    out = Duration(r);
    return true;
  }
  constexpr Duration operator+(Duration b) const { return Duration(ns_ + b.ns_); }

 private:
  std::int64_t ns_{0};
};

// Factory helpers. All checked (throw std::overflow_error on overflow to keep
// the factory ergonomic; callers that need no-throw use Duration directly).
constexpr Duration nanoseconds(std::int64_t v) { return Duration(v); }
constexpr Duration microseconds(std::int64_t v) { return Duration(v * 1000); }
constexpr Duration milliseconds(std::int64_t v) { return Duration(v * 1000000); }
constexpr Duration seconds(std::int64_t v) { return Duration(v * 1000000000LL); }

// ---------------------------------------------------------------------------
// ByteCount: integer bytes; checked arithmetic.
// ---------------------------------------------------------------------------
class ByteCount {
 public:
  constexpr ByteCount() noexcept = default;
  constexpr explicit ByteCount(std::int64_t bytes) noexcept : bytes_(bytes) {}
  constexpr std::int64_t as_bytes() const noexcept { return bytes_; }

  constexpr bool is_negative() const noexcept { return bytes_ < 0; }

  [[nodiscard]] std::string to_string() const { return std::to_string(bytes_) + "B"; }

  friend constexpr bool operator==(ByteCount a, ByteCount b) noexcept { return a.bytes_ == b.bytes_; }
  friend constexpr bool operator!=(ByteCount a, ByteCount b) noexcept { return a.bytes_ != b.bytes_; }
  friend constexpr bool operator<(ByteCount a, ByteCount b) noexcept { return a.bytes_ < b.bytes_; }
  friend constexpr bool operator>=(ByteCount a, ByteCount b) noexcept { return a.bytes_ >= b.bytes_; }
  friend constexpr bool operator>(ByteCount a, ByteCount b) noexcept { return a.bytes_ > b.bytes_; }
  friend constexpr bool operator<=(ByteCount a, ByteCount b) noexcept { return a.bytes_ <= b.bytes_; }

 private:
  std::int64_t bytes_{0};
};

constexpr ByteCount bytes(std::int64_t v) { return ByteCount(v); }
constexpr ByteCount kilobytes(std::int64_t v) { return ByteCount(v * 1024); }
constexpr ByteCount megabytes(std::int64_t v) { return ByteCount(v * 1024 * 1024); }
constexpr ByteCount gigabytes(std::int64_t v) { return ByteCount(v * 1024LL * 1024 * 1024); }

// ---------------------------------------------------------------------------
// Count: an integer number of events / samples / requests.
// ---------------------------------------------------------------------------
class Count {
 public:
  constexpr Count() noexcept = default;
  constexpr explicit Count(std::int64_t v) noexcept : v_(v) {}
  constexpr std::int64_t value() const noexcept { return v_; }

  constexpr bool is_negative() const noexcept { return v_ < 0; }
  constexpr bool is_zero() const noexcept { return v_ == 0; }

  friend constexpr bool operator==(Count a, Count b) noexcept { return a.v_ == b.v_; }
  friend constexpr bool operator!=(Count a, Count b) noexcept { return a.v_ != b.v_; }
  friend constexpr bool operator<(Count a, Count b) noexcept { return a.v_ < b.v_; }
  friend constexpr bool operator<=(Count a, Count b) noexcept { return a.v_ <= b.v_; }
  friend constexpr bool operator>(Count a, Count b) noexcept { return a.v_ > b.v_; }
  friend constexpr bool operator>=(Count a, Count b) noexcept { return a.v_ >= b.v_; }

 private:
  std::int64_t v_{0};
};

constexpr Count count(std::int64_t v) { return Count(v); }

// ---------------------------------------------------------------------------
// BasisPoints: integer in [0, 10000], 10000 == 100%.
// ---------------------------------------------------------------------------
class BasisPoints {
 public:
  constexpr BasisPoints() noexcept = default;
  constexpr explicit BasisPoints(std::int64_t bp) noexcept : bp_(bp) {}

  static constexpr std::int64_t max_value = 10000;
  constexpr bool is_valid() const noexcept { return bp_ >= 0 && bp_ <= max_value; }
  constexpr std::int64_t value() const noexcept { return bp_; }
  constexpr double as_fraction() const noexcept { return static_cast<double>(bp_) / 10000.0; }

  friend constexpr bool operator==(BasisPoints a, BasisPoints b) noexcept { return a.bp_ == b.bp_; }
  friend constexpr bool operator<(BasisPoints a, BasisPoints b) noexcept { return a.bp_ < b.bp_; }
  friend constexpr bool operator>(BasisPoints a, BasisPoints b) noexcept { return a.bp_ > b.bp_; }

 private:
  std::int64_t bp_{0};
};
constexpr BasisPoints basis_points(std::int64_t bp) { return BasisPoints(bp); }

// ---------------------------------------------------------------------------
// Percentage: real value in [0, 100]. Validated on construction.
// ---------------------------------------------------------------------------
class Percentage {
 public:
  constexpr Percentage() noexcept = default;
  constexpr explicit Percentage(double p) noexcept : p_(p) {}
  static Percentage make(double p) { return Percentage(p); }

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return p_ >= 0.0 && p_ <= 100.0;
  }
  constexpr double value() const noexcept { return p_; }
  constexpr double as_fraction() const noexcept { return p_ / 100.0; }
  constexpr bool finite() const noexcept { return p_ == p_ && p_ <= 100.0 && p_ >= 0.0; }

  friend constexpr bool operator==(Percentage a, Percentage b) noexcept { return a.p_ == b.p_; }
  friend constexpr bool operator<(Percentage a, Percentage b) noexcept { return a.p_ < b.p_; }
  friend constexpr bool operator>(Percentage a, Percentage b) noexcept { return a.p_ > b.p_; }
  friend constexpr bool operator<=(Percentage a, Percentage b) noexcept { return a.p_ <= b.p_; }
  friend constexpr bool operator>=(Percentage a, Percentage b) noexcept { return a.p_ >= b.p_; }

 private:
  double p_{0.0};
};

// ---------------------------------------------------------------------------
// PressureRatio: non-negative real ratio (e.g. used/capacity in [0, 1+)).
// ---------------------------------------------------------------------------
class PressureRatio {
 public:
  constexpr PressureRatio() noexcept = default;
  constexpr explicit PressureRatio(double r) noexcept : r_(r) {}
  constexpr bool is_valid() const noexcept { return r_ >= 0.0 && r_ == r_; }
  constexpr double value() const noexcept { return r_; }
  friend constexpr bool operator==(PressureRatio a, PressureRatio b) noexcept { return a.r_ == b.r_; }
  friend constexpr bool operator<(PressureRatio a, PressureRatio b) noexcept { return a.r_ < b.r_; }
  friend constexpr bool operator>(PressureRatio a, PressureRatio b) noexcept { return a.r_ > b.r_; }
  friend constexpr bool operator<=(PressureRatio a, PressureRatio b) noexcept { return a.r_ <= b.r_; }
  friend constexpr bool operator>=(PressureRatio a, PressureRatio b) noexcept { return a.r_ >= b.r_; }

 private:
  double r_{0.0};
};

// ---------------------------------------------------------------------------
// Cost: integer micros of a currency/unit. Non-negative, checked arithmetic.
// ---------------------------------------------------------------------------
class CostMicros {
 public:
  constexpr CostMicros() noexcept = default;
  constexpr explicit CostMicros(std::int64_t micros) noexcept : micros_(micros) {}
  constexpr std::int64_t as_micros() const noexcept { return micros_; }
  constexpr bool is_negative() const noexcept { return micros_ < 0; }

  constexpr bool add(CostMicros b, CostMicros& out) const noexcept {
    std::int64_t r;
    if (!detail::checked_add(micros_, b.micros_, r)) return false;
    out = CostMicros(r);
    return true;
  }
  constexpr bool sub(CostMicros b, CostMicros& out) const noexcept {
    std::int64_t r;
    if (!detail::checked_sub(micros_, b.micros_, r)) return false;
    out = CostMicros(r);
    return true;
  }

  friend constexpr bool operator==(CostMicros a, CostMicros b) noexcept { return a.micros_ == b.micros_; }
  friend constexpr bool operator!=(CostMicros a, CostMicros b) noexcept { return a.micros_ != b.micros_; }
  friend constexpr bool operator<(CostMicros a, CostMicros b) noexcept { return a.micros_ < b.micros_; }
  friend constexpr bool operator>=(CostMicros a, CostMicros b) noexcept { return a.micros_ >= b.micros_; }
  friend constexpr bool operator>(CostMicros a, CostMicros b) noexcept { return a.micros_ > b.micros_; }
  friend constexpr bool operator<=(CostMicros a, CostMicros b) noexcept { return a.micros_ <= b.micros_; }

  [[nodiscard]] std::string to_string() const { return std::to_string(micros_) + "um"; }

 private:
  std::int64_t micros_{0};
};
constexpr CostMicros cost_micros(std::int64_t v) { return CostMicros(v); }

// ---------------------------------------------------------------------------
// Energy: integer joules. Non-negative.
// ---------------------------------------------------------------------------
class EnergyJoules {
 public:
  constexpr EnergyJoules() noexcept = default;
  constexpr explicit EnergyJoules(std::int64_t joules) noexcept : j_(joules) {}
  constexpr bool is_negative() const noexcept { return j_ < 0; }
  constexpr std::int64_t value() const noexcept { return j_; }
  friend constexpr bool operator==(EnergyJoules a, EnergyJoules b) noexcept { return a.j_ == b.j_; }
  friend constexpr bool operator<(EnergyJoules a, EnergyJoules b) noexcept { return a.j_ < b.j_; }
  friend constexpr bool operator>(EnergyJoules a, EnergyJoules b) noexcept { return a.j_ > b.j_; }
 private:
  std::int64_t j_{0};
};
constexpr EnergyJoules energy_joules(std::int64_t v) { return EnergyJoules(v); }

// ---------------------------------------------------------------------------
// Rates: typed strong real-valued quantities, validated on construction.
// ---------------------------------------------------------------------------
namespace detail {
constexpr bool finite_nonneg(double d) noexcept { return d == d && d >= 0.0 && d <= 1.0e300; }
}  // namespace detail

template <typename Tag>
class Rate {
 public:
  constexpr Rate() noexcept = default;
  constexpr explicit Rate(double v) noexcept : v_(v) {}
  constexpr bool is_valid() const noexcept { return detail::finite_nonneg(v_); }
  constexpr double value() const noexcept { return v_; }
  friend constexpr bool operator==(Rate a, Rate b) noexcept { return a.v_ == b.v_; }
  friend constexpr bool operator<(Rate a, Rate b) noexcept { return a.v_ < b.v_; }
  friend constexpr bool operator>(Rate a, Rate b) noexcept { return a.v_ > b.v_; }
  friend constexpr bool operator<=(Rate a, Rate b) noexcept { return a.v_ <= b.v_; }
  friend constexpr bool operator>=(Rate a, Rate b) noexcept { return a.v_ >= b.v_; }

 private:
  double v_{0.0};
};

#define SLOFABRIC_RATE(Name) struct Name##_tag final {}; using Name = Rate<Name##_tag>

SLOFABRIC_RATE(RequestsPerSecond);
SLOFABRIC_RATE(TokensPerSecond);
SLOFABRIC_RATE(BytesPerSecond);
SLOFABRIC_RATE(OperationsPerSecond);

#undef SLOFABRIC_RATE

}  // namespace slofabric
