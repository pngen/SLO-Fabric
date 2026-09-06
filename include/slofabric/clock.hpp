#pragma once

// SLO Fabric - injectable clock.
//
// All timing-sensitive logic (windows, grace periods, budgets, burn rate,
// freshness, expiry, cooldown, hysteresis, recovery timing) must observe time
// through an IClock. Deterministic tests use a ManualClock; production uses a
// monotonic clock for ordering plus a wall-clock source for durability.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "slofabric/units.hpp"

namespace slofabric {

// Abstract time source. Time is expressed as Duration (integer nanoseconds).
class IClock {
 public:
  virtual ~IClock() = default;

  // Monotonic-ordered time. Used for ordering, windows, deadlines. Must be
  // stable within a process; MUST NOT jump backward.
  virtual Duration now() const = 0;

  // Wall-clock Unix epoch time (ns since 1970). Used only for durable
  // timestamps and provenance. May jump; never used for ordering.
  virtual std::int64_t wall_epoch_ns() const = 0;

  // Human-readable timestamp for diagnostics.
  virtual std::string describe(Duration at) const = 0;
};

// Monotonic wall clock using a steady source. Ordering is stable.
class MonotonicClock final : public IClock {
 public:
  MonotonicClock() : wall_(std::chrono::system_clock::now()) {}
  Duration now() const override {
    auto t = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    return Duration(ns);
  }
  std::int64_t wall_epoch_ns() const override {
    auto t = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
  }
  std::string describe(Duration at) const override { return at.to_string(); }

 private:
  std::chrono::system_clock::time_point wall_;
};

// Wall-clock clock that orders by Unix epoch time. May advance; rollback is
// the caller's responsibility to detect.
class WallClock final : public IClock {
 public:
  Duration now() const override {
    return Duration(wall_epoch_ns());
  }
  std::int64_t wall_epoch_ns() const override {
    auto t = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
  }
  std::string describe(Duration at) const override { return at.to_string(); }
};

// Deterministic test clock. The operator advances time; the clock never
// produces a value the test did not set. rollback is explicit and immediate
// (used to test clock-rollback rejection logic).
class ManualClock final : public IClock {
 public:
  ManualClock() = default;
  explicit ManualClock(Duration initial) : now_(initial), wall_(initial.as_ns()) {}

  Duration now() const override { return now_; }
  std::int64_t wall_epoch_ns() const override { return wall_; }
  std::string describe(Duration at) const override { return at.to_string(); }

  void set_now(Duration d) { now_ = d; }
  void advance(Duration d) {
    std::int64_t r = 0;
    if (detail_checked_add(now_.as_ns(), d.as_ns(), r)) now_ = Duration(r);
  }
  // Explicit rollback: sets the clock backward.
  void rollback(Duration d) { now_ = d; }
  // Set a specific wall time (ns Unix epoch).
  void set_wall(std::int64_t wall) { wall_ = wall; }

 private:
  static bool detail_checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) {
    bool ok = (b >= 0 ? a <= std::numeric_limits<std::int64_t>::max() - b
                      : a >= std::numeric_limits<std::int64_t>::min() - b);
    if (ok) out = a + b;
    return ok;
  }
  Duration now_{0};
  std::int64_t wall_{0};
};

}  // namespace slofabric
