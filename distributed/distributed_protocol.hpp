#pragma once
// Distributed proof message payloads (typed, bounded). The transport framing
// (magic + length + CRC) lives in slofabric/net.hpp.

#include <cstdint>
#include <string>

#include "slofabric/binary.hpp"
#include "slofabric/identities.hpp"
#include "slofabric/net.hpp"

namespace slofabric_proof {

inline constexpr std::uint16_t kDefaultPort = 45700;

// Hello: worker -> coordinator.
inline std::vector<std::uint8_t> encode_hello(const std::string& name, std::uint64_t boot_id,
                                              std::uint64_t epoch) {
  slofabric::ByteWriter w;
  w.write_string(name);
  w.write_u64(boot_id);
  w.write_u64(epoch);
  return w.take();
}
inline bool decode_hello(const std::vector<std::uint8_t>& p, std::string& name,
                         std::uint64_t& boot_id, std::uint64_t& epoch) {
  slofabric::ByteReader r(p.data(), p.size());
  if (!r.read_string(name, 256)) return false;
  if (!r.read_u64(boot_id)) return false;
  if (!r.read_u64(epoch)) return false;
  return true;
}

// RegisterAck: coordinator -> worker.
inline std::vector<std::uint8_t> encode_ack(std::uint64_t epoch, std::uint64_t granted_boot,
                                            int status_code, const std::string& status) {
  slofabric::ByteWriter w;
  w.write_u64(epoch);
  w.write_u64(granted_boot);
  w.write_u8(static_cast<std::uint8_t>(status_code));
  w.write_string(status);
  return w.take();
}
inline bool decode_ack(const std::vector<std::uint8_t>& p, std::uint64_t& epoch,
                       std::uint64_t& granted_boot, int& status_code, std::string& status) {
  slofabric::ByteReader r(p.data(), p.size());
  if (!r.read_u64(epoch)) return false;
  if (!r.read_u64(granted_boot)) return false;
  std::uint8_t sc; if (!r.read_u8(sc)) return false;
  status_code = sc;
  if (!r.read_string(status, 512)) return false;
  return true;
}

// Evidence: worker -> coordinator. value is a double (latency ns / availability bp
// / throughput rate). dimension selects interpretation.
inline std::vector<std::uint8_t> encode_evidence(std::uint64_t objective_id,
                                                 std::uint8_t dimension,
                                                 double value, std::int64_t sample_count,
                                                 std::int64_t time_ns, std::uint64_t boot_id,
                                                 std::uint64_t epoch) {
  slofabric::ByteWriter w;
  w.write_u64(objective_id);
  w.write_u8(dimension);
  w.write_pod(value);
  w.write_i64(sample_count);
  w.write_i64(time_ns);
  w.write_u64(boot_id);
  w.write_u64(epoch);
  return w.take();
}
inline bool decode_evidence(const std::vector<std::uint8_t>& p, std::uint64_t& objective_id,
                            std::uint8_t& dimension, double& value, std::int64_t& sample_count,
                            std::int64_t& time_ns, std::uint64_t& boot_id, std::uint64_t& epoch) {
  slofabric::ByteReader r(p.data(), p.size());
  if (!r.read_u64(objective_id)) return false;
  if (!r.read_u8(dimension)) return false;
  if (!r.read_pod(value)) return false;
  if (!r.read_i64(sample_count)) return false;
  if (!r.read_i64(time_ns)) return false;
  if (!r.read_u64(boot_id)) return false;
  if (!r.read_u64(epoch)) return false;
  return true;
}

// EvaluateReply: coordinator -> control client. Holds the decision fields as text.
inline std::vector<std::uint8_t> encode_reply(std::uint64_t epoch, std::uint64_t contract_gen,
                                              std::uint64_t policy_gen, std::uint8_t compliance,
                                              std::uint8_t binding_dim, std::uint8_t action,
                                              std::uint64_t objective_id, double measured,
                                              const std::string& text) {
  slofabric::ByteWriter w;
  w.write_u64(epoch);
  w.write_u64(contract_gen);
  w.write_u64(policy_gen);
  w.write_u8(compliance);
  w.write_u8(binding_dim);
  w.write_u8(action);
  w.write_u64(objective_id);
  w.write_pod(measured);
  w.write_string(text);
  return w.take();
}
inline bool decode_reply(const std::vector<std::uint8_t>& p, std::uint64_t& epoch,
                         std::uint64_t& contract_gen, std::uint64_t& policy_gen,
                         std::uint8_t& compliance, std::uint8_t& binding_dim,
                         std::uint8_t& action, std::uint64_t& objective_id,
                         double& measured, std::string& text) {
  slofabric::ByteReader r(p.data(), p.size());
  if (!r.read_u64(epoch)) return false;
  if (!r.read_u64(contract_gen)) return false;
  if (!r.read_u64(policy_gen)) return false;
  if (!r.read_u8(compliance)) return false;
  if (!r.read_u8(binding_dim)) return false;
  if (!r.read_u8(action)) return false;
  if (!r.read_u64(objective_id)) return false;
  if (!r.read_pod(measured)) return false;
  if (!r.read_string(text, 2048)) return false;
  return true;
}

}  // namespace slofabric_proof
