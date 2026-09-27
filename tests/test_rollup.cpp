// Rollups, shared-domain accounting without double counting, bottleneck
// selection, and candidate-load evaluation.

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

/// busway.main -> {pdu.a -> {rackpdu.a1, rackpdu.a2}, pdu.b}
Facility build_tree() {
  Facility facility;
  facility.add_domain(DomainBuilder("busway.main").nominal_watts(400000).usable_watts(400000));
  facility.add_domain(
      DomainBuilder("pdu.a").nominal_watts(200000).usable_watts(200000).parent("busway.main"));
  facility.add_domain(
      DomainBuilder("pdu.b").nominal_watts(200000).usable_watts(200000).parent("busway.main"));
  facility.add_domain(
      DomainBuilder("rackpdu.a1").nominal_watts(50000).usable_watts(50000).parent("pdu.a"));
  facility.add_domain(
      DomainBuilder("rackpdu.a2").nominal_watts(50000).usable_watts(50000).parent("pdu.a"));
  return facility;
}

}  // namespace

PC_TEST(rollup, containment_is_a_forest) {
  Facility facility = build_tree();
  const auto& state = facility.state();
  PC_CHECK_EQ(state->ancestry_of(PC_REQUIRE_OK(DomainId::parse("rackpdu.a1", 128))).size(),
              std::size_t{3});
  PC_CHECK_EQ(state->depth_of(PC_REQUIRE_OK(DomainId::parse("busway.main", 128))),
              std::uint32_t{0});
  PC_CHECK_EQ(state->depth_of(PC_REQUIRE_OK(DomainId::parse("rackpdu.a1", 128))),
              std::uint32_t{2});
  PC_CHECK(state->is_ancestor_of(PC_REQUIRE_OK(DomainId::parse("busway.main", 128)),
                                 PC_REQUIRE_OK(DomainId::parse("rackpdu.a1", 128))));
  PC_CHECK(!state->is_ancestor_of(PC_REQUIRE_OK(DomainId::parse("pdu.b", 128)),
                                  PC_REQUIRE_OK(DomainId::parse("rackpdu.a1", 128))));
  PC_CHECK_EQ(state->children_of(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)))->size(),
              std::size_t{2});
}

PC_TEST(rollup, loads_roll_up_exactly_once_per_ancestor) {
  Facility facility = build_tree();
  facility.add_load(make_load("load.a1", "rackpdu.a1", 10000));
  facility.add_load(make_load("load.a2", "rackpdu.a2", 5000));
  facility.add_load(make_load("load.b", "pdu.b", 7000, LoadClass::Protected));

  const auto& state = facility.state();
  const DomainId busway = PC_REQUIRE_OK(DomainId::parse("busway.main", 128));
  const DomainId pdu_a = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  const DomainId pdu_b = PC_REQUIRE_OK(DomainId::parse("pdu.b", 128));

  PC_CHECK_EQ(state->rolled_up_committed_of(busway).milliwatts(), 15000000);
  PC_CHECK_EQ(state->rolled_up_protected_of(busway).milliwatts(), 7000000);
  PC_CHECK_EQ(state->rolled_up_committed_of(pdu_a).milliwatts(), 15000000);
  PC_CHECK_EQ(state->rolled_up_protected_of(pdu_a).milliwatts(), 0);
  PC_CHECK_EQ(state->rolled_up_committed_of(pdu_b).milliwatts(), 0);
  PC_CHECK_EQ(state->rolled_up_protected_of(pdu_b).milliwatts(), 7000000);

  // The sum of every load in the model equals the root's rollup, which is only
  // true when no load is counted twice.
  std::int64_t total = 0;
  for (const auto& entry : state->loads()) {
    total += entry.second.load.milliwatts();
  }
  PC_CHECK_EQ(total, state->rolled_up_committed_of(busway).milliwatts() +
                         state->rolled_up_protected_of(busway).milliwatts());
}

PC_TEST(rollup, a_load_reduces_headroom_at_every_ancestor) {
  Facility facility = build_tree();
  facility.add_load(make_load("load.a1", "rackpdu.a1", 10000));
  const auto& state = facility.state();
  PC_CHECK_EQ(state->derivation_of(PC_REQUIRE_OK(DomainId::parse("rackpdu.a1", 128)))
                  ->allocatable_headroom.milliwatts(),
              40000000);
  PC_CHECK_EQ(state->derivation_of(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)))
                  ->allocatable_headroom.milliwatts(),
              190000000);
  PC_CHECK_EQ(state->derivation_of(PC_REQUIRE_OK(DomainId::parse("busway.main", 128)))
                  ->allocatable_headroom.milliwatts(),
              390000000);
  PC_CHECK_EQ(state->derivation_of(PC_REQUIRE_OK(DomainId::parse("pdu.b", 128)))
                  ->allocatable_headroom.milliwatts(),
              200000000);
}

PC_TEST(rollup, bottleneck_is_the_most_restrictive_domain_on_the_path) {
  Facility facility = build_tree();
  facility.add_load(make_load("load.a", "pdu.a", 190000));
  const DomainAssessment assessment = facility.assess("rackpdu.a1");
  PC_CHECK_EQ(assessment.effective_allocatable_headroom->milliwatts(), 10000000);
  PC_CHECK(assessment.bottleneck.has_value());
  PC_CHECK_EQ(assessment.bottleneck->value(), std::string("pdu.a"));
  PC_CHECK_EQ(assessment.bottleneck_headroom->milliwatts(), 10000000);
  PC_CHECK_EQ(assessment.path.size(), std::size_t{3});
  PC_CHECK_EQ(assessment.path.front().domain.value(), std::string("rackpdu.a1"));
  PC_CHECK_EQ(assessment.path.back().domain.value(), std::string("busway.main"));
}

PC_TEST(rollup, bottleneck_ties_resolve_toward_the_root) {
  // Every domain on the path has the same headroom, so the tie must resolve to
  // the domain closest to the root and the answer must be stable.
  Facility facility;
  facility.add_domain(DomainBuilder("a.root").nominal_watts(1000).usable_watts(1000));
  facility.add_domain(DomainBuilder("b.mid").nominal_watts(1000).usable_watts(1000).parent("a.root"));
  facility.add_domain(DomainBuilder("c.leaf").nominal_watts(1000).usable_watts(1000).parent("b.mid"));
  const DomainAssessment first = facility.assess("c.leaf");
  const DomainAssessment second = facility.assess("c.leaf");
  PC_CHECK_EQ(first.bottleneck->value(), std::string("a.root"));
  PC_CHECK_EQ(first.digest, second.digest);
}

PC_TEST(rollup, candidate_at_a_leaf_consumes_every_ancestor) {
  Facility facility = build_tree();
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "rackpdu.a1", 40000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Admissible);
  PC_CHECK_EQ(evaluation.bottleneck->value(), std::string("rackpdu.a1"));
  PC_CHECK_EQ(evaluation.remaining_at_bottleneck->milliwatts(), 10000000);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::Admissible);
}

PC_TEST(rollup, candidate_beyond_an_ancestor_is_refused_with_the_ancestor_named) {
  Facility facility = build_tree();
  facility.add_load(make_load("load.a", "pdu.a", 195000));
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "rackpdu.a1", 10000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Refused);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::InsufficientHeadroom);
  PC_CHECK_EQ(evaluation.cause, StatusCode::CapacityExceeded);
  PC_CHECK_EQ(evaluation.bottleneck->value(), std::string("pdu.a"));
  PC_CHECK_EQ(evaluation.remaining_at_bottleneck->milliwatts(), 5000000);
}

PC_TEST(rollup, admissible_candidate_names_an_upstream_bottleneck) {
  Facility facility = build_tree();
  facility.add_load(make_load("load.a", "pdu.a", 195000));
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "rackpdu.a1", 1000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Admissible);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::BottleneckUpstream);
  PC_CHECK_EQ(evaluation.bottleneck->value(), std::string("pdu.a"));
}

PC_TEST(rollup, batch_evaluation_is_cumulative) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000).usable_watts(10000));
  std::vector<CandidateLoad> candidates;
  for (int index = 0; index < 3; ++index) {
    CandidateLoad candidate;
    candidate.id = PC_REQUIRE_OK(LoadId::parse("cand." + std::to_string(index), 128));
    candidate.domain = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
    candidate.load = PC_REQUIRE_OK(Power::from_watts(4000));
    candidates.push_back(candidate);
  }
  const std::vector<CandidateEvaluation> results = facility.evaluate_batch(candidates);
  PC_CHECK_EQ(results[0].verdict, CandidateVerdict::Admissible);
  PC_CHECK_EQ(results[1].verdict, CandidateVerdict::Admissible);
  // The third does not fit once the first two are accounted for, even though it
  // would fit on its own.
  PC_CHECK_EQ(results[2].verdict, CandidateVerdict::Refused);
  PC_CHECK_EQ(results[2].remaining_at_bottleneck->milliwatts(), 2000000);

  const CandidateEvaluation alone = facility.evaluate("cand.solo", "pdu.a", 4000);
  PC_CHECK_EQ(alone.verdict, CandidateVerdict::Admissible);
}

PC_TEST(rollup, candidate_does_not_mutate_committed_state) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000).usable_watts(10000));
  const Generation before = facility.generation();
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "pdu.a", 9000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Admissible);
  PC_CHECK_EQ(facility.generation(), before);
  PC_CHECK_EQ(facility.state()->load_count(), std::size_t{0});
  PC_CHECK_EQ(facility.assess("pdu.a").derivation->allocatable_headroom.milliwatts(), 10000000);
}

PC_TEST(rollup, candidate_reusing_a_committed_identity_is_refused) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000).usable_watts(10000));
  facility.add_load(make_load("load.existing", "pdu.a", 1000));
  const CandidateEvaluation evaluation = facility.evaluate("load.existing", "pdu.a", 1000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Refused);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::LoadAlreadyCommitted);
  PC_CHECK_EQ(evaluation.cause, StatusCode::AlreadyExists);
}

PC_TEST(rollup, candidate_for_an_unknown_domain_is_refused_not_indeterminate) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000).usable_watts(10000));
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "pdu.missing", 1000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Refused);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::DomainNotFound);
  PC_CHECK_EQ(evaluation.cause, StatusCode::NotFound);
}

PC_TEST(rollup, candidate_batch_rejects_malformed_requests_before_evaluating) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000).usable_watts(10000));
  std::vector<CandidateLoad> zero_load;
  CandidateLoad candidate;
  candidate.id = PC_REQUIRE_OK(LoadId::parse("cand.1", 128));
  candidate.domain = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  candidate.load = Power(0);
  zero_load.push_back(candidate);
  PC_REQUIRE_STATUS(
      facility.engine().evaluate_batch(zero_load, EvaluationContext{Tick(0), std::nullopt}),
      StatusCode::InvalidArgument);

  std::vector<CandidateLoad> duplicated;
  candidate.load = Power(1000);
  duplicated.push_back(candidate);
  duplicated.push_back(candidate);
  PC_REQUIRE_STATUS(
      facility.engine().evaluate_batch(duplicated, EvaluationContext{Tick(0), std::nullopt}),
      StatusCode::DuplicateIdentity);
}

PC_TEST(rollup, erase_refuses_while_dependents_exist) {
  Facility facility = build_tree();
  facility.add_load(make_load("load.a1", "rackpdu.a1", 1000));
  const DomainId pdu_a = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  PC_REQUIRE_STATUS(facility.engine().erase_domain(pdu_a, facility.context()).status(),
                    StatusCode::Conflict);
  facility.remove_load("load.a1");
  PC_REQUIRE_STATUS(facility.engine().erase_domain(pdu_a, facility.context()).status(),
                    StatusCode::Conflict);
  facility.remove_domain("rackpdu.a1");
  facility.remove_domain("rackpdu.a2");
  PC_CHECK(facility.engine().erase_domain(pdu_a, facility.context()).ok());
}

PC_TEST(rollup, unknown_parent_is_refused) {
  Facility facility;
  const auto committed = facility.engine().put_domain(
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).parent("missing").build(),
      PresenceExpectation::MustNotExist, facility.context());
  PC_REQUIRE_STATUS(committed.status(), StatusCode::NotFound);
}

PC_TEST(rollup, a_load_must_name_a_known_domain) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  const auto committed = facility.engine().put_load(
      make_load("load.a", "pdu.missing", 100), PresenceExpectation::MustNotExist,
      facility.context());
  PC_REQUIRE_STATUS(committed.status(), StatusCode::NotFound);
}

PC_TEST(rollup, load_must_cite_the_authority_that_granted_it) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  LoadRecord load = make_load("load.a", "pdu.a", 100);
  load.authority = AuthorityRef{};
  const auto committed = facility.engine().put_load(load, PresenceExpectation::MustNotExist,
                                                    facility.context());
  PC_REQUIRE_STATUS(committed.status(), StatusCode::InvalidArgument);
}

PC_TEST(rollup, group_constraint_caps_the_candidate) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(200000).usable_watts(200000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b"}, 1));
  // The group tolerates one loss, so only the smaller member's worth of capacity
  // is allocatable even though the two together nameplate 300 MW and ups.a alone
  // could physically carry 200 MW.
  PC_CHECK_EQ(facility.assess("ups.a").derivation->allocatable_headroom.milliwatts(), 200000000);
  PC_CHECK_EQ(facility.assess("ups.a").effective_allocatable_headroom->milliwatts(), 100000000);
  const CandidateEvaluation fits = facility.evaluate("cand.small", "ups.a", 99000);
  PC_CHECK_EQ(fits.verdict, CandidateVerdict::Admissible);
  PC_CHECK(fits.redundancy_limited);
  PC_CHECK_EQ(fits.bottleneck->value(), std::string("ups.a"));

  const CandidateEvaluation refused = facility.evaluate("cand.big", "ups.a", 100001);
  PC_CHECK_EQ(refused.verdict, CandidateVerdict::Refused);
  PC_CHECK(refused.redundancy_limited);
  PC_CHECK_EQ(refused.reason, ReasonCode::InsufficientHeadroom);
}

PC_TEST(rollup, candidate_is_refused_when_a_group_is_at_its_tolerance_limit) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(
      DomainBuilder("ups.c").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Fault));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b", "ups.c"}, 1));
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "ups.a", 1000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Refused);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::RedundancyToleranceAtLimit);
  PC_CHECK_EQ(evaluation.cause, StatusCode::RedundancyViolated);
}

PC_TEST(rollup, candidate_is_refused_when_the_obligation_is_already_violated) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(
      DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Fault));
  facility.add_domain(
      DomainBuilder("ups.c").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Fault));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b", "ups.c"}, 1));
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "ups.a", 1000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Refused);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::RedundancyToleranceExceeded);
}

PC_TEST(rollup, protected_load_reduces_group_carrying_ability) {
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").nominal_watts(100000).usable_watts(100000));
  facility.add_group(make_group("group.ups", {"ups.a", "ups.b"}, 1));
  facility.add_load(make_load("load.prot", "ups.a", 20000, LoadClass::Protected));
  const GroupDerivation& group = *facility.state()->group_derivation_of(
      PC_REQUIRE_OK(GroupId::parse("group.ups", 128)));
  // Protected load reduces what a member can carry for the group by exactly the
  // same amount as an equivalent committed load.
  PC_CHECK_EQ(group.member_carryable_total.milliwatts(), 180000000);
  PC_CHECK_EQ(group.survivable_capacity.milliwatts(), 80000000);
  PC_CHECK_EQ(group.group_load.milliwatts(), 0);
  PC_CHECK_EQ(group.effective_headroom.milliwatts(), 80000000);
}
