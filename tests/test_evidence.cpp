// Evidence freshness, the unknown-versus-zero distinction, authority and
// generation fencing, and revalidation.

#include <cstdint>
#include <string>

#include "power_capacity/assessment.hpp"
#include "power_capacity/capacity.hpp"
#include "support/facility.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_load;

PC_TEST(evidence, freshness_window_is_half_open) {
  EvidenceRef reference;
  reference.id = PC_REQUIRE_OK(EvidenceId::parse("ev", 128));
  reference.source = PC_REQUIRE_OK(SourceId::parse("src", 128));
  reference.observed_at = Tick(100);
  reference.valid_until = Tick(200);
  reference.revision = Revision(1);
  reference.source_generation = Generation(1);

  PC_CHECK_EQ(evaluate_evidence(reference, Tick(99)), EvidenceState::NotYetValid);
  PC_CHECK_EQ(evaluate_evidence(reference, Tick(100)), EvidenceState::Current);
  PC_CHECK_EQ(evaluate_evidence(reference, Tick(199)), EvidenceState::Current);
  PC_CHECK_EQ(evaluate_evidence(reference, Tick(200)), EvidenceState::Stale);
  PC_CHECK_EQ(evaluate_evidence(std::nullopt, Tick(100)), EvidenceState::Missing);
  PC_CHECK(evidence_is_current(reference, Tick(150)));
  PC_CHECK(!evidence_is_current(reference, Tick(250)));
}

PC_TEST(evidence, a_window_that_does_not_advance_is_refused) {
  PowerDomain domain = DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build();
  EvidenceRef reference = *domain.evidence;
  reference.observed_at = Tick(100);
  reference.valid_until = Tick(100);
  domain.evidence = reference;
  PC_REQUIRE_STATUS(validate_domain(domain, ResourceLimits::defaults()),
                    StatusCode::InvalidArgument);
}

PC_TEST(evidence, stale_evidence_is_unknown_and_never_zero) {
  Facility facility;
  facility.add_domain(
      DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000).evidence(Tick(10), Tick(20)));
  const DomainAssessment fresh = facility.assess("pdu.a", Tick(15));
  PC_CHECK(fresh.known);
  PC_CHECK_EQ(fresh.derivation->allocatable_headroom.milliwatts(), 100000000);

  const DomainAssessment stale = facility.assess("pdu.a", Tick(25));
  PC_CHECK(!stale.known);
  PC_CHECK_EQ(stale.reason, ReasonCode::EvidenceStale);
  PC_CHECK(!stale.derivation.has_value());
  PC_CHECK(!stale.effective_allocatable_headroom.has_value());

  const DomainAssessment future = facility.assess("pdu.a", Tick(5));
  PC_CHECK(!future.known);
  PC_CHECK_EQ(future.reason, ReasonCode::EvidenceFromFuture);
}

PC_TEST(evidence, stale_evidence_on_an_ancestor_makes_the_answer_unknown) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000)
                          .evidence(Tick(0), Tick(1000)));
  facility.add_domain(DomainBuilder("rackpdu.a").nominal_watts(10000).usable_watts(10000)
                          .parent("pdu.a")
                          .evidence(Tick(0), Tick(20)));
  const DomainAssessment assessment = facility.assess("rackpdu.a", Tick(30));
  PC_CHECK(!assessment.known);
  PC_CHECK_EQ(assessment.reason, ReasonCode::EvidenceStale);

  const DomainAssessment current = facility.assess("rackpdu.a", Tick(10));
  PC_CHECK(current.known);
}

PC_TEST(evidence, missing_evidence_is_distinct_from_stale_evidence) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).no_evidence());
  PC_CHECK_EQ(facility.assess("pdu.a").reason, ReasonCode::EvidenceMissing);
}

PC_TEST(evidence, candidate_is_indeterminate_when_evidence_is_not_current) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000)
                          .evidence(Tick(0), Tick(10)));
  facility.set_as_of(Tick(50));
  const CandidateEvaluation evaluation = facility.evaluate("cand.1", "pdu.a", 1000);
  PC_CHECK_EQ(evaluation.verdict, CandidateVerdict::Indeterminate);
  PC_CHECK_EQ(evaluation.reason, ReasonCode::EvidenceStale);
  PC_CHECK_EQ(evaluation.cause, StatusCode::EvidenceStale);

  Facility missing;
  missing.add_domain(DomainBuilder("pdu.b").nominal_watts(100000).usable_watts(100000).no_evidence());
  const CandidateEvaluation unknown = missing.evaluate("cand.2", "pdu.b", 1000);
  PC_CHECK_EQ(unknown.verdict, CandidateVerdict::Indeterminate);
  PC_CHECK_EQ(unknown.cause, StatusCode::EvidenceMissing);
}

PC_TEST(evidence, expected_generation_fences_a_stale_query) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  CapacityQuery query;
  query.domain = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  query.as_of = facility.as_of();
  query.expected_generation = Generation(facility.generation().value() - 1);
  PC_REQUIRE_STATUS(facility.engine().assess(query), StatusCode::StaleGeneration);
  query.expected_generation = facility.generation();
  PC_CHECK(facility.engine().assess(query).ok());
}

PC_TEST(evidence, expected_authority_fences_a_stale_query) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  CapacityQuery query;
  query.domain = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  query.as_of = facility.as_of();
  query.expected_epoch = Epoch(9);
  PC_REQUIRE_STATUS(facility.engine().assess(query), StatusCode::StaleAuthority);
  query.expected_epoch = facility.epoch();
  query.expected_incarnation = Incarnation(9);
  PC_REQUIRE_STATUS(facility.engine().assess(query), StatusCode::StaleAuthority);
}

PC_TEST(evidence, mutation_requires_an_explicit_generation_precondition) {
  Facility facility;
  MutationContext context = facility.context();
  context.expected_generation.reset();
  const auto committed = facility.engine().put_domain(
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist, context);
  PC_REQUIRE_STATUS(committed.status(), StatusCode::InvalidArgument);
}

PC_TEST(evidence, mutation_with_a_stale_generation_is_refused) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  MutationContext context = facility.context(Generation(1));
  const auto committed = facility.engine().put_domain(
      DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist, context);
  PC_REQUIRE_STATUS(committed.status(), StatusCode::StaleGeneration);
}

PC_TEST(evidence, mutation_with_a_stale_authority_binding_is_refused) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  MutationContext context = facility.context();
  context.epoch = Epoch(4);
  const auto committed = facility.engine().put_domain(
      DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist, context);
  PC_REQUIRE_STATUS(committed.status(), StatusCode::StaleAuthority);
}

PC_TEST(evidence, source_generations_fence_a_stale_mutation) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  facility.advance_source("topology", 1);
  facility.advance_source("topology", 2);

  MutationContext stale = facility.context();
  stale.expected_source_generations[PC_REQUIRE_OK(SourceId::parse("topology", 128))] = Generation(1);
  const auto refused = facility.engine().put_domain(
      DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist, stale);
  PC_REQUIRE_STATUS(refused.status(), StatusCode::StaleSourceGeneration);

  MutationContext current = facility.context();
  current.expected_source_generations[PC_REQUIRE_OK(SourceId::parse("topology", 128))] = Generation(2);
  PC_CHECK(facility.engine()
               .put_domain(DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
                           PresenceExpectation::MustNotExist, current)
               .ok());
}

PC_TEST(evidence, a_source_may_not_be_advanced_without_being_read) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  MutationContext context = facility.context();
  context.advanced_source_generations[PC_REQUIRE_OK(SourceId::parse("topology", 128))] = Generation(1);
  const auto committed = facility.engine().put_domain(
      DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist, context);
  PC_REQUIRE_STATUS(committed.status(), StatusCode::InvalidArgument);
}

PC_TEST(evidence, a_source_generation_may_only_move_forward) {
  Facility facility;
  facility.advance_source("topology", 5);
  MutationContext context = facility.context();
  const SourceId source = PC_REQUIRE_OK(SourceId::parse("topology", 128));
  context.expected_source_generations[source] = Generation(5);
  PC_REQUIRE_STATUS(facility.engine().advance_source_generation(source, Generation(5), context),
                    StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(facility.engine().advance_source_generation(source, Generation(4), context),
                    StatusCode::InvalidArgument);
}

PC_TEST(evidence, idempotency_key_replays_without_applying_twice) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  MutationContext context = facility.context();
  context.idempotency_key = PC_REQUIRE_OK(IdempotencyKey::parse("op-1", 64));
  const auto first = facility.engine().put_domain(
      DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist, context);
  PC_CHECK(first.ok());
  PC_CHECK(!first.value().already_applied);

  // Replaying the same key at the same generation returns the recorded result
  // without touching the model.
  const Generation after_first = facility.generation();
  const auto replay = facility.engine().put_domain(
      DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist, context);
  PC_CHECK(replay.ok());
  PC_CHECK(replay.value().already_applied);
  PC_CHECK_EQ(replay.value().generation.value(), after_first.value());
  PC_CHECK_EQ(facility.engine().state().value()->domain_count(), std::size_t{2});
}

PC_TEST(evidence, idempotency_history_is_bounded) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.holder").nominal_watts(1000).usable_watts(1000));
  const std::size_t limit = ResourceLimits::defaults().max_applied_operations;
  for (std::size_t index = 0; index < limit + 20; ++index) {
    MutationContext context = facility.context();
    context.idempotency_key =
        PC_REQUIRE_OK(IdempotencyKey::parse("key-" + std::to_string(index), 64));
    const auto committed = facility.engine().put_domain(
        DomainBuilder("pdu." + std::to_string(index)).nominal_watts(1000).usable_watts(1000).build(),
        PresenceExpectation::MustNotExist, context);
    PC_CHECK(committed.ok());
  }
  PC_CHECK_EQ(facility.state()->applied_operations().size(), limit);
  // The oldest keys are gone, so the model never accumulates unbounded history.
  PC_CHECK_EQ(facility.state()->applied_operations().front().key.value(),
              std::string("key-20"));
}

PC_TEST(evidence, assessment_token_still_valid_after_an_unrelated_mutation) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000));
  const DomainAssessment assessment = facility.assess("pdu.a");
  const RevalidationOutcome outcome = facility.revalidate(assessment.token());
  PC_CHECK_EQ(outcome.verdict, RevalidationVerdict::StillValid);
  PC_CHECK_EQ(outcome.reason, ReasonCode::Ok);
}

PC_TEST(evidence, assessment_token_is_superseded_when_the_generation_advances) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000));
  const DomainAssessment assessment = facility.assess("pdu.a");
  facility.add_load(make_load("load.a", "pdu.a", 1000));
  const RevalidationOutcome outcome = facility.revalidate(assessment.token());
  PC_CHECK_EQ(outcome.verdict, RevalidationVerdict::Superseded);
  PC_CHECK_EQ(outcome.reason, ReasonCode::GenerationAdvanced);
  PC_CHECK_EQ(outcome.cause, StatusCode::StaleGeneration);
  // A hard fence reports the fence and does not recompute; the caller asks again.
  PC_CHECK(!outcome.current.has_value());
  const DomainAssessment refreshed = facility.assess("pdu.a");
  PC_CHECK_EQ(refreshed.derivation->committed_load.milliwatts(), 1000000);
  PC_CHECK_EQ(facility.revalidate(refreshed.token()).verdict, RevalidationVerdict::StillValid);
}

PC_TEST(evidence, assessment_token_is_superseded_when_the_authority_binding_moves) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000));
  const DomainAssessment assessment = facility.assess("pdu.a");

  RevalidationRequest request;
  request.as_of = facility.as_of();
  request.epoch = Epoch(1);
  request.incarnation = Incarnation(1);
  PC_CHECK(facility.engine().revalidate_recovered_state(request).ok());

  const RevalidationOutcome outcome = facility.revalidate(assessment.token());
  PC_CHECK_EQ(outcome.verdict, RevalidationVerdict::Superseded);
  PC_CHECK_EQ(outcome.reason, ReasonCode::AuthorityEpochChanged);
}

PC_TEST(evidence, revalidation_is_indeterminate_when_evidence_expired) {
  Facility facility;
  facility.add_domain(
      DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000).evidence(Tick(0), Tick(100)));
  const DomainAssessment assessment = facility.assess("pdu.a", Tick(50));
  PC_CHECK(assessment.known);

  RevalidationRequest request;
  request.as_of = Tick(500);
  request.epoch = facility.epoch();
  request.incarnation = facility.incarnation();
  const auto outcome = facility.engine().revalidate(assessment.token(), request);
  PC_CHECK(outcome.ok());
  PC_CHECK_EQ(outcome.value().verdict, RevalidationVerdict::Indeterminate);
  PC_CHECK_EQ(outcome.value().reason, ReasonCode::EvidenceStale);
}

PC_TEST(evidence, token_from_a_previous_session_is_superseded) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000));
  const DomainAssessment assessment = facility.assess("pdu.a");

  // A second engine over the same in-memory model cannot exist, so the session
  // boundary is produced by changing the token's session identity directly. The
  // engine must refuse it rather than silently accepting a foreign token.
  AssessmentToken foreign = assessment.token();
  foreign.session = SessionId::generate();
  RevalidationRequest request;
  request.as_of = facility.as_of();
  request.epoch = facility.epoch();
  request.incarnation = facility.incarnation();
  const auto outcome = facility.engine().revalidate(foreign, request);
  PC_CHECK(outcome.ok());
  PC_CHECK_EQ(outcome.value().verdict, RevalidationVerdict::Superseded);
  PC_CHECK_EQ(outcome.value().reason, ReasonCode::StoreReopened);
}
