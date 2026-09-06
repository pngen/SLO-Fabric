#pragma once

// SLO Fabric - deterministic evaluation window.
//
// A window accumulates samples (time + numeric value) for one objective and
// computes aggregates. Ingestion is strictly ordered: clock rollback, duplicate
// sequence numbers, and (for tumbling/fixed windows) late samples are iterated.
// Sliding windows evict by age; rolling-count windows evict by count; tumbling
// and fixed-interval buckets close at fixed boundaries.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "slofabric/objective.hpp"
#include "slofabric/quantile.hpp"
#include "slofabric/units.hpp"

namespace slofabric {

enum class IngestStatus : std::uint8_t {
  Accepted,
  Duplicate,     // seq not strictly increasing
  Rollback,      // time moved backward
  Late,          // closed window already advanced past this time
  Closed,        // window type does not accept samples (e.g. external aggregate)
  Invalid,       // NaN/inf value or invalid window params
};

constexpr std::string_view ingest_status_name(IngestStatus s) noexcept {
  using I = IngestStatus;
  switch (s) {
    case I::Accepted: return "accepted";
    case I::Duplicate: return "duplicate";
    case I::Rollback: return "rollback";
    case I::Late: return "late";
    case I::Closed: return "closed";
    case I::Invalid: return "invalid";
  }
  return "unknown";
}

struct WindowSample {
  Duration time;
  double value;
  std::uint64_t seq;
};

class SampleWindow {
 public:
  SampleWindow(WindowType type, Duration duration, std::size_t capacity = 1024)
      : type_(type), interval_(duration), capacity_(capacity == 0 ? 1 : capacity) {
    quantile_ = QuantileStore(capacity_);
  }

  WindowType type() const noexcept { return type_; }

  void reset() {
    samples_.clear();
    quantile_.clear();
    last_time_ = Duration(0);
    last_seq_ = 0;
    bucket_start_ = Duration(0);
    has_bucket_ = false;
  }

  IngestStatus ingest(Duration time, double value, std::uint64_t seq) {
    if (value != value || value > 1.0e300) return IngestStatus::Invalid;
    if (seq <= last_seq_) return IngestStatus::Duplicate;

    if (!has_bucket_) {
      last_time_ = time;
      last_seq_ = seq;
      bucket_start_ = time;
      has_bucket_ = true;
      push(time, value, seq);
      return IngestStatus::Accepted;
    }

    // Clock rollback rejection.
    if (time < last_time_) return IngestStatus::Rollback;

    last_seq_ = seq;

    switch (type_) {
      case WindowType::Instantaneous: {
        // Keep only the most recent sample.
        samples_.clear();
        quantile_.clear();
        push(time, value, seq);
        last_time_ = time;
        return IngestStatus::Accepted;
      }
      case WindowType::Sliding: {
        push(time, value, seq);
        evict_by_age(time);
        while (samples_.size() > capacity_) evict_front();
        last_time_ = time;
        return IngestStatus::Accepted;
      }
      case WindowType::RollingCount: {
        push(time, value, seq);
        while (samples_.size() > capacity_) evict_front();
        last_time_ = time;
        return IngestStatus::Accepted;
      }
      case WindowType::Tumbling:
      case WindowType::FixedInterval: {
        // Fixed disjoint buckets of length interval_.
        if (interval_.as_ns() <= 0) return IngestStatus::Invalid;
        const std::int64_t bucket = time.as_ns() / interval_.as_ns();
        const std::int64_t start_bucket = bucket_start_.as_ns() / interval_.as_ns();
        if (bucket < start_bucket) {
          last_time_ = time;  // observed but rejected
          return IngestStatus::Late;
        }
        if (bucket > start_bucket) {
          // Close current bucket; start a new one.
          samples_.clear();
          quantile_.clear();
          bucket_start_ = Duration(bucket * interval_.as_ns());
        }
        push(time, value, seq);
        last_time_ = time;
        return IngestStatus::Accepted;
      }
    }
    return IngestStatus::Invalid;
  }

  std::size_t size() const noexcept { return samples_.size(); }
  bool empty() const noexcept { return samples_.empty(); }

  std::vector<WindowSample> snapshot() const { return std::vector<WindowSample>(samples_.begin(), samples_.end()); }
  std::vector<double> sample_values() const {
    std::vector<double> v;
    v.reserve(samples_.size());
    for (const auto& s : samples_) v.push_back(s.value);
    return v;
  }

  double sum() const {
    double s = 0;
    for (const auto& x : samples_) s += x.value;
    return s;
  }
  double mean() const {
    if (samples_.empty()) return 0.0;
    return sum() / static_cast<double>(samples_.size());
  }
  double min() const {
    double m = 0;
    bool first = true;
    for (const auto& x : samples_) { if (first || x.value < m) { m = x.value; first = false; } }
    return m;
  }
  double max() const {
    double m = 0;
    bool first = true;
    for (const auto& x : samples_) { if (first || x.value > m) { m = x.value; first = false; } }
    return m;
  }
  double last_value() const { return samples_.empty() ? 0.0 : samples_.back().value; }

  double quantile(double p) const {
    // Deterministic nearest-rank over ALL current in-window samples (the deque is
    // already bounded by the window). Never silently drops in-window samples.
    std::vector<double> s;
    s.reserve(samples_.size());
    for (const auto& x : samples_) s.push_back(x.value);
    std::sort(s.begin(), s.end());
    if (s.empty()) return 0.0;
    p = std::clamp(p, 0.0, 1.0);
    std::size_t idx;
    if (p <= 0.0) idx = 0;
    else {
      double rank = std::ceil(p * static_cast<double>(s.size()));
      if (rank < 1.0) rank = 1.0;
      idx = static_cast<std::size_t>(rank) - 1;
      if (idx >= s.size()) idx = s.size() - 1;
    }
    return s[idx];
  }
  bool quantile_sufficient(double p) const { return !samples_.empty() && std::ceil(p * samples_.size()) >= 1.0; }

 private:
  void push(Duration time, double value, std::uint64_t seq) {
    samples_.push_back(WindowSample{time, value, seq});
    quantile_.add(value);
  }
  void evict_front() {
    if (!samples_.empty()) samples_.pop_front();
  }
  void evict_by_age(Duration now) {
    const std::int64_t ns = interval_.as_ns();
    if (ns <= 0) return;
    while (!samples_.empty() && now.as_ns() - samples_.front().time.as_ns() >= ns) {
      samples_.pop_front();
    }
  }

  WindowType type_;
  Duration interval_;
  std::size_t capacity_;
  std::deque<WindowSample> samples_;
  QuantileStore quantile_;
  Duration last_time_{0};
  std::uint64_t last_seq_{0};
  Duration bucket_start_{0};
  bool has_bucket_{false};
};

}  // namespace slofabric
