// Domain declaration invariants, the documented derivation order, and the
// documented reason precedence.

#include <cstdint>
#include <string>

#include "power_capacity/capacity.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/power_domain.hpp"
#include "support/facility.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_load;

namespace {

const ResourceLimits kLimits = ResourceLimits::defaults();

}  // namespace

PC_TEST(model, usable_capacity_may_not_exceed_nameplate) {
  const PowerDomain domain =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1200).build();
  PC_REQUIRE_STATUS(validate_domain(domain, kLimits), StatusCode::InvalidArgument);
}

PC_TEST(model, negative_capacities_are_refused) {
  PowerDomain domain = DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build();
  domain.nominal_capacity = Power(-1);
  PC_REQUIRE_STATUS(validate_domain(domain, kLimits), StatusCode::InvalidArgument);
  domain = DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build();
  domain.usable_capacity = Power(-1);
  PC_REQUIRE_STATUS(validate_domain(domain, kLimits), StatusCode::InvalidArgument);
}

PC_TEST(model, self_parent_is_refused) {
  const PowerDomain domain =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).parent("pdu.a").build();
  PC_REQUIRE_STATUS(validate_domain(domain, kLimits), StatusCode::InvalidArgument);
}

PC_TEST(model, state_and_cause_must_agree) {
  PowerDomain available = DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build();
  available.state_cause = StateCause::Fault;
  PC_REQUIRE_STATUS(validate_domain(available, kLimits), StatusCode::InvalidArgument);

  PowerDomain degraded_without_ratio =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build();
  degraded_without_ratio.state = OperationalState::Degraded;
  degraded_without_ratio.state_cause = StateCause::Fault;
  PC_REQUIRE_STATUS(validate_domain(degraded_without_ratio, kLimits), StatusCode::InvalidArgument);

  PowerDomain unavailable_without_cause =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).unavailable(StateCause::None)
          .build();
  PC_REQUIRE_STATUS(validate_domain(unavailable_without_cause, kLimits),
                    StatusCode::InvalidArgument);

  PowerDomain unavailable_with_ratio =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000)
          .unavailable(StateCause::Fault)
          .build();
  unavailable_with_ratio.degradation_ratio = PC_REQUIRE_OK(Ratio::from_basis_points(5000));
  PC_REQUIRE_STATUS(validate_domain(unavailable_with_ratio, kLimits), StatusCode::InvalidArgument);
}

PC_TEST(model, reserve_policy_must_be_stated_once) {
  PowerDomain none_with_value = DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build();
  none_with_value.reserve.absolute = Power(1);
  PC_REQUIRE_STATUS(validate_domain(none_with_value, kLimits), StatusCode::InvalidArgument);

  PowerDomain absolute_with_ratio =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).reserve_watts(10).build();
  absolute_with_ratio.reserve.ratio = PC_REQUIRE_OK(Ratio::from_basis_points(100));
  PC_REQUIRE_STATUS(validate_domain(absolute_with_ratio, kLimits), StatusCode::InvalidArgument);

  PowerDomain ratio_with_absolute =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).reserve_bp(1000).build();
  ratio_with_absolute.reserve.absolute = Power(1);
  PC_REQUIRE_STATUS(validate_domain(ratio_with_absolute, kLimits), StatusCode::InvalidArgument);

  PowerDomain zero_ratio =
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).build();
  zero_ratio.reserve.mode = ReservePolicy::Mode::Ratio;
  PC_REQUIRE_STATUS(validate_domain(zero_ratio, kLimits), StatusCode::InvalidArgument);

  const PowerDomain good = DomainBuilder("pdu.a")
                               .nominal_watts(1000)
                               .usable_watts(1000)
                               .reserve_greater_of(100, 2000)
                               .build();
  PC_CHECK(validate_domain(good, kLimits).ok());
}

PC_TEST(model, component_capacity_limit_is_enforced) {
  PowerDomain domain = DomainBuilder("pdu.a")
                           .nominal_watts(kLimits.max_component_milliwatts / 1000)
                           .usable_watts(kLimits.max_component_milliwatts / 1000)
                           .build();
  PC_CHECK(validate_domain(domain, kLimits).ok());
  domain.nominal_capacity = Power(kLimits.max_component_milliwatts + 1);
  domain.usable_capacity = Power(kLimits.max_component_milliwatts + 1);
  PC_REQUIRE_STATUS(validate_domain(domain, kLimits), StatusCode::LimitExceeded);
}

PC_TEST(model, derivation_follows_the_documented_order) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a")
                          .nominal_watts(100000)
                          .usable_watts(90000)
                          .derate_bp(8000)
                          .reserve_bp(2500));
  facility.add_load(make_load("load.committed", "pdu.a", 10000));
  facility.add_load(make_load("load.protected", "pdu.a", 5000, LoadClass::Protected));

  const DomainAssessment assessment = facility.assess("pdu.a");
  PC_CHECK(assessment.known);
  PC_CHECK_EQ(assessment.reason, ReasonCode::Ok);
  const CapacityDerivation& derivation = *assessment.derivation;
  PC_CHECK_EQ(derivation.nominal.milliwatts(), 100000000);
  PC_CHECK_EQ(derivation.usable.milliwatts(), 90000000);
  PC_CHECK_EQ(derivation.derated.milliwatts(), 72000000);
  PC_CHECK_EQ(derivation.operational.milliwatts(), 72000000);
  PC_CHECK_EQ(derivation.reserve.milliwatts(), 18000000);
  PC_CHECK_EQ(derivation.safe_capacity.milliwatts(), 54000000);
  PC_CHECK_EQ(derivation.protected_load.milliwatts(), 5000000);
  PC_CHECK_EQ(derivation.carryable_capacity.milliwatts(), 49000000);
  PC_CHECK_EQ(derivation.committed_load.milliwatts(), 10000000);
  PC_CHECK_EQ(derivation.allocatable_headroom.milliwatts(), 39000000);
  PC_CHECK(!derivation.reserve_clamped);
  PC_CHECK(!derivation.protected_clamped);
  PC_CHECK(!derivation.committed_clamped);
}

PC_TEST(model, unavailable_is_a_known_zero_and_not_unknown) {
  Facility facility;
  facility.add_domain(
      DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Maintenance));
  const DomainAssessment assessment = facility.assess("pdu.a");
  PC_CHECK(assessment.known);
  PC_CHECK_EQ(assessment.reason, ReasonCode::DomainUnavailableMaintenance);
  PC_CHECK_EQ(assessment.derivation->operational.milliwatts(), 0);
  PC_CHECK_EQ(assessment.derivation->allocatable_headroom.milliwatts(), 0);
  // The nameplate value is still reported: an unavailable domain is not a domain
  // with no capacity, it is a domain whose capacity cannot be used.
  PC_CHECK_EQ(assessment.derivation->nominal.milliwatts(), 100000000);
}

PC_TEST(model, unavailable_domain_reports_stranded_load) {
  Facility facility;
  facility.add_domain(
      DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000).unavailable(StateCause::Fault));
  facility.add_load(make_load("load.a", "pdu.a", 40000));
  const DomainAssessment assessment = facility.assess("pdu.a");
  PC_CHECK_EQ(assessment.reason, ReasonCode::DomainUnavailableFault);
  PC_CHECK(assessment.derivation->load_on_unavailable);
  PC_CHECK_EQ(assessment.derivation->committed_load.milliwatts(), 40000000);
  PC_CHECK_EQ(assessment.derivation->allocatable_headroom.milliwatts(), 0);
}

PC_TEST(model, degraded_domain_applies_its_declared_reduction) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a")
                          .nominal_watts(100000)
                          .usable_watts(100000)
                          .degraded(StateCause::Fault, 6000));
  const DomainAssessment assessment = facility.assess("pdu.a");
  PC_CHECK_EQ(assessment.reason, ReasonCode::DomainDegradedFault);
  PC_CHECK_EQ(assessment.derivation->derated.milliwatts(), 100000000);
  PC_CHECK_EQ(assessment.derivation->operational.milliwatts(), 60000000);
  PC_CHECK(assessment.derivation->degraded);
  PC_CHECK(assessment.path_degraded);
}

PC_TEST(model, reserve_greater_of_uses_the_larger_value) {
  // Nominal 100000 W. Basis points are hundredths of a percent, so 500 bp is 5%.
  Facility absolute_wins;
  absolute_wins.add_domain(DomainBuilder("pdu.a")
                               .nominal_watts(100000)
                               .usable_watts(100000)
                               .reserve_greater_of(10000, 500));
  const DomainAssessment first = absolute_wins.assess("pdu.a");
  // 5% of 100000 W is 5000 W; the absolute reserve of 10000 W is larger.
  PC_CHECK_EQ(first.derivation->reserve.milliwatts(), 10000000);

  Facility ratio_wins;
  ratio_wins.add_domain(DomainBuilder("pdu.b")
                            .nominal_watts(100000)
                            .usable_watts(100000)
                            .reserve_greater_of(1000, 8000));
  const DomainAssessment second = ratio_wins.assess("pdu.b");
  // 80% of 100000 W is 80000 W; the absolute reserve of 1000 W is smaller.
  PC_CHECK_EQ(second.derivation->reserve.milliwatts(), 80000000);
}

PC_TEST(model, reason_precedence_is_documented_and_stable) {
  // 1. Unavailability dominates every other condition.
  {
    Facility facility;
    facility.add_domain(DomainBuilder("pdu.a")
                            .nominal_watts(1000)
                            .usable_watts(1000)
                            .unavailable(StateCause::Decommissioned));
    facility.add_load(make_load("load.a", "pdu.a", 5000));
    PC_CHECK_EQ(facility.assess("pdu.a").reason, ReasonCode::DomainUnavailableDecommissioned);
  }
  // 2. Then an overcommit, whose clamped zero must not be reported as zero.
  {
    Facility facility;
    facility.add_domain(DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000));
    facility.add_load(make_load("load.b", "pdu.b", 5000));
    const DomainAssessment assessment = facility.assess("pdu.b");
    PC_CHECK_EQ(assessment.reason, ReasonCode::CommittedLoadExceedsCapacity);
    PC_CHECK(assessment.derivation->committed_clamped);
    PC_CHECK_EQ(assessment.derivation->allocatable_headroom.milliwatts(), 0);
  }
  // 3. Then protected load exceeding what reserve left.
  {
    Facility facility;
    facility.add_domain(DomainBuilder("pdu.c").nominal_watts(1000).usable_watts(1000));
    facility.add_load(make_load("load.c", "pdu.c", 5000, LoadClass::Protected));
    const DomainAssessment assessment = facility.assess("pdu.c");
    PC_CHECK_EQ(assessment.reason, ReasonCode::ProtectedLoadExceedsCapacity);
    PC_CHECK(assessment.derivation->protected_clamped);
  }
  // 4. Then a reserve larger than the operational capacity.
  {
    Facility facility;
    facility.add_domain(
        DomainBuilder("pdu.d").nominal_watts(1000).usable_watts(1000).reserve_watts(5000));
    const DomainAssessment assessment = facility.assess("pdu.d");
    PC_CHECK_EQ(assessment.reason, ReasonCode::ReserveExceedsCapacity);
    PC_CHECK(assessment.derivation->reserve_clamped);
  }
  // 5. Then degradation, which still leaves usable capacity.
  {
    Facility facility;
    facility.add_domain(DomainBuilder("pdu.e")
                            .nominal_watts(1000)
                            .usable_watts(1000)
                            .degraded(StateCause::Maintenance, 5000));
    PC_CHECK_EQ(facility.assess("pdu.e").reason, ReasonCode::DomainDegradedMaintenance);
  }
  // 6. Then a derate that removes everything.
  {
    Facility facility;
    facility.add_domain(DomainBuilder("pdu.f").nominal_watts(1000).usable_watts(1000).derate_bp(0));
    PC_CHECK_EQ(facility.assess("pdu.f").reason, ReasonCode::DerateRemovesCapacity);
  }
  // 7. Then a zero nameplate.
  {
    Facility facility;
    facility.add_domain(DomainBuilder("pdu.g").nominal_watts(0).usable_watts(0));
    PC_CHECK_EQ(facility.assess("pdu.g").reason, ReasonCode::ZeroNameplateCapacity);
  }
}

PC_TEST(model, unknown_domain_is_an_error_not_an_unknown_value) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  CapacityQuery query;
  query.domain = PC_REQUIRE_OK(DomainId::parse("pdu.missing", 128));
  query.as_of = Tick(0);
  PC_REQUIRE_STATUS(facility.engine().assess(query), StatusCode::NotFound);
}

PC_TEST(model, empty_domain_name_is_refused) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  CapacityQuery query;
  query.as_of = Tick(0);
  PC_REQUIRE_STATUS(facility.engine().assess(query), StatusCode::InvalidArgument);
}

PC_TEST(model, domain_kind_never_implies_redundancy) {
  // Two domains named and classified as though they were an N+1 pair get no
  // redundancy behaviour at all: the obligation must be stated explicitly.
  Facility facility;
  facility.add_domain(DomainBuilder("ups.a").kind(DomainKind::UpsModule).nominal_watts(100000).usable_watts(100000));
  facility.add_domain(DomainBuilder("ups.b").kind(DomainKind::UpsModule).nominal_watts(100000).usable_watts(100000));
  const DomainAssessment first = facility.assess("ups.a");
  PC_CHECK(!first.redundancy_group.has_value());
  PC_CHECK_EQ(first.derivation->allocatable_headroom.milliwatts(), 100000000);
}

PC_TEST(model, every_domain_requires_evidence_to_be_known) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).no_evidence());
  const DomainAssessment assessment = facility.assess("pdu.a");
  PC_CHECK(!assessment.known);
  PC_CHECK_EQ(assessment.reason, ReasonCode::EvidenceMissing);
  PC_CHECK(!assessment.derivation.has_value());
  PC_CHECK(!assessment.effective_allocatable_headroom.has_value());
  PC_CHECK(!assessment.bottleneck.has_value());
}
