// Redundancy arithmetic, including an independent brute-force reference model
// over every tolerated failure set.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "power_capacity/capacity.hpp"
#include "support/facility.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_group;
using pc_test::make_load;

namespace {

/// Independent reference model: the worst tolerated failure set is enumerated
/// exhaustively instead of using the closed form.
Power brute_force_survivable(const std::vector<Power>& carryable, std::uint32_t tolerance) {
  const std::size_t count = carryable.size();
  Power worst(std::numeric_limits<std::int64_t>::max());
  const std::uint64_t combinations = std::uint64_t{1} << count;
  for (std::uint64_t mask = 0; mask < combinations; ++mask) {
    std::size_t failed = 0;
    for (std::size_t index = 0; index < count; ++index) {
      if ((mask & (std::uint64_t{1} << index)) != 0) {
        ++failed;
      }
    }
    if (failed != tolerance) {
      continue;
    }
    std::int64_t surviving = 0;
    for (std::size_t index = 0; index < count; ++index) {
      if ((mask & (std::uint64_t{1} << index)) == 0) {
        surviving += carryable[index].milliwatts();
      }
    }
    worst = std::min(worst, Power(surviving));
  }
  return worst;
}

std::vector<GroupMemberView> members_from(const std::vector<Power>& carryable,
                                          const std::vector<Power>& committed) {
  std::vector<GroupMemberView> members;
  for (std::size_t index = 0; index < carryable.size(); ++index) {
    GroupMemberView view;
    view.domain = PC_REQUIRE_OK(DomainId::parse("member." + std::to_string(index), 128));
    view.carryable = carryable[index];
    view.committed = committed[index];
    members.push_back(view);
  }
  return members;
}

}  // namespace

PC_TEST(redundancy, closed_form_matches_brute_force) {
  // Exhaustive comparison over every tolerance for member counts up to six.
  for (std::size_t count = 2; count <= 6; ++count) {
    std::vector<Power> carryable;
    for (std::size_t index = 0; index < count; ++index) {
      carryable.push_back(Power(static_cast<std::int64_t>((index + 1) * 7000 + index * index)));
    }
    std::vector<Power> committed(count, Power(0));
    for (std::uint32_t tolerance = 0; tolerance < count; ++tolerance) {
      RedundancyGroup group;
      group.id = PC_REQUIRE_OK(GroupId::parse("group.g", 128));
      group.policy_authority = PC_REQUIRE_OK(AuthorityRef::parse("authority.p", 128));
      for (std::size_t index = 0; index < count; ++index) {
        group.members.push_back(
            PC_REQUIRE_OK(DomainId::parse("member." + std::to_string(index), 128)));
      }
      group.required_simultaneous_failures = tolerance;
      const Result<GroupDerivation> derived =
          derive_group_capacity(group, members_from(carryable, committed),
                                ResourceLimits::defaults());
      PC_CHECK(derived.ok());
      if (!derived.ok()) {
        continue;
      }
      PC_CHECK_EQ(derived.value().survivable_capacity.milliwatts(),
                  brute_force_survivable(carryable, tolerance).milliwatts());
    }
  }
}

PC_TEST(redundancy, n_plus_one_subtracts_the_largest_member) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(60000).usable_watts(60000));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b"}, 1, RedundancyClass::NPlusOne));

  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.ups", 128)));
  PC_CHECK_EQ(group.member_carryable_total.milliwatts(), 160000000);
  PC_CHECK_EQ(group.tolerated_loss.milliwatts(), 100000000);
  PC_CHECK_EQ(group.survivable_capacity.milliwatts(), 60000000);
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 60000000);
  PC_CHECK_EQ(group.reason, ReasonCode::Ok);
}

PC_TEST(redundancy, two_n_with_two_members_is_the_smaller_member) {
  Facility facility;
  facility.add_domain(DomainBuilder("feed.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("feed.b").nominal_watts(120000).usable_watts(120000));
  facility.add_group(make_group("group.feed", {"feed.a", "feed.b"}, 1, RedundancyClass::TwoN));
  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.feed", 128)));
  PC_CHECK_EQ(group.survivable_capacity.milliwatts(), 100000000);
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 100000000);
}

PC_TEST(redundancy, zero_tolerance_is_a_plain_sum) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000).usable_watts(10000));
  facility.add_domain(DomainBuilder("pdu.b").nominal_watts(20000).usable_watts(20000));
  facility.add_group(make_group("group.pdu", {"pdu.a", "pdu.b"}, 0, RedundancyClass::N));
  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.pdu", 128)));
  PC_CHECK_EQ(group.survivable_capacity.milliwatts(), 30000000);
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 30000000);
  PC_CHECK_EQ(group.reason, ReasonCode::Ok);
}

PC_TEST(redundancy, declared_class_is_a_label_and_not_an_obligation) {
  // The label says N+1 but the obligation states zero tolerated failures. The
  // obligation wins, because a label is metadata and never authority.
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000).usable_watts(10000));
  facility.add_domain(DomainBuilder("pdu.b").nominal_watts(20000).usable_watts(20000));
  facility.add_group(make_group("group.pdu", {"pdu.a", "pdu.b"}, 0, RedundancyClass::NPlusOne));
  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.pdu", 128)));
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 30000000);
}

PC_TEST(redundancy, an_unavailable_member_consumes_the_failure_budget) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(
      DomainBuilder("ups.c").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Fault));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b", "ups.c"}, 1, RedundancyClass::NPlusOne));

  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.ups", 128)));
  PC_CHECK_EQ(group.unavailable_members, std::size_t{1});
  PC_CHECK(group.tolerance_consumed);
  PC_CHECK(!group.tolerance_exceeded);
  // The obligation is met exactly at its limit, so nothing more may be committed
  // even though the survivors could physically carry another 100 MW.
  PC_CHECK_EQ(group.tolerated_loss.milliwatts(), 0);
  PC_CHECK_EQ(group.survivable_capacity.milliwatts(), 200000000);
  PC_CHECK_EQ(group.raw_headroom.milliwatts(), 0);
  PC_CHECK_EQ(group.physical_headroom.milliwatts(), 200000000);
  PC_CHECK_EQ(group.reason, ReasonCode::RedundancyToleranceAtLimit);
}

PC_TEST(redundancy, losing_more_members_than_tolerated_violates_the_obligation) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(
      DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Fault));
  facility.add_domain(
      DomainBuilder("ups.c").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Fault));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b", "ups.c"}, 1, RedundancyClass::NPlusOne));
  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.ups", 128)));
  PC_CHECK(group.tolerance_exceeded);
  PC_CHECK_EQ(group.raw_headroom.milliwatts(), 0);
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 0);
  PC_CHECK_EQ(group.reason, ReasonCode::RedundancyToleranceExceeded);
}

PC_TEST(redundancy, shared_upstream_bounds_the_group) {
  // Both members hang off one busway that is far smaller than what the group
  // arithmetic alone would allow. The shared upstream must bind, and it must be
  // counted once rather than once per member.
  Facility facility;
  facility.add_domain(DomainBuilder("busway.main").nominal_watts(50000).usable_watts(50000));
  facility.add_domain(
      DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000).parent("busway.main"));
  facility.add_domain(
      DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000).parent("busway.main"));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b"}, 1, RedundancyClass::NPlusOne));

  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.ups", 128)));
  PC_CHECK_EQ(group.raw_headroom.milliwatts(), 100000000);
  PC_CHECK_EQ(group.shared_upstream_headroom->milliwatts(), 50000000);
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 50000000);
  PC_CHECK_EQ(group.reason, ReasonCode::SharedUpstreamLimits);
  PC_CHECK(group.shared_upstream_limiter.has_value());
  PC_CHECK_EQ(group.shared_upstream_limiter->value(), std::string("busway.main"));
}

PC_TEST(redundancy, shared_upstream_without_an_ancestor_does_not_limit) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b"}, 1, RedundancyClass::NPlusOne));
  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.ups", 128)));
  PC_CHECK(!group.shared_upstream_limiter.has_value());
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 100000000);
}

PC_TEST(redundancy, group_members_must_be_pairwise_independent) {
  Facility facility;
  facility.add_domain(DomainBuilder("busway.main").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(
      DomainBuilder("ups.a").nominal_watts(50000).usable_watts(50000).parent("busway.main"));
  const auto committed = facility.engine().put_group(
      make_group("group.bad", {"busway.main", "ups.a"}, 1), PresenceExpectation::MustNotExist,
      facility.context());
  PC_REQUIRE_STATUS(committed.status(), StatusCode::InvariantViolation);
}

PC_TEST(redundancy, a_domain_belongs_to_at_most_one_group) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.c").nominal_watts(100000).usable_watts(100000));
  facility.add_group(make_group("group.one", {"ups.a", "ups.b"}, 1));
  const auto second = facility.engine().put_group(make_group("group.two", {"ups.a", "ups.c"}, 1),
                                                 PresenceExpectation::MustNotExist,
                                                 facility.context());
  PC_REQUIRE_STATUS(second.status(), StatusCode::Conflict);
}

PC_TEST(redundancy, group_requires_an_explicit_policy_authority) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  RedundancyGroup group = make_group("group.ups", {"ups.a", "ups.b"}, 1);
  group.policy_authority = AuthorityRef{};
  const auto committed = facility.engine().put_group(group, PresenceExpectation::MustNotExist,
                                                     facility.context());
  PC_REQUIRE_STATUS(committed.status(), StatusCode::InvalidArgument);
}

PC_TEST(redundancy, group_shape_is_validated) {
  RedundancyGroup group = make_group("group.ups", {"ups.a", "ups.b"}, 1);
  PC_CHECK(validate_group(group, ResourceLimits::defaults()).ok());

  RedundancyGroup unsorted = group;
  unsorted.members = {PC_REQUIRE_OK(DomainId::parse("ups.b", 128)),
                      PC_REQUIRE_OK(DomainId::parse("ups.a", 128))};
  PC_REQUIRE_STATUS(validate_group(unsorted, ResourceLimits::defaults()),
                    StatusCode::InvalidArgument);

  RedundancyGroup too_few = group;
  too_few.members.resize(1);
  PC_REQUIRE_STATUS(validate_group(too_few, ResourceLimits::defaults()),
                    StatusCode::InvalidArgument);

  RedundancyGroup every_member = group;
  every_member.required_simultaneous_failures = 2;
  PC_REQUIRE_STATUS(validate_group(every_member, ResourceLimits::defaults()),
                    StatusCode::InvalidArgument);

  RedundancyGroup excessive = group;
  excessive.required_simultaneous_failures = 1000;
  PC_REQUIRE_STATUS(validate_group(excessive, ResourceLimits::defaults()),
                    StatusCode::LimitExceeded);

  std::vector<DomainId> duplicates = group.members;
  duplicates.push_back(group.members.front());
  PC_REQUIRE_STATUS(canonicalize_group_members(duplicates), StatusCode::DuplicateIdentity);

  std::vector<DomainId> canonical = {PC_REQUIRE_OK(DomainId::parse("ups.b", 128)),
                                     PC_REQUIRE_OK(DomainId::parse("ups.a", 128))};
  PC_CHECK(canonicalize_group_members(canonical).ok());
  PC_CHECK_EQ(canonical.front().value(), std::string("ups.a"));
}

PC_TEST(redundancy, group_headroom_falls_by_the_committed_load) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b"}, 1));
  facility.add_load(make_load("load.a", "ups.a", 30000));

  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.ups", 128)));
  PC_CHECK_EQ(group.group_load.milliwatts(), 30000000);
  PC_CHECK_EQ(group.survivable_capacity.milliwatts(), 100000000);
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 70000000);
}
