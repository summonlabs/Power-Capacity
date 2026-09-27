#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/power_domain.hpp"
#include "power_capacity/redundancy.hpp"
#include "power_capacity/status.hpp"
#include "power_capacity/units.hpp"

namespace power_capacity {

/// Stable reason codes.
///
/// Numeric values and textual tokens are both part of the public contract, are
/// never reused, and are the machine-readable explanation attached to every
/// capacity value, refusal, and revalidation outcome.
enum class ReasonCode : std::int32_t {
  Ok = 0,
  /// No evidence record was supplied for a quantity that requires one.
  EvidenceMissing = 1,
  /// The evidence record exists but its validity window closed at or before the
  /// requested instant.
  EvidenceStale = 2,
  /// The evidence record claims an observation instant later than the requested
  /// instant and is therefore not yet authoritative.
  EvidenceFromFuture = 3,
  DomainUnavailableMaintenance = 4,
  DomainUnavailableFault = 5,
  DomainUnavailableDecommissioned = 6,
  DomainUnavailableUnknown = 7,
  DomainDegradedMaintenance = 8,
  DomainDegradedFault = 9,
  DomainDegradedEvidenceUnknown = 10,
  /// The nameplate capacity is zero, so no capacity can be derived.
  ZeroNameplateCapacity = 11,
  /// A nonzero usable capacity was reduced to zero by derating.
  DerateRemovesCapacity = 12,
  /// The reserve alone consumes the whole operational capacity.
  ReserveExceedsCapacity = 13,
  /// Protected load alone exceeds what remains after reserve.
  ProtectedLoadExceedsCapacity = 14,
  /// Committed load alone exceeds what remains after reserve and protected load.
  CommittedLoadExceedsCapacity = 15,
  /// A load is attributed to a domain that is currently unavailable, so that
  /// load is stranded and is being carried elsewhere or not at all.
  LoadOnUnavailableDomain = 16,
  /// The group has already lost exactly as many members as it is required to
  /// survive, so no further load may be added.
  RedundancyToleranceAtLimit = 17,
  /// The group has lost more members than it is required to survive.
  RedundancyToleranceExceeded = 18,
  /// No redundancy group was declared for a domain that was queried as part of
  /// one.
  RedundancyGroupUnknown = 19,
  /// A shared upstream domain of the group members is more restrictive than the
  /// group arithmetic.
  SharedUpstreamLimits = 20,
  /// The binding constraint on an evaluated path is an ancestor of the queried
  /// domain rather than the domain itself.
  BottleneckUpstream = 21,
  /// Recovered state has not been revalidated against current evidence and
  /// authority.
  NotRevalidated = 22,
  /// The referenced domain is not present in the authoritative state.
  DomainNotFound = 23,
  /// The presented artifact belongs to a different store identity.
  StoreIdentityMismatch = 24,
  /// The engine session that produced a token has ended; the store was reopened.
  StoreReopened = 25,
  /// The control-plane epoch or incarnation changed.
  AuthorityEpochChanged = 26,
  /// A source generation the caller depended on has advanced.
  SourceGenerationAdvanced = 27,
  /// The evidence record revision changed.
  EvidenceRevisionChanged = 28,
  /// Recovered state was revalidated against current evidence.
  RecoveredStateRevalidated = 29,
  /// The candidate load fits.
  Admissible = 30,
  /// The capacity generation advanced.
  GenerationAdvanced = 31,
  /// A domain on the evaluated path is degraded.
  PathContainsDegradedDomain = 32,
  /// The value cannot be determined from current evidence and authority.
  Unknown = 33,
  /// The requested load does not fit in the effective allocatable headroom at
  /// the binding constraint on the evaluated path.
  InsufficientHeadroom = 34,
  /// A candidate load reuses the identity of a load that is already committed.
  LoadAlreadyCommitted = 35,
};

const char* to_string(ReasonCode code) noexcept;

/// The complete exact derivation of one domain's capacity.
///
/// Order of derivation, applied to the domain's own declarations and to the
/// rolled-up loads of its subtree:
///
///   derated      = floor(usable * derate_ratio)
///   operational  = 0 if unavailable, else floor(derated * degradation_ratio)
///   reserve      = per reserve policy applied to operational
///   safe         = max(0, operational - reserve)
///   carryable    = max(0, safe - protected_load)
///   allocatable  = max(0, carryable - committed_load)
///
/// Every subtraction reports whether it clamped, so a clamped zero is never
/// reported as an ordinary zero.
struct CapacityDerivation {
  Power nominal{};
  Power usable{};
  Power derated{};
  Power operational{};
  Power reserve{};
  Power protected_load{};
  Power committed_load{};
  Power safe_capacity{};
  Power carryable_capacity{};
  Power allocatable_headroom{};

  bool reserve_clamped = false;
  bool protected_clamped = false;
  bool committed_clamped = false;
  /// Committed or protected load is attributed to a domain that is unavailable.
  bool load_on_unavailable = false;
  bool degraded = false;

  ReasonCode reason = ReasonCode::Ok;
};

/// Derives a domain's capacity from its own declarations plus the rolled-up load
/// of its subtree.
///
/// `rolled_up_protected_load` and `rolled_up_committed_load` must already include
/// the domain's directly attached loads and the totals of every descendant.
Result<CapacityDerivation> derive_domain_capacity(const PowerDomain& domain,
                                                  Power rolled_up_protected_load,
                                                  Power rolled_up_committed_load,
                                                  const ResourceLimits& limits);

/// One member of a redundancy group, as seen by the group arithmetic.
struct GroupMemberView {
  DomainId domain;
  /// `safe_capacity - protected_load`: the total load this member can carry.
  Power carryable{};
  /// Load currently attributed to this member and its subtree.
  Power committed{};
  bool unavailable = false;
  bool degraded = false;
};

/// The exact redundancy arithmetic of one group.
///
/// The group can carry `survivable_capacity` and currently carries `group_load`.
/// `survivable_capacity` is the sum of member carrying ability minus the
/// carrying ability of the members that the obligation allows to be lost at the
/// same time, which is the closed form of
/// `min over all tolerated failure sets of the sum over survivors`.
struct GroupDerivation {
  GroupId group;
  std::uint32_t tolerance = 0;
  std::size_t member_count = 0;
  std::size_t unavailable_members = 0;
  std::size_t degraded_members = 0;
  /// Sum of `carryable` over all members.
  Power member_carryable_total{};
  /// Carrying ability removed by the tolerated simultaneous failures.
  Power tolerated_loss{};
  /// `member_carryable_total - tolerated_loss`.
  Power survivable_capacity{};
  /// Sum of `committed` over all members.
  Power group_load{};
  /// `max(0, survivable_capacity - group_load)`, before shared-upstream limits.
  Power raw_headroom{};
  /// The load the surviving members could still physically carry while the
  /// obligation is unmet or exactly at its limit. Reported for diagnosis; it is
  /// not allocatable, because committing it would consume a margin the group is
  /// required to keep.
  Power physical_headroom{};
  bool tolerance_consumed = false;
  bool tolerance_exceeded = false;

  /// The most restrictive common ancestor of the members, when one exists.
  std::optional<DomainId> shared_upstream_limiter;
  std::optional<Power> shared_upstream_headroom{};
  /// `min(raw_headroom, shared_upstream_headroom)` when the limiter exists.
  Power effective_headroom{};

  ReasonCode reason = ReasonCode::Ok;
};

/// Derives a group's redundancy arithmetic from its members.
///
/// Members must be distinct and ordered arbitrarily; the arithmetic is
/// order-independent because the tolerated loss is taken over the largest
/// carrying abilities.
Result<GroupDerivation> derive_group_capacity(const RedundancyGroup& group,
                                              const std::vector<GroupMemberView>& members,
                                              const ResourceLimits& limits);

/// Applies a shared-upstream limit to a group derivation in place.
void apply_shared_upstream_limit(GroupDerivation& derivation, DomainId limiter,
                                 Power limiter_headroom) noexcept;

}  // namespace power_capacity
