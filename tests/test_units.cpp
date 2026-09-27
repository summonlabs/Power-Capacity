// Exact-unit arithmetic, identifier validation, and status token stability.

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "power_capacity/ids.hpp"
#include "power_capacity/status.hpp"
#include "power_capacity/units.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;

namespace {

const StatusCode kAllStatusCodes[] = {
    StatusCode::Ok,
    StatusCode::InvalidArgument,
    StatusCode::NotFound,
    StatusCode::AlreadyExists,
    StatusCode::DuplicateIdentity,
    StatusCode::Conflict,
    StatusCode::PreconditionFailed,
    StatusCode::StaleGeneration,
    StatusCode::StaleAuthority,
    StatusCode::StaleSourceGeneration,
    StatusCode::IncompatibleVersion,
    StatusCode::Corruption,
    StatusCode::LimitExceeded,
    StatusCode::Unsupported,
    StatusCode::Unavailable,
    StatusCode::Unknown,
    StatusCode::Indeterminate,
    StatusCode::PermissionDenied,
    StatusCode::IoFailure,
    StatusCode::LockConflict,
    StatusCode::InvariantViolation,
    StatusCode::CapacityExceeded,
    StatusCode::RedundancyViolated,
    StatusCode::EvidenceMissing,
    StatusCode::EvidenceStale,
    StatusCode::NotRevalidated,
    StatusCode::Closed,
    StatusCode::EndianMismatch,
    StatusCode::ReadOnly,
};

const ReasonCode kAllReasonCodes[] = {
    ReasonCode::Ok,
    ReasonCode::EvidenceMissing,
    ReasonCode::EvidenceStale,
    ReasonCode::EvidenceFromFuture,
    ReasonCode::DomainUnavailableMaintenance,
    ReasonCode::DomainUnavailableFault,
    ReasonCode::DomainUnavailableDecommissioned,
    ReasonCode::DomainUnavailableUnknown,
    ReasonCode::DomainDegradedMaintenance,
    ReasonCode::DomainDegradedFault,
    ReasonCode::DomainDegradedEvidenceUnknown,
    ReasonCode::ZeroNameplateCapacity,
    ReasonCode::DerateRemovesCapacity,
    ReasonCode::ReserveExceedsCapacity,
    ReasonCode::ProtectedLoadExceedsCapacity,
    ReasonCode::CommittedLoadExceedsCapacity,
    ReasonCode::LoadOnUnavailableDomain,
    ReasonCode::RedundancyToleranceAtLimit,
    ReasonCode::RedundancyToleranceExceeded,
    ReasonCode::RedundancyGroupUnknown,
    ReasonCode::SharedUpstreamLimits,
    ReasonCode::BottleneckUpstream,
    ReasonCode::NotRevalidated,
    ReasonCode::DomainNotFound,
    ReasonCode::StoreIdentityMismatch,
    ReasonCode::StoreReopened,
    ReasonCode::AuthorityEpochChanged,
    ReasonCode::SourceGenerationAdvanced,
    ReasonCode::EvidenceRevisionChanged,
    ReasonCode::RecoveredStateRevalidated,
    ReasonCode::Admissible,
    ReasonCode::GenerationAdvanced,
    ReasonCode::PathContainsDegradedDomain,
    ReasonCode::Unknown,
    ReasonCode::InsufficientHeadroom,
    ReasonCode::LoadAlreadyCommitted,
};

}  // namespace

PC_TEST(units, power_construction_is_exact) {
  PC_CHECK_EQ(PC_REQUIRE_OK(Power::from_watts(1500)).milliwatts(), 1500 * 1000);
  PC_CHECK_EQ(PC_REQUIRE_OK(Power::from_kilowatts(5)).milliwatts(), 5 * 1000 * 1000);
  PC_CHECK_EQ(PC_REQUIRE_OK(Power::from_megawatts(2)).milliwatts(), 2 * 1000 * 1000 * 1000);
  PC_CHECK_EQ(Power(1234).to_watts_string(), std::string("1.234"));
  PC_CHECK_EQ(Power(-2500).to_watts_string(), std::string("-2.500"));
  PC_CHECK_EQ(Power(0).to_watts_string(), std::string("0.000"));
  PC_CHECK_EQ(Power(1000).to_watts_string(), std::string("1.000"));
}

PC_TEST(units, power_construction_overflow_is_refused) {
  PC_REQUIRE_STATUS(Power::from_watts(std::numeric_limits<std::int64_t>::max()),
                    StatusCode::LimitExceeded);
  PC_REQUIRE_STATUS(Power::from_megawatts(std::numeric_limits<std::int64_t>::max()),
                    StatusCode::LimitExceeded);
}

PC_TEST(units, checked_arithmetic_reports_overflow) {
  const Power maximum(std::numeric_limits<std::int64_t>::max());
  PC_REQUIRE_STATUS(add_power(maximum, Power(1)), StatusCode::LimitExceeded);
  PC_REQUIRE_STATUS(subtract_power(Power(std::numeric_limits<std::int64_t>::min()), Power(1)),
                    StatusCode::LimitExceeded);
  PC_CHECK_EQ(PC_REQUIRE_OK(add_power(Power(5), Power(7))).milliwatts(), 12);
  PC_CHECK_EQ(PC_REQUIRE_OK(subtract_power(Power(5), Power(7))).milliwatts(), -2);
}

PC_TEST(units, clamped_subtraction_reports_the_clamp) {
  const ClampedDifference exact = subtract_power_clamped(Power(10), Power(4));
  PC_CHECK_EQ(exact.value.milliwatts(), 6);
  PC_CHECK(!exact.clamped);

  const ClampedDifference clamped = subtract_power_clamped(Power(4), Power(10));
  PC_CHECK_EQ(clamped.value.milliwatts(), 0);
  PC_CHECK(clamped.clamped);

  const ClampedDifference zero = subtract_power_clamped(Power(4), Power(4));
  PC_CHECK_EQ(zero.value.milliwatts(), 0);
  PC_CHECK(!zero.clamped);
}

PC_TEST(units, ratio_range_is_enforced) {
  PC_REQUIRE_STATUS(Ratio::from_basis_points(-1), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(Ratio::from_basis_points(10001), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(Ratio::from_percent(101), StatusCode::InvalidArgument);
  PC_CHECK_EQ(PC_REQUIRE_OK(Ratio::from_percent(90)).basis_points(), 9000);
  PC_CHECK_EQ(Ratio::full().basis_points(), 10000);
  PC_CHECK_EQ(Ratio::zero().basis_points(), 0);
  PC_CHECK_EQ(PC_REQUIRE_OK(Ratio::from_percent(12)).to_percent_string(), std::string("12.00"));
  PC_CHECK_EQ(PC_REQUIRE_OK(Ratio::from_basis_points(1)).to_percent_string(), std::string("0.01"));
}

PC_TEST(units, mul_div_floor_is_exact_and_rounds_down) {
  PC_CHECK_EQ(PC_REQUIRE_OK(mul_div_floor(10, 3, 4)), std::uint64_t{7});
  PC_CHECK_EQ(PC_REQUIRE_OK(mul_div_floor(0, 999, 7)), std::uint64_t{0});
  PC_REQUIRE_STATUS(mul_div_floor(1, 1, 0), StatusCode::InvalidArgument);

  // The intermediate product exceeds 64 bits; the result must still be exact.
  const std::uint64_t large = 1'000'000'000'000'000ULL;  // 1e15
  PC_CHECK_EQ(PC_REQUIRE_OK(mul_div_floor(large, 10000, 10000)), large);
  PC_CHECK_EQ(PC_REQUIRE_OK(mul_div_floor(large, 10000, 100000)),
              static_cast<std::uint64_t>(100'000'000'000'000ULL));

  // A quotient that cannot fit in 64 bits is refused, never truncated.
  PC_REQUIRE_STATUS(mul_div_floor(std::numeric_limits<std::uint64_t>::max(), 2, 1),
                    StatusCode::LimitExceeded);
}

PC_TEST(units, apply_ratio_rounds_down_and_rejects_negatives) {
  PC_CHECK_EQ(PC_REQUIRE_OK(apply_ratio(Power(1001), PC_REQUIRE_OK(Ratio::from_basis_points(5000))))
                  .milliwatts(),
              500);
  PC_CHECK_EQ(PC_REQUIRE_OK(apply_ratio(Power(1000), Ratio::full())).milliwatts(), 1000);
  PC_CHECK_EQ(PC_REQUIRE_OK(apply_ratio(Power(1000), Ratio::zero())).milliwatts(), 0);
  PC_REQUIRE_STATUS(apply_ratio(Power(-1), Ratio::full()), StatusCode::InvalidArgument);

  // Derating never rounds up, so a chain of derates can never invent capacity.
  Power value(999);
  for (int step = 0; step < 8; ++step) {
    value = PC_REQUIRE_OK(apply_ratio(value, PC_REQUIRE_OK(Ratio::from_basis_points(9000))));
  }
  PC_CHECK(value.milliwatts() <= 999);
}

PC_TEST(ids, identifier_charset_is_enforced) {
  PC_CHECK(PC_REQUIRE_OK(DomainId::parse("row.a-pdu_01:x@y", 128)).value() ==
           std::string("row.a-pdu_01:x@y"));
  PC_REQUIRE_STATUS(DomainId::parse("", 128), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(DomainId::parse("has space", 128), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(DomainId::parse("has/slash", 128), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(DomainId::parse("has\\backslash", 128), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(DomainId::parse("has\ttab", 128), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(DomainId::parse(std::string("nul\0byte", 8), 128), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(DomainId::parse("dotted..ok", 4), StatusCode::LimitExceeded);
  PC_REQUIRE_STATUS(DomainId::parse("\xc3\xa9", 128), StatusCode::InvalidArgument);
}

PC_TEST(ids, families_do_not_interconvert) {
  const DomainId domain = PC_REQUIRE_OK(DomainId::parse("rack.r1", 128));
  const LoadId load = PC_REQUIRE_OK(LoadId::parse("rack.r1", 128));
  // Same text, different types: a compile-time guarantee that the two can never
  // be substituted for one another.
  static_assert(!std::is_convertible_v<DomainId, LoadId>);
  static_assert(!std::is_convertible_v<LoadId, DomainId>);
  PC_CHECK_EQ(domain.value(), load.value());
}

PC_TEST(ids, fingerprints_require_lowercase_hex_and_non_zero) {
  const StoreIdentity identity = PC_REQUIRE_OK(
      StoreIdentity::parse("0123456789abcdef0123456789abcdef"));
  PC_CHECK_EQ(identity.to_hex(), std::string("0123456789abcdef0123456789abcdef"));
  PC_CHECK_EQ(identity.high(), std::uint64_t{0x0123456789abcdefULL});
  PC_REQUIRE_STATUS(StoreIdentity::parse("0123456789ABCDEF0123456789ABCDEF"),
                    StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(StoreIdentity::parse("0123456789abcdef"), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(StoreIdentity::parse("00000000000000000000000000000000"),
                    StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(StoreIdentity::from_components(0, 0), StatusCode::InvalidArgument);

  const StoreIdentity generated = StoreIdentity::generate();
  PC_CHECK(!generated.is_zero());
  PC_CHECK_NE(generated, StoreIdentity::generate());
}

PC_TEST(ids, ordinals_do_not_interconvert) {
  static_assert(!std::is_convertible_v<Generation, Epoch>);
  static_assert(!std::is_convertible_v<Tick, Generation>);
  PC_CHECK_EQ(Generation(7).value(), std::uint64_t{7});
  PC_CHECK_EQ(Tick(-1).value(), std::int64_t{-1});
  PC_CHECK(Generation(7) < Generation(8));
}

PC_TEST(status, tokens_are_stable_and_distinct) {
  for (const StatusCode code : kAllStatusCodes) {
    PC_CHECK(std::string(to_string(code)) != std::string("unknown_status_code"));
  }
  PC_CHECK_EQ(std::string(to_string(StatusCode::LockConflict)), std::string("lock_conflict"));
  PC_CHECK_EQ(std::string(to_string(StatusCode::StaleSourceGeneration)),
              std::string("stale_source_generation"));
  PC_CHECK_EQ(std::string(to_string(StatusCode::EndianMismatch)), std::string("endian_mismatch"));
}

PC_TEST(status, reason_tokens_are_stable_and_distinct) {
  std::vector<std::string> tokens;
  for (const ReasonCode code : kAllReasonCodes) {
    const std::string token = to_string(code);
    PC_CHECK_MSG(token != std::string("unknown") || code == ReasonCode::Unknown,
                 "reason code " + std::to_string(static_cast<int>(code)) +
                     " has no stable token");
    tokens.push_back(token);
  }
  for (std::size_t left = 0; left < tokens.size(); ++left) {
    for (std::size_t right = left + 1; right < tokens.size(); ++right) {
      PC_CHECK_MSG(tokens[left] != tokens[right],
                   "duplicate reason token " + tokens[left] + " at " + std::to_string(left) +
                       " and " + std::to_string(right));
    }
  }
}

PC_TEST(status, reason_and_status_are_distinct_types) {
  // A reason is an explanation and a status is an outcome. They are separate
  // enumerations with no implicit conversion between them, so a caller can never
  // pass one where the other is expected. Overlapping tokens between the two
  // enumerations are therefore unambiguous: they are always rendered under a
  // distinct key, `error` or `reason`.
  static_assert(!std::is_convertible_v<ReasonCode, StatusCode>);
  static_assert(!std::is_convertible_v<StatusCode, ReasonCode>);
  static_assert(!std::is_same_v<ReasonCode, StatusCode>);
  for (const ReasonCode reason : kAllReasonCodes) {
    const std::string token = to_string(reason);
    PC_CHECK(!token.empty());
    for (const StatusCode status : kAllStatusCodes) {
      if (token == std::string(to_string(status))) {
        // The only acceptable overlap is an exact semantic match, and the report
        // always labels which enumeration produced the token.
        PC_CHECK(token == std::string("ok") || token == std::string("unknown") ||
                 token == std::string("evidence_missing") ||
                 token == std::string("evidence_stale") ||
                 token == std::string("not_revalidated"));
      }
    }
  }
}

PC_TEST(status, result_carries_value_or_error) {
  const Result<int> good(7);
  PC_CHECK(good.ok());
  PC_CHECK_EQ(good.value(), 7);
  const Result<int> bad = Status::error(StatusCode::NotFound, "missing");
  PC_CHECK(!bad.ok());
  PC_CHECK_EQ(bad.status().code(), StatusCode::NotFound);
  PC_CHECK_EQ(bad.status().to_string(), std::string("not_found: missing"));
  PC_CHECK_EQ(Status::success().to_string(), std::string("ok"));
  PC_CHECK(Status::success().ok());
  PC_CHECK(!Status::error(StatusCode::Conflict, "x").ok());
}
