#pragma once

// SLO Fabric - objective definition and semantics.
//
// Every objective carries explicit, centralized semantics: dimension, target,
// directionality (comparison), hard/soft, evaluation window, minimum evidence,
// breach/recovery thresholds, hysteresis, priority, and enforcement class.
// The runtime never hard-codes "less is good"; it reads these semantics.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "slofabric/identities.hpp"
#include "slofabric/objective_value.hpp"

namespace slofabric {

// The supportable SLO dimensions.
enum class Dimension : std::uint8_t {
  Latency,
  Availability,
  Throughput,
  RecoveryTime,
  MemoryPressure,
  Cost,
};

constexpr std::string_view dimension_name(Dimension d) noexcept {
  using D = Dimension;
  switch (d) {
    case D::Latency: return "latency";
    case D::Availability: return "availability";
    case D::Throughput: return "throughput";
    case D::RecoveryTime: return "recovery_time";
    case D::MemoryPressure: return "memory_pressure";
    case D::Cost: return "cost";
  }
  return "unknown_dimension";
}

// Directionality of comparison. AtMost = lower-is-better (e.g. latency,
// cost, pressure, recovery time). AtLeast = higher-is-better (e.g.
// availability, throughput).
enum class Comparison : std::uint8_t { AtMost, AtLeast };

constexpr std::string_view comparison_name(Comparison c) noexcept {
  return c == Comparison::AtMost ? "at_most" : "at_least";
}

// Hard objectives are binding constraints; soft objectives are ranked after
// hard obligations are satisfied.
enum class HardSoft : std::uint8_t { Hard, Soft };

constexpr std::string_view hardsoft_name(HardSoft h) noexcept {
  return h == HardSoft::Hard ? "hard" : "soft";
}

// Evaluation window shape.
enum class WindowType : std::uint8_t {
  Instantaneous,      // evaluate the single most recent sample
  Sliding,            // fixed-duration sliding window
  Tumbling,           // fixed-duration disjoint buckets
  FixedInterval,      // fixed recording interval (one bucket per period)
  RollingCount,       // last N samples
};

constexpr std::string_view window_type_name(WindowType w) noexcept {
  using W = WindowType;
  switch (w) {
    case W::Instantaneous: return "instantaneous";
    case W::Sliding: return "sliding";
    case W::Tumbling: return "tumbling";
    case W::FixedInterval: return "fixed_interval";
    case W::RollingCount: return "rolling_count";
  }
  return "unknown_window";
}

// Enforcement authorization class for a given objective.
enum class EnforcementClass : std::uint8_t {
  None,       // no enforcement intent produced
  Advisory,   // publish suggestion only
  Authorized, // may authorize a request to an adjacent runtime
  Mandatory,  // must authorize (subject to cooldown)
};

constexpr std::string_view enforcement_class_name(EnforcementClass e) noexcept {
  using E = EnforcementClass;
  switch (e) {
    case E::None: return "none";
    case E::Advisory: return "advisory";
    case E::Authorized: return "authorized";
    case E::Mandatory: return "mandatory";
  }
  return "unknown_enforcement";
}

// Dimension-specific sub-kind (which percentile, which availability mode,
// which throughput unit, which cost shape). Kept narrow; each objective picks
// exactly one sub-kind.
enum class ObjectiveKind : std::uint8_t {
  // Latency
  LatencyMean,
  LatencyPercentile,   // see percentile rank
  LatencyMaxDeadline,
  // Availability
  AvailabilityTime,    // eligible_time vs serving time
  AvailabilityRequest, // successful requests / eligible requests
  // Throughput (rate tag selects Requests/Tokens/Bytes/Operations per second)
  ThroughputRate,
  // Recovery
  RecoveryMaxDuration,
  // Memory pressure
  MemoryPressureMax,   // maximum sustained pressure ratio
  MemoryMinHeadroom,   // minimum free headroom bytes
  // Cost
  CostPerRequest,
  CostPerToken,
  CostWindowBudget,
};

constexpr std::string_view objective_kind_name(ObjectiveKind k) noexcept {
  using K = ObjectiveKind;
  switch (k) {
    case K::LatencyMean: return "latency_mean";
    case K::LatencyPercentile: return "latency_percentile";
    case K::LatencyMaxDeadline: return "latency_max_deadline";
    case K::AvailabilityTime: return "availability_time";
    case K::AvailabilityRequest: return "availability_request";
    case K::ThroughputRate: return "throughput_rate";
    case K::RecoveryMaxDuration: return "recovery_max_duration";
    case K::MemoryPressureMax: return "memory_pressure_max";
    case K::MemoryMinHeadroom: return "memory_min_headroom";
    case K::CostPerRequest: return "cost_per_request";
    case K::CostPerToken: return "cost_per_token";
    case K::CostWindowBudget: return "cost_window_budget";
  }
  return "unknown_objective_kind";
}

// Supported throughput rate tags (kept typed, not interchangeable).
enum class RateTag : std::uint8_t { Requests, Tokens, Bytes, Operations };

// Availability accounting mode.
enum class AvailabilityMode : std::uint8_t { Time, Requests };

// The full objective definition.
struct Objective {
  ObjectiveId id;
  ObjectiveGen gen;
  std::string name;

  Dimension dim = Dimension::Latency;
  ObjectiveKind kind = ObjectiveKind::LatencyMean;
  Comparison comparison = Comparison::AtMost;
  HardSoft hard = HardSoft::Hard;
  EnforcementClass enforcement = EnforcementClass::Authorized;

  // Target/limits.
  ObjectiveValue target;

  // Hysteresis: optional breach/recovery thresholds. If absent, the target is
  // the breach threshold (for AtMost, value > target => violation; for AtLeast,
  // value < target => violation) and the recovery threshold equals the target.
  std::optional<ObjectiveValue> breach_threshold;
  std::optional<ObjectiveValue> recovery_threshold;

  // Minimum evidence required to produce a definitive compliance state.
  Count min_evidence{1};
  // Fraction of allowed uncertainty in value comparisons (0 = none).
  double allowed_uncertainty = 0.0;
  // Evidence becomes stale after this much time. 0 treats evidence as current
  // only if it is the most recent sample within the evaluation window.
  Duration freshness_ttl{0};

  // Grace period before a breach is declared (e.g. sustained pressure).
  Duration grace_period{0};

  // Evaluation window.
  WindowType window_type = WindowType::Instantaneous;
  Duration window_duration{0};   // for Sliding/Tumbling/FixedInterval
  Count window_count{64};        // for RollingCount (sample cap)

  Priority priority{0};          // lower number = higher priority

  // Percentile rank in [0,1] for LatencyPercentile.
  double percentile_rank = 0.99;

  // Availability mode.
  AvailabilityMode availability_mode = AvailabilityMode::Time;

  // Throughput rate tag.
  RateTag rate_tag = RateTag::Requests;

  // Effective-from / expiry handled by the contract, not the objective.

  // Convenience: is the target unit consistent with the dimension?
  [[nodiscard]] bool target_unit_matches_dimension() const;
};

// The expected unit kind for each dimension's objective values.
inline UnitKind expected_unit_for(Dimension d) noexcept {
  using D = Dimension;
  switch (d) {
    case D::Latency: return UnitKind::Nanoseconds;
    case D::Availability: return UnitKind::BasisPoints;
    case D::Throughput: return UnitKind::RequestsPerSecond;
    case D::RecoveryTime: return UnitKind::Nanoseconds;
    case D::MemoryPressure: return UnitKind::PressureRatio;
    case D::Cost: return UnitKind::CurrencyMicros;
  }
  return UnitKind::Count;
}

inline bool Objective::target_unit_matches_dimension() const {
  return unit_kind_of(target) == expected_unit_for(dim);
}

}  // namespace slofabric
