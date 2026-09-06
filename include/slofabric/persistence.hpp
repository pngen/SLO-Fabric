#pragma once

// SLO Fabric - versioned, integrity-checked persistence.
//
// Durable state is written with an explicit format version, canonical
// deterministic encoding, checked lengths and arithmetic, bounded allocation,
// and a CRC-32 integrity footer. Corruption, truncation, trailing garbage, and
// unknown versions are rejected. Saves are transactional: write to a temporary
// file, flush, verify, then atomically rename over the target.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "slofabric/compliance.hpp"
#include "slofabric/contract.hpp"
#include "slofabric/enforcement.hpp"
#include "slofabric/identities.hpp"
#include "slofabric/objective.hpp"
#include "slofabric/policy.hpp"
#include "slofabric/status.hpp"
#include "slofabric/units.hpp"

namespace slofabric {

inline constexpr std::uint32_t kPersistenceMagic = 0x534C4F46u;  // "SLOF"
inline constexpr std::uint32_t kPersistenceVersion = 2;
inline constexpr std::size_t kMaxPersistenceBytes = 256 * 1024 * 1024;  // 256 MiB cap
inline constexpr std::size_t kMaxCollectionItems = 1u << 20;            // 1 Mi cap

// Per-objective budget / state snapshot persisted for resume.
struct PersistedObjectiveState {
  ObjectiveId objective_id;
  ObjectiveGen objective_gen;
  Count budget_total;
  Count budget_consumed;
  Duration budget_window;
  Duration budget_window_start;
  ComplianceState last_state = ComplianceState::InsufficientEvidence;
  Duration last_state_time{0};
  Duration cooldown_until{0};
};

// A canonical decision record for historical replay / auditing.
struct EvaluationRecord {
  EvaluationId evaluation_id;
  EvaluationGen evaluation_gen;
  SloContractGen contract_gen;
  PolicyGen policy_gen;
  EvidenceGen evidence_gen;
  CoordinatorEpoch epoch;
  Duration time;
  ComplianceState compliance = ComplianceState::InsufficientEvidence;
  Dimension binding_dimension = Dimension::Latency;
  ObjectiveId binding_objective;
  EnforcementAction selected_action = EnforcementAction::NoAction;
  std::uint64_t digest = 0;
};

// Everything that should survive a coordinator restart.
struct DurableState {
  std::uint32_t format_version = kPersistenceVersion;
  CoordinatorEpoch epoch;
  std::vector<SloPolicy> policies;
  std::vector<SloContract> contracts;
  std::vector<PersistedObjectiveState> objective_states;
  std::vector<EnforcementReceipt> enforcement_history;
  std::vector<EvaluationRecord> evaluation_history;
};

// Canonical serialization. Deterministic: same state -> same bytes.
std::vector<std::uint8_t> serialize_state(const DurableState& state);

// Deserialize with full validation. Rejects corruption, truncation, trailing
// garbage, unknown version, and oversized/absurd counts.
Result<DurableState> deserialize_state(const std::uint8_t* data, std::size_t n);
inline Result<DurableState> deserialize_state(const std::vector<std::uint8_t>& v) {
  return deserialize_state(v.data(), v.size());
}

// Transactional save: temp -> write -> flush -> verify -> rename.
Status save_state(const std::filesystem::path& path, const DurableState& state);
Status load_state(const std::filesystem::path& path, DurableState& out);

// Save/load a single record for testing small file semantics.
Status save_bytes_atomic(const std::filesystem::path& path,
                         const std::vector<std::uint8_t>& bytes);

}  // namespace slofabric
