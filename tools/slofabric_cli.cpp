// slofabric_cli.cpp - CLI inspection tool for the SLO Fabric library.
//
// A deterministic, read-mostly command-line inspector. Every invocation builds a
// fixed demo scenario (one policy, one contract with a latency p99 objective and
// a cost-window objective, plus a pre-ingested evidence set) so each subcommand
// produces meaningful, deterministic output. A ManualClock is driven by the
// requested evaluation time (from --at <ns> and/or the per-command <at_ns>).

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"
#include "slofabric/persistence.hpp"
#include "slofabric/adapter.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace slofabric;

namespace {

// ---------------------------------------------------------------------------
// Small parsing helpers (never throw; no exceptions cross user input).
// ---------------------------------------------------------------------------
bool parse_i64(std::string_view s, std::int64_t& out) {
  if (s.empty()) return false;
  std::string str(s);
  char* end = nullptr;
  errno = 0;
  long long v = std::strtoll(str.c_str(), &end, 10);
  if (errno != 0 || end == str.c_str() || *end != '\0') return false;
  out = static_cast<std::int64_t>(v);
  return true;
}

bool parse_u64(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  std::string str(s);
  char* end = nullptr;
  errno = 0;
  unsigned long long v = std::strtoull(str.c_str(), &end, 10);
  if (errno != 0 || end == str.c_str() || *end != '\0') return false;
  out = static_cast<std::uint64_t>(v);
  return true;
}

bool parse_double(std::string_view s, double& out) {
  if (s.empty()) return false;
  std::string str(s);
  char* end = nullptr;
  errno = 0;
  double v = std::strtod(str.c_str(), &end);
  if (end == str.c_str() || *end != '\0') return false;
  if (std::isnan(v) || std::isinf(v)) return false;
  out = v;
  return true;
}

template <typename IdT>
IdT make_id(const std::string& s, bool& ok) {
  std::uint64_t v = 0;
  ok = parse_u64(s, v);
  return IdT(v);
}

// ---------------------------------------------------------------------------
// Fixed demo scenario.
// ---------------------------------------------------------------------------
constexpr std::int64_t kNsPerSecond = 1000000000LL;
constexpr std::uint64_t kPolicyId = 1;
constexpr std::uint64_t kContractId = 1;
constexpr std::uint64_t kServiceId = 100;
constexpr std::uint64_t kWorkloadId = 200;
constexpr std::uint64_t kLatencyOid = 7;
constexpr std::uint64_t kCostOid = 8;
constexpr std::uint64_t kLatencyGen = 1;
constexpr std::uint64_t kCostGen = 1;

SloPolicy make_demo_policy() {
  SloPolicy p;
  p.id = PolicyId(kPolicyId);
  p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft;
  p.tie_break = TieBreak::ByName;
  p.name = "hard-then-soft-policy";
  p.description = "resolve hard objectives first, then soft ranking";
  return p;
}

SloContract make_demo_contract() {
  SloContract c;
  c.id = SloContractId(kContractId);
  c.gen = SloContractGen(1);
  c.service = ServiceId(kServiceId);
  c.workload = WorkloadId(kWorkloadId);
  c.tenant = TenantId(1);
  c.policy_id = PolicyId(kPolicyId);
  c.policy_gen = PolicyGen(1);
  c.effective_from = Duration(0);
  c.expiration = std::nullopt;
  c.epoch = CoordinatorEpoch(1);
  c.lifecycle = ContractLifecycle::Active;
  c.name = "demo-contract";
  c.provenance = "cli-demo";

  // Latency p99: target 100ms, min 32 samples, 60s sliding window.
  Objective lat;
  lat.id = ObjectiveId(kLatencyOid);
  lat.gen = ObjectiveGen(kLatencyGen);
  lat.name = "p99-latency";
  lat.dim = Dimension::Latency;
  lat.kind = ObjectiveKind::LatencyPercentile;
  lat.percentile_rank = 0.99;
  lat.comparison = Comparison::AtMost;
  lat.hard = HardSoft::Hard;
  lat.enforcement = EnforcementClass::Authorized;
  lat.target = milliseconds(100);
  lat.min_evidence = Count(32);
  lat.window_type = WindowType::Sliding;
  lat.window_duration = seconds(60);
  lat.window_count = Count(64);
  lat.priority = Priority(1);
  c.objectives.push_back(lat);

  // Cost window budget: target 1,000,000 micros.
  Objective cost;
  cost.id = ObjectiveId(kCostOid);
  cost.gen = ObjectiveGen(kCostGen);
  cost.name = "cost-budget";
  cost.dim = Dimension::Cost;
  cost.kind = ObjectiveKind::CostWindowBudget;
  cost.comparison = Comparison::AtMost;
  cost.hard = HardSoft::Hard;
  cost.enforcement = EnforcementClass::Authorized;
  cost.target = CostMicros(1000000);
  cost.min_evidence = Count(1);
  cost.window_type = WindowType::Sliding;
  cost.window_duration = seconds(60);
  cost.window_count = Count(64);
  cost.priority = Priority(2);
  c.objectives.push_back(cost);

  return c;
}

void ingest_demo_evidence(SloFabric& f, Duration at) {
  const SourceBootId src(1);
  const SourceBootGen src_gen(1);
  const CoordinatorEpoch epoch(1);
  const std::int64_t base = at.as_ns();
  std::uint64_t id = 1000;

  // 40 latency samples, 1s apart, ending 1s before `at`. ~120ms -> violating.
  for (std::int64_t i = 0; i < 40; ++i) {
    std::int64_t offset = (40 - i) * kNsPerSecond;  // 40s .. 1s
    Duration t(base - offset);
    EvidenceRecord e;
    e.id = EvidenceId(id++);
    e.gen = EvidenceGen(1);
    e.dimension = Dimension::Latency;
    e.objective_id = ObjectiveId(kLatencyOid);
    e.objective_gen = ObjectiveGen(kLatencyGen);
    e.source = src;
    e.source_gen = src_gen;
    e.epoch = epoch;
    e.time = t;
    e.value = Duration(120000000LL);  // 120ms
    f.ingest(e, t);
  }

  // 10 cost samples, 1s apart, ending 1s before `at`. Sum = 400000 micros.
  for (std::int64_t i = 0; i < 10; ++i) {
    std::int64_t offset = (10 - i) * kNsPerSecond;  // 10s .. 1s
    Duration t(base - offset);
    EvidenceRecord e;
    e.id = EvidenceId(id++);
    e.gen = EvidenceGen(1);
    e.dimension = Dimension::Cost;
    e.objective_id = ObjectiveId(kCostOid);
    e.objective_gen = ObjectiveGen(kCostGen);
    e.source = src;
    e.source_gen = src_gen;
    e.epoch = epoch;
    e.time = t;
    e.value = CostMicros(40000);  // 10 * 40000 = 400000 micros
    f.ingest(e, t);
  }
}

void build_demo(SloFabric& f, Duration at) {
  auto check = [](const Status& s, const char* op) {
    if (!s.ok()) {
      std::cout << "[setup] " << op << " failed: " << status_code_name(s.code)
                << ": " << s.message << "\n";
    }
  };

  check(f.set_policy(make_demo_policy()), "set_policy");
  check(f.add_contract(make_demo_contract()), "add_contract");
  check(f.activate_contract(SloContractId(kContractId), at), "activate_contract");
  check(f.set_objective_budget(ObjectiveId(kCostOid), Count(1000), seconds(60)),
        "set_objective_budget");
  ingest_demo_evidence(f, at);

  // Seed exactly one baseline evaluation so `replay` has a stored record and the
  // objective-state accumulator has a meaningful last_state.
  auto r = f.evaluate(ServiceId(kServiceId), WorkloadId(kWorkloadId), at);
  if (!r.ok()) {
    std::cout << "[setup] baseline evaluate failed: " << status_code_name(r.status().code)
              << ": " << r.status().message << "\n";
  }
}

// ---------------------------------------------------------------------------
// Subcommand implementations.
// ---------------------------------------------------------------------------
void print_status_line(const Status& s) {
  std::cout << "  status: " << status_code_name(s.code);
  if (!s.message.empty()) std::cout << ": " << s.message;
  std::cout << "\n";
}

void cmd_inspect_contract(SloFabric& f, SloContractId id) {
  auto c = f.find_contract(id);
  if (!c) {
    std::cout << "contract not found: " << id.to_string() << "\n";
    return;
  }
  const SloContract& ctr = *c;
  std::cout << "contract: " << ctr.id.to_string() << " (gen " << ctr.gen.to_string() << ")\n";
  std::cout << "  name: " << ctr.name << "\n";
  std::cout << "  lifecycle: " << contract_lifecycle_name(ctr.lifecycle) << "\n";
  std::cout << "  service: " << ctr.service.to_string()
            << "  workload: " << ctr.workload.to_string()
            << "  tenant: " << ctr.tenant.to_string() << "\n";
  std::cout << "  policy: " << ctr.policy_id.to_string()
            << " (gen " << ctr.policy_gen.to_string() << ")\n";
  std::cout << "  epoch: " << ctr.epoch.to_string() << "\n";
  std::cout << "  effective_from: " << ctr.effective_from.as_ns() << "ns\n";
  if (ctr.expiration) {
    std::cout << "  expiration: " << ctr.expiration->as_ns() << "ns\n";
  } else {
    std::cout << "  expiration: none\n";
  }
  std::cout << "  objectives:\n";
  for (std::size_t i = 0; i < ctr.objectives.size(); ++i) {
    const Objective& o = ctr.objectives[i];
    std::cout << "    [" << i << "] id=" << o.id.to_string()
              << " gen=" << o.gen.to_string() << " name=" << o.name << "\n";
    std::cout << "        dim=" << dimension_name(o.dim)
              << " kind=" << objective_kind_name(o.kind) << "\n";
    if (o.kind == ObjectiveKind::LatencyPercentile) {
      std::cout << "        percentile_rank=" << o.percentile_rank << "\n";
    }
    std::cout << "        target=" << to_string(o.target)
              << " unit=" << unit_kind_name(unit_kind_of(o.target)) << "\n";
    std::cout << "        comparison=" << comparison_name(o.comparison)
              << " hard=" << hardsoft_name(o.hard)
              << " enforcement=" << enforcement_class_name(o.enforcement) << "\n";
    std::cout << "        min_evidence=" << o.min_evidence.value()
              << " window=" << window_type_name(o.window_type)
              << " duration=" << o.window_duration.as_ns() << "ns"
              << " priority=" << o.priority.value << "\n";
  }
}

void cmd_evaluate(SloFabric& f, ServiceId svc, WorkloadId wl, Duration at) {
  auto r = f.evaluate(svc, wl, at);
  if (!r.ok()) {
    std::cout << "evaluate failed: " << status_code_name(r.status().code)
              << ": " << r.status().message << "\n";
    return;
  }
  const EvaluationResult& res = r.value_unchecked();
  const Explanation& ex = res.explanation;
  std::cout << "status: " << status_code_name(res.status.code) << "\n";
  std::cout << "evaluation_id: " << ex.evaluation_id.to_string()
            << " gen " << ex.evaluation_gen.to_string() << "\n";
  std::cout << "compliance: " << compliance_state_name(ex.compliance) << "\n";
  std::cout << "binding_dimension: " << dimension_name(ex.binding_dimension) << "\n";
  std::cout << "binding_objective: " << ex.binding_objective.to_string() << "\n";
  std::cout << "selected_action: " << enforcement_action_name(ex.selected_action) << "\n";
  std::cout << "policy_rationale: " << ex.policy_rationale << "\n";
  std::cout << "objectives:\n";
  for (const auto& os : ex.objectives) {
    std::cout << "  - id=" << os.id.to_string() << " name=" << os.name
              << " state=" << compliance_state_name(os.state);
    if (os.current_value) std::cout << " value=" << to_string(*os.current_value);
    std::cout << " target=" << to_string(os.target)
              << " samples=" << os.sample_count.value()
              << " freshness=" << freshness_name(os.freshness) << "\n";
  }
  std::cout << "ranked_intents:\n";
  for (const auto& ri : ex.ranked_intents) {
    std::cout << "  - action=" << enforcement_action_name(ri.action)
              << " selected=" << (ri.selected ? "true" : "false")
              << " rationale=" << ri.rationale << "\n";
  }
}

void cmd_ingest_sample(SloFabric& f, ObjectiveId oid, const std::string& dim,
                       const std::string& valueStr, Duration at) {
  double val = 0.0;
  if (!parse_double(valueStr, val)) {
    std::cout << "invalid value: " << valueStr << "\n";
    return;
  }

  EvidenceRecord e;
  e.id = EvidenceId(9000001);
  e.gen = EvidenceGen(1);
  e.source = SourceBootId(1);
  e.source_gen = SourceBootGen(1);
  e.epoch = f.current_epoch();
  e.time = at;

  const std::int64_t iv = static_cast<std::int64_t>(val);
  if (dim == "latency") {
    e.dimension = Dimension::Latency;
    e.value = Duration(iv);
  } else if (dim == "cost") {
    e.dimension = Dimension::Cost;
    e.value = CostMicros(iv);
  } else if (dim == "availability") {
    e.dimension = Dimension::Availability;
    e.value = BasisPoints(iv);
  } else if (dim == "throughput") {
    e.dimension = Dimension::Throughput;
    e.value = RequestsPerSecond(val);
  } else if (dim == "memory_pressure") {
    e.dimension = Dimension::MemoryPressure;
    e.value = PressureRatio(val);
  } else {
    std::cout << "unknown dimension: " << dim << "\n";
    return;
  }

  e.objective_id = oid;
  e.objective_gen = ObjectiveGen(1);
  Status s = f.ingest(e, at);
  std::cout << "ingest result: " << status_code_name(s.code);
  if (!s.message.empty()) std::cout << ": " << s.message;
  std::cout << "\n";
}

void cmd_show_compliance(SloFabric& f, ServiceId svc, WorkloadId wl, Duration at) {
  auto r = f.evaluate(svc, wl, at);
  if (!r.ok()) {
    std::cout << "evaluate failed: " << status_code_name(r.status().code)
              << ": " << r.status().message << "\n";
    return;
  }
  const Explanation& ex = r.value_unchecked().explanation;
  std::cout << "compliance: " << compliance_state_name(ex.compliance) << "\n";
  std::cout << "objectives:\n";
  for (const auto& os : ex.objectives) {
    std::cout << "  - " << os.name << " [" << dimension_name(os.dim) << "]\n";
    std::cout << "      state: " << compliance_state_name(os.state) << "\n";
    if (os.current_value) std::cout << "      current_value: " << to_string(*os.current_value) << "\n";
    std::cout << "      target: " << to_string(os.target) << "\n";
    std::cout << "      samples: " << os.sample_count.value()
              << " sufficient: " << (os.sufficient ? "true" : "false") << "\n";
    std::cout << "      freshness: " << freshness_name(os.freshness) << "\n";
    std::cout << "      predicted: " << predicted_risk_name(os.predicted) << "\n";
    if (os.budget_state) {
      std::cout << "      budget: " << budget_state_name(*os.budget_state) << "\n";
    }
  }
}

void cmd_show_budgets(SloFabric& f, SloContractId id) {
  auto os = f.objective_statuses(id);
  if (os.empty()) {
    std::cout << "contract not found: " << id.to_string() << "\n";
    return;
  }
  bool any = false;
  for (const auto& s : os) {
    std::cout << "objective " << s.objective.name << " (" << s.objective.id.to_string() << "): ";
    if (s.budget_state) {
      std::cout << "state=" << budget_state_name(*s.budget_state) << "\n";
      any = true;
    } else {
      std::cout << "none\n";
    }
  }
  if (!any) std::cout << "no budgets set\n";
}

void cmd_show_binding(SloFabric& f, ServiceId svc, WorkloadId wl, Duration at) {
  auto r = f.evaluate(svc, wl, at);
  if (!r.ok()) {
    std::cout << "evaluate failed: " << status_code_name(r.status().code)
              << ": " << r.status().message << "\n";
    return;
  }
  const Explanation& ex = r.value_unchecked().explanation;
  std::cout << "binding_dimension: " << dimension_name(ex.binding_dimension) << "\n";
  std::cout << "binding_objective: " << ex.binding_objective.to_string() << "\n";
  std::cout << "secondary_constraints:";
  if (ex.secondary_constraints.empty()) {
    std::cout << " none";
  } else {
    for (const auto& id : ex.secondary_constraints) {
      std::cout << " " << id.to_string();
    }
  }
  std::cout << "\n";
}

void cmd_show_intent(SloFabric& f, ServiceId svc, WorkloadId wl, Duration at) {
  auto r = f.evaluate(svc, wl, at);
  if (!r.ok()) {
    std::cout << "evaluate failed: " << status_code_name(r.status().code)
              << ": " << r.status().message << "\n";
    return;
  }
  const EnforcementIntent& it = r.value_unchecked().intent;
  std::cout << "action: " << enforcement_action_name(it.action) << "\n";
  std::cout << "class: " << enforcement_class_name(it.cls) << "\n";
  std::cout << "dimension: " << dimension_name(it.dimension) << "\n";
  std::cout << "objective_id: " << it.objective_id.to_string()
            << " gen " << it.objective_gen.to_string() << "\n";
  std::cout << "service: " << it.service.to_string()
            << " workload: " << it.workload.to_string() << "\n";
  std::cout << "epoch: " << it.epoch.to_string() << "\n";
  std::cout << "contract_gen: " << it.contract_gen.to_string()
            << " policy_gen: " << it.policy_gen.to_string() << "\n";
  std::cout << "evidence_gen: " << it.evidence_gen.to_string()
            << " evaluation_id: " << it.evaluation_id.to_string()
            << " evaluation_gen: " << it.evaluation_gen.to_string() << "\n";
  std::cout << "workload_gen: " << it.workload_gen.to_string() << "\n";
  std::cout << "cooldown: " << it.cooldown.as_ns() << "ns\n";
  std::cout << "reason: " << it.reason << "\n";
}

void cmd_what_would_change(SloFabric& f, ServiceId svc, WorkloadId wl, Duration at) {
  auto r = f.evaluate(svc, wl, at);
  if (!r.ok()) {
    std::cout << "evaluate failed: " << status_code_name(r.status().code)
              << ": " << r.status().message << "\n";
    return;
  }
  const WhatWouldChange& w = r.value_unchecked().explanation.what_would_change;
  std::cout << "reasons:\n";
  if (w.reasons.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto& rc : w.reasons) {
      std::cout << "  - " << change_reason_name(rc) << "\n";
    }
  }
  std::cout << "required_changes:\n";
  if (w.required_changes.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto& rc : w.required_changes) {
      std::cout << "  - reason=" << change_reason_name(rc.reason) << ": " << rc.description << "\n";
    }
  }
}

void cmd_inspect_state(const std::filesystem::path& path) {
  DurableState st;
  Status s = load_state(path, st);
  if (!s.ok()) {
    std::cout << "load failed: " << status_code_name(s.code) << ": " << s.message << "\n";
    return;
  }
  std::cout << "format_version: " << st.format_version << "\n";
  std::cout << "epoch: " << st.epoch.to_string() << "\n";
  std::cout << "policies:\n";
  for (const auto& p : st.policies) {
    std::cout << "  - id=" << p.id.to_string() << " gen=" << p.gen.to_string()
              << " name=" << p.name << " mode=" << policy_mode_name(p.mode) << "\n";
  }
  std::cout << "contracts:\n";
  for (const auto& c : st.contracts) {
    std::cout << "  - id=" << c.id.to_string() << " gen=" << c.gen.to_string()
              << " name=" << c.name << " lifecycle=" << contract_lifecycle_name(c.lifecycle)
              << " objectives=" << c.objectives.size() << "\n";
  }
  std::cout << "objective_states:\n";
  for (const auto& o : st.objective_states) {
    std::cout << "  - id=" << o.objective_id.to_string() << " gen=" << o.objective_gen.to_string()
              << " last_state=" << compliance_state_name(o.last_state)
              << " budget_total=" << o.budget_total.value()
              << " budget_consumed=" << o.budget_consumed.value() << "\n";
  }
  std::cout << "enforcement_history: " << st.enforcement_history.size() << "\n";
  std::cout << "evaluation_history: " << st.evaluation_history.size() << "\n";
}

void cmd_replay(SloFabric& f, EvaluationId id) {
  auto r = f.get_evaluation(id);
  if (!r.ok()) {
    std::cout << "evaluation not found: " << id.to_string() << "\n";
    return;
  }
  const EvaluationRecord& rec = r.value_unchecked();
  std::cout << "evaluation_id: " << rec.evaluation_id.to_string()
            << " gen " << rec.evaluation_gen.to_string() << "\n";
  std::cout << "contract_gen: " << rec.contract_gen.to_string()
            << " policy_gen: " << rec.policy_gen.to_string()
            << " evidence_gen: " << rec.evidence_gen.to_string() << "\n";
  std::cout << "epoch: " << rec.epoch.to_string() << "\n";
  std::cout << "time: " << rec.time.as_ns() << "ns\n";
  std::cout << "compliance: " << compliance_state_name(rec.compliance) << "\n";
  std::cout << "binding_dimension: " << dimension_name(rec.binding_dimension) << "\n";
  std::cout << "binding_objective: " << rec.binding_objective.to_string() << "\n";
  std::cout << "selected_action: " << enforcement_action_name(rec.selected_action) << "\n";
  std::cout << "digest: " << rec.digest << "\n";
}

void print_usage() {
  std::cout <<
      "usage: slofabric_cli [--at <ns>] <subcommand> [args...]\n"
      "\n"
      "inspect-contract <id>\n"
      "evaluate <svc> <wl> <at_ns>\n"
      "ingest-sample <obj_id> <dim> <value> <at_ns>\n"
      "show-compliance <svc> <wl> <at_ns>\n"
      "show-budgets <contract_id>\n"
      "show-binding <svc> <wl> <at_ns>\n"
      "show-intent <svc> <wl> <at_ns>\n"
      "what-would-change <svc> <wl> <at_ns>\n"
      "inspect-state <path>\n"
      "replay <evaluation_id>\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  std::string subcmd;
  std::vector<std::string> args;
  bool g_at_set = false;
  std::int64_t g_at = 0;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--at" || (a.rfind("--at=", 0) == 0)) {
      std::string val;
      if (a == "--at") {
        if (i + 1 >= argc) {
          std::cout << "error: --at requires a value\n";
          return 2;
        }
        val = argv[++i];
      } else {
        val = a.substr(5);
      }
      if (!parse_i64(val, g_at)) {
        std::cout << "error: invalid --at value: " << val << "\n";
        return 2;
      }
      g_at_set = true;
      ++i;
      continue;
    }
    if (subcmd.empty()) {
      subcmd = a;
    } else {
      args.push_back(a);
    }
    ++i;
  }

  if (subcmd.empty()) {
    print_usage();
    return 2;
  }

  int need = -1;
  bool has_at_pos = false;
  if (subcmd == "inspect-contract") {
    need = 1;
  } else if (subcmd == "evaluate") {
    need = 3;
    has_at_pos = true;
  } else if (subcmd == "ingest-sample") {
    need = 4;
    has_at_pos = true;
  } else if (subcmd == "show-compliance") {
    need = 3;
    has_at_pos = true;
  } else if (subcmd == "show-budgets") {
    need = 1;
  } else if (subcmd == "show-binding") {
    need = 3;
    has_at_pos = true;
  } else if (subcmd == "show-intent") {
    need = 3;
    has_at_pos = true;
  } else if (subcmd == "what-would-change") {
    need = 3;
    has_at_pos = true;
  } else if (subcmd == "inspect-state") {
    need = 1;
  } else if (subcmd == "replay") {
    need = 1;
  }

  if (need < 0) {
    std::cout << "unknown subcommand: " << subcmd << "\n";
    print_usage();
    return 2;
  }
  if (static_cast<int>(args.size()) != need) {
    std::cout << "wrong argument count for '" << subcmd << "' (expected " << need
              << ", got " << args.size() << ")\n";
    return 2;
  }

  // Resolve the evaluation time. Commands with an explicit <at_ns> use it;
  // the rest fall back to --at or a sensible fixed default.
  Duration at;
  if (has_at_pos) {
    std::int64_t n = 0;
    if (!parse_i64(args.back(), n)) {
      std::cout << "invalid at_ns: " << args.back() << "\n";
      return 2;
    }
    at = Duration(n);
  } else {
    at = g_at_set ? Duration(g_at) : seconds(70);
  }

  if (subcmd == "inspect-state") {
    cmd_inspect_state(std::filesystem::path(args[0]));
    return 0;
  }

  SloFabric fabric(std::make_shared<ManualClock>(at));
  build_demo(fabric, at);

  if (subcmd == "inspect-contract") {
    cmd_inspect_contract(fabric, make_id<SloContractId>(args[0]));
    return 0;
  }
  if (subcmd == "evaluate") {
    cmd_evaluate(fabric, make_id<ServiceId>(args[0]),
                 make_id<WorkloadId>(args[1]), at);
    return 0;
  }
  if (subcmd == "ingest-sample") {
    cmd_ingest_sample(fabric, make_id<ObjectiveId>(args[0]), args[1], args[2], at);
    return 0;
  }
  if (subcmd == "show-compliance") {
    cmd_show_compliance(fabric, make_id<ServiceId>(args[0]),
                        make_id<WorkloadId>(args[1]), at);
    return 0;
  }
  if (subcmd == "show-budgets") {
    cmd_show_budgets(fabric, make_id<SloContractId>(args[0]));
    return 0;
  }
  if (subcmd == "show-binding") {
    cmd_show_binding(fabric, make_id<ServiceId>(args[0]),
                     make_id<WorkloadId>(args[1]), at);
    return 0;
  }
  if (subcmd == "show-intent") {
    cmd_show_intent(fabric, make_id<ServiceId>(args[0]),
                    make_id<WorkloadId>(args[1]), at);
    return 0;
  }
  if (subcmd == "what-would-change") {
    cmd_what_would_change(fabric, make_id<ServiceId>(args[0]),
                          make_id<WorkloadId>(args[1]), at);
    return 0;
  }
  if (subcmd == "replay") {
    cmd_replay(fabric, make_id<EvaluationId>(args[0]));
    return 0;
  }

  print_usage();
  return 2;
}
