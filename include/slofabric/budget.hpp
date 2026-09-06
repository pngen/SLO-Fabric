#pragma once

// SLO Fabric - error budget semantics.
//
// A budget is a finite allowance over a window. It tracks total, consumed,
// remaining, and burn rate. Consumption is exact (integer counts). The
// invariant "remaining <= total" always holds; if consumed exceeds total the
// excess is tracked as overrun without violating the invariant.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>

#include "slofabric/units.hpp"

namespace slofabric {

enum class BudgetState : std::uint8_t {
  Healthy,
  NearLimit,
  Exhausted,
};

constexpr std::string_view budget_state_name(BudgetState s) noexcept {
  using B = BudgetState;
  switch (s) {
    case B::Healthy: return "healthy";
    case B::NearLimit: return "near_limit";
    case B::Exhausted: return "exhausted";
  }
  return "unknown_budget_state";
}

enum class BurnRate : std::uint8_t {
  Normal,
  Elevated,
  Critical,
};

constexpr std::string_view burn_rate_name(BurnRate b) noexcept {
  using B = BurnRate;
  switch (b) {
    case B::Normal: return "normal";
    case B::Elevated: return "elevated";
    case B::Critical: return "critical";
  }
  return "unknown_burn_rate";
}

class ErrorBudget {
 public:
  ErrorBudget(Count total, Duration window)
      : total_(total), window_(window) {
    if (window_.as_ns() < 0) window_ = Duration(0);
  }

  Count total() const noexcept { return total_; }
  Count consumed() const noexcept { return consumed_; }
  Count remaining() const noexcept {
    std::int64_t rem = total_.value() - consumed_.value();
    if (rem < 0) rem = 0;
    return Count(rem);
  }
  Count overrun() const noexcept {
    std::int64_t o = consumed_.value() - total_.value();
    return Count(o < 0 ? 0 : o);
  }
  Duration window() const noexcept { return window_; }

  // Consume one budget unit (exact). Caller has already deduplicated the event.
  BudgetState consume() {
    std::int64_t c = consumed_.value();
    if (c < std::numeric_limits<std::int64_t>::max()) consumed_ = Count(c + 1);
    return state();
  }
  BudgetState consume_count(Count amount) {
    std::int64_t c = consumed_.value();
    std::int64_t a = amount.value();
    if (a > 0 && c <= std::numeric_limits<std::int64_t>::max() - a) c += a;
    consumed_ = Count(c);
    return state();
  }

  BudgetState state() const noexcept {
    std::int64_t total = total_.value();
    if (total == 0) return BudgetState::Exhausted;
    std::int64_t rem = total - consumed_.value();
    if (rem <= 0) return BudgetState::Exhausted;
    if (static_cast<double>(rem) <= static_cast<double>(total) * 0.10) return BudgetState::NearLimit;
    return BudgetState::Healthy;
  }

  double fraction_elapsed(Duration now) const noexcept {
    std::int64_t w = window_.as_ns();
    if (w <= 0) return 1.0;
    std::int64_t e = now.as_ns() - window_start_.as_ns();
    if (e < 0) e = 0;
    double f = static_cast<double>(e) / static_cast<double>(w);
    if (f > 1.0) f = 1.0;
    return f;
  }

  double burn_rate(Duration now) const noexcept {
    std::int64_t total = total_.value();
    if (total <= 0) return 0.0;
    double frac = fraction_elapsed(now);
    if (frac <= 0.0) return 0.0;
    double consumed_frac = static_cast<double>(consumed_.value()) / static_cast<double>(total);
    return consumed_frac / frac;
  }

  BurnRate classify_burn(Duration now) const noexcept {
    double b = burn_rate(now);
    if (b > 2.0) return BurnRate::Critical;
    if (b > 1.0) return BurnRate::Elevated;
    return BurnRate::Normal;
  }

  void reset_window(Duration now) {
    consumed_ = Count(0);
    window_start_ = now;
  }

  bool window_rolled(Duration now) const noexcept {
    std::int64_t w = window_.as_ns();
    if (w <= 0) return false;
    return now.as_ns() - window_start_.as_ns() >= w;
  }

  void set_window_start(Duration now) { window_start_ = now; }
  Duration window_start() const noexcept { return window_start_; }

 private:
  Count total_;
  Count consumed_{0};
  Duration window_;
  Duration window_start_{0};
};

}  // namespace slofabric
