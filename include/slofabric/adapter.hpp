#pragma once

// SLO Fabric - narrow typed enforcement adapters.
//
// Each adjacent runtime implements a narrow interface. SLO Fabric emits typed
// intents through the correct interface; invalid cross-action calls are
// impossible because the interface only exposes legal actions for that runtime.
// A DeterministicReferenceAdapter records every dispatch for testing and closed-
// loop verification.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "slofabric/enforcement.hpp"
#include "slofabric/status.hpp"

namespace slofabric {

class IAdmissionEnforcer {
 public:
  virtual ~IAdmissionEnforcer() = default;
  virtual Status admit(const EnforcementIntent& i) = 0;
  virtual Status defer(const EnforcementIntent& i) = 0;
  virtual Status reject(const EnforcementIntent& i) = 0;
  virtual Status throttle(const EnforcementIntent& i) = 0;
};

class ISchedulerEnforcer {
 public:
  virtual ~ISchedulerEnforcer() = default;
  virtual Status change_queue_priority(const EnforcementIntent& i) = 0;
  virtual Status reserve_capacity(const EnforcementIntent& i) = 0;
};

class IResourceEnforcer {
 public:
  virtual ~IResourceEnforcer() = default;
  virtual Status increase_capacity(const EnforcementIntent& i) = 0;
  virtual Status replicate(const EnforcementIntent& i) = 0;
  virtual Status promote_warm_capacity(const EnforcementIntent& i) = 0;
  virtual Status change_placement(const EnforcementIntent& i) = 0;
};

class IRecoveryEnforcer {
 public:
  virtual ~IRecoveryEnforcer() = default;
  virtual Status failover(const EnforcementIntent& i) = 0;
  virtual Status recover(const EnforcementIntent& i) = 0;
  virtual Status replan_recovery(const EnforcementIntent& i) = 0;
  virtual Status drain(const EnforcementIntent& i) = 0;
};

class IMemoryEnforcer {
 public:
  virtual ~IMemoryEnforcer() = default;
  virtual Status reclaim_memory(const EnforcementIntent& i) = 0;
  virtual Status reduce_residency(const EnforcementIntent& i) = 0;
};

class IPreemptionEnforcer {
 public:
  virtual ~IPreemptionEnforcer() = default;
  virtual Status preempt_lower_priority(const EnforcementIntent& i) = 0;
};

class IPowerEnforcer {
 public:
  virtual ~IPowerEnforcer() = default;
  virtual Status limit_power(const EnforcementIntent& i) = 0;
};

// Records the well-formedness of a dispatched intent (all generations present
// and matching) so stale dispatch is detectable before any side effect.
inline Status validate_intent_generations(const EnforcementIntent& i) noexcept {
  if (!i.contract_gen.valid()) return invalid_input("intent missing contract generation");
  if (!i.policy_gen.valid()) return invalid_input("intent missing policy generation");
  if (!i.evaluation_gen.valid()) return invalid_input("intent missing evaluation generation");
  if (!i.epoch.valid()) return invalid_input("intent missing coordinator epoch");
  return ok();
}

// Deterministic reference sink implementing every narrow interface. Used for
// tests, examples, and closed-loop verification. Records all dispatches in
// insertion order and can be configured to reject stale generation fields or
// to simulate a failure.
class ReferenceEnforcementSink final : public IAdmissionEnforcer,
                                       public ISchedulerEnforcer,
                                       public IResourceEnforcer,
                                       public IRecoveryEnforcer,
                                       public IMemoryEnforcer,
                                       public IPreemptionEnforcer,
                                       public IPowerEnforcer {
 public:
  struct DispatchRecord {
    EnforcementAction action;
    EnforcementIntent intent;
    Status status;
    std::uint64_t seq;
  };

  // When set, the sink rejects dispatches whose generation fields do not match
  // the currently authorized generations. This models "stale action rejects".
  void set_authorized_generations(CoordinatorEpoch epoch, SloContractGen cg,
                                  PolicyGen pg, EvaluationGen eg) {
    fence_ = true; epoch_ = epoch; contract_gen_ = cg; policy_gen_ = pg; eval_gen_ = eg;
  }
  void disable_generation_fence() { fence_ = false; }

  Status admit(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Admit, i); }
  Status defer(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Defer, i); }
  Status reject(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Reject, i); }
  Status throttle(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Throttle, i); }
  Status change_queue_priority(const EnforcementIntent& i) override { return dispatch(EnforcementAction::ChangeQueuePriority, i); }
  Status reserve_capacity(const EnforcementIntent& i) override { return dispatch(EnforcementAction::ReserveCapacity, i); }
  Status increase_capacity(const EnforcementIntent& i) override { return dispatch(EnforcementAction::IncreaseCapacity, i); }
  Status replicate(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Replicate, i); }
  Status promote_warm_capacity(const EnforcementIntent& i) override { return dispatch(EnforcementAction::PromoteWarmCapacity, i); }
  Status change_placement(const EnforcementIntent& i) override { return dispatch(EnforcementAction::ChangePlacement, i); }
  Status failover(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Failover, i); }
  Status recover(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Recover, i); }
  Status replan_recovery(const EnforcementIntent& i) override { return dispatch(EnforcementAction::ReplanRecovery, i); }
  Status drain(const EnforcementIntent& i) override { return dispatch(EnforcementAction::Drain, i); }
  Status reclaim_memory(const EnforcementIntent& i) override { return dispatch(EnforcementAction::ReclaimMemory, i); }
  Status reduce_residency(const EnforcementIntent& i) override { return dispatch(EnforcementAction::ReduceResidency, i); }
  Status preempt_lower_priority(const EnforcementIntent& i) override { return dispatch(EnforcementAction::PreemptLowerPriority, i); }
  Status limit_power(const EnforcementIntent& i) override { return dispatch(EnforcementAction::LimitPower, i); }

  const std::vector<DispatchRecord>& records() const noexcept { return records_; }
  std::size_t dispatch_count() const noexcept { return records_.size(); }
  void clear() { records_.clear(); }

  Status dispatch(EnforcementAction action, EnforcementIntent i) {
    Status v = validate_intent_generations(i);
    if (!v.ok()) {
      records_.push_back(DispatchRecord{action, std::move(i), v, ++seq_});
      return v;
    }
    if (fence_) {
      if (i.epoch != epoch_ || i.contract_gen != contract_gen_ ||
          i.policy_gen != policy_gen_ || i.evaluation_gen != eval_gen_) {
        Status s = stale_authority("intent generation does not match current authority");
        records_.push_back(DispatchRecord{action, std::move(i), s, ++seq_});
        return s;
      }
    }
    if (fail_all_) {
      Status s = enforcement_failed("simulated adapter failure");
      records_.push_back(DispatchRecord{action, std::move(i), s, ++seq_});
      return s;
    }
    Status s = ok();
    records_.push_back(DispatchRecord{action, std::move(i), s, ++seq_});
    return s;
  }

  void set_fail_all(bool b) { fail_all_ = b; }

 private:
  std::vector<DispatchRecord> records_;
  std::uint64_t seq_{0};
  bool fence_{false};
  bool fail_all_{false};
  CoordinatorEpoch epoch_;
  SloContractGen contract_gen_;
  PolicyGen policy_gen_;
  EvaluationGen eval_gen_;
};

// Route an intent to the correct narrow interface. Returns the adapter status.
// The action must map to exactly one interface; this is the single place the
// action->interface mapping lives, so invalid dispatches are rejected here.
class EnforcementRouter {
 public:
  IAdmissionEnforcer* admission = nullptr;
  ISchedulerEnforcer* scheduler = nullptr;
  IResourceEnforcer* resource = nullptr;
  IRecoveryEnforcer* recovery = nullptr;
  IMemoryEnforcer* memory = nullptr;
  IPreemptionEnforcer* preemption = nullptr;
  IPowerEnforcer* power = nullptr;

  Status dispatch(const EnforcementIntent& i) const {
    using A = EnforcementAction;
    switch (i.action) {
      case A::Admit: return admission ? admission->admit(i) : not_ready("no admission enforcer");
      case A::Defer: return admission ? admission->defer(i) : not_ready("no admission enforcer");
      case A::Reject: return admission ? admission->reject(i) : not_ready("no admission enforcer");
      case A::Throttle: return admission ? admission->throttle(i) : not_ready("no admission enforcer");
      case A::ChangeQueuePriority: return scheduler ? scheduler->change_queue_priority(i) : not_ready("no scheduler enforcer");
      case A::ReserveCapacity: return scheduler ? scheduler->reserve_capacity(i) : not_ready("no scheduler enforcer");
      case A::IncreaseCapacity: return resource ? resource->increase_capacity(i) : not_ready("no resource enforcer");
      case A::Replicate: return resource ? resource->replicate(i) : not_ready("no resource enforcer");
      case A::PromoteWarmCapacity: return resource ? resource->promote_warm_capacity(i) : not_ready("no resource enforcer");
      case A::ChangePlacement: return resource ? resource->change_placement(i) : not_ready("no resource enforcer");
      case A::Failover: return recovery ? recovery->failover(i) : not_ready("no recovery enforcer");
      case A::Recover: return recovery ? recovery->recover(i) : not_ready("no recovery enforcer");
      case A::ReplanRecovery: return recovery ? recovery->replan_recovery(i) : not_ready("no recovery enforcer");
      case A::Drain: return recovery ? recovery->drain(i) : not_ready("no recovery enforcer");
      case A::ReclaimMemory: return memory ? memory->reclaim_memory(i) : not_ready("no memory enforcer");
      case A::ReduceResidency: return memory ? memory->reduce_residency(i) : not_ready("no memory enforcer");
      case A::PreemptLowerPriority: return preemption ? preemption->preempt_lower_priority(i) : not_ready("no preemption enforcer");
      case A::LimitPower: return power ? power->limit_power(i) : not_ready("no power enforcer");
      case A::NoAction: return ok();
      case A::ReduceBatchSize:
      case A::IncreaseBatchSize:
      case A::Escalate:
      case A::ManualInterventionRequired:
        return ok();
    }
    return invalid_input("unmapped enforcement action");
  }
};

}  // namespace slofabric
