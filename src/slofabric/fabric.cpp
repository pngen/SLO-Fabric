#include "slofabric/fabric.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "slofabric/budget.hpp"
#include "slofabric/persistence.hpp"
#include "slofabric/sample_window.hpp"

namespace slofabric {
namespace {

constexpr double kMargin = 0.05;
constexpr double kEpsilon = 1e-9;

ObjectiveValue make_value(UnitKind u, double v) {
  switch (u) {
    case UnitKind::Nanoseconds: return Duration(static_cast<std::int64_t>(v));
    case UnitKind::Bytes: return ByteCount(static_cast<std::int64_t>(v));
    case UnitKind::Count: return Count(static_cast<std::int64_t>(v));
    case UnitKind::BasisPoints: return BasisPoints(static_cast<std::int64_t>(v));
    case UnitKind::Percent: return Percentage(v);
    case UnitKind::PressureRatio: return PressureRatio(v);
    case UnitKind::RequestsPerSecond: return RequestsPerSecond(v);
    case UnitKind::TokensPerSecond: return TokensPerSecond(v);
    case UnitKind::BytesPerSecond: return BytesPerSecond(v);
    case UnitKind::OperationsPerSecond: return OperationsPerSecond(v);
    case UnitKind::CurrencyMicros: return CostMicros(static_cast<std::int64_t>(v));
    case UnitKind::EnergyJoules: return EnergyJoules(static_cast<std::int64_t>(v));
    case UnitKind::Microseconds: return Duration(static_cast<std::int64_t>(v * 1000.0));
    case UnitKind::Milliseconds: return Duration(static_cast<std::int64_t>(v * 1e6));
    case UnitKind::Seconds: return Duration(static_cast<std::int64_t>(v * 1e9));
    case UnitKind::KiloBytes: return ByteCount(static_cast<std::int64_t>(v * 1024.0));
    case UnitKind::MegaBytes: return ByteCount(static_cast<std::int64_t>(v * 1024.0 * 1024.0));
    case UnitKind::GigaBytes: return ByteCount(static_cast<std::int64_t>(v * 1024.0 * 1024.0 * 1024.0));
  }
  return Count(0);
}

struct AvailabilityAccount {
  AvailabilityMode mode = AvailabilityMode::Time;
  Duration eligible_time{0};
  Duration serving_time{0};
  Count eligible{0};
  Count successful{0};
  bool has_denominator() const noexcept {
    if (mode == AvailabilityMode::Time) return eligible_time.as_ns() > 0;
    return eligible.value() > 0;
  }
  double ratio() const noexcept {
    if (mode == AvailabilityMode::Time) {
      if (eligible_time.as_ns() <= 0) return 0.0;
      return static_cast<double>(serving_time.as_ns()) / static_cast<double>(eligible_time.as_ns());
    }
    if (eligible.value() <= 0) return 0.0;
    return static_cast<double>(successful.value()) / static_cast<double>(eligible.value());
  }
};

struct CostWindow {
  std::deque<std::pair<Duration, double>> items;
  std::size_t cap = 4096;
  std::int64_t cap_ns = 0;
  void add(Duration t, double cost, std::int64_t window_ns) {
    cap_ns = window_ns;
    items.push_back({t, cost});
    evict(t);
  }
  void evict(Duration now) {
    while (!items.empty() && cap_ns > 0 && now.as_ns() - items.front().first.as_ns() > cap_ns) items.pop_front();
    while (items.size() > cap) items.pop_front();
  }
  double sum() const noexcept { double s = 0; for (auto& p : items) s += p.second; return s; }
  void reset() { items.clear(); cap_ns = 0; }
  std::size_t size() const noexcept { return items.size(); }
};

struct RecoveryTracker {
  bool observed = false;
  bool authoritative = false;
  bool initiated = false;
  bool restored = false;
  bool verified = false;
  Duration authoritative_at{0};
  Duration restored_at{0};
  Duration verified_at{0};
  bool active() const noexcept { return authoritative && !verified; }
  Duration current_recovery_duration(Duration now) const noexcept {
    if (!authoritative) return Duration(0);
    Duration end = verified ? verified_at : (restored ? restored_at : now);
    return Duration(end.as_ns() - authoritative_at.as_ns());
  }
};

struct ObjectiveAccumulator {
  Objective def;
  SampleWindow window{WindowType::Sliding, Duration(0), 64};
  bool uses_window = false;
  bool uses_availability = false;
  bool uses_cost = false;
  bool uses_recovery = false;
  AvailabilityAccount avail;
  CostWindow cost;
  RecoveryTracker recovery;
  bool has_budget = false;
  ErrorBudget budget{Count(0), Duration(0)};
  ComplianceState last_state = ComplianceState::InsufficientEvidence;
  Duration last_state_time{0};
  Duration cooldown_until{0};
  std::deque<EvidenceId> recent;
  std::unordered_set<EvidenceId> recent_set;
  std::size_t dedup_cap = 4096;
  std::uint64_t next_seq = 1;
  bool seen(EvidenceId id) const { return recent_set.find(id) != recent_set.end(); }
  void remember(EvidenceId id) {
    if (recent.size() >= dedup_cap) { auto old = recent.front(); recent.pop_front(); recent_set.erase(old); }
    recent.push_back(id); recent_set.insert(id);
  }
  void rebuild_window(const Objective& o) {
    WindowType wt = o.window_type;
    if (wt != WindowType::Instantaneous && wt != WindowType::RollingCount &&
        wt != WindowType::Tumbling && wt != WindowType::FixedInterval && wt != WindowType::Sliding)
      wt = WindowType::Sliding;
    window = SampleWindow(wt, o.window_duration,
                          static_cast<std::size_t>(std::max<std::int64_t>(1, o.window_count.value())));
  }
  void route_for(const Objective& o) {
    uses_window = uses_availability = uses_cost = uses_recovery = false;
    switch (o.dim) {
      case Dimension::Availability: uses_availability = true; break;
      case Dimension::Cost: uses_cost = true; break;
      case Dimension::RecoveryTime: uses_recovery = true; break;
      default: uses_window = true; break;
    }
    avail.mode = o.availability_mode;
    rebuild_window(o);
  }
  explicit ObjectiveAccumulator(const Objective& o) : def(o) { route_for(o); }
};

void reset_for_new_gen(ObjectiveAccumulator& a, const Objective& o) {
  a.window.reset(); a.avail = AvailabilityAccount(); a.cost = CostWindow(); a.recovery = RecoveryTracker();
  a.last_state = ComplianceState::InsufficientEvidence; a.last_state_time = Duration(0);
  a.cooldown_until = Duration(0); a.recent.clear(); a.recent_set.clear();
  a.next_seq = 1;
  a.def = o; a.route_for(o);
}

}  // end anonymous namespace

struct SloFabric::Impl {
  std::shared_ptr<IClock> clock;
  CoordinatorEpoch epoch{1};
  std::map<PolicyId, SloPolicy> policies;
  std::map<SloContractId, SloContract> contracts;
  std::unordered_map<ObjectiveId, ObjectiveAccumulator> accumulators;
  std::vector<EnforcementReceipt> enforcement_history;
  std::vector<EvaluationRecord> evaluation_history;
  std::unordered_map<EvaluationId, EvaluationRecord> evaluation_by_id;
  std::unordered_set<EvidenceId> global_evidence;
  std::deque<EvidenceId> global_evidence_ring;
  std::size_t evidence_ring_cap = 8192;
  std::uint64_t evaluation_counter = 0;
  std::uint64_t enforcement_counter = 0;
  std::uint64_t dispatch_counter = 0;

  explicit Impl(std::shared_ptr<IClock> c) : clock(std::move(c)) {
    if (!clock) clock = std::make_shared<MonotonicClock>();
  }

  Status note_evidence(EvidenceId id) {
    if (id.invalid()) return invalid_input("evidence id required");
    if (global_evidence.find(id) != global_evidence.end()) return cancelled("duplicate evidence id");
    if (global_evidence_ring.size() >= evidence_ring_cap) {
      auto old = global_evidence_ring.front();
      global_evidence_ring.pop_front();
      global_evidence.erase(old);
    }
    global_evidence_ring.push_back(id);
    global_evidence.insert(id);
    return ok();
  }

  ObjectiveAccumulator& get_accumulator(const Objective& o) {
    auto it = accumulators.find(o.id);
    if (it == accumulators.end()) {
      ObjectiveAccumulator acc{o};
      acc.route_for(o);
      it = accumulators.emplace(o.id, std::move(acc)).first;
    } else if (it->second.def.gen != o.gen) {
      reset_for_new_gen(it->second, o);
    }
    return it->second;
  }
};

// Freshness of evidence in an accumulator relative to now.
Freshness evaluate_freshness(const ObjectiveAccumulator& acc, Duration now) {
  const Objective& o = acc.def;
  if (acc.uses_availability) {
    bool has = acc.avail.has_denominator();
    if (!has) return Freshness::Expired;
    return Freshness::Current;
  }
  if (acc.uses_recovery) {
    if (!acc.recovery.authoritative) return Freshness::Expired;
    return Freshness::Current;
  }
  if (acc.uses_cost) {
    if (acc.cost.items.empty()) return Freshness::Expired;
    return Freshness::Current;
  }
  // Window-valued objectives.
  if (acc.window.empty()) return Freshness::Expired;
  auto snap = acc.window.snapshot();
  Duration latest = snap.back().time;
  std::int64_t age = now.as_ns() - latest.as_ns();
  std::int64_t ttl = o.freshness_ttl.as_ns();
  if (age < 0) return Freshness::Stale;   // future-dated -> not trusted
  if (ttl <= 0) {
    // default: current if within the evaluation window duration.
    std::int64_t w = o.window_duration.as_ns();
    if (w > 0 && age >= w) return Freshness::Stale;
    if (w <= 0) return Freshness::Current;
    return Freshness::Current;
  }
  if (age > ttl) return Freshness::Stale;
  return Freshness::Current;
}

// Compute the measured value for an objective from its accumulator.
bool compute_measured(ObjectiveAccumulator& acc, Duration now, double& value,
                      UnitKind& unit, bool& sufficient, Count& samples,
                      std::string& detail) {
  const Objective& o = acc.def;
  unit = expected_unit_for(o.dim);
  sufficient = false;
  samples = Count(0);
  value = 0.0;
  if (o.dim == Dimension::Availability) {
    if (!acc.avail.has_denominator()) { detail = "no eligible denominator"; return false; }
    value = acc.avail.ratio() * 10000.0;  // final is basis points
    unit = UnitKind::BasisPoints;
    sufficient = acc.avail.has_denominator();
    samples = acc.avail.mode == AvailabilityMode::Time ? Count(1) : acc.avail.eligible;
    return true;
  }
  if (o.dim == Dimension::RecoveryTime) {
    if (!acc.recovery.authoritative) { detail = "no authoritative recovery boundary"; return false; }
    value = static_cast<double>(acc.recovery.current_recovery_duration(now).as_ns());
    sufficient = true; samples = Count(1);
    return true;
  }
  if (o.dim == Dimension::Cost) {
    if (o.kind == ObjectiveKind::CostWindowBudget) {
      if (acc.cost.items.empty()) { detail = "no cost evidence"; return false; }
      value = acc.cost.sum();
      sufficient = acc.cost.size() >= 1;
      samples = Count(static_cast<std::int64_t>(acc.cost.size()));
      return true;
    }
    if (acc.window.empty()) { detail = "no cost evidence"; return false; }
    value = acc.window.mean();
    sufficient = acc.window.size() >= static_cast<std::size_t>(o.min_evidence.value());
    samples = Count(static_cast<std::int64_t>(acc.window.size()));
    return true;
  }
  // Window-valued (latency, throughput, memory pressure).
  if (acc.window.empty()) { detail = "no evidence"; return false; }
  samples = Count(static_cast<std::int64_t>(acc.window.size()));
  std::size_t min_ev = static_cast<std::size_t>(std::max<std::int64_t>(1, o.min_evidence.value()));
  if (acc.window.size() < min_ev) { detail = "insufficient samples"; return false; }
  switch (o.dim) {
    case Dimension::Latency:
      if (o.kind == ObjectiveKind::LatencyPercentile) {
        if (!acc.window.quantile_sufficient(o.percentile_rank)) { detail = "insufficient percentile evidence"; return false; }
        value = acc.window.quantile(o.percentile_rank);
      } else if (o.kind == ObjectiveKind::LatencyMean) {
        value = acc.window.mean();
      } else {
        value = acc.window.max();
      }
      break;
    case Dimension::Throughput:
      value = acc.window.mean();
      break;
    case Dimension::MemoryPressure:
      if (o.kind == ObjectiveKind::MemoryMinHeadroom) value = acc.window.min();
      else value = acc.window.max();
      break;
    default:
      value = acc.window.mean();
      break;
  }
  sufficient = true;
  return true;
}

// Zone classification with explicit hysteresis.
enum class Zone { Violation, Near, Compliant };

double numeric_limit(const Objective& o, bool breach) {
  if (breach && o.breach_threshold) return numeric_of(*o.breach_threshold);
  if (!breach && o.recovery_threshold) return numeric_of(*o.recovery_threshold);
  return numeric_of(o.target);
}

double physical_max(UnitKind u) noexcept {
  switch (u) {
    case UnitKind::BasisPoints: return 10000.0;
    case UnitKind::Percent: return 100.0;
    case UnitKind::PressureRatio: return 1000000.0;
    default: return 0.0;  // no natural absolute cap
  }
}

Zone classify_zone(const Objective& o, double measured) {
  double breach = numeric_limit(o, true);
  double recovery = numeric_limit(o, false);
  bool hyst = breach != recovery;
  double cap = physical_max(unit_kind_of(o.target));
  if (o.comparison == Comparison::AtMost) {
    if (measured > breach + kEpsilon) return Zone::Violation;
    if (hyst) { if (measured > recovery + kEpsilon) return Zone::Near; return Zone::Compliant; }
    if (measured > breach * (1.0 - kMargin)) return Zone::Near;
    return Zone::Compliant;
  }
  if (measured < breach - kEpsilon) return Zone::Violation;
  if (hyst) { if (measured < recovery - kEpsilon) return Zone::Near; return Zone::Compliant; }
  if (measured < breach * (1.0 + kMargin) && (cap <= 0.0 || measured < cap)) return Zone::Near;
  return Zone::Compliant;
}

ComplianceState step_state(const Objective& o, double measured, ComplianceState last) {
  Zone z = classify_zone(o, measured);
  switch (z) {
    case Zone::Violation: return ComplianceState::Violating;
    case Zone::Compliant: return ComplianceState::Compliant;
    case Zone::Near:
      if (last == ComplianceState::Violating || last == ComplianceState::Recovering) return last;
      return ComplianceState::NearLimit;
  }
  return ComplianceState::InsufficientEvidence;
}

// Deterministic linear-fit trend prediction over window samples (ns time vs value).
PredictedRisk predict_risk(const Objective& o, const ObjectiveAccumulator& acc,
                           Duration now, double measured, double breach) {
  if (o.dim == Dimension::Cost && o.kind == ObjectiveKind::CostWindowBudget) {
    if (acc.cost.items.empty()) return PredictedRisk::None;
    double target = numeric_of(o.target);
    Duration first = acc.cost.items.front().first;
    Duration span = Duration(now.as_ns() - first.as_ns());
    double frac = span.as_ns() > 0 ? static_cast<double>(now.as_ns() - first.as_ns()) /
                                        static_cast<double>(std::max<std::int64_t>(1, o.window_duration.as_ns())) : 1.0;
    if (frac <= 0.0) frac = 1.0;
    double projected = acc.cost.sum() / frac;
    if (projected > target * 1.5) return PredictedRisk::High;
    if (projected > target) return PredictedRisk::Elevated;
    return PredictedRisk::None;
  }
  if (o.dim == Dimension::Latency || o.dim == Dimension::MemoryPressure) {
    if (acc.window.size() < 4) return PredictedRisk::None;
    if (o.comparison != Comparison::AtMost) return PredictedRisk::None;
    auto snap = acc.window.snapshot();
    double n = static_cast<double>(snap.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    double x0 = static_cast<double>(snap.front().time.as_ns());
    for (auto& s : snap) {
      double x = static_cast<double>(s.time.as_ns() - x0);
      sx += x; sy += s.value; sxx += x * x; sxy += x * s.value;
    }
    double denom = n * sxx - sx * sx;
    if (denom == 0.0) return PredictedRisk::None;
    double slope = (n * sxy - sx * sy) / denom;
    if (slope <= 0.0) return PredictedRisk::None;   // improving or flat
    double horizon_ns = static_cast<double>(o.window_duration.as_ns() > 0 ? o.window_duration.as_ns() : 60000000000LL);
    double projected = measured + slope * horizon_ns;
    if (projected > breach * 1.5) return PredictedRisk::High;
    if (projected > breach) return PredictedRisk::Elevated;
    return PredictedRisk::None;
  }
  return PredictedRisk::None;
}

EnforcementAction action_for(const Objective& o, ComplianceState state) {
  if (state == ComplianceState::Violating || state == ComplianceState::Recovering) {
    switch (o.dim) {
      case Dimension::Latency: return EnforcementAction::IncreaseCapacity;
      case Dimension::Availability: return EnforcementAction::Replicate;
      case Dimension::Throughput: return EnforcementAction::IncreaseCapacity;
      case Dimension::RecoveryTime: return EnforcementAction::Failover;
      case Dimension::MemoryPressure: return EnforcementAction::ReclaimMemory;
      case Dimension::Cost: return EnforcementAction::ManualInterventionRequired;
    }
  }
  if (state == ComplianceState::AtRisk || state == ComplianceState::NearLimit) {
    switch (o.dim) {
      case Dimension::Latency: return EnforcementAction::PromoteWarmCapacity;
      case Dimension::Availability: return EnforcementAction::Replicate;
      case Dimension::Throughput: return EnforcementAction::IncreaseBatchSize;
      case Dimension::RecoveryTime: return EnforcementAction::ReplanRecovery;
      case Dimension::MemoryPressure: return EnforcementAction::ReduceResidency;
      case Dimension::Cost: return EnforcementAction::ManualInterventionRequired;
    }
  }
  return EnforcementAction::NoAction;
}

std::string severity_reason(const Objective& o, ComplianceState state) {
  (void)o;
  using C = ComplianceState;
  if (state == C::Violating) return "current violation; enforcement required";
  if (state == C::Recovering) return "violation detected; recovery in flight";
  if (state == C::AtRisk) return "predicted violation; preventive action"; 
  if (state == C::NearLimit) return "approaching breach threshold";
  if (state == C::InsufficientEvidence) return "insufficient evidence";
  if (state == C::RevalidationRequired) return "evidence requires revalidation";
  if (state == C::Suspended) return "contract suspended";
  return "compliant";
}

// ---------------------------------------------------------------------------
// Construction / clock
// ---------------------------------------------------------------------------
SloFabric::SloFabric() : impl_(std::make_unique<Impl>(std::make_shared<MonotonicClock>())) {}
SloFabric::SloFabric(std::shared_ptr<IClock> c) : impl_(std::make_unique<Impl>(std::move(c))) {}
SloFabric::~SloFabric() = default;
SloFabric::SloFabric(SloFabric&&) noexcept = default;
SloFabric& SloFabric::operator=(SloFabric&&) noexcept = default;
void SloFabric::set_clock(std::shared_ptr<IClock> c) noexcept { impl_->clock = std::move(c); if (!impl_->clock) impl_->clock = std::make_shared<MonotonicClock>(); }

// ---------------------------------------------------------------------------
// Policy / contract lifecycle
// ---------------------------------------------------------------------------
Status SloFabric::set_policy(SloPolicy policy) {
  auto it = impl_->policies.find(policy.id);
  if (it != impl_->policies.end() && it->second.gen >= policy.gen)
    return policy_superseded("policy generation must increase");
  impl_->policies[policy.id] = std::move(policy);
  return ok();
}

Status SloFabric::add_contract(const SloContract& contract) {
  if (contract.objectives.empty()) return objective_invalid("contract has no objectives");
  for (const auto& o : contract.objectives) {
    if (!o.id.valid()) return objective_invalid("objective id required");
    if (!o.gen.valid()) return objective_invalid("objective generation required");
    if (!o.target_unit_matches_dimension()) return unit_mismatch("objective target unit does not match dimension");
  }
  auto it = impl_->contracts.find(contract.id);
  if (it != impl_->contracts.end()) {
    if (it->second.gen >= contract.gen) return invalid_input("contract generation must increase");
    it->second.lifecycle = ContractLifecycle::Superseded;
  }
  impl_->contracts[contract.id] = contract;
  return ok();
}

Status SloFabric::activate_contract(SloContractId id, Duration now) {
  auto it = impl_->contracts.find(id);
  if (it == impl_->contracts.end()) return not_found("unknown contract");
  it->second.lifecycle = ContractLifecycle::Active;
  if (it->second.effective_from > now) it->second.effective_from = now;
  return ok();
}
Status SloFabric::suspend_contract(SloContractId id, Duration now) {
  (void)now;
  auto it = impl_->contracts.find(id);
  if (it == impl_->contracts.end()) return not_found("unknown contract");
  it->second.lifecycle = ContractLifecycle::Suspended;
  return ok();
}
Status SloFabric::expire_contract(SloContractId id, Duration now) {
  auto it = impl_->contracts.find(id);
  if (it == impl_->contracts.end()) return not_found("unknown contract");
  it->second.lifecycle = ContractLifecycle::Expired;
  it->second.expiration = it->second.expiration.has_value() ? it->second.expiration : now;
  return ok();
}
Status SloFabric::supersede_contract(const SloContract& replacement, Duration now) {
  (void)now;
  auto it = impl_->contracts.find(replacement.id);
  if (it != impl_->contracts.end()) {
    if (it->second.gen >= replacement.gen) return invalid_input("contract generation must increase");
    it->second.lifecycle = ContractLifecycle::Superseded;
  }
  impl_->contracts[replacement.id] = replacement;
  return ok();
}
Status SloFabric::retire_contract(SloContractId id, Duration now) {
  (void)now;
  auto it = impl_->contracts.find(id);
  if (it == impl_->contracts.end()) return not_found("unknown contract");
  it->second.lifecycle = ContractLifecycle::Retired;
  return ok();
}
std::optional<SloContract> SloFabric::find_contract(SloContractId id) const {
  auto it = impl_->contracts.find(id);
  if (it == impl_->contracts.end()) return std::nullopt;
  return it->second;
}

// ---------------------------------------------------------------------------
// Evidence ingestion
// ---------------------------------------------------------------------------
namespace {
bool unit_compatible(const Objective& o, const EvidenceRecord& e) {
  UnitKind eu = unit_kind_of(e.value);
  if (o.dim == Dimension::Throughput) {
    switch (o.rate_tag) {
      case RateTag::Requests: return eu == UnitKind::RequestsPerSecond;
      case RateTag::Tokens: return eu == UnitKind::TokensPerSecond;
      case RateTag::Bytes: return eu == UnitKind::BytesPerSecond;
      case RateTag::Operations: return eu == UnitKind::OperationsPerSecond;
    }
  }
  if (o.dim == Dimension::MemoryPressure && o.kind == ObjectiveKind::MemoryMinHeadroom)
    return eu == UnitKind::Bytes;
  if (o.dim == Dimension::Cost) return eu == UnitKind::CurrencyMicros;
  if (o.dim == Dimension::Availability) return eu == UnitKind::BasisPoints || eu == UnitKind::Percent;
  return eu == expected_unit_for(o.dim);
}
}  // namespace

Status SloFabric::ingest(const EvidenceRecord& e, Duration now) {
  (void)now;
  Status n = impl_->note_evidence(e.id);
  if (!n.ok()) return n;
  if (e.epoch.valid() && e.epoch != impl_->epoch)
    return stale_authority("evidence coordinator epoch does not match current");
  const Objective* def = nullptr;
  for (auto& cid : impl_->contracts)
    for (auto& o : cid.second.objectives)
      if (o.id == e.objective_id) { if (!def || o.gen > def->gen) def = &o; }
  if (!def) return objective_invalid("unknown objective");
  if (e.objective_gen.valid() && e.objective_gen != def->gen)
    return stale_authority("objective generation mismatch");
  ObjectiveAccumulator& acc = impl_->get_accumulator(*def);
  if (acc.seen(e.id)) return cancelled("duplicate evidence");
  acc.remember(e.id);
  if (!unit_compatible(*def, e)) return unit_mismatch("evidence unit does not match objective");

  if (e.dimension == Dimension::RecoveryTime)
    return invalid_input("use record_recovery_event for recovery-time objectives");

  switch (e.dimension) {
    case Dimension::Latency:
    case Dimension::Throughput:
    case Dimension::MemoryPressure: {
      IngestStatus st = acc.window.ingest(e.time, numeric_of(e.value), acc.next_seq++);
      switch (st) {
        case IngestStatus::Accepted: return ok();
        case IngestStatus::Duplicate: return cancelled("duplicate sample");
        case IngestStatus::Rollback: return stale_evidence("clock rollback / reordered sample rejected");
        case IngestStatus::Late: return invalid_input("late sample for closed window");
        case IngestStatus::Invalid: return invalid_input("invalid sample");
        case IngestStatus::Closed: return invalid_input("window closed");
      }
      return internal();
    }
    case Dimension::Availability: {
      double bp = numeric_of(e.value);
      std::int64_t weight = std::max<std::int64_t>(1, e.sample_count.value());
      double serving_units = (bp / 10000.0) * static_cast<double>(weight);
      if (def->availability_mode == AvailabilityMode::Time) {
        acc.avail.eligible_time = Duration(acc.avail.eligible_time.as_ns() + weight);
        acc.avail.serving_time = Duration(acc.avail.serving_time.as_ns() + static_cast<std::int64_t>(serving_units));
      } else {
        acc.avail.eligible = Count(acc.avail.eligible.value() + weight);
        acc.avail.successful = Count(acc.avail.successful.value() + static_cast<std::int64_t>(serving_units));
      }
      return ok();
    }
    case Dimension::Cost: {
      if (def->kind == ObjectiveKind::CostWindowBudget)
        acc.cost.add(e.time, numeric_of(e.value), def->window_duration.as_ns());
      else
        acc.window.ingest(e.time, numeric_of(e.value), acc.next_seq++);
      return ok();
    }
    default:
      return invalid_input("unsupported dimension");
  }
}

Status SloFabric::record_recovery_event(ObjectiveId id, RecoveryEventPhase phase, Duration time) {
  ObjectiveAccumulator* acc = nullptr;
  const Objective* def = nullptr;
  for (auto& cid : impl_->contracts)
    for (auto& o : cid.second.objectives)
      if (o.id == id) { if (!def || o.gen > def->gen) def = &o; }
  if (!def) return objective_invalid("unknown recovery objective");
  acc = &impl_->get_accumulator(*def);
  acc->recovery.observed = true;
  switch (phase) {
    case RecoveryEventPhase::Observed: break;  // observed already set
    case RecoveryEventPhase::Authoritative: acc->recovery.authoritative = true; acc->recovery.authoritative_at = time; break;
    case RecoveryEventPhase::Initiated: acc->recovery.initiated = true; break;
    case RecoveryEventPhase::Restored: acc->recovery.restored = true; acc->recovery.restored_at = time; break;
    case RecoveryEventPhase::Verified: acc->recovery.verified = true; acc->recovery.verified_at = time; break;
  }
  return ok();
}

Status SloFabric::ingest_violation_event(ObjectiveId id, Duration now) {
  (void)now;
  ObjectiveAccumulator* acc = nullptr;
  for (auto& cid : impl_->contracts)
    for (auto& o : cid.second.objectives)
      if (o.id == id) { acc = &impl_->get_accumulator(o); break; }
  if (!acc) return objective_invalid("unknown objective");
  if (!acc->has_budget) return invalid_input("objective has no budget");
  acc->budget.consume();
  return ok();
}

// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------
Status SloFabric::set_objective_budget(ObjectiveId id, Count total, Duration window) {
  const Objective* def = nullptr;
  for (auto& cid : impl_->contracts)
    for (auto& o : cid.second.objectives)
      if (o.id == id) { if (!def || o.gen > def->gen) def = &o; }
  if (!def) return objective_invalid("unknown objective");
  ObjectiveAccumulator& acc = impl_->get_accumulator(*def);
  if (total.value() < 0) return invalid_input("budget total must be non-negative");
  acc.has_budget = true;
  acc.budget = ErrorBudget(total, window);
  return ok();
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
namespace {
struct ObjEval {
  const Objective* obj;
  ObjectiveState st;
  double measured = 0.0;
  bool sufficient = false;
  ComplianceState state = ComplianceState::InsufficientEvidence;
  PredictedRisk risk = PredictedRisk::None;
  int severity_rank = 99;
  double magnitude = 0.0;
};

int severity_rank_of(ComplianceState s) {
  using C = ComplianceState;
  switch (s) {
    case C::Violating: return 0;
    case C::Recovering: return 1;
    case C::AtRisk: return 2;
    case C::NearLimit: return 3;
    case C::Compliant: return 4;
    case C::InsufficientEvidence: return 5;
    case C::RevalidationRequired: return 6;
    case C::Suspended: return 7;
  }
  return 99;
}

double severity_magnitude(const Objective& o, double measured) {
  double breach = numeric_limit(o, true);
  if (breach <= 0) return 0.0;
  if (o.comparison == Comparison::AtMost) return measured / breach;
  return breach / (measured > 0 ? measured : 1.0);
}

std::uint64_t stable_hash(const std::string& s) {
  std::uint64_t h = 14695981039346656037ULL;
  for (char c : s) { h ^= static_cast<std::uint8_t>(c); h *= 1099511628211ULL; }
  return h;
}
}  // namespace

Result<EvaluationResult> SloFabric::evaluate(ServiceId service, WorkloadId workload, Duration now) {
  const SloContract* c = nullptr;
  for (auto& kv : impl_->contracts) {
    const SloContract& cand = kv.second;
    if (cand.service == service && cand.workload == workload && cand.can_authorize(now)) {
      if (!c || cand.gen > c->gen) c = &cand;
    }
  }
  if (!c) return Result<EvaluationResult>(contract_inactive("no active contract for scope"));
  auto pit = impl_->policies.find(c->policy_id);
  if (pit == impl_->policies.end() || pit->second.gen != c->policy_gen)
    return Result<EvaluationResult>(policy_superseded("policy generation does not match contract"));
  const SloPolicy& policy = pit->second;

  EvaluationResult res;
  res.status = ok();
  Explanation& ex = res.explanation;
  ex.contract_id = c->id; ex.contract_gen = c->gen;
  ex.policy_id = policy.id; ex.policy_gen = policy.gen;
  ex.service = service; ex.workload = workload;
  ex.epoch = impl_->epoch;
  ex.evaluation_id = EvaluationId(impl_->evaluation_counter + 1);
  ex.evaluation_gen = EvaluationGen(impl_->evaluation_counter + 1);
  ex.workload_gen = WorkloadGen(1);
  ++impl_->evaluation_counter;

  std::vector<ObjEval> evals;
  std::string digest_input = std::to_string(c->gen.value) + "|" + std::to_string(policy.gen.value) + "|";

  for (const auto& obj : c->objectives) {
    ObjectiveAccumulator& acc = impl_->get_accumulator(obj);
    ObjEval ev;
    ev.obj = &obj;
    ev.st.id = obj.id; ev.st.gen = obj.gen; ev.st.name = obj.name;
    ev.st.dim = obj.dim; ev.st.kind = obj.kind;
    ev.st.target = obj.target;
    ev.st.breach_threshold = obj.breach_threshold;
    ev.st.recovery_threshold = obj.recovery_threshold;
    ev.st.comparison = obj.comparison; ev.st.hard = obj.hard;

    double measured = 0.0; UnitKind unit = UnitKind::Count;
    bool sufficient = false; Count samples; std::string detail;
    bool got = compute_measured(acc, now, measured, unit, sufficient, samples, detail);
    ev.measured = measured;
    ev.st.sample_count = samples;
    Freshness fr = evaluate_freshness(acc, now);
    ev.st.freshness = fr;
    ev.st.sufficient = sufficient;
    ev.st.predicted = PredictedRisk::None;
    ev.st.budget_state = std::nullopt;
    ev.st.burn = std::nullopt;
    if (acc.has_budget) {
      ev.st.budget_state = acc.budget.state();
      ev.st.burn = acc.budget.classify_burn(now);
    }

    if (!got || !sufficient || fr == Freshness::Stale || fr == Freshness::Expired) {
      if (fr == Freshness::Stale) {
        ev.state = ComplianceState::RevalidationRequired;
        ev.st.detail = "stale evidence requires revalidation";
      } else {
        ev.state = ComplianceState::InsufficientEvidence;
        ev.st.detail = detail.empty() ? "no current evidence" : detail;
      }
      ev.st.state = ev.state;
      ev.st.provenance = Provenance::Unknown;
      ev.st.detail = ev.st.detail.empty() ? "no evidence" : ev.st.detail;
      acc.last_state = ev.state;
      acc.last_state_time = now;
      ex.objectives.push_back(ev.st);
      evals.push_back(ev);
      continue;
    }

    ev.state = step_state(obj, measured, acc.last_state);
    if (acc.has_budget && acc.budget.state() == BudgetState::Exhausted && ev.state == ComplianceState::Compliant)
      ev.state = ComplianceState::NearLimit;
    ev.risk = predict_risk(obj, acc, now, measured, numeric_limit(obj, true));
    ev.st.state = ev.state;
    ev.st.current_value = make_value(expected_unit_for(obj.dim), measured);
    ev.st.sufficient = true;
    ev.st.predicted = ev.risk;
    ev.st.detail = severity_reason(obj, ev.state);
    ev.st.provenance = Provenance::Measured;
    acc.last_state = ev.state;
    acc.last_state_time = now;
    ev.severity_rank = severity_rank_of(ev.state);
    ev.magnitude = severity_magnitude(obj, measured);
    digest_input += obj.name + "=" + std::to_string(static_cast<int>(ev.state)) + ";" + std::to_string(measured) + ";";
    ex.objectives.push_back(ev.st);
    evals.push_back(ev);
  }

  ComplianceState overall = ComplianceState::Compliant;
  bool any_insufficient = false, any_revalidation = false;
  for (auto& e : evals) {
    if (e.state == ComplianceState::InsufficientEvidence) any_insufficient = true;
    if (e.state == ComplianceState::RevalidationRequired) any_revalidation = true;
  }

  const Objective* binding = nullptr;
  if (any_revalidation) overall = ComplianceState::RevalidationRequired;
  else if (any_insufficient) overall = ComplianceState::InsufficientEvidence;
  else {
    std::vector<ObjEval*> hard;
    for (auto& e : evals) if (e.state != ComplianceState::Compliant) hard.push_back(&e);
    if (hard.empty()) hard.push_back(&evals[0]);
    std::sort(hard.begin(), hard.end(), [](const ObjEval* a, const ObjEval* b) {
      if (a->severity_rank != b->severity_rank) return a->severity_rank < b->severity_rank;
      if (a->magnitude != b->magnitude) return a->magnitude > b->magnitude;
      if (a->obj->priority.value != b->obj->priority.value) return a->obj->priority.value < b->obj->priority.value;
      return a->obj->name < b->obj->name;
    });
    binding = hard.front()->obj;
    overall = hard.front()->state;
  }

  ex.compliance = overall;
  ex.binding_dimension = binding ? binding->dim : Dimension::Latency;
  if (binding) ex.binding_objective = binding->id;
  if (binding) {
    for (auto& e : evals)
      if (e.obj->id != binding->id && e.state != ComplianceState::Compliant)
        ex.secondary_constraints.push_back(e.obj->id);
  }
  for (auto& e : evals) if (e.risk != PredictedRisk::None) ex.predicted_risks.push_back(e.risk);

  EnforcementIntent intent;
  intent.service = service; intent.workload = workload;
  intent.epoch = impl_->epoch;
  intent.contract_gen = c->gen;
  intent.policy_gen = policy.gen;
  intent.evaluation_id = ex.evaluation_id;
  intent.evaluation_gen = ex.evaluation_gen;
  intent.evidence_gen = EvidenceGen(impl_->evaluation_counter);
  intent.workload_gen = WorkloadGen(1);
  intent.action = EnforcementAction::NoAction;
  intent.cooldown = seconds(5);
  if (binding) {
    const Objective& bobj = *binding;
    Duration cd = Duration(0);
    auto it2 = impl_->accumulators.find(bobj.id);
    if (it2 != impl_->accumulators.end()) cd = it2->second.cooldown_until;
    EnforcementAction action = action_for(bobj, overall);
    if (bobj.enforcement == EnforcementClass::None) action = EnforcementAction::NoAction;
    if (now < cd) action = EnforcementAction::NoAction;
    intent.action = action;
    intent.cls = bobj.enforcement;
    intent.dimension = bobj.dim;
    intent.objective_id = bobj.id;
    intent.objective_gen = bobj.gen;
    intent.reason = severity_reason(bobj, overall) + "; binding objective: " + bobj.name;
    ex.selected_action = action;
    for (auto& e : evals) {
      if (e.state == ComplianceState::Violating || e.state == ComplianceState::Recovering ||
          e.state == ComplianceState::AtRisk || e.state == ComplianceState::NearLimit) {
        RankedIntent ri;
        ri.action = action_for(*e.obj, e.state);
        ri.selected = (e.obj->id == bobj.id);
        ri.rationale = e.obj->name + ": " + severity_reason(*e.obj, e.state);
        ex.ranked_intents.push_back(std::move(ri));
      }
    }
    WhatWouldChange wwc;
    for (auto& e : evals) if (e.obj->id == bobj.id) {
      RequiredChange rc;
      switch (bobj.dim) {
        case Dimension::Latency: rc.reason = ChangeReason::LatencyBelowRecovery; break;
        case Dimension::MemoryPressure: rc.reason = ChangeReason::MemoryPressureFalls; break;
        case Dimension::Cost: rc.reason = ChangeReason::CostBudgetIncreases; break;
        case Dimension::Availability: rc.reason = ChangeReason::WarmReplicaBecomesReady; break;
        case Dimension::Throughput: rc.reason = ChangeReason::ThroughputRises; break;
        case Dimension::RecoveryTime: rc.reason = ChangeReason::RecoveryCandidateImproves; break;
      }
      rc.description = std::string("move ") + bobj.name + " below its recovery threshold";
      wwc.required_changes.push_back(std::move(rc));
    }
    ex.what_would_change = std::move(wwc);
  }
  ex.policy_rationale = std::string(policy_mode_name(policy.mode)) + ": " + policy.description;
  res.intent = intent;

  if (impl_->evaluation_history.size() >= 1024) impl_->evaluation_history.erase(impl_->evaluation_history.begin());
  EvaluationRecord rec;
  rec.evaluation_id = ex.evaluation_id;
  rec.evaluation_gen = ex.evaluation_gen;
  rec.contract_gen = c->gen;
  rec.policy_gen = policy.gen;
  rec.evidence_gen = intent.evidence_gen;
  rec.epoch = impl_->epoch;
  rec.time = now;
  rec.compliance = overall;
  rec.binding_dimension = ex.binding_dimension;
  rec.binding_objective = ex.binding_objective;
  rec.selected_action = intent.action;
  rec.digest = stable_hash(digest_input);
  impl_->evaluation_history.push_back(rec);
  impl_->evaluation_by_id[rec.evaluation_id] = rec;

  return res;
}

// ---------------------------------------------------------------------------
// Enforcement lifecycle
// ---------------------------------------------------------------------------
Result<EnforcementReceipt> SloFabric::authorize(const EnforcementIntent& intent, Duration now) {
  if (intent.epoch != impl_->epoch)
    return Result<EnforcementReceipt>(stale_authority("intent coordinator epoch is stale"));
  if (intent.action == EnforcementAction::NoAction)
    return Result<EnforcementReceipt>(invalid_input("cannot authorize a no-op intent"));
  // Verify the target contract is still active at the same generation.
  const SloContract* c = nullptr;
  for (auto& kv : impl_->contracts)
    for (auto& o : kv.second.objectives)
      if (o.id == intent.objective_id && kv.second.service == intent.service) { c = &kv.second; break; }
  if (!c) return Result<EnforcementReceipt>(contract_inactive("target contract not found"));
  if (c->gen != intent.contract_gen)
    return Result<EnforcementReceipt>(stale_authority("contract generation does not match intent"));
  if (!c->can_authorize(now))
    return Result<EnforcementReceipt>(contract_inactive("contract cannot authorize at this time"));
  auto pit = impl_->policies.find(c->policy_id);
  if (pit == impl_->policies.end() || pit->second.gen != intent.policy_gen)
    return Result<EnforcementReceipt>(policy_superseded("policy generation does not match intent"));

  EnforcementReceipt rc;
  rc.id = EnforcementId(impl_->enforcement_counter + 1);
  rc.gen = EnforcementGen(impl_->enforcement_counter + 1);
  ++impl_->enforcement_counter;
  rc.action = intent.action;
  rc.lifecycle = EnforcementLifecycle::Authorized;
  rc.epoch = intent.epoch;
  rc.contract_gen = intent.contract_gen;
  rc.policy_gen = intent.policy_gen;
  rc.evaluation_id = intent.evaluation_id;
  rc.evaluation_gen = intent.evaluation_gen;
  rc.workload_gen = intent.workload_gen;
  rc.service = intent.service;
  rc.workload = intent.workload;
  rc.objective_id = intent.objective_id;
  rc.objective_gen = intent.objective_gen;
  rc.reason = intent.reason;

  // Set cooldown on the binding objective to prevent thrash.
  auto it = impl_->accumulators.find(intent.objective_id);
  if (it != impl_->accumulators.end())
    it->second.cooldown_until = Duration(now.as_ns() + intent.cooldown.as_ns());

  if (impl_->enforcement_history.size() >= 1024) impl_->enforcement_history.erase(impl_->enforcement_history.begin());
  impl_->enforcement_history.push_back(rc);
  return rc;
}

Status SloFabric::dispatch(const EnforcementIntent& intent, ReferenceEnforcementSink& sink) {
  ++impl_->dispatch_counter;
  return sink.dispatch(intent.action, intent);
}

Status SloFabric::transition(EnforcementId id, EnforcementLifecycle to, Duration now) {
  (void)now;
  for (auto& rc : impl_->enforcement_history) {
    if (rc.id == id) {
      if (!enforcement_transition_legal(rc.lifecycle, to))
        return cancelled("illegal enforcement lifecycle transition");
      rc.lifecycle = to;
      return ok();
    }
  }
  return not_found("enforcement not found");
}

Status SloFabric::complete_enforcement(EnforcementId id, Duration now) {
  (void)now;
  for (auto& rc : impl_->enforcement_history) {
    if (rc.id == id) {
      if (rc.lifecycle == EnforcementLifecycle::Acknowledged) {
        rc.lifecycle = EnforcementLifecycle::Effective;
        return ok();
      }
      if (rc.lifecycle == EnforcementLifecycle::Effective) return ok();
      return cancelled("enforcement not in a state that can complete");
    }
  }
  return not_found("enforcement not found");
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
Status SloFabric::save(const std::filesystem::path& path) const {
  DurableState st;
  st.epoch = impl_->epoch;
  for (auto& kv : impl_->policies) st.policies.push_back(kv.second);
  for (auto& kv : impl_->contracts) st.contracts.push_back(kv.second);
  for (auto& kv : impl_->accumulators) {
    PersistedObjectiveState ps;
    ps.objective_id = kv.first;
    ps.objective_gen = kv.second.def.gen;
    ps.budget_total = kv.second.has_budget ? kv.second.budget.total() : Count(0);
    ps.budget_consumed = kv.second.has_budget ? kv.second.budget.consumed() : Count(0);
    ps.budget_window = kv.second.has_budget ? kv.second.budget.window() : Duration(0);
    ps.budget_window_start = kv.second.has_budget ? kv.second.budget.window_start() : Duration(0);
    ps.last_state = kv.second.last_state;
    ps.last_state_time = kv.second.last_state_time;
    ps.cooldown_until = kv.second.cooldown_until;
    st.objective_states.push_back(ps);
  }
  st.enforcement_history = impl_->enforcement_history;
  st.evaluation_history = impl_->evaluation_history;
  return save_state(path, st);
}

Status SloFabric::load(const std::filesystem::path& path) {
  DurableState st;
  Status s = load_state(path, st);
  if (!s.ok()) return s;
  // Advance coordinator epoch (restart).
  impl_->epoch = st.epoch.next();
  impl_->policies.clear();
  for (auto& p : st.policies) impl_->policies[p.id] = p;
  impl_->contracts.clear();
  for (auto& c : st.contracts) impl_->contracts[c.id] = c;
  impl_->accumulators.clear();
  for (auto& ps : st.objective_states) {
    // Find the objective definition and rebuild the accumulator.
    for (auto& kv : impl_->contracts)
      for (auto& o : kv.second.objectives)
        if (o.id == ps.objective_id && o.gen == ps.objective_gen) {
          ObjectiveAccumulator& acc = impl_->get_accumulator(o);
          if (ps.budget_total.value() != 0) {
            acc.has_budget = true;
            acc.budget = ErrorBudget(ps.budget_total, ps.budget_window);
            acc.budget.set_window_start(ps.budget_window_start);
            acc.budget.consume_count(ps.budget_consumed);
          }
          acc.last_state = ComplianceState::RevalidationRequired;
          acc.last_state_time = ps.last_state_time;
          acc.cooldown_until = ps.cooldown_until;
        }
  }
  impl_->enforcement_history = st.enforcement_history;
  impl_->evaluation_history = st.evaluation_history;
  impl_->evaluation_by_id.clear();
  for (auto& e : impl_->evaluation_history) impl_->evaluation_by_id[e.evaluation_id] = e;
  return ok();
}

// ---------------------------------------------------------------------------
// Inspection / query
// ---------------------------------------------------------------------------
CoordinatorEpoch SloFabric::current_epoch() const noexcept { return impl_->epoch; }
std::size_t SloFabric::contract_count() const noexcept { return impl_->contracts.size(); }
std::size_t SloFabric::evaluation_count() const noexcept { return impl_->evaluation_history.size(); }

std::vector<ObjectiveStatus> SloFabric::objective_statuses(SloContractId id) const {
  std::vector<ObjectiveStatus> out;
  auto it = impl_->contracts.find(id);
  if (it == impl_->contracts.end()) return out;
  for (const auto& o : it->second.objectives) {
    ObjectiveStatus os;
    os.objective = o;
    auto ait = impl_->accumulators.find(o.id);
    if (ait != impl_->accumulators.end()) {
      os.state = ait->second.last_state;
      os.freshness = Freshness::Unknown;
      os.sample_count = Count(static_cast<std::int64_t>(ait->second.window.size()));
      if (ait->second.has_budget) os.budget_state = ait->second.budget.state();
    } else {
      os.state = ComplianceState::InsufficientEvidence;
      os.freshness = Freshness::Unknown;
      os.sample_count = Count(0);
    }
    out.push_back(std::move(os));
  }
  return out;
}

Result<EvaluationRecord> SloFabric::get_evaluation(EvaluationId id) const {
  auto it = impl_->evaluation_by_id.find(id);
  if (it == impl_->evaluation_by_id.end()) return Result<EvaluationRecord>(not_found("evaluation not found"));
  return it->second;
}

}  // namespace slofabric




