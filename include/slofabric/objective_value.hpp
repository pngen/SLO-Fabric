#pragma once

// SLO Fabric - typed objective values.
//
// A single ObjectiveValue carries a concrete measurement/ziel/target. The
// variant guarantees that the unit is part of the type: a byte count and a
// nanosecond duration are simply different types, so cross-unit arithmetic and
// comparison is impossible without an explicit, type-checked conversion.

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "slofabric/units.hpp"

namespace slofabric {

// Lexicographic set of unit kinds. This is the "which physical quantity"
// discriminator that remains visible even after a value is stored.
enum class UnitKind : std::uint8_t {
  Nanoseconds,
  Microseconds,
  Milliseconds,
  Seconds,
  Bytes,
  KiloBytes,
  MegaBytes,
  GigaBytes,
  Count,
  BasisPoints,        // availability: 0..10000
  Percent,            // percentage: 0..100
  PressureRatio,      // non-negative ratio
  RequestsPerSecond,
  TokensPerSecond,
  BytesPerSecond,
  OperationsPerSecond,
  CurrencyMicros,     // cost
  EnergyJoules,
};

constexpr std::string_view unit_kind_name(UnitKind u) noexcept {
  using U = UnitKind;
  switch (u) {
    case U::Nanoseconds: return "nanoseconds";
    case U::Microseconds: return "microseconds";
    case U::Milliseconds: return "milliseconds";
    case U::Seconds: return "seconds";
    case U::Bytes: return "bytes";
    case U::KiloBytes: return "kilobytes";
    case U::MegaBytes: return "megabytes";
    case U::GigaBytes: return "gigabytes";
    case U::Count: return "count";
    case U::BasisPoints: return "basis_points";
    case U::Percent: return "percent";
    case U::PressureRatio: return "pressure_ratio";
    case U::RequestsPerSecond: return "requests_per_second";
    case U::TokensPerSecond: return "tokens_per_second";
    case U::BytesPerSecond: return "bytes_per_second";
    case U::OperationsPerSecond: return "operations_per_second";
    case U::CurrencyMicros: return "currency_micros";
    case U::EnergyJoules: return "energy_joules";
  }
  return "unknown_unit";
}

// Strongly typed value for an objective target or measurement.
using ObjectiveValue = std::variant<
    Duration, ByteCount, Count, BasisPoints, Percentage, PressureRatio,
    RequestsPerSecond, TokensPerSecond, BytesPerSecond, OperationsPerSecond,
    CostMicros, EnergyJoules>;

// The unit kind carried by a value.
constexpr UnitKind unit_kind_of(const ObjectiveValue& v) noexcept {
  if (std::holds_alternative<Duration>(v)) return UnitKind::Nanoseconds;
  if (std::holds_alternative<ByteCount>(v)) return UnitKind::Bytes;
  if (std::holds_alternative<Count>(v)) return UnitKind::Count;
  if (std::holds_alternative<BasisPoints>(v)) return UnitKind::BasisPoints;
  if (std::holds_alternative<Percentage>(v)) return UnitKind::Percent;
  if (std::holds_alternative<PressureRatio>(v)) return UnitKind::PressureRatio;
  if (std::holds_alternative<RequestsPerSecond>(v)) return UnitKind::RequestsPerSecond;
  if (std::holds_alternative<TokensPerSecond>(v)) return UnitKind::TokensPerSecond;
  if (std::holds_alternative<BytesPerSecond>(v)) return UnitKind::BytesPerSecond;
  if (std::holds_alternative<OperationsPerSecond>(v)) return UnitKind::OperationsPerSecond;
  if (std::holds_alternative<CostMicros>(v)) return UnitKind::CurrencyMicros;
  if (std::holds_alternative<EnergyJoules>(v)) return UnitKind::EnergyJoules;
  return UnitKind::Count;
}

// Numeric view for comparison. Only valid when the two compared values share a
// unit kind. Doubles are used only for rate/ratio/percentage; integral units
// remain exact (returned as a double that is exactly representable <= 2^53).
inline double numeric_of(const ObjectiveValue& v) noexcept {
  if (const auto* d = std::get_if<Duration>(&v)) return static_cast<double>(d->as_ns());
  if (const auto* b = std::get_if<ByteCount>(&v)) return static_cast<double>(b->as_bytes());
  if (const auto* c = std::get_if<Count>(&v)) return static_cast<double>(c->value());
  if (const auto* bp = std::get_if<BasisPoints>(&v)) return static_cast<double>(bp->value());
  if (const auto* p = std::get_if<Percentage>(&v)) return p->value();
  if (const auto* r = std::get_if<PressureRatio>(&v)) return r->value();
  if (const auto* r = std::get_if<RequestsPerSecond>(&v)) return r->value();
  if (const auto* r = std::get_if<TokensPerSecond>(&v)) return r->value();
  if (const auto* r = std::get_if<BytesPerSecond>(&v)) return r->value();
  if (const auto* r = std::get_if<OperationsPerSecond>(&v)) return r->value();
  if (const auto* m = std::get_if<CostMicros>(&v)) return static_cast<double>(m->as_micros());
  if (const auto* e = std::get_if<EnergyJoules>(&v)) return static_cast<double>(e->value());
  return 0.0;
}

inline bool unit_kind_matches(const ObjectiveValue& a, const ObjectiveValue& b) noexcept {
  return unit_kind_of(a) == unit_kind_of(b);
}

// Human-readable value + unit.
inline std::string to_string(const ObjectiveValue& v) {
  switch (unit_kind_of(v)) {
    case UnitKind::Nanoseconds: return std::get<Duration>(v).to_string();
    case UnitKind::Bytes: return std::get<ByteCount>(v).to_string();
    case UnitKind::Count: return std::to_string(std::get<Count>(v).value());
    case UnitKind::BasisPoints: return std::to_string(std::get<BasisPoints>(v).value()) + "bp";
    case UnitKind::Percent: return std::to_string(std::get<Percentage>(v).value()) + "%";
    case UnitKind::PressureRatio: return std::to_string(std::get<PressureRatio>(v).value());
    case UnitKind::RequestsPerSecond: return std::to_string(std::get<RequestsPerSecond>(v).value()) + "req/s";
    case UnitKind::TokensPerSecond: return std::to_string(std::get<TokensPerSecond>(v).value()) + "tok/s";
    case UnitKind::BytesPerSecond: return std::to_string(std::get<BytesPerSecond>(v).value()) + "B/s";
    case UnitKind::OperationsPerSecond: return std::to_string(std::get<OperationsPerSecond>(v).value()) + "op/s";
    case UnitKind::CurrencyMicros: return std::get<CostMicros>(v).to_string();
    case UnitKind::EnergyJoules: return std::to_string(std::get<EnergyJoules>(v).value()) + "J";
    case UnitKind::Microseconds:
    case UnitKind::Milliseconds:
    case UnitKind::Seconds:
    case UnitKind::KiloBytes:
    case UnitKind::MegaBytes:
    case UnitKind::GigaBytes:
      return std::to_string(numeric_of(v));
  }
  return "";
}

}  // namespace slofabric
