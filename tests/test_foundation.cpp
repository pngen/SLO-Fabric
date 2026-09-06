#include "test_framework.hpp"

// Foundation smoke test: identities, units, clock, objective, contracts,
// evidence, windows, budgets, enforcement, adapters, explanation.
#include "slofabric/adapter.hpp"
#include "slofabric/budget.hpp"
#include "slofabric/clock.hpp"
#include "slofabric/compliance.hpp"
#include "slofabric/contract.hpp"
#include "slofabric/enforcement.hpp"
#include "slofabric/evidence.hpp"
#include "slofabric/freshness.hpp"
#include "slofabric/identities.hpp"
#include "slofabric/objective.hpp"
#include "slofabric/objective_value.hpp"
#include "slofabric/policy.hpp"
#include "slofabric/provenance.hpp"
#include "slofabric/quantile.hpp"
#include "slofabric/sample_window.hpp"
#include "slofabric/status.hpp"
#include "slofabric/units.hpp"
#include "slofabric/version.hpp"
#include "slofabric/what_would_change.hpp"

using namespace slofabric;

TEST("foundation: identities are distinct and ordered") {
  SloContractId a(5);
  SloContractId b(5);
  SloContractId c(6);
  SloContractGen ga(1);
  CHECK(a == b);
  CHECK(a != c);
  CHECK(a < c);
  CHECK(ga.valid());
  CHECK(SloContractGen().invalid());
  CHECK_EQ(a.to_string(), std::string("5"));
}

TEST("foundation: typed units and checked arithmetic") {
  Duration d = milliseconds(150);
  CHECK_EQ(d.as_ms(), 150LL);
  CHECK(!(d < milliseconds(100)));
  ByteCount b = megabytes(2);
  CHECK_EQ(b.as_bytes(), 2 * 1024 * 1024LL);
  Percentage p(99.5);
  CHECK(p.is_valid());
  BasisPoints bp(9990);
  CHECK(bp.is_valid());
  CostMicros cost(1234);
  CHECK(!cost.is_negative());
  RequestsPerSecond r(12.5);
  CHECK(r.is_valid());
}

TEST("foundation: objective value unit matches dimension") {
  Objective o;
  o.dim = Dimension::Latency;
  o.target = milliseconds(100);
  CHECK(o.target_unit_matches_dimension());
  Objective o2;
  o2.dim = Dimension::Latency;
  o2.target = bytes(100);
  CHECK(!o2.target_unit_matches_dimension());
}

TEST("foundation: contract lifecycle and authority") {
  SloContract c;
  c.id = SloContractId(1);
  c.gen = SloContractGen(1);
  c.lifecycle = ContractLifecycle::Draft;
  CHECK(!c.can_authorize(seconds(10)));
  c.lifecycle = ContractLifecycle::Active;
  c.effective_from = seconds(0);
  CHECK(c.can_authorize(seconds(10)));
  c.expiration = seconds(5);
  CHECK(!c.can_authorize(seconds(10)));
}

TEST("foundation: sample window handles ordering and duplicates") {
  SampleWindow w(WindowType::Sliding, seconds(10), 8);
  CHECK(w.ingest(seconds(1), 10.0, 1) == IngestStatus::Accepted);
  CHECK(w.ingest(seconds(2), 20.0, 2) == IngestStatus::Accepted);
  CHECK(w.ingest(seconds(2), 20.0, 2) == IngestStatus::Duplicate);
  CHECK(w.ingest(seconds(1), 5.0, 3) == IngestStatus::Rollback);
  CHECK_EQ(w.size(), std::size_t(2));
  CHECK_EQ(w.mean(), 15.0);
  CHECK_EQ(w.last_value(), 20.0);
}

TEST("foundation: quantile is deterministic") {
  QuantileStore s(16);
  s.add(10); s.add(20); s.add(30); s.add(40); s.add(50);
  CHECK_EQ(s.quantile(0.5), 30.0);
  CHECK_EQ(s.quantile(1.0), 50.0);
  CHECK_EQ(s.quantile(0.0), 10.0);
  CHECK(s.sufficient(0.5));
}

TEST("foundation: error budget exact and invariant") {
  ErrorBudget b(Count(100), seconds(60));
  CHECK_EQ(b.remaining().value(), 100LL);
  CHECK(b.state() == BudgetState::Healthy);

  // Consume 90: remaining 10 -> NearLimit (10% threshold).
  b.consume_count(Count(90));
  CHECK_EQ(b.consumed().value(), 90LL);
  CHECK_EQ(b.remaining().value(), 10LL);
  CHECK(b.state() == BudgetState::NearLimit);

  // Consume 9 more: remaining 1, still NearLimit.
  b.consume_count(Count(9));
  CHECK_EQ(b.remaining().value(), 1LL);

  // Consume 2 more: consumed 101 > total 100, remaining clamps to 0.
  b.consume_count(Count(2));
  CHECK_EQ(b.remaining().value(), 0LL);
  CHECK(b.state() == BudgetState::Exhausted);
  CHECK_EQ(b.overrun().value(), 1LL);
  CHECK(b.overrun().value() >= 0LL);  // invariant: remaining never exceeds total
}

TEST("foundation: enforcement lifecycle transition enforcement") {
  CHECK(enforcement_transition_legal(EnforcementLifecycle::Proposed, EnforcementLifecycle::Authorized));
  CHECK(enforcement_transition_legal(EnforcementLifecycle::Authorized, EnforcementLifecycle::Dispatched));
  CHECK(enforcement_transition_legal(EnforcementLifecycle::Acknowledged, EnforcementLifecycle::Effective));
  CHECK(!enforcement_transition_legal(EnforcementLifecycle::Cancelled, EnforcementLifecycle::Effective));
  CHECK(!enforcement_transition_legal(EnforcementLifecycle::Expired, EnforcementLifecycle::Effective));
}

TEST("foundation: reference enforcement sink records and fences") {
  ReferenceEnforcementSink sink;
  EnforcementIntent i;
  i.action = EnforcementAction::ReclaimMemory;
  i.contract_gen = SloContractGen(1);
  i.policy_gen = PolicyGen(1);
  i.evaluation_gen = EvaluationGen(1);
  i.evaluation_id = EvaluationId(1);
  i.epoch = CoordinatorEpoch(1);
  Status s = sink.dispatch(EnforcementAction::ReclaimMemory, i);
  CHECK(s.ok());
  CHECK_EQ(sink.dispatch_count(), std::size_t(1));
  CHECK_EQ(sink.records()[0].action, EnforcementAction::ReclaimMemory);

  // Stale generation must reject.
  sink.set_authorized_generations(CoordinatorEpoch(2), SloContractGen(2),
                                  PolicyGen(2), EvaluationGen(2));
  EnforcementIntent i2 = i;
  i2.contract_gen = SloContractGen(1);
  Status s2 = sink.dispatch(EnforcementAction::ReclaimMemory, i2);
  CHECK(!s2.ok());
  CHECK(s2.code == StatusCode::StaleAuthority);
}
