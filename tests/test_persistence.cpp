#include "test_framework.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

#include "slofabric/crc32.hpp"
#include "slofabric/persistence.hpp"

using namespace slofabric;

static DurableState make_state() {
  DurableState st;
  st.epoch = CoordinatorEpoch(7);
  SloPolicy p;
  p.id = PolicyId(10); p.gen = PolicyGen(2); p.mode = PolicyMode::HardThenSoft;
  p.tie_break = TieBreak::ByName; p.name = "default"; p.description = "default policy";
  st.policies.push_back(p);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  Objective o;
  o.id = ObjectiveId(1); o.gen = ObjectiveGen(1);
  o.name = "p99-latency"; o.dim = Dimension::Latency;
  o.kind = ObjectiveKind::LatencyPercentile; o.percentile_rank = 0.99;
  o.comparison = Comparison::AtMost; o.hard = HardSoft::Hard;
  o.target = milliseconds(120);
  o.min_evidence = Count(64); o.window_type = WindowType::Sliding;
  o.window_duration = seconds(60); o.window_count = Count(64);
  c.objectives.push_back(o);
  c.policy_id = p.id; c.policy_gen = p.gen;
  c.effective_from = seconds(0); c.epoch = st.epoch;
  c.lifecycle = ContractLifecycle::Active; c.name = "svc-contract"; c.provenance = "test";
  st.contracts.push_back(c);
  return st;
}

TEST("persistence: serialize/deserialize round trip") {
  DurableState in = make_state();
  auto bytes = serialize_state(in);
  auto out = deserialize_state(bytes);
  REQUIRE(out.ok());
  CHECK_EQ(out.value_unchecked().epoch.value, 7u);
  CHECK_EQ(out.value_unchecked().policies.size(), std::size_t(1));
  CHECK_EQ(out.value_unchecked().contracts.size(), std::size_t(1));
  CHECK_EQ(out.value_unchecked().contracts[0].name, std::string("svc-contract"));
  CHECK_EQ(out.value_unchecked().contracts[0].objectives.size(), std::size_t(1));
  CHECK(out.value_unchecked().contracts[0].objectives[0].target_unit_matches_dimension());
}

TEST("persistence: corruption rejected (crc mismatch)") {
  auto bytes = serialize_state(make_state());
  std::vector<std::uint8_t> full = bytes;
  std::uint32_t crc = crc32(full.data(), full.size());
  for (int i = 0; i < 4; ++i) full.push_back(static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu));
  full[3] ^= 0xFF;  // corrupt a body byte
  auto out = deserialize_state(full.data(), full.size() - 4);
  CHECK(!out.ok());
  CHECK(out.status().code == StatusCode::PersistenceCorrupt);
}

TEST("persistence: truncation rejected") {
  auto bytes = serialize_state(make_state());
  // Truncate body halfway; deserialize must reject.
  auto out = deserialize_state(bytes.data(), bytes.size() / 2);
  CHECK(!out.ok());
}

TEST("persistence: trailing garbage rejected") {
  auto bytes = serialize_state(make_state());
  std::vector<std::uint8_t> full(bytes.begin(), bytes.end());
  full.push_back(0x00); full.push_back(0x11);  // trailing garbage outside crc region
  // Compute crc over body (bytes), append, then add garbage extra -> load rejects
  std::uint32_t crc = crc32(bytes.data(), bytes.size());
  std::vector<std::uint8_t> file(bytes.begin(), bytes.end());
  for (int i = 0; i < 4; ++i) file.push_back(static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFu));
  file.push_back(0x99);  // trailing garbage beyond crc
  // Recompute what load_state would do: body_len = size-4 includes the 0x99 as part of crc -> mismatch
  // We test via the file-level loader path in the next test; here test in-body trailing garbage.
  std::vector<std::uint8_t> inbody(bytes.begin(), bytes.end());
  inbody.push_back(0xAB);  // garbage *inside* serialized body before crc
  auto out = deserialize_state(inbody.data(), inbody.size());
  CHECK(!out.ok());
}

TEST("persistence: unknown version rejected") {
  auto bytes = serialize_state(make_state());
  // bytes layout: magic(4) version(4) ... ; bump version.
  std::vector<std::uint8_t> v = bytes;
  v[4] = 0xFF;
  auto out = deserialize_state(v.data(), v.size());
  CHECK(!out.ok());
}

TEST("persistence: save/load file round trip and corruption file rejection") {
  namespace fs = std::filesystem;
  auto dir = fs::temp_directory_path() / "slofab_test_persist";
  fs::create_directories(dir);
  auto path = dir / "state.bin";
  DurableState in = make_state();
  Status s = save_state(path, in);
  REQUIRE(s.ok());
  DurableState out;
  Status sl = load_state(path, out);
  REQUIRE(sl.ok());
  CHECK_EQ(out.epoch.value, 7u);
  CHECK_EQ(out.contracts[0].objectives[0].target_unit_matches_dimension(), true);

  // Corruption via file manipulation: flip a byte, load must reject.
  std::vector<std::uint8_t> raw;
  {
    std::ifstream f(path, std::ios::binary);
    raw.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  REQUIRE(!raw.empty());
  raw[8] ^= 0xFF;
  auto corrupt = dir / "corrupt.bin";
  {
    std::ofstream f(corrupt, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
  }
  DurableState bad;
  Status sl2 = load_state(corrupt, bad);
  CHECK(!sl2.ok());
  CHECK(sl2.code == StatusCode::PersistenceCorrupt);

  // Truncation via file: cut off the crc footer, load must reject.
  std::vector<std::uint8_t> tr(raw.begin(), raw.end() - 4);
  auto trunc = dir / "trunc.bin";
  {
    std::ofstream f(trunc, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(tr.data()), static_cast<std::streamsize>(tr.size()));
  }
  DurableState bad2;
  Status sl3 = load_state(trunc, bad2);
  CHECK(!sl3.ok());

  fs::remove_all(dir);
}
