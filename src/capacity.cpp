#include "power_capacity/capacity.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace power_capacity {
namespace {

ReasonCode unavailable_reason(StateCause cause) noexcept {
  switch (cause) {
    case StateCause::Maintenance:
      return ReasonCode::DomainUnavailableMaintenance;
    case StateCause::Fault:
      return ReasonCode::DomainUnavailableFault;
    case StateCause::Decommissioned:
      return ReasonCode::DomainUnavailableDecommissioned;
    case StateCause::EvidenceUnknown:
    case StateCause::None:
      break;
  }
  return ReasonCode::DomainUnavailableUnknown;
}

ReasonCode degraded_reason(StateCause cause) noexcept {
  switch (cause) {
    case StateCause::Maintenance:
      return ReasonCode::DomainDegradedMaintenance;
    case StateCause::Fault:
      return ReasonCode::DomainDegradedFault;
    case StateCause::EvidenceUnknown:
    case StateCause::None:
      break;
  }
  return ReasonCode::DomainDegradedEvidenceUnknown;
}

Status check_aggregate(Power value, const ResourceLimits& limits, const char* what) {
  if (value.milliwatts() > limits.max_aggregate_milliwatts) {
    return Status::error(StatusCode::LimitExceeded,
                         std::string(what) + " rollup of " + std::to_string(value.milliwatts()) +
                             " mW exceeds the aggregate limit of " +
                             std::to_string(limits.max_aggregate_milliwatts) + " mW");
  }
  return Status::success();
}

}  // namespace

const char* to_string(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::Ok:
      return "ok";
    case ReasonCode::EvidenceMissing:
      return "evidence_missing";
    case ReasonCode::EvidenceStale:
      return "evidence_stale";
    case ReasonCode::EvidenceFromFuture:
      return "evidence_from_future";
    case ReasonCode::DomainUnavailableMaintenance:
      return "domain_unavailable_maintenance";
    case ReasonCode::DomainUnavailableFault:
      return "domain_unavailable_fault";
    case ReasonCode::DomainUnavailableDecommissioned:
      return "domain_unavailable_decommissioned";
    case ReasonCode::DomainUnavailableUnknown:
      return "domain_unavailable_unknown";
    case ReasonCode::DomainDegradedMaintenance:
      return "domain_degraded_maintenance";
    case ReasonCode::DomainDegradedFault:
      return "domain_degraded_fault";
    case ReasonCode::DomainDegradedEvidenceUnknown:
      return "domain_degraded_evidence_unknown";
    case ReasonCode::ZeroNameplateCapacity:
      return "zero_nameplate_capacity";
    case ReasonCode::DerateRemovesCapacity:
      return "derate_removes_capacity";
    case ReasonCode::ReserveExceedsCapacity:
      return "reserve_exceeds_capacity";
    case ReasonCode::ProtectedLoadExceedsCapacity:
      return "protected_load_exceeds_capacity";
    case ReasonCode::CommittedLoadExceedsCapacity:
      return "committed_load_exceeds_capacity";
    case ReasonCode::LoadOnUnavailableDomain:
      return "load_on_unavailable_domain";
    case ReasonCode::RedundancyToleranceAtLimit:
      return "redundancy_tolerance_at_limit";
    case ReasonCode::RedundancyToleranceExceeded:
      return "redundancy_tolerance_exceeded";
    case ReasonCode::RedundancyGroupUnknown:
      return "redundancy_group_unknown";
    case ReasonCode::SharedUpstreamLimits:
      return "shared_upstream_limits";
    case ReasonCode::BottleneckUpstream:
      return "bottleneck_upstream";
    case ReasonCode::NotRevalidated:
      return "not_revalidated";
    case ReasonCode::DomainNotFound:
      return "domain_not_found";
    case ReasonCode::StoreIdentityMismatch:
      return "store_identity_mismatch";
    case ReasonCode::StoreReopened:
      return "store_reopened";
    case ReasonCode::AuthorityEpochChanged:
      return "authority_epoch_changed";
    case ReasonCode::SourceGenerationAdvanced:
      return "source_generation_advanced";
    case ReasonCode::EvidenceRevisionChanged:
      return "evidence_revision_changed";
    case ReasonCode::RecoveredStateRevalidated:
      return "recovered_state_revalidated";
    case ReasonCode::Admissible:
      return "admissible";
    case ReasonCode::GenerationAdvanced:
      return "generation_advanced";
    case ReasonCode::PathContainsDegradedDomain:
      return "path_contains_degraded_domain";
    case ReasonCode::Unknown:
      return "unknown";
    case ReasonCode::InsufficientHeadroom:
      return "insufficient_headroom";
    case ReasonCode::LoadAlreadyCommitted:
      return "load_already_committed";
  }
  return "unknown";
}

Result<CapacityDerivation> derive_domain_capacity(const PowerDomain& domain,
                                                  Power rolled_up_protected_load,
                                                  Power rolled_up_committed_load,
                                                  const ResourceLimits& limits) {
  Status status = check_aggregate(rolled_up_protected_load, limits, "protected load");
  if (!status.ok()) {
    return status;
  }
  status = check_aggregate(rolled_up_committed_load, limits, "committed load");
  if (!status.ok()) {
    return status;
  }

  CapacityDerivation derivation;
  derivation.nominal = domain.nominal_capacity;
  derivation.usable = domain.usable_capacity;
  derivation.protected_load = rolled_up_protected_load;
  derivation.committed_load = rolled_up_committed_load;

  const Result<Power> derated = apply_ratio(derivation.usable, domain.derate_ratio);
  if (!derated.ok()) {
    return derated.status();
  }
  derivation.derated = derated.value();

  if (domain.state == OperationalState::Unavailable) {
    derivation.operational = Power(0);
  } else {
    const Result<Power> operational = apply_ratio(derivation.derated, domain.degradation_ratio);
    if (!operational.ok()) {
      return operational.status();
    }
    derivation.operational = operational.value();
  }

  Power reserve(0);
  switch (domain.reserve.mode) {
    case ReservePolicy::Mode::None:
      break;
    case ReservePolicy::Mode::Absolute:
      reserve = domain.reserve.absolute;
      break;
    case ReservePolicy::Mode::Ratio: {
      const Result<Power> ratio_reserve = apply_ratio(derivation.operational, domain.reserve.ratio);
      if (!ratio_reserve.ok()) {
        return ratio_reserve.status();
      }
      reserve = ratio_reserve.value();
      break;
    }
    case ReservePolicy::Mode::GreaterOfAbsoluteAndRatio: {
      const Result<Power> ratio_reserve = apply_ratio(derivation.operational, domain.reserve.ratio);
      if (!ratio_reserve.ok()) {
        return ratio_reserve.status();
      }
      reserve = max_power(domain.reserve.absolute, ratio_reserve.value());
      break;
    }
  }
  derivation.reserve = reserve;

  const ClampedDifference safe = subtract_power_clamped(derivation.operational, derivation.reserve);
  derivation.safe_capacity = safe.value;
  derivation.reserve_clamped = safe.clamped;

  const ClampedDifference carryable =
      subtract_power_clamped(derivation.safe_capacity, derivation.protected_load);
  derivation.carryable_capacity = carryable.value;
  derivation.protected_clamped = carryable.clamped;

  const ClampedDifference allocatable =
      subtract_power_clamped(derivation.carryable_capacity, derivation.committed_load);
  derivation.allocatable_headroom = allocatable.value;
  derivation.committed_clamped = allocatable.clamped;

  derivation.degraded = domain.state == OperationalState::Degraded;
  derivation.load_on_unavailable =
      domain.state == OperationalState::Unavailable &&
      (!rolled_up_committed_load.is_zero() || !rolled_up_protected_load.is_zero());

  // Documented precedence for the single primary reason code. Unavailability
  // dominates; then the clamps, because a negative remainder means load must be
  // removed; then degradation, then derating, then a zero nameplate.
  if (domain.state == OperationalState::Unavailable) {
    derivation.reason = unavailable_reason(domain.state_cause);
  } else if (derivation.committed_clamped) {
    derivation.reason = ReasonCode::CommittedLoadExceedsCapacity;
  } else if (derivation.protected_clamped) {
    derivation.reason = ReasonCode::ProtectedLoadExceedsCapacity;
  } else if (derivation.reserve_clamped) {
    derivation.reason = ReasonCode::ReserveExceedsCapacity;
  } else if (domain.state == OperationalState::Degraded) {
    derivation.reason = degraded_reason(domain.state_cause);
  } else if (derivation.derated.is_zero() && !derivation.usable.is_zero()) {
    derivation.reason = ReasonCode::DerateRemovesCapacity;
  } else if (derivation.nominal.is_zero()) {
    derivation.reason = ReasonCode::ZeroNameplateCapacity;
  } else {
    derivation.reason = ReasonCode::Ok;
  }
  return derivation;
}

Result<GroupDerivation> derive_group_capacity(const RedundancyGroup& group,
                                              const std::vector<GroupMemberView>& members,
                                              const ResourceLimits& limits) {
  if (members.size() != group.members.size()) {
    return Status::error(StatusCode::InvalidArgument,
                         "group '" + group.id.value() + "' was given " +
                             std::to_string(members.size()) + " member views for " +
                             std::to_string(group.members.size()) + " declared members");
  }

  GroupDerivation derivation;
  derivation.group = group.id;
  derivation.tolerance = group.required_simultaneous_failures;
  derivation.member_count = members.size();

  std::vector<Power> available_carryable;
  available_carryable.reserve(members.size());

  for (const GroupMemberView& member : members) {
    if (member.unavailable) {
      ++derivation.unavailable_members;
    }
    if (member.degraded) {
      ++derivation.degraded_members;
    }
    const Result<Power> carryable_total =
        add_power(derivation.member_carryable_total, member.carryable);
    if (!carryable_total.ok()) {
      return carryable_total.status();
    }
    derivation.member_carryable_total = carryable_total.value();

    const Result<Power> load_total = add_power(derivation.group_load, member.committed);
    if (!load_total.ok()) {
      return load_total.status();
    }
    derivation.group_load = load_total.value();

    if (!member.unavailable) {
      available_carryable.push_back(member.carryable);
    }
  }

  Status status = check_aggregate(derivation.member_carryable_total, limits,
                                  "redundancy group carrying ability");
  if (!status.ok()) {
    return status;
  }
  status = check_aggregate(derivation.group_load, limits, "redundancy group load");
  if (!status.ok()) {
    return status;
  }

  // The obligation is that any `tolerance` members may be lost simultaneously.
  // Members that are already unavailable have consumed part of that budget, so
  // only the remainder is charged against the surviving members' carrying
  // ability.
  std::size_t effective_tolerance = 0;
  if (derivation.unavailable_members < derivation.tolerance) {
    effective_tolerance =
        static_cast<std::size_t>(derivation.tolerance) - derivation.unavailable_members;
  }
  if (effective_tolerance > available_carryable.size()) {
    effective_tolerance = available_carryable.size();
  }
  std::sort(available_carryable.begin(), available_carryable.end(),
            [](Power left, Power right) { return left.milliwatts() > right.milliwatts(); });

  Power tolerated_loss(0);
  for (std::size_t index = 0; index < effective_tolerance; ++index) {
    const Result<Power> total = add_power(tolerated_loss, available_carryable[index]);
    if (!total.ok()) {
      return total.status();
    }
    tolerated_loss = total.value();
  }
  derivation.tolerated_loss = tolerated_loss;

  const ClampedDifference survivable =
      subtract_power_clamped(derivation.member_carryable_total, derivation.tolerated_loss);
  derivation.survivable_capacity = survivable.value;

  const ClampedDifference raw = subtract_power_clamped(derivation.survivable_capacity,
                                                       derivation.group_load);
  derivation.raw_headroom = raw.value;

  // Physical margin with the current survivors, ignoring the obligation. This is
  // reported so that an operator can see the difference between "cannot serve"
  // and "must not commit".
  const ClampedDifference physical =
      subtract_power_clamped(derivation.member_carryable_total, derivation.group_load);
  derivation.physical_headroom = physical.value;

  derivation.tolerance_exceeded = derivation.unavailable_members > derivation.tolerance;
  derivation.tolerance_consumed = derivation.unavailable_members >= derivation.tolerance;

  if (derivation.tolerance_exceeded) {
    // The obligation is already violated. The group keeps serving the load it
    // has, but it may not take on more.
    derivation.raw_headroom = Power(0);
    derivation.reason = ReasonCode::RedundancyToleranceExceeded;
  } else if (derivation.unavailable_members == derivation.tolerance && derivation.tolerance > 0) {
    // The obligation is met exactly at its limit: no failure margin remains, so
    // no further commitment may consume the surviving margin.
    derivation.raw_headroom = Power(0);
    derivation.reason = ReasonCode::RedundancyToleranceAtLimit;
  } else {
    derivation.reason = ReasonCode::Ok;
  }
  derivation.effective_headroom = derivation.raw_headroom;
  return derivation;
}

void apply_shared_upstream_limit(GroupDerivation& derivation, DomainId limiter,
                                 Power limiter_headroom) noexcept {
  derivation.shared_upstream_limiter = limiter;
  derivation.shared_upstream_headroom = limiter_headroom;
  derivation.effective_headroom = min_power(derivation.raw_headroom, limiter_headroom);
  if (derivation.reason == ReasonCode::Ok && limiter_headroom < derivation.raw_headroom) {
    derivation.reason = ReasonCode::SharedUpstreamLimits;
  }
}

}  // namespace power_capacity
