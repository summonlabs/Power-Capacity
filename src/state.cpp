#include "power_capacity/state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "detail/validate.hpp"

namespace power_capacity {
namespace {

Status check_count(std::size_t actual, std::size_t limit, const char* what) {
  if (actual > limit) {
    return Status::error(StatusCode::LimitExceeded,
                         std::string(what) + " count " + std::to_string(actual) +
                             " exceeds the limit of " + std::to_string(limit));
  }
  return Status::success();
}

Status add_checked(Power& accumulator, Power value, const char* what,
                   const ResourceLimits& limits) {
  const Result<Power> sum = add_power(accumulator, value);
  if (!sum.ok()) {
    return Status::error(sum.status().code(), std::string(what) + ": " + sum.status().message());
  }
  if (sum.value().milliwatts() > limits.max_aggregate_milliwatts) {
    return Status::error(StatusCode::LimitExceeded,
                         std::string(what) + " rollup of " +
                             std::to_string(sum.value().milliwatts()) +
                             " mW exceeds the aggregate limit of " +
                             std::to_string(limits.max_aggregate_milliwatts) + " mW");
  }
  accumulator = sum.value();
  return Status::success();
}

}  // namespace

struct CapacityState::Impl {
  CapacityContent content;
  ResourceLimits limits;

  std::map<DomainId, std::vector<DomainId>> children;
  std::map<DomainId, std::uint32_t> depth;
  std::map<DomainId, GroupId> domain_group;

  std::map<DomainId, Power> direct_committed;
  std::map<DomainId, Power> direct_protected;
  std::map<DomainId, Power> rollup_committed;
  std::map<DomainId, Power> rollup_protected;

  std::map<DomainId, CapacityDerivation> derivations;
  std::map<GroupId, GroupDerivation> group_derivations;
};

const CapacityState::Impl& CapacityState::impl() const noexcept {
  static const Impl empty;
  return impl_ ? *impl_ : empty;
}

Result<std::shared_ptr<const CapacityState>> CapacityState::build(CapacityContent content,
                                                                 const ResourceLimits& limits) {
  Status status = check_count(content.domains.size(), limits.max_domains, "domain");
  if (!status.ok()) {
    return status;
  }
  status = check_count(content.loads.size(), limits.max_loads, "load");
  if (!status.ok()) {
    return status;
  }
  status = check_count(content.groups.size(), limits.max_groups, "redundancy group");
  if (!status.ok()) {
    return status;
  }
  status = check_count(content.source_generations.size(), limits.max_sources, "source");
  if (!status.ok()) {
    return status;
  }
  status = check_count(content.applied_operations.size(), limits.max_applied_operations,
                       "applied operation");
  if (!status.ok()) {
    return status;
  }
  if (content.store_identity.is_zero()) {
    return Status::error(StatusCode::InvariantViolation,
                         "capacity state requires a non-zero store identity");
  }
  if (content.created_path.size() > limits.max_path_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "recorded store path is " + std::to_string(content.created_path.size()) +
                             " bytes, limit is " + std::to_string(limits.max_path_bytes));
  }
  if (content.created_at.value() < 0 || content.updated_at.value() < content.created_at.value() ||
      content.last_revalidated_at.value() < 0) {
    return Status::error(StatusCode::InvariantViolation,
                         "capacity state has an inconsistent instant ordering");
  }

  for (const auto& entry : content.domains) {
    if (entry.first != entry.second.id) {
      return Status::error(StatusCode::InvariantViolation,
                           "domain map key '" + entry.first.value() +
                               "' does not match the record identity '" +
                               entry.second.id.value() + "'");
    }
    status = validate_domain(entry.second, limits);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& entry : content.loads) {
    if (entry.first != entry.second.id) {
      return Status::error(StatusCode::InvariantViolation,
                           "load map key '" + entry.first.value() +
                               "' does not match the record identity '" +
                               entry.second.id.value() + "'");
    }
    status = validate_load(entry.second, limits);
    if (!status.ok()) {
      return status;
    }
    if (content.domains.find(entry.second.domain) == content.domains.end()) {
      return Status::error(StatusCode::NotFound,
                           "load '" + entry.second.id.value() + "' is attached to unknown domain '" +
                               entry.second.domain.value() + "'");
    }
  }
  for (const auto& entry : content.groups) {
    if (entry.first != entry.second.id) {
      return Status::error(StatusCode::InvariantViolation,
                           "redundancy group map key '" + entry.first.value() +
                               "' does not match the record identity '" +
                               entry.second.id.value() + "'");
    }
    status = validate_group(entry.second, limits);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& entry : content.source_generations) {
    status = validate_identifier(entry.first.value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "source id: " + status.message());
    }
  }
  for (const auto& entry : content.applied_operations) {
    status = validate_identifier(entry.key.value(), limits.max_idempotency_key_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "applied operation key: " + status.message());
    }
  }

  auto impl = std::make_shared<Impl>();
  impl->limits = limits;

  // --- containment forest -------------------------------------------------
  for (const auto& entry : content.domains) {
    const PowerDomain& domain = entry.second;
    if (domain.parent.has_value()) {
      if (content.domains.find(*domain.parent) == content.domains.end()) {
        return Status::error(StatusCode::NotFound,
                             "domain '" + domain.id.value() + "' declares unknown parent '" +
                                 domain.parent->value() + "'");
      }
      impl->children[*domain.parent].push_back(domain.id);
    } else {
      impl->children[domain.id];
    }
  }

  // Depth via an explicit parent walk with grey marking, so a cycle is detected
  // rather than followed, and no recursion is used.
  enum class Mark : std::uint8_t { White, Grey, Black };
  std::map<DomainId, Mark> marks;
  for (const auto& entry : content.domains) {
    marks.emplace(entry.first, Mark::White);
  }
  std::vector<DomainId> chain;
  for (const auto& entry : content.domains) {
    if (marks[entry.first] == Mark::Black) {
      continue;
    }
    chain.clear();
    DomainId current = entry.first;
    for (;;) {
      Mark& mark = marks[current];
      if (mark == Mark::Grey) {
        return Status::error(StatusCode::InvariantViolation,
                             "domain containment contains a cycle through '" + current.value() +
                                 "'");
      }
      if (mark == Mark::Black) {
        break;
      }
      mark = Mark::Grey;
      chain.push_back(current);
      const auto found = content.domains.find(current);
      if (found == content.domains.end()) {
        return Status::error(StatusCode::NotFound,
                             "domain '" + current.value() + "' is missing from the state");
      }
      if (!found->second.parent.has_value()) {
        break;
      }
      current = *found->second.parent;
    }
    while (!chain.empty()) {
      const DomainId node = chain.back();
      chain.pop_back();
      marks[node] = Mark::Black;
      const PowerDomain& domain = content.domains.at(node);
      std::uint32_t node_depth = 0;
      if (domain.parent.has_value()) {
        const auto parent_depth = impl->depth.find(*domain.parent);
        if (parent_depth == impl->depth.end()) {
          return Status::error(StatusCode::InvariantViolation,
                               "domain '" + node.value() +
                                   "' could not be ordered relative to its parent");
        }
        if (parent_depth->second >= limits.max_tree_depth) {
          return Status::error(StatusCode::LimitExceeded,
                               "domain containment depth exceeds the limit of " +
                                   std::to_string(limits.max_tree_depth) + " at '" +
                                   node.value() + "'");
        }
        node_depth = parent_depth->second + 1;
      }
      impl->depth.emplace(node, node_depth);
    }
  }

  // --- redundancy group membership ---------------------------------------
  for (const auto& entry : content.groups) {
    const RedundancyGroup& group = entry.second;
    std::set<DomainId> member_set(group.members.begin(), group.members.end());
    for (const DomainId& member : group.members) {
      const auto domain = content.domains.find(member);
      if (domain == content.domains.end()) {
        return Status::error(StatusCode::NotFound,
                             "redundancy group '" + group.id.value() + "' names unknown member '" +
                                 member.value() + "'");
      }
      const auto existing = impl->domain_group.find(member);
      if (existing != impl->domain_group.end()) {
        return Status::error(
            StatusCode::Conflict,
            "domain '" + member.value() + "' is a member of both redundancy group '" +
                existing->second.value() + "' and redundancy group '" + group.id.value() + "'");
      }
      impl->domain_group.emplace(member, group.id);
    }
    // Pairwise independence: no member may be an ancestor of another member.
    // This is what makes the group arithmetic free of double counting, because a
    // load attached below one member can then reach at most one member.
    for (const DomainId& member : group.members) {
      DomainId cursor = member;
      const PowerDomain* domain = &content.domains.at(cursor);
      while (domain->parent.has_value()) {
        cursor = *domain->parent;
        if (member_set.find(cursor) != member_set.end()) {
          return Status::error(StatusCode::InvariantViolation,
                               "redundancy group '" + group.id.value() + "' member '" +
                                   cursor.value() + "' is an ancestor of member '" +
                                   member.value() +
                                   "'; group members must be pairwise independent");
        }
        domain = &content.domains.at(cursor);
      }
    }
  }

  // --- load attribution and rollups --------------------------------------
  for (const auto& entry : content.domains) {
    impl->direct_committed.emplace(entry.first, Power(0));
    impl->direct_protected.emplace(entry.first, Power(0));
  }
  for (const auto& entry : content.loads) {
    const LoadRecord& load = entry.second;
    auto& target = load.load_class == LoadClass::Protected ? impl->direct_protected[load.domain]
                                                           : impl->direct_committed[load.domain];
    status = add_checked(target, load.load, "direct load", limits);
    if (!status.ok()) {
      return status;
    }
  }

  std::vector<std::pair<std::uint32_t, DomainId>> order;
  order.reserve(content.domains.size());
  for (const auto& entry : content.domains) {
    order.emplace_back(impl->depth.at(entry.first), entry.first);
  }
  std::sort(order.begin(), order.end(), [](const auto& left, const auto& right) {
    if (left.first != right.first) {
      return left.first > right.first;
    }
    return left.second < right.second;
  });

  for (const auto& entry : order) {
    const DomainId& id = entry.second;
    Power committed = impl->direct_committed.at(id);
    Power protected_load = impl->direct_protected.at(id);
    const auto child_it = impl->children.find(id);
    if (child_it != impl->children.end()) {
      for (const DomainId& child : child_it->second) {
        status = add_checked(committed, impl->rollup_committed.at(child), "committed load rollup",
                             limits);
        if (!status.ok()) {
          return status;
        }
        status = add_checked(protected_load, impl->rollup_protected.at(child),
                             "protected load rollup", limits);
        if (!status.ok()) {
          return status;
        }
      }
    }
    impl->rollup_committed.emplace(id, committed);
    impl->rollup_protected.emplace(id, protected_load);
  }

  // --- capacity derivations ----------------------------------------------
  for (const auto& entry : content.domains) {
    const DomainId& id = entry.first;
    const Result<CapacityDerivation> derivation =
        derive_domain_capacity(entry.second, impl->rollup_protected.at(id),
                               impl->rollup_committed.at(id), limits);
    if (!derivation.ok()) {
      return Status::error(derivation.status().code(),
                           "domain '" + id.value() + "': " + derivation.status().message());
    }
    impl->derivations.emplace(id, derivation.value());
  }

  // --- redundancy group derivations --------------------------------------
  for (const auto& entry : content.groups) {
    const RedundancyGroup& group = entry.second;
    std::vector<GroupMemberView> members;
    members.reserve(group.members.size());
    for (const DomainId& member : group.members) {
      const PowerDomain& domain = content.domains.at(member);
      const CapacityDerivation& derivation = impl->derivations.at(member);
      GroupMemberView view;
      view.domain = member;
      view.carryable = derivation.carryable_capacity;
      view.committed = derivation.committed_load;
      view.unavailable = domain.state == OperationalState::Unavailable;
      view.degraded = domain.state == OperationalState::Degraded;
      members.push_back(view);
    }
    const Result<GroupDerivation> derivation =
        derive_group_capacity(group, members, limits);
    if (!derivation.ok()) {
      return Status::error(derivation.status().code(), "redundancy group '" + group.id.value() +
                                                           "': " + derivation.status().message());
    }
    GroupDerivation group_derivation = derivation.value();

    // Common ancestors of every member form the shared delivery path. The most
    // restrictive of them bounds what the group as a whole can claim, which is
    // how shared upstream capacity is accounted once instead of per member.
    std::set<DomainId> common;
    bool first = true;
    for (const DomainId& member : group.members) {
      std::set<DomainId> ancestry;
      DomainId cursor = member;
      const PowerDomain* domain = &content.domains.at(cursor);
      while (domain->parent.has_value()) {
        cursor = *domain->parent;
        ancestry.insert(cursor);
        domain = &content.domains.at(cursor);
      }
      if (first) {
        common = std::move(ancestry);
        first = false;
      } else {
        std::set<DomainId> intersection;
        std::set_intersection(common.begin(), common.end(), ancestry.begin(), ancestry.end(),
                              std::inserter(intersection, intersection.begin()));
        common = std::move(intersection);
      }
      if (common.empty()) {
        break;
      }
    }
    std::optional<DomainId> limiter;
    std::optional<Power> limiter_headroom;
    for (const DomainId& candidate : common) {
      const auto derivation_it = impl->derivations.find(candidate);
      if (derivation_it == impl->derivations.end()) {
        continue;
      }
      const Power headroom = derivation_it->second.allocatable_headroom;
      if (!limiter_headroom.has_value() || headroom < *limiter_headroom) {
        limiter = candidate;
        limiter_headroom = headroom;
      }
    }
    if (limiter.has_value() && limiter_headroom.has_value()) {
      apply_shared_upstream_limit(group_derivation, *limiter, *limiter_headroom);
    }
    impl->group_derivations.emplace(group.id, group_derivation);
  }

  impl->content = std::move(content);
  auto holder = std::make_shared<CapacityState>();
  holder->impl_ = std::move(impl);
  return std::shared_ptr<const CapacityState>(std::move(holder));
}

const CapacityContent& CapacityState::content() const noexcept { return impl().content; }
const ResourceLimits& CapacityState::limits() const noexcept { return impl().limits; }
StoreIdentity CapacityState::store_identity() const noexcept { return impl().content.store_identity; }
Generation CapacityState::generation() const noexcept { return impl().content.generation; }
Epoch CapacityState::epoch() const noexcept { return impl().content.epoch; }
Incarnation CapacityState::incarnation() const noexcept { return impl().content.incarnation; }
Tick CapacityState::created_at() const noexcept { return impl().content.created_at; }
Tick CapacityState::updated_at() const noexcept { return impl().content.updated_at; }
Tick CapacityState::last_revalidated_at() const noexcept {
  return impl().content.last_revalidated_at;
}
const std::string& CapacityState::created_path() const noexcept { return impl().content.created_path; }

const std::map<DomainId, PowerDomain>& CapacityState::domains() const noexcept {
  return impl().content.domains;
}
const std::map<LoadId, LoadRecord>& CapacityState::loads() const noexcept {
  return impl().content.loads;
}
const std::map<GroupId, RedundancyGroup>& CapacityState::groups() const noexcept {
  return impl().content.groups;
}
const std::map<SourceId, Generation>& CapacityState::source_generations() const noexcept {
  return impl().content.source_generations;
}
const std::vector<AppliedOperation>& CapacityState::applied_operations() const noexcept {
  return impl().content.applied_operations;
}

const PowerDomain* CapacityState::find_domain(const DomainId& id) const noexcept {
  const auto& map = impl().content.domains;
  const auto found = map.find(id);
  return found == map.end() ? nullptr : &found->second;
}

const LoadRecord* CapacityState::find_load(const LoadId& id) const noexcept {
  const auto& map = impl().content.loads;
  const auto found = map.find(id);
  return found == map.end() ? nullptr : &found->second;
}

const RedundancyGroup* CapacityState::find_group(const GroupId& id) const noexcept {
  const auto& map = impl().content.groups;
  const auto found = map.find(id);
  return found == map.end() ? nullptr : &found->second;
}

std::optional<Generation> CapacityState::find_source_generation(const SourceId& id) const noexcept {
  const auto& map = impl().content.source_generations;
  const auto found = map.find(id);
  if (found == map.end()) {
    return std::nullopt;
  }
  return found->second;
}

const std::vector<DomainId>* CapacityState::children_of(const DomainId& id) const noexcept {
  const auto& map = impl().children;
  const auto found = map.find(id);
  return found == map.end() ? nullptr : &found->second;
}

std::optional<GroupId> CapacityState::group_of(const DomainId& id) const noexcept {
  const auto& map = impl().domain_group;
  const auto found = map.find(id);
  if (found == map.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::uint32_t CapacityState::depth_of(const DomainId& id) const noexcept {
  const auto& map = impl().depth;
  const auto found = map.find(id);
  return found == map.end() ? 0U : found->second;
}

std::vector<DomainId> CapacityState::ancestry_of(const DomainId& id) const {
  std::vector<DomainId> chain;
  const PowerDomain* domain = find_domain(id);
  if (domain == nullptr) {
    return chain;
  }
  chain.push_back(id);
  while (domain->parent.has_value()) {
    chain.push_back(*domain->parent);
    domain = find_domain(*domain->parent);
    if (domain == nullptr) {
      break;
    }
  }
  return chain;
}

bool CapacityState::is_ancestor_of(const DomainId& ancestor,
                                   const DomainId& descendant) const noexcept {
  const PowerDomain* domain = find_domain(descendant);
  while (domain != nullptr && domain->parent.has_value()) {
    if (*domain->parent == ancestor) {
      return true;
    }
    domain = find_domain(*domain->parent);
  }
  return false;
}

const CapacityDerivation* CapacityState::derivation_of(const DomainId& id) const noexcept {
  const auto& map = impl().derivations;
  const auto found = map.find(id);
  return found == map.end() ? nullptr : &found->second;
}

const GroupDerivation* CapacityState::group_derivation_of(const GroupId& id) const noexcept {
  const auto& map = impl().group_derivations;
  const auto found = map.find(id);
  return found == map.end() ? nullptr : &found->second;
}

Power CapacityState::rolled_up_committed_of(const DomainId& id) const noexcept {
  const auto& map = impl().rollup_committed;
  const auto found = map.find(id);
  return found == map.end() ? Power(0) : found->second;
}

Power CapacityState::rolled_up_protected_of(const DomainId& id) const noexcept {
  const auto& map = impl().rollup_protected;
  const auto found = map.find(id);
  return found == map.end() ? Power(0) : found->second;
}

std::size_t CapacityState::domain_count() const noexcept { return impl().content.domains.size(); }
std::size_t CapacityState::load_count() const noexcept { return impl().content.loads.size(); }
std::size_t CapacityState::group_count() const noexcept { return impl().content.groups.size(); }
std::size_t CapacityState::source_count() const noexcept {
  return impl().content.source_generations.size();
}

}  // namespace power_capacity
