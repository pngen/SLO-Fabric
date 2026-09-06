#pragma once

// SLO Fabric - strongly typed identity and generation model.
//
// Distinct authority domains must not be interchangeable integers. We model
// each identity and each generation as a distinct value type keyed by a
// unique tag. Equality, ordering, hashing, formatting, and canonical
// encoding are all deterministic.

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>

namespace slofabric {

// A strongly typed 64-bit identity. value 0 is reserved as "not set"/empty.
template <typename Tag>
struct Id {
  using tag_type = Tag;
  using value_type = std::uint64_t;
  value_type value{0};

  constexpr Id() noexcept = default;
  constexpr explicit Id(value_type v) noexcept : value(v) {}

  constexpr bool valid() const noexcept { return value != value_type{0}; }
  constexpr bool invalid() const noexcept { return value == value_type{0}; }

  // Canonical decimal string form. Deterministic.
  [[nodiscard]] std::string to_string() const { return std::to_string(value); }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.value == b.value; }
  friend constexpr bool operator!=(Id a, Id b) noexcept { return a.value != b.value; }
  friend constexpr bool operator<(Id a, Id b) noexcept { return a.value < b.value; }
  friend constexpr bool operator<=(Id a, Id b) noexcept { return a.value <= b.value; }
  friend constexpr bool operator>(Id a, Id b) noexcept { return a.value > b.value; }
  friend constexpr bool operator>=(Id a, Id b) noexcept { return a.value >= b.value; }
};

// A strongly typed 64-bit generation counter. Generation 0 is invalid /
// "unassigned"; generations begin at 1.
template <typename Tag>
struct Gen {
  using tag_type = Tag;
  using value_type = std::uint64_t;
  value_type value{0};

  constexpr Gen() noexcept = default;
  constexpr explicit Gen(value_type v) noexcept : value(v) {}

  constexpr bool valid() const noexcept { return value != value_type{0}; }
  constexpr bool invalid() const noexcept { return value == value_type{0}; }

  constexpr Gen next() const noexcept { return Gen(value + 1); }
  constexpr std::uint64_t raw() const noexcept { return value; }

  [[nodiscard]] std::string to_string() const { return std::to_string(value); }

  friend constexpr bool operator==(Gen a, Gen b) noexcept { return a.value == b.value; }
  friend constexpr bool operator!=(Gen a, Gen b) noexcept { return a.value != b.value; }
  friend constexpr bool operator<(Gen a, Gen b) noexcept { return a.value < b.value; }
  friend constexpr bool operator<=(Gen a, Gen b) noexcept { return a.value <= b.value; }
  friend constexpr bool operator>(Gen a, Gen b) noexcept { return a.value > b.value; }
  friend constexpr bool operator>=(Gen a, Gen b) noexcept { return a.value >= b.value; }
};

// Priority: lower value = higher priority, for resolving competing
// objectives. Deterministically ordered.
struct Priority {
  int value{0};
  constexpr Priority() noexcept = default;
  constexpr explicit Priority(int v) noexcept : value(v) {}
  friend constexpr bool operator==(Priority a, Priority b) noexcept { return a.value == b.value; }
  friend constexpr bool operator<(Priority a, Priority b) noexcept { return a.value < b.value; }
  friend constexpr bool operator>(Priority a, Priority b) noexcept { return a.value > b.value; }
  friend constexpr bool operator<=(Priority a, Priority b) noexcept { return a.value <= b.value; }
  friend constexpr bool operator>=(Priority a, Priority b) noexcept { return a.value >= b.value; }
};

// Declare a full authority domain: an Id and a Generation.
#define SLOFABRIC_DOMAIN(Name)                                              struct Name##Id_tag final {};                                             using Name##Id = ::slofabric::Id<Name##Id_tag>;                           struct Name##Gen_tag final {};                                            using Name##Gen = ::slofabric::Gen<Name##Gen_tag>

// Epochs are monotonic counters (a Generation semantics with a dedicated tag).
#define SLOFABRIC_EPOCH(Name)                                               struct Name##_tag final {};                                               using Name = ::slofabric::Gen<Name##_tag>

// Domains from the specification.
SLOFABRIC_DOMAIN(Service);
SLOFABRIC_DOMAIN(Workload);
SLOFABRIC_DOMAIN(Tenant);
SLOFABRIC_DOMAIN(SloContract);
SLOFABRIC_DOMAIN(Objective);
SLOFABRIC_DOMAIN(Policy);
SLOFABRIC_DOMAIN(Evidence);
SLOFABRIC_DOMAIN(Evaluation);
SLOFABRIC_DOMAIN(Enforcement);

SLOFABRIC_EPOCH(CoordinatorEpoch);

// Boot identities are per-OS-process incarnations.
SLOFABRIC_DOMAIN(SourceBoot);
SLOFABRIC_DOMAIN(WorkerBoot);

// Additional authority domains used by enforcement / resources.
SLOFABRIC_DOMAIN(Action);
SLOFABRIC_DOMAIN(Dispatch);
SLOFABRIC_DOMAIN(Attempt);
SLOFABRIC_DOMAIN(Resource);
SLOFABRIC_DOMAIN(Topology);
SLOFABRIC_DOMAIN(RecoveryPlan);

#undef SLOFABRIC_DOMAIN
#undef SLOFABRIC_EPOCH

// Stream formatting for IDs and generations.
template <typename Tag>
std::ostream& operator<<(std::ostream& os, const Id<Tag>& id) {
  os << id.to_string();
  return os;
}
template <typename Tag>
std::ostream& operator<<(std::ostream& os, const Gen<Tag>& g) {
  os << g.to_string();
  return os;
}

}  // namespace slofabric

// std::hash specializations (deterministic).
namespace std {
template <typename Tag>
struct hash<::slofabric::Id<Tag>> {
  size_t operator()(const ::slofabric::Id<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value);
  }
};
template <typename Tag>
struct hash<::slofabric::Gen<Tag>> {
  size_t operator()(const ::slofabric::Gen<Tag>& g) const noexcept {
    return std::hash<std::uint64_t>{}(g.value);
  }
};
}  // namespace std
