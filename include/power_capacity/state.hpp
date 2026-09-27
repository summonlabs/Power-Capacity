#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "power_capacity/capacity.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/load.hpp"
#include "power_capacity/power_domain.hpp"
#include "power_capacity/redundancy.hpp"
#include "power_capacity/status.hpp"

namespace power_capacity {

/// A durable record that an idempotency key was applied at a generation.
struct AppliedOperation {
  IdempotencyKey key;
  Generation generation;
};

/// The mutable inputs of one authoritative capacity state.
struct CapacityContent {
  std::map<DomainId, PowerDomain> domains;
  std::map<LoadId, LoadRecord> loads;
  std::map<GroupId, RedundancyGroup> groups;
  std::map<SourceId, Generation> source_generations;
  /// Bounded history of the most recently applied idempotency keys, oldest
  /// first. Its length never exceeds `ResourceLimits::max_applied_operations`.
  std::vector<AppliedOperation> applied_operations;

  StoreIdentity store_identity;
  Generation generation;
  Epoch epoch;
  Incarnation incarnation;
  Tick created_at;
  Tick updated_at;
  Tick last_revalidated_at;
  /// The path the store was created at, recorded so that a path binding check can
  /// detect a store that was moved or swapped in.
  std::string created_path;
};

/// An immutable, fully validated capacity state with every derived rollup already
/// computed.
///
/// Construction is the only way to create one, and construction either produces a
/// state in which every invariant holds or fails with a typed error. Queries
/// against a state therefore never have to defend against a partially applied
/// mutation, and readers never observe a half-built index.
class CapacityState {
 public:
  CapacityState() = default;

  /// Validates all invariants, computes all rollups, and returns an immutable
  /// state. Returns `Corruption` for a structurally broken input and
  /// `InvariantViolation` for a semantically impossible one.
  static Result<std::shared_ptr<const CapacityState>> build(CapacityContent content,
                                                            const ResourceLimits& limits);

  const CapacityContent& content() const noexcept;
  const ResourceLimits& limits() const noexcept;

  StoreIdentity store_identity() const noexcept;
  Generation generation() const noexcept;
  Epoch epoch() const noexcept;
  Incarnation incarnation() const noexcept;
  Tick created_at() const noexcept;
  Tick updated_at() const noexcept;
  Tick last_revalidated_at() const noexcept;
  const std::string& created_path() const noexcept;

  const std::map<DomainId, PowerDomain>& domains() const noexcept;
  const std::map<LoadId, LoadRecord>& loads() const noexcept;
  const std::map<GroupId, RedundancyGroup>& groups() const noexcept;
  const std::map<SourceId, Generation>& source_generations() const noexcept;
  const std::vector<AppliedOperation>& applied_operations() const noexcept;

  const PowerDomain* find_domain(const DomainId& id) const noexcept;
  const LoadRecord* find_load(const LoadId& id) const noexcept;
  const RedundancyGroup* find_group(const GroupId& id) const noexcept;
  std::optional<Generation> find_source_generation(const SourceId& id) const noexcept;

  /// Immediate children, sorted ascending. Null when the domain is unknown.
  const std::vector<DomainId>* children_of(const DomainId& id) const noexcept;
  /// The group a domain belongs to, when it belongs to one.
  std::optional<GroupId> group_of(const DomainId& id) const noexcept;
  /// 0 for a root domain.
  std::uint32_t depth_of(const DomainId& id) const noexcept;
  /// The domain itself followed by each ancestor up to the root.
  std::vector<DomainId> ancestry_of(const DomainId& id) const;
  bool is_ancestor_of(const DomainId& ancestor, const DomainId& descendant) const noexcept;

  const CapacityDerivation* derivation_of(const DomainId& id) const noexcept;
  const GroupDerivation* group_derivation_of(const GroupId& id) const noexcept;

  Power rolled_up_committed_of(const DomainId& id) const noexcept;
  Power rolled_up_protected_of(const DomainId& id) const noexcept;

  std::size_t domain_count() const noexcept;
  std::size_t load_count() const noexcept;
  std::size_t group_count() const noexcept;
  std::size_t source_count() const noexcept;

 private:
  struct Impl;
  /// Returns the backing implementation, or a shared empty one when this handle
  /// is default-constructed.
  const Impl& impl() const noexcept;

  std::shared_ptr<const Impl> impl_;
};

}  // namespace power_capacity
