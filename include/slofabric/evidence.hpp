#pragma once

// SLO Fabric - typed evidence model.
//
// Evaluation consumes typed evidence. Measurement and obligation are separate:
// a measured value does not itself define an objective. Provenance classifies
// how a value came to exist, and freshness is tracked explicitly. Missing
// evidence remains missing - UNKNOWN never becomes compliant.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "slofabric/identities.hpp"
#include "slofabric/objective_value.hpp"
#include "slofabric/provenance.hpp"
#include "slofabric/units.hpp"

namespace slofabric {

// Whether a measurement was completed, partial, or inconclusive.
enum class MeasurementStatus : std::uint8_t {
  Complete,
  Partial,
  Inconclusive,
};

constexpr std::string_view measurement_status_name(MeasurementStatus s) noexcept {
  using M = MeasurementStatus;
  switch (s) {
    case M::Complete: return "complete";
    case M::Partial: return "partial";
    case M::Inconclusive: return "inconclusive";
  }
  return "unknown_measurement_status";
}

// How a value was aggregated.
enum class AggregationType : std::uint8_t {
  Raw,
  Sum,
  Mean,
  Min,
  Max,
  Count,
  PercentileRaw,
  PercentileApprox,
  None,
};

constexpr std::string_view aggregation_type_name(AggregationType a) noexcept {
  using A = AggregationType;
  switch (a) {
    case A::Raw: return "raw";
    case A::Sum: return "sum";
    case A::Mean: return "mean";
    case A::Min: return "min";
    case A::Max: return "max";
    case A::Count: return "count";
    case A::PercentileRaw: return "percentile_raw";
    case A::PercentileApprox: return "percentile_approx";
    case A::None: return "none";
  }
  return "unknown_aggregation";
}

// A single typed evidence record feeding evaluation.
struct EvidenceRecord {
  EvidenceId id;
  EvidenceGen gen;
  Dimension dimension = Dimension::Latency;
  ObjectiveId objective_id;
  ObjectiveGen objective_gen;

  SourceBootId source;
  SourceBootGen source_gen;
  CoordinatorEpoch epoch;

  // Observation time on the coordinator's injectable clock.
  Duration time{0};
  // Wall-clock Unix nanosecond timestamp (for provenance/durability).
  std::int64_t wall_ns{0};

  Provenance provenance = Provenance::Measured;
  MeasurementStatus status = MeasurementStatus::Complete;
  AggregationType aggregation = AggregationType::Raw;

  // Number of underlying samples aggregated into value.
  Count sample_count{1};

  // Optional confidence in [0,1].
  std::optional<double> confidence;

  // The strong value measured/derived/estimated.
  ObjectiveValue value;

  // Synthetic evidence is never reported as measured.
  [[nodiscard]] bool is_synthetic() const noexcept {
    return provenance == Provenance::Synthetic || provenance == Provenance::Estimated;
  }
};

// Convenience constructors per dimension (typed).
inline EvidenceRecord make_latency_evidence(EvidenceId id, EvidenceGen gen,
                                            ObjectiveId oid, ObjectiveGen ogen,
                                            SourceBootId src, CoordinatorEpoch epoch,
                                            Duration time, Duration latency) {
  EvidenceRecord r;
  r.id = id; r.gen = gen; r.dimension = Dimension::Latency;
  r.objective_id = oid; r.objective_gen = ogen;
  r.source = src; r.epoch = epoch; r.time = time;
  r.value = latency;
  return r;
}

inline EvidenceRecord make_cost_evidence(EvidenceId id, EvidenceGen gen,
                                         ObjectiveId oid, ObjectiveGen ogen,
                                         SourceBootId src, CoordinatorEpoch epoch,
                                         Duration time, CostMicros cost) {
  EvidenceRecord r;
  r.id = id; r.gen = gen; r.dimension = Dimension::Cost;
  r.objective_id = oid; r.objective_gen = ogen;
  r.source = src; r.epoch = epoch; r.time = time;
  r.value = cost;
  return r;
}

inline EvidenceRecord make_availability_evidence(EvidenceId id, EvidenceGen gen,
                                                 ObjectiveId oid, ObjectiveGen ogen,
                                                 SourceBootId src, CoordinatorEpoch epoch,
                                                 Duration time, BasisPoints availability) {
  EvidenceRecord r;
  r.id = id; r.gen = gen; r.dimension = Dimension::Availability;
  r.objective_id = oid; r.objective_gen = ogen;
  r.source = src; r.epoch = epoch; r.time = time;
  r.value = availability;
  return r;
}

inline EvidenceRecord make_pressure_evidence(EvidenceId id, EvidenceGen gen,
                                             ObjectiveId oid, ObjectiveGen ogen,
                                             SourceBootId src, CoordinatorEpoch epoch,
                                             Duration time, PressureRatio pressure) {
  EvidenceRecord r;
  r.id = id; r.gen = gen; r.dimension = Dimension::MemoryPressure;
  r.objective_id = oid; r.objective_gen = ogen;
  r.source = src; r.epoch = epoch; r.time = time;
  r.value = pressure;
  return r;
}

inline EvidenceRecord make_throughput_evidence(EvidenceId id, EvidenceGen gen,
                                               ObjectiveId oid, ObjectiveGen ogen,
                                               SourceBootId src, CoordinatorEpoch epoch,
                                               Duration time, double rate,
                                               RateTag tag = RateTag::Requests) {
  EvidenceRecord r;
  r.id = id; r.gen = gen; r.dimension = Dimension::Throughput;
  r.objective_id = oid; r.objective_gen = ogen;
  r.source = src; r.epoch = epoch; r.time = time;
  switch (tag) {
    case RateTag::Requests: r.value = RequestsPerSecond(rate); break;
    case RateTag::Tokens: r.value = TokensPerSecond(rate); break;
    case RateTag::Bytes: r.value = BytesPerSecond(rate); break;
    case RateTag::Operations: r.value = OperationsPerSecond(rate); break;
  }
  return r;
}

}  // namespace slofabric
