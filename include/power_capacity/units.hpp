#pragma once

#include <compare>
#include <cstdint>
#include <string>

#include "power_capacity/status.hpp"

namespace power_capacity {

/// Exact electrical power.
///
/// The canonical unit is the milliwatt, stored as a signed 64-bit integer. All
/// authoritative capacity, load, reserve, and derating accounting uses this type
/// with checked arithmetic. Floating point is never used for an authoritative
/// quantity; `to_watts_string` renders exact decimal text for presentation only.
class Power {
 public:
  static constexpr std::int64_t milliwatts_per_watt = 1000;
  static constexpr std::int64_t milliwatts_per_kilowatt = 1000 * milliwatts_per_watt;
  static constexpr std::int64_t milliwatts_per_megawatt = 1000 * milliwatts_per_kilowatt;

  constexpr Power() noexcept = default;
  explicit constexpr Power(std::int64_t milliwatts) noexcept : milliwatts_(milliwatts) {}

  static Result<Power> from_watts(std::int64_t watts) noexcept;
  static Result<Power> from_kilowatts(std::int64_t kilowatts) noexcept;
  static Result<Power> from_megawatts(std::int64_t megawatts) noexcept;

  constexpr std::int64_t milliwatts() const noexcept { return milliwatts_; }
  constexpr bool is_zero() const noexcept { return milliwatts_ == 0; }
  constexpr bool is_negative() const noexcept { return milliwatts_ < 0; }

  /// Exact decimal rendering in watts with three fractional digits, for example
  /// `1500.000` or `-2.500`. Never produces exponent notation.
  std::string to_watts_string() const;

  friend constexpr bool operator==(Power, Power) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Power, Power) noexcept = default;

 private:
  std::int64_t milliwatts_ = 0;
};

/// Checked addition. Fails with `StatusCode::LimitExceeded` on 64-bit overflow.
Result<Power> add_power(Power left, Power right);
/// Checked subtraction. Fails with `StatusCode::LimitExceeded` on 64-bit overflow.
Result<Power> subtract_power(Power left, Power right);

struct ClampedDifference {
  Power value{};
  /// True when the exact difference was negative and was clamped to zero.
  bool clamped = false;
};

/// Subtraction that clamps a negative result to zero and reports that it did so.
/// Capacity accounting uses this so that an overcommitted domain reports zero
/// headroom together with an explicit reason instead of a negative number.
ClampedDifference subtract_power_clamped(Power left, Power right) noexcept;

Power max_power(Power left, Power right) noexcept;
Power min_power(Power left, Power right) noexcept;

/// An exact ratio in basis points. 10000 basis points is 1.0.
///
/// There is no default constructor: a ratio must be stated explicitly, so that a
/// derating factor can never be silently zero because a field was left unset.
class Ratio {
 public:
  static constexpr std::int32_t scale = 10000;

  Ratio() = delete;
  Ratio(const Ratio&) = default;
  Ratio& operator=(const Ratio&) = default;

  static Result<Ratio> from_basis_points(std::int32_t basis_points) noexcept;
  static Result<Ratio> from_percent(std::int32_t percent) noexcept;

  static constexpr Ratio zero() noexcept { return Ratio(0); }
  static constexpr Ratio full() noexcept { return Ratio(scale); }

  constexpr std::int32_t basis_points() const noexcept { return basis_points_; }
  constexpr bool is_zero() const noexcept { return basis_points_ == 0; }
  constexpr bool is_full() const noexcept { return basis_points_ == scale; }

  /// Exact decimal rendering in percent with two fractional digits.
  std::string to_percent_string() const;

  friend constexpr bool operator==(Ratio, Ratio) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Ratio, Ratio) noexcept = default;

 private:
  explicit constexpr Ratio(std::int32_t basis_points) noexcept : basis_points_(basis_points) {}

  std::int32_t basis_points_ = 0;
};

/// `floor(value * multiplier / divisor)` with a 128-bit intermediate product.
///
/// The result is exact and rounds toward zero, which for non-negative capacity
/// values is the conservative direction. Fails with `LimitExceeded` only when the
/// quotient itself does not fit in 64 bits, or with `InvalidArgument` when
/// `divisor` is zero.
Result<std::uint64_t> mul_div_floor(std::uint64_t value, std::uint64_t multiplier,
                                    std::uint64_t divisor) noexcept;

/// Applies a ratio to a non-negative power, rounding down. Negative input is
/// rejected with `InvalidArgument`; ratios never apply to negative quantities in
/// this model.
Result<Power> apply_ratio(Power value, Ratio ratio);

}  // namespace power_capacity
