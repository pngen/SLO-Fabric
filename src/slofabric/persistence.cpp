#include "slofabric/persistence.hpp"

#include <atomic>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "slofabric/binary.hpp"
#include "slofabric/crc32.hpp"

namespace slofabric {

namespace {

// ---- ObjectiveValue (variant) encoding ----
constexpr std::uint8_t kValDuration = 0;
constexpr std::uint8_t kValByteCount = 1;
constexpr std::uint8_t kValCount = 2;
constexpr std::uint8_t kValBasisPoints = 3;
constexpr std::uint8_t kValPercentage = 4;
constexpr std::uint8_t kValPressure = 5;
constexpr std::uint8_t kValRps = 6;
constexpr std::uint8_t kValTps = 7;
constexpr std::uint8_t kValBps = 8;
constexpr std::uint8_t kValOps = 9;
constexpr std::uint8_t kValCost = 10;
constexpr std::uint8_t kValEnergy = 11;

void write_value(ByteWriter& w, const ObjectiveValue& v) {
  if (std::holds_alternative<Duration>(v)) { w.write_u8(kValDuration); w.write_i64(std::get<Duration>(v).as_ns()); return; }
  if (std::holds_alternative<ByteCount>(v)) { w.write_u8(kValByteCount); w.write_i64(std::get<ByteCount>(v).as_bytes()); return; }
  if (std::holds_alternative<Count>(v)) { w.write_u8(kValCount); w.write_i64(std::get<Count>(v).value()); return; }
  if (std::holds_alternative<BasisPoints>(v)) { w.write_u8(kValBasisPoints); w.write_i64(std::get<BasisPoints>(v).value()); return; }
  if (std::holds_alternative<Percentage>(v)) { w.write_u8(kValPercentage); double d = std::get<Percentage>(v).value(); w.write_pod(d); return; }
  if (std::holds_alternative<PressureRatio>(v)) { w.write_u8(kValPressure); double d = std::get<PressureRatio>(v).value(); w.write_pod(d); return; }
  if (std::holds_alternative<RequestsPerSecond>(v)) { w.write_u8(kValRps); double d = std::get<RequestsPerSecond>(v).value(); w.write_pod(d); return; }
  if (std::holds_alternative<TokensPerSecond>(v)) { w.write_u8(kValTps); double d = std::get<TokensPerSecond>(v).value(); w.write_pod(d); return; }
  if (std::holds_alternative<BytesPerSecond>(v)) { w.write_u8(kValBps); double d = std::get<BytesPerSecond>(v).value(); w.write_pod(d); return; }
  if (std::holds_alternative<OperationsPerSecond>(v)) { w.write_u8(kValOps); double d = std::get<OperationsPerSecond>(v).value(); w.write_pod(d); return; }
  if (std::holds_alternative<CostMicros>(v)) { w.write_u8(kValCost); w.write_i64(std::get<CostMicros>(v).as_micros()); return; }
  if (std::holds_alternative<EnergyJoules>(v)) { w.write_u8(kValEnergy); w.write_i64(std::get<EnergyJoules>(v).value()); return; }
  w.write_u8(255);  // unknown; deserialize rejects
}

bool read_value(ByteReader& r, ObjectiveValue& out) {
  std::uint8_t tag;
  if (!r.read_u8(tag)) return false;
  std::int64_t i;
  double d;
  switch (tag) {
    case kValDuration: if (!r.read_i64(i)) return false; out = Duration(i); return true;
    case kValByteCount: if (!r.read_i64(i)) return false; out = ByteCount(i); return true;
    case kValCount: if (!r.read_i64(i)) return false; out = Count(i); return true;
    case kValBasisPoints: if (!r.read_i64(i)) return false; out = BasisPoints(i); return true;
    case kValPercentage: if (!r.read_pod(d)) return false; if (!Percentage(d).is_valid()) return false; out = Percentage(d); return true;
    case kValPressure: if (!r.read_pod(d)) return false; if (!PressureRatio(d).is_valid()) return false; out = PressureRatio(d); return true;
    case kValRps: if (!r.read_pod(d)) return false; if (!RequestsPerSecond(d).is_valid()) return false; out = RequestsPerSecond(d); return true;
    case kValTps: if (!r.read_pod(d)) return false; if (!TokensPerSecond(d).is_valid()) return false; out = TokensPerSecond(d); return true;
    case kValBps: if (!r.read_pod(d)) return false; if (!BytesPerSecond(d).is_valid()) return false; out = BytesPerSecond(d); return true;
    case kValOps: if (!r.read_pod(d)) return false; if (!OperationsPerSecond(d).is_valid()) return false; out = OperationsPerSecond(d); return true;
    case kValCost: if (!r.read_i64(i)) return false; if (i < 0) return false; out = CostMicros(i); return true;
    case kValEnergy: if (!r.read_i64(i)) return false; if (i < 0) return false; out = EnergyJoules(i); return true;
    default: return false;
  }
}

void write_string(ByteWriter& w, const std::string& s) { w.write_string(s); }
bool read_string(ByteReader& r, std::string& s) { return r.read_string(s, kMaxPersistenceBytes); }

template <typename Tag>
void write_id(ByteWriter& w, const Id<Tag>& id) { w.write_u64(id.value); }
template <typename Tag>
bool read_id(ByteReader& r, Id<Tag>& id) { std::uint64_t v; if (!r.read_u64(v)) return false; id = Id<Tag>(v); return true; }
template <typename Tag>
void write_gen(ByteWriter& w, const Gen<Tag>& g) { w.write_u64(g.value); }
template <typename Tag>
bool read_gen(ByteReader& r, Gen<Tag>& g) { std::uint64_t v; if (!r.read_u64(v)) return false; g = Gen<Tag>(v); return true; }

bool encode_objective(ByteWriter& w, const Objective& o) {
  write_id(w, o.id); write_gen(w, o.gen);
  w.write_string(o.name);
  w.write_u8(static_cast<std::uint8_t>(o.dim));
  w.write_u8(static_cast<std::uint8_t>(o.kind));
  w.write_u8(static_cast<std::uint8_t>(o.comparison));
  w.write_u8(static_cast<std::uint8_t>(o.hard));
  w.write_u8(static_cast<std::uint8_t>(o.enforcement));
  write_value(w, o.target);
  w.write_bool(o.breach_threshold.has_value());
  if (o.breach_threshold) write_value(w, *o.breach_threshold);
  w.write_bool(o.recovery_threshold.has_value());
  if (o.recovery_threshold) write_value(w, *o.recovery_threshold);
  w.write_i64(o.min_evidence.value());
  w.write_pod(o.allowed_uncertainty);
  w.write_i64(o.freshness_ttl.as_ns());
  w.write_i64(o.grace_period.as_ns());
  w.write_u8(static_cast<std::uint8_t>(o.window_type));
  w.write_i64(o.window_duration.as_ns());
  w.write_i64(o.window_count.value());
  w.write_i32(static_cast<std::int32_t>(o.priority.value));
  w.write_pod(o.percentile_rank);
  w.write_u8(static_cast<std::uint8_t>(o.availability_mode));
  w.write_u8(static_cast<std::uint8_t>(o.rate_tag));
  return true;
}

bool decode_objective(ByteReader& r, Objective& o) {
  if (!read_id(r, o.id)) return false;
  if (!read_gen(r, o.gen)) return false;
  if (!read_string(r, o.name)) return false;
  std::uint8_t b;
  if (!r.read_u8(b) || b > 5) return false; o.dim = static_cast<Dimension>(b);
  if (!r.read_u8(b) || b > 11) return false; o.kind = static_cast<ObjectiveKind>(b);
  if (!r.read_u8(b) || b > 1) return false; o.comparison = static_cast<Comparison>(b);
  if (!r.read_u8(b) || b > 1) return false; o.hard = static_cast<HardSoft>(b);
  if (!r.read_u8(b) || b > 3) return false; o.enforcement = static_cast<EnforcementClass>(b);
  if (!read_value(r, o.target)) return false;
  bool has;
  if (!r.read_bool(has)) return false; if (has) { ObjectiveValue v; if (!read_value(r, v)) return false; o.breach_threshold = v; }
  if (!r.read_bool(has)) return false; if (has) { ObjectiveValue v; if (!read_value(r, v)) return false; o.recovery_threshold = v; }
  std::int64_t i64;
  if (!r.read_i64(i64) || i64 < 0) return false; o.min_evidence = Count(i64);
  if (!r.read_pod(o.allowed_uncertainty) || o.allowed_uncertainty < 0.0 || o.allowed_uncertainty > 1.0) return false;
  if (!r.read_i64(i64) || i64 < 0) return false; o.freshness_ttl = Duration(i64);
  if (!r.read_i64(i64)) return false; o.grace_period = Duration(i64);
  if (!r.read_u8(b) || b > 4) return false; o.window_type = static_cast<WindowType>(b);
  if (!r.read_i64(i64)) return false; o.window_duration = Duration(i64);
  if (!r.read_i64(i64) || i64 <= 0) return false; o.window_count = Count(i64);
  std::int32_t i32;
  if (!r.read_i32(i32)) return false; o.priority = Priority(static_cast<int>(i32));
  if (!r.read_pod(o.percentile_rank) || o.percentile_rank < 0.0 || o.percentile_rank > 1.0) return false;
  if (!r.read_u8(b) || b > 1) return false; o.availability_mode = static_cast<AvailabilityMode>(b);
  if (!r.read_u8(b) || b > 3) return false; o.rate_tag = static_cast<RateTag>(b);
  if (!o.target_unit_matches_dimension()) return false;  // unit must match dimension
  return true;
}

bool encode_contract(ByteWriter& w, const SloContract& c) {
  write_id(w, c.id); write_gen(w, c.gen);
  write_id(w, c.service); write_id(w, c.workload); write_id(w, c.tenant);
  w.write_u64(static_cast<std::uint64_t>(c.objectives.size()));
  for (const auto& o : c.objectives) encode_objective(w, o);
  write_id(w, c.policy_id); write_gen(w, c.policy_gen);
  w.write_i64(c.effective_from.as_ns());
  w.write_bool(c.expiration.has_value());
  if (c.expiration) w.write_i64(c.expiration->as_ns());
  write_gen(w, c.epoch);
  w.write_u8(static_cast<std::uint8_t>(c.lifecycle));
  w.write_string(c.name);
  w.write_string(c.provenance);
  return true;
}

bool decode_contract(ByteReader& r, SloContract& c) {
  if (!read_id(r, c.id)) return false;
  if (!read_gen(r, c.gen)) return false;
  if (!read_id(r, c.service)) return false;
  if (!read_id(r, c.workload)) return false;
  if (!read_id(r, c.tenant)) return false;
  std::uint64_t n;
  if (!r.read_u64(n) || n > kMaxCollectionItems) return false;
  c.objectives.clear();
  c.objectives.reserve(n);
  for (std::uint64_t i = 0; i < n; ++i) { Objective o; if (!decode_objective(r, o)) return false; c.objectives.push_back(std::move(o)); }
  if (!read_id(r, c.policy_id)) return false;
  if (!read_gen(r, c.policy_gen)) return false;
  std::int64_t i64;
  if (!r.read_i64(i64)) return false; c.effective_from = Duration(i64);
  bool has;
  if (!r.read_bool(has)) return false; if (has) { if (!r.read_i64(i64)) return false; c.expiration = Duration(i64); }
  if (!read_gen(r, c.epoch)) return false;
  std::uint8_t b;
  if (!r.read_u8(b) || b > 5) return false; c.lifecycle = static_cast<ContractLifecycle>(b);
  if (!read_string(r, c.name)) return false;
  if (!read_string(r, c.provenance)) return false;
  return true;
}

bool encode_policy(ByteWriter& w, const SloPolicy& p) {
  write_id(w, p.id); write_gen(w, p.gen);
  w.write_u8(static_cast<std::uint8_t>(p.mode));
  w.write_u8(static_cast<std::uint8_t>(p.tie_break));
  w.write_string(p.name); w.write_string(p.description);
  return true;
}

bool decode_policy(ByteReader& r, SloPolicy& p) {
  if (!read_id(r, p.id)) return false;
  if (!read_gen(r, p.gen)) return false;
  std::uint8_t b;
  if (!r.read_u8(b) || b > 1) return false; p.mode = static_cast<PolicyMode>(b);
  if (!r.read_u8(b) || b > 2) return false; p.tie_break = static_cast<TieBreak>(b);
  if (!read_string(r, p.name)) return false;
  if (!read_string(r, p.description)) return false;
  return true;
}

bool encode_receipt(ByteWriter& w, const EnforcementReceipt& rc) {
  write_id(w, rc.id); write_gen(w, rc.gen);
  w.write_u8(static_cast<std::uint8_t>(rc.action));
  w.write_u8(static_cast<std::uint8_t>(rc.lifecycle));
  write_gen(w, rc.epoch); write_gen(w, rc.contract_gen); write_gen(w, rc.policy_gen);
  write_id(w, rc.evaluation_id); write_gen(w, rc.evaluation_gen);
  write_gen(w, rc.workload_gen);
  write_id(w, rc.service); write_id(w, rc.workload);
  write_id(w, rc.objective_id); write_gen(w, rc.objective_gen);
  w.write_string(rc.reason);
  return true;
}

bool decode_receipt(ByteReader& r, EnforcementReceipt& rc) {
  if (!read_id(r, rc.id)) return false;
  if (!read_gen(r, rc.gen)) return false;
  std::uint8_t b;
  if (!r.read_u8(b) || b > 23) return false; rc.action = static_cast<EnforcementAction>(b);
  if (!r.read_u8(b) || b > 9) return false; rc.lifecycle = static_cast<EnforcementLifecycle>(b);
  if (!read_gen(r, rc.epoch)) return false;
  if (!read_gen(r, rc.contract_gen)) return false;
  if (!read_gen(r, rc.policy_gen)) return false;
  if (!read_id(r, rc.evaluation_id)) return false;
  if (!read_gen(r, rc.evaluation_gen)) return false;
  if (!read_gen(r, rc.workload_gen)) return false;
  if (!read_id(r, rc.service)) return false;
  if (!read_id(r, rc.workload)) return false;
  if (!read_id(r, rc.objective_id)) return false;
  if (!read_gen(r, rc.objective_gen)) return false;
  if (!read_string(r, rc.reason)) return false;
  return true;
}

bool encode_objstate(ByteWriter& w, const PersistedObjectiveState& s) {
  write_id(w, s.objective_id); write_gen(w, s.objective_gen);
  w.write_i64(s.budget_total.value()); w.write_i64(s.budget_consumed.value());
  w.write_i64(s.budget_window.as_ns()); w.write_i64(s.budget_window_start.as_ns());
  w.write_u8(static_cast<std::uint8_t>(s.last_state));
  w.write_i64(s.last_state_time.as_ns()); w.write_i64(s.cooldown_until.as_ns());
  return true;
}

bool decode_objstate(ByteReader& r, PersistedObjectiveState& s) {
  if (!read_id(r, s.objective_id)) return false;
  if (!read_gen(r, s.objective_gen)) return false;
  std::int64_t i64;
  if (!r.read_i64(i64) || i64 < 0) return false; s.budget_total = Count(i64);
  if (!r.read_i64(i64) || i64 < 0) return false; s.budget_consumed = Count(i64);
  if (!r.read_i64(i64)) return false; s.budget_window = Duration(i64);
  if (!r.read_i64(i64)) return false; s.budget_window_start = Duration(i64);
  std::uint8_t b;
  if (!r.read_u8(b) || b > 7) return false; s.last_state = static_cast<ComplianceState>(b);
  if (!r.read_i64(i64)) return false; s.last_state_time = Duration(i64);
  if (!r.read_i64(i64)) return false; s.cooldown_until = Duration(i64);
  return true;
}

bool encode_eval(ByteWriter& w, const EvaluationRecord& e) {
  write_id(w, e.evaluation_id); write_gen(w, e.evaluation_gen);
  write_gen(w, e.contract_gen); write_gen(w, e.policy_gen); write_gen(w, e.evidence_gen);
  write_gen(w, e.epoch); w.write_i64(e.time.as_ns());
  w.write_u8(static_cast<std::uint8_t>(e.compliance));
  w.write_u8(static_cast<std::uint8_t>(e.binding_dimension));
  write_id(w, e.binding_objective);
  w.write_u8(static_cast<std::uint8_t>(e.selected_action));
  w.write_u64(e.digest);
  return true;
}

bool decode_eval(ByteReader& r, EvaluationRecord& e) {
  if (!read_id(r, e.evaluation_id)) return false;
  if (!read_gen(r, e.evaluation_gen)) return false;
  if (!read_gen(r, e.contract_gen)) return false;
  if (!read_gen(r, e.policy_gen)) return false;
  if (!read_gen(r, e.evidence_gen)) return false;
  if (!read_gen(r, e.epoch)) return false;
  std::int64_t i64;
  if (!r.read_i64(i64)) return false; e.time = Duration(i64);
  std::uint8_t b;
  if (!r.read_u8(b) || b > 7) return false; e.compliance = static_cast<ComplianceState>(b);
  if (!r.read_u8(b) || b > 5) return false; e.binding_dimension = static_cast<Dimension>(b);
  if (!read_id(r, e.binding_objective)) return false;
  if (!r.read_u8(b) || b > 23) return false; e.selected_action = static_cast<EnforcementAction>(b);
  if (!r.read_u64(e.digest)) return false;
  return true;
}

}  // namespace

std::vector<std::uint8_t> serialize_state(const DurableState& state) {
  ByteWriter w;
  w.write_u32(kPersistenceMagic);
  w.write_u32(kPersistenceVersion);
  write_gen(w, state.epoch);
  w.write_u64(static_cast<std::uint64_t>(state.policies.size()));
  for (const auto& p : state.policies) encode_policy(w, p);
  w.write_u64(static_cast<std::uint64_t>(state.contracts.size()));
  for (const auto& c : state.contracts) encode_contract(w, c);
  w.write_u64(static_cast<std::uint64_t>(state.objective_states.size()));
  for (const auto& s : state.objective_states) encode_objstate(w, s);
  w.write_u64(static_cast<std::uint64_t>(state.enforcement_history.size()));
  for (const auto& rc : state.enforcement_history) encode_receipt(w, rc);
  w.write_u64(static_cast<std::uint64_t>(state.evaluation_history.size()));
  for (const auto& e : state.evaluation_history) encode_eval(w, e);
  return w.take();
}

Result<DurableState> deserialize_state(const std::uint8_t* data, std::size_t n) {
  if (!data) return persistence_corrupt("null data");
  if (n > kMaxPersistenceBytes) return persistence_corrupt("oversized payload");
  ByteReader r(data, n);
  DurableState st;
  std::uint32_t magic, version;
  if (!r.read_u32(magic)) return persistence_corrupt("short header");
  if (magic != kPersistenceMagic) return persistence_corrupt("bad magic");
  if (!r.read_u32(version)) return persistence_corrupt("short header");
  if (version != kPersistenceVersion) return persistence_corrupt("unknown version");
  if (!read_gen(r, st.epoch)) return persistence_corrupt("bad epoch");
  std::uint64_t count;
  if (!r.read_u64(count) || count > kMaxCollectionItems) return persistence_corrupt("bad policy count");
  st.policies.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) { SloPolicy p; if (!decode_policy(r, p)) return persistence_corrupt("bad policy"); st.policies.push_back(std::move(p)); }
  if (!r.read_u64(count) || count > kMaxCollectionItems) return persistence_corrupt("bad contract count");
  st.contracts.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) { SloContract c; if (!decode_contract(r, c)) return persistence_corrupt("bad contract"); st.contracts.push_back(std::move(c)); }
  if (!r.read_u64(count) || count > kMaxCollectionItems) return persistence_corrupt("bad objective_state count");
  st.objective_states.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) { PersistedObjectiveState s; if (!decode_objstate(r, s)) return persistence_corrupt("bad objective state"); st.objective_states.push_back(std::move(s)); }
  if (!r.read_u64(count) || count > kMaxCollectionItems) return persistence_corrupt("bad enforcement count");
  st.enforcement_history.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) { EnforcementReceipt rc; if (!decode_receipt(r, rc)) return persistence_corrupt("bad receipt"); st.enforcement_history.push_back(std::move(rc)); }
  if (!r.read_u64(count) || count > kMaxCollectionItems) return persistence_corrupt("bad evaluation count");
  st.evaluation_history.reserve(count);
  for (std::uint64_t i = 0; i < count; ++i) { EvaluationRecord e; if (!decode_eval(r, e)) return persistence_corrupt("bad evaluation"); st.evaluation_history.push_back(std::move(e)); }
  if (!r.exhausted()) return persistence_corrupt("trailing garbage");
  return st;
}

Status save_bytes_atomic(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
  // Use a unique temp name per call so concurrent saves to the same path do not
  // collide on the temporary file. The final path is always replaced with a
  // complete, verified file (last writer wins, never a torn state).
  static std::atomic<std::uint64_t> g_tmp_seq{0};
  std::filesystem::path tmp = path;
  tmp += ".tmp." + std::to_string(g_tmp_seq.fetch_add(1));
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return internal("cannot open temp file for write");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) return internal("write failed");
  }
#if defined(_WIN32)
  // Atomic replace (concurrency-safe: a concurrent writer simply replaces the
  // file with a complete, verified one; last writer wins, never a torn state).
  if (::MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return ok();
  return internal("atomic replace failed");
#else
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) return internal("atomic replace failed: " + ec.message());
  return ok();
#endif
}

Status save_state(const std::filesystem::path& path, const DurableState& state) {
  std::vector<std::uint8_t> body = serialize_state(state);
  std::uint32_t crc = crc32(body.data(), body.size());
  std::vector<std::uint8_t> file = body;
  file.reserve(body.size() + 4);
  for (int i = 0; i < 4; ++i) file.push_back(static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu));
  return save_bytes_atomic(path, file);
}

Status load_state(const std::filesystem::path& path, DurableState& out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return not_found("persistence file not found");
  std::streamsize sz = in.tellg();
  if (sz < 4) return persistence_corrupt("file too short");
  if (static_cast<std::size_t>(sz) > kMaxPersistenceBytes + 4) return persistence_corrupt("oversized file");
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> full(static_cast<std::size_t>(sz));
  in.read(reinterpret_cast<char*>(full.data()), sz);
  if (!in) return persistence_corrupt("read failed");
  std::size_t body_len = full.size() - 4;
  const std::uint8_t* crc_bytes = full.data() + body_len;
  std::uint32_t stored = static_cast<std::uint32_t>(crc_bytes[0]) | (static_cast<std::uint32_t>(crc_bytes[1]) << 8) |
                         (static_cast<std::uint32_t>(crc_bytes[2]) << 16) | (static_cast<std::uint32_t>(crc_bytes[3]) << 24);
  std::uint32_t computed = crc32(full.data(), body_len);
  if (stored != computed) return persistence_corrupt("checksum mismatch");
  auto result = deserialize_state(full.data(), body_len);
  if (!result.ok()) return result.status();
  out = std::move(result.value_unchecked());
  return ok();
}

}  // namespace slofabric
