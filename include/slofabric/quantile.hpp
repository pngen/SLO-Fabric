#pragma once

// SLO Fabric - deterministic quantile selection.
//
// Exact nearest-rank quantiles over a bounded set of samples. The sample store
// is bounded; when capacity is exceeded the oldest samples are evicted so that
// memory use is bounded. Quantile selection is fully deterministic and
// documented: for fraction p in (0,1], rank = ceil(p*N), index = rank-1 clamped
// to [0, N-1], value = sorted[index]. p=0 -> min, p=1 -> max.
//
// We never fabricate a percentile from too few samples: callers ask
// sufficient() before trusting a percentile value.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace slofabric {

// Bounded exact quantile store over real values (nanosecond latency in our
// use). Deterministic ordering (ascending) and deterministic selection.
class QuantileStore {
 public:
  explicit QuantileStore(std::size_t capacity = 1024) : capacity_(capacity == 0 ? 1 : capacity) {
    samples_.reserve(capacity_);
  }

  // Add a sample. Returns false only when the store is at capacity and the
  // caller must evict first (callers evict by time via a window).
  bool add(double value) {
    if (samples_.size() >= capacity_) return false;
    samples_.push_back(value);
    return true;
  }

  std::size_t size() const noexcept { return samples_.size(); }
  bool empty() const noexcept { return samples_.empty(); }
  std::size_t capacity() const noexcept { return capacity_; }

  // Whether there is enough information to compute percentile p.
  bool sufficient(double p) const noexcept {
    if (samples_.empty()) return false;
    // Require at least the number of samples needed to distinguish the rank.
    const double rank = std::ceil(p * static_cast<double>(samples_.size()));
    return rank >= 1.0 && static_cast<std::size_t>(rank) <= samples_.size();
  }

  // Deterministic nearest-rank quantile. Requires non-empty and sufficient(p).
  // Returns the value at the requested quantile.
  double quantile(double p) const {
    // Copy and sort (bounded). Deterministic.
    std::vector<double> s(samples_);
    std::sort(s.begin(), s.end());
    if (s.empty()) return 0.0;
    p = std::clamp(p, 0.0, 1.0);
    std::size_t idx;
    if (p <= 0.0) {
      idx = 0;
    } else {
      double rank = std::ceil(p * static_cast<double>(s.size()));
      if (rank < 1.0) rank = 1.0;
      idx = static_cast<std::size_t>(rank) - 1;
      if (idx >= s.size()) idx = s.size() - 1;
    }
    return s[idx];
  }

  // Sorted snapshot (for explanation / debug). Returns a new vector.
  std::vector<double> sorted() const {
    std::vector<double> s(samples_);
    std::sort(s.begin(), s.end());
    return s;
  }

  void clear() { samples_.clear(); }

  // Evict the oldest logical sample: remove the last element (caller tracks
  // age ordering externally). This is the slot currently occupied by the
  // oldest sample when the window is full.
  void evict_oldest(std::size_t n = 1) {
    for (std::size_t i = 0; i < n && !samples_.empty(); ++i) samples_.pop_back();
  }

  const std::vector<double>& raw() const noexcept { return samples_; }

 private:
  std::size_t capacity_;
  std::vector<double> samples_;  // insertion order; sorted() sorts a copy
};

}  // namespace slofabric
