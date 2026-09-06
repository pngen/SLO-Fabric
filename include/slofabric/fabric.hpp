#pragma once

// SLO Fabric - public facade.
//
// The SloFabric runtime owns contract definition, measurement (via evidence),
// evaluation, prediction, violation state, enforcement authority, action
// selection, recovery toward compliance, and historical accounting. It does NOT
// own the adjacent runtime mechanisms (scheduling, placement, memory
// reclamation, recovery execution, cost planning); it emits typed, generation-
// bound enforcement intents to those runtimes through adapters.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "slofabric/adapter.hpp"
#include "slofabric/clock.hpp"
#include "slofabric/contract.hpp"
#include "slofabric/enforcement.hpp"
#include "slofabric/evidence.hpp"
#include "slofabric/explanation.hpp"
#include "slofabric/identities.hpp"
#include "slofabric/persistence.hpp"
#include "slofabric/policy.hpp"
#include "slofabric/status.hpp"
#include "slofabric/units.hpp"

namespace slofabric {

// The complete, inspectable output of one evaluation.
struct EvaluationResult {
  Status status;
  Explanation explanation;   // typed, deterministic, fully inspectable
  EnforcementIntent intent;  // the authorized intent (generation-bound)
};

// Recovery lifecycle phases for recovery-time objectives. These model the
// authoritative boundaries provided by Recovery Planner / Failover Fabric
// evidence; SLO Fabric evaluates the recovery-time objective without
// duplicating recovery planning.
enum class RecoveryEventPhase : std::uint8_t {
  Observed,
  Authoritative,
  Initiated,
  Restored,
  Verified,
};

// A snapshot of objective tracking state, exposed for inspection / CLI.
struct ObjectiveStatus {
  Objective objective;
  ComplianceState state;
  std::optional<ObjectiveValue> current_value;
  Freshness freshness;
  Count sample_count;
  std::optional<BudgetState> budget_state;
  std::optional<Count> budget_consumed;
};

// Thread-safety contract: SloFabric is internally thread-safe. All public methods that
// read or mutate shared state serialize through one internal mutex, so a single instance
// may be used concurrently by multiple threads. No public method holds the lock while making
// an external call (dispatch) or blocking I/O (save/load), and no public method re-enters the
// lock. See README "Thread safety".
class SloFabric {
 public:
  SloFabric();
  explicit SloFabric(std::shared_ptr<IClock> clock);
  ~SloFabric();
  SloFabric(SloFabric&&) noexcept;
  SloFabric& operator=(SloFabric&&) noexcept;
  SloFabric(const SloFabric&) = delete;
  SloFabric& operator=(const SloFabric&) = delete;

  void set_clock(std::shared_ptr<IClock> clock) noexcept;

  // ---- Policy / contract lifecycle ----
  Status set_policy(SloPolicy policy);
  Status add_contract(const SloContract& contract);
  Status activate_contract(SloContractId id, Duration now);
  Status suspend_contract(SloContractId id, Duration now);
  Status expire_contract(SloContractId id, Duration now);
  Status supersede_contract(const SloContract& replacement, Duration now);
  Status retire_contract(SloContractId id, Duration now);
  std::optional<SloContract> find_contract(SloContractId id) const;

  // ---- Evidence ----
  // Ingest typed evidence for an objective. Deduplicated; stale generation
  // rejected. Returns provenance-aware status.
  Status ingest(const EvidenceRecord& evidence, Duration now);
  // Ingest an explicit violation event (for budget-backed objectives).
  Status ingest_violation_event(ObjectiveId objective_id, Duration now);
  // Record a recovery lifecycle boundary for a recovery-time objective.
  Status record_recovery_event(ObjectiveId objective_id, RecoveryEventPhase phase, Duration time);
  // Attach an explicit error budget to an objective (exact, deduplicated).
  Status set_objective_budget(ObjectiveId objective_id, Count total, Duration window);

  // ---- Evaluation ----
  Result<EvaluationResult> evaluate(ServiceId service, WorkloadId workload, Duration now);

  // ---- Enforcement lifecycle ----
  Result<EnforcementReceipt> authorize(const EnforcementIntent& intent, Duration now);
  Status dispatch(const EnforcementIntent& intent, ReferenceEnforcementSink& sink);
  Status transition(EnforcementId id, EnforcementLifecycle to, Duration now);
  Status complete_enforcement(EnforcementId id, Duration now);

  // ---- Persistence ----
  Status save(const std::filesystem::path& path) const;
  Status load(const std::filesystem::path& path);

  // ---- Inspection ----
  CoordinatorEpoch current_epoch() const noexcept;
  std::vector<ObjectiveStatus> objective_statuses(SloContractId id) const;
  std::size_t contract_count() const noexcept;
  std::size_t evaluation_count() const noexcept;
  // Historical replay: reconstruct the decision for a stored record.
  Result<EvaluationRecord> get_evaluation(EvaluationId id) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace slofabric
