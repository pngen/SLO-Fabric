#pragma once

// SLO Fabric - typed status/error model.
//
// Every operation that can fail returns a Status describing the failure
// category. A Status is a value type carrying a category plus an optional
// human-readable diagnostic. It is NOT an exception; the library uses
// explicit status returns and std::expected-style results so control flow
// is explicit and deterministic.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace slofabric {

// Canonical error/status categories. This is the single source of truth for
// failure classification. The string form is stable and part of the public
// API surface (used for diagnostics and canonical encoding).
enum class StatusCode : std::uint8_t {
  Ok = 0,
  InvalidInput,
  StaleAuthority,
  StaleEvidence,
  InsufficientEvidence,
  UnitMismatch,
  ContractInactive,
  PolicySuperseded,
  ObjectiveInvalid,
  PersistenceCorrupt,
  ProtocolError,
  ResourceExhausted,
  EnforcementFailed,
  OutcomeUnknown,
  Cancelled,
  ShuttingDown,
  NotReady,
  NotFound,
  Internal,
};

// Stable, lowercase snake_case string for a status code.
constexpr std::string_view status_code_name(StatusCode c) noexcept {
  using S = StatusCode;
  switch (c) {
    case S::Ok: return "ok";
    case S::InvalidInput: return "invalid_input";
    case S::StaleAuthority: return "stale_authority";
    case S::StaleEvidence: return "stale_evidence";
    case S::InsufficientEvidence: return "insufficient_evidence";
    case S::UnitMismatch: return "unit_mismatch";
    case S::ContractInactive: return "contract_inactive";
    case S::PolicySuperseded: return "policy_superseded";
    case S::ObjectiveInvalid: return "objective_invalid";
    case S::PersistenceCorrupt: return "persistence_corrupt";
    case S::ProtocolError: return "protocol_error";
    case S::ResourceExhausted: return "resource_exhausted";
    case S::EnforcementFailed: return "enforcement_failed";
    case S::OutcomeUnknown: return "outcome_unknown";
    case S::Cancelled: return "cancelled";
    case S::ShuttingDown: return "shutting_down";
    case S::NotReady: return "not_ready";
    case S::NotFound: return "not_found";
    case S::Internal: return "internal";
  }
  return "unknown";
}

struct Status {
  StatusCode code = StatusCode::Ok;
  std::string message;

  Status() = default;
  Status(StatusCode c, std::string msg = {}) : code(c), message(std::move(msg)) {}

  bool ok() const noexcept { return code == StatusCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code == b.code && a.message == b.message;
  }
  friend bool operator!=(const Status& a, const Status& b) noexcept { return !(a == b); }
};

// Convenience helpers.
inline Status ok() { return Status(); }
inline Status invalid_input(std::string m = {}) { return Status(StatusCode::InvalidInput, std::move(m)); }
inline Status stale_authority(std::string m = {}) { return Status(StatusCode::StaleAuthority, std::move(m)); }
inline Status stale_evidence(std::string m = {}) { return Status(StatusCode::StaleEvidence, std::move(m)); }
inline Status insufficient_evidence(std::string m = {}) { return Status(StatusCode::InsufficientEvidence, std::move(m)); }
inline Status unit_mismatch(std::string m = {}) { return Status(StatusCode::UnitMismatch, std::move(m)); }
inline Status contract_inactive(std::string m = {}) { return Status(StatusCode::ContractInactive, std::move(m)); }
inline Status policy_superseded(std::string m = {}) { return Status(StatusCode::PolicySuperseded, std::move(m)); }
inline Status objective_invalid(std::string m = {}) { return Status(StatusCode::ObjectiveInvalid, std::move(m)); }
inline Status persistence_corrupt(std::string m = {}) { return Status(StatusCode::PersistenceCorrupt, std::move(m)); }
inline Status protocol_error(std::string m = {}) { return Status(StatusCode::ProtocolError, std::move(m)); }
inline Status resource_exhausted(std::string m = {}) { return Status(StatusCode::ResourceExhausted, std::move(m)); }
inline Status enforcement_failed(std::string m = {}) { return Status(StatusCode::EnforcementFailed, std::move(m)); }
inline Status outcome_unknown(std::string m = {}) { return Status(StatusCode::OutcomeUnknown, std::move(m)); }
inline Status cancelled(std::string m = {}) { return Status(StatusCode::Cancelled, std::move(m)); }
inline Status shutting_down(std::string m = {}) { return Status(StatusCode::ShuttingDown, std::move(m)); }
inline Status not_ready(std::string m = {}) { return Status(StatusCode::NotReady, std::move(m)); }
inline Status not_found(std::string m = {}) { return Status(StatusCode::NotFound, std::move(m)); }
inline Status internal(std::string m = {}) { return Status(StatusCode::Internal, std::move(m)); }

// A Result carries either a value or a Status. Value semantics; movable.
template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)), status_() {}
  Result(Status status) : value_(std::nullopt), status_(std::move(status)) {}
  Result(T value, Status status) : value_(std::move(value)), status_(std::move(status)) {}

  bool ok() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return ok(); }

  T& value() {
    if (!ok()) throw std::runtime_error("Result::value() on error: " + status_.message);
    return *value_;
  }
  const T& value() const {
    if (!ok()) throw std::runtime_error("Result::value() on error: " + status_.message);
    return *value_;
  }
  // Unchecked access (caller must have checked ok()).
  T& value_unchecked() noexcept { return *value_; }
  const T& value_unchecked() const noexcept { return *value_; }

  const Status& status() const noexcept { return status_; }
  Status&& take_status() { return std::move(status_); }

  std::optional<T>& data() noexcept { return value_; }
  const std::optional<T>& data() const noexcept { return value_; }

 private:
  std::optional<T> value_;
  Status status_;
};

}  // namespace slofabric
