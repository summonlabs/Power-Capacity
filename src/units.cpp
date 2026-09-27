#include "power_capacity/units.hpp"

#include <cstdint>
#include <limits>
#include <string>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#endif

namespace power_capacity {
namespace {

/// Multiplies two 64-bit values into a 128-bit product without losing bits.
///
/// The 128-bit product is required for exact fixed-point derating: the
/// intermediate `value * basis_points` legitimately exceeds 64 bits for large
/// aggregates multiplied by a 10000-scale ratio.
struct WideProduct {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  bool overflowed = false;
};

WideProduct multiply_wide(std::uint64_t left, std::uint64_t right) noexcept {
  WideProduct product;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
  product.low = _umul128(left, right, &product.high);
#elif defined(__SIZEOF_INT128__)
  const unsigned __int128 wide =
      static_cast<unsigned __int128>(left) * static_cast<unsigned __int128>(right);
  product.low = static_cast<std::uint64_t>(wide);
  product.high = static_cast<std::uint64_t>(wide >> 64);
#else
  if (right != 0 && left > (std::numeric_limits<std::uint64_t>::max() / right)) {
    product.overflowed = true;
    return product;
  }
  product.low = left * right;
  product.high = 0;
#endif
  return product;
}

/// Divides a 128-bit value by a 64-bit divisor using shift-and-subtract. Correct
/// for every case in which the quotient fits in 64 bits, which the caller checks.
std::uint64_t divide_wide(std::uint64_t high, std::uint64_t low, std::uint64_t divisor) noexcept {
  std::uint64_t quotient = 0;
  std::uint64_t remainder = high;
  for (int bit = 63; bit >= 0; --bit) {
    const std::uint64_t carry = remainder >> 63;
    remainder = (remainder << 1) | ((low >> static_cast<unsigned>(bit)) & 1ULL);
    quotient <<= 1;
    if (carry != 0 || remainder >= divisor) {
      remainder -= divisor;
      quotient |= 1ULL;
    }
  }
  return quotient;
}

Result<std::int64_t> scale_checked(std::int64_t value, std::int64_t factor,
                                   const char* unit_name) noexcept {
  if (value > 0 && value > std::numeric_limits<std::int64_t>::max() / factor) {
    return Status::error(StatusCode::LimitExceeded,
                         std::string("power in ") + unit_name + " exceeds the 64-bit milliwatt range");
  }
  if (value < 0 && value < std::numeric_limits<std::int64_t>::min() / factor) {
    return Status::error(StatusCode::LimitExceeded,
                         std::string("power in ") + unit_name + " exceeds the 64-bit milliwatt range");
  }
  return value * factor;
}

std::string format_fixed(std::int64_t value, std::int64_t scale, int digits) {
  const bool negative = value < 0;
  const std::uint64_t magnitude =
      negative ? (~static_cast<std::uint64_t>(value) + 1ULL) : static_cast<std::uint64_t>(value);
  const std::uint64_t divisor = static_cast<std::uint64_t>(scale);
  const std::uint64_t whole = magnitude / divisor;
  std::uint64_t fraction = magnitude % divisor;

  std::string text;
  if (negative) {
    text.push_back('-');
  }
  text += std::to_string(whole);
  if (digits > 0) {
    text.push_back('.');
    std::string fraction_text(static_cast<std::size_t>(digits), '0');
    for (int index = digits - 1; index >= 0; --index) {
      fraction_text[static_cast<std::size_t>(index)] = static_cast<char>('0' + (fraction % 10));
      fraction /= 10;
    }
    text += fraction_text;
  }
  return text;
}

}  // namespace

Result<Power> Power::from_watts(std::int64_t watts) noexcept {
  const Result<std::int64_t> milliwatts = scale_checked(watts, milliwatts_per_watt, "watts");
  if (!milliwatts.ok()) {
    return milliwatts.status();
  }
  return Power(milliwatts.value());
}

Result<Power> Power::from_kilowatts(std::int64_t kilowatts) noexcept {
  const Result<std::int64_t> milliwatts = scale_checked(kilowatts, milliwatts_per_kilowatt, "kilowatts");
  if (!milliwatts.ok()) {
    return milliwatts.status();
  }
  return Power(milliwatts.value());
}

Result<Power> Power::from_megawatts(std::int64_t megawatts) noexcept {
  const Result<std::int64_t> milliwatts = scale_checked(megawatts, milliwatts_per_megawatt, "megawatts");
  if (!milliwatts.ok()) {
    return milliwatts.status();
  }
  return Power(milliwatts.value());
}

std::string Power::to_watts_string() const {
  return format_fixed(milliwatts_, milliwatts_per_watt, 3);
}

Result<Power> add_power(Power left, Power right) {
  const std::int64_t a = left.milliwatts();
  const std::int64_t b = right.milliwatts();
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return Status::error(StatusCode::LimitExceeded, "power addition overflowed the 64-bit range");
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return Status::error(StatusCode::LimitExceeded, "power addition underflowed the 64-bit range");
  }
  return Power(a + b);
}

Result<Power> subtract_power(Power left, Power right) {
  const std::int64_t a = left.milliwatts();
  const std::int64_t b = right.milliwatts();
  if (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) {
    return Status::error(StatusCode::LimitExceeded, "power subtraction overflowed the 64-bit range");
  }
  if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) {
    return Status::error(StatusCode::LimitExceeded, "power subtraction underflowed the 64-bit range");
  }
  return Power(a - b);
}

ClampedDifference subtract_power_clamped(Power left, Power right) noexcept {
  ClampedDifference difference;
  const std::int64_t a = left.milliwatts();
  const std::int64_t b = right.milliwatts();
  if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) {
    // Cannot happen for the non-negative quantities this model subtracts, but a
    // clamp is still the safe answer.
    difference.value = Power(0);
    difference.clamped = true;
    return difference;
  }
  const std::int64_t raw = a - b;
  if (raw < 0) {
    difference.value = Power(0);
    difference.clamped = true;
    return difference;
  }
  difference.value = Power(raw);
  difference.clamped = false;
  return difference;
}

Power max_power(Power left, Power right) noexcept {
  return left.milliwatts() >= right.milliwatts() ? left : right;
}

Power min_power(Power left, Power right) noexcept {
  return left.milliwatts() <= right.milliwatts() ? left : right;
}

Result<Ratio> Ratio::from_basis_points(std::int32_t basis_points) noexcept {
  if (basis_points < 0 || basis_points > scale) {
    return Status::error(StatusCode::InvalidArgument,
                         "ratio basis points must be in [0, 10000], got " +
                             std::to_string(basis_points));
  }
  return Ratio(basis_points);
}

Result<Ratio> Ratio::from_percent(std::int32_t percent) noexcept {
  if (percent < 0 || percent > 100) {
    return Status::error(StatusCode::InvalidArgument,
                         "ratio percent must be in [0, 100], got " + std::to_string(percent));
  }
  return Ratio(percent * 100);
}

std::string Ratio::to_percent_string() const {
  return format_fixed(basis_points_, 100, 2);
}

Result<std::uint64_t> mul_div_floor(std::uint64_t value, std::uint64_t multiplier,
                                    std::uint64_t divisor) noexcept {
  if (divisor == 0) {
    return Status::error(StatusCode::InvalidArgument, "mul_div_floor divisor must not be zero");
  }
  if (value == 0 || multiplier == 0) {
    return std::uint64_t{0};
  }
  const WideProduct product = multiply_wide(value, multiplier);
  if (product.overflowed) {
    return Status::error(StatusCode::LimitExceeded,
                         "mul_div_floor requires a 128-bit intermediate product that this toolchain "
                         "cannot compute exactly");
  }
  if (product.high >= divisor) {
    return Status::error(StatusCode::LimitExceeded, "mul_div_floor quotient exceeds 64 bits");
  }
  return divide_wide(product.high, product.low, divisor);
}

Result<Power> apply_ratio(Power value, Ratio ratio) {
  if (value.is_negative()) {
    return Status::error(StatusCode::InvalidArgument, "ratio cannot be applied to a negative power");
  }
  if (ratio.is_full()) {
    return value;
  }
  if (ratio.is_zero() || value.is_zero()) {
    return Power(0);
  }
  const Result<std::uint64_t> scaled =
      mul_div_floor(static_cast<std::uint64_t>(value.milliwatts()),
                    static_cast<std::uint64_t>(ratio.basis_points()),
                    static_cast<std::uint64_t>(Ratio::scale));
  if (!scaled.ok()) {
    return scaled.status();
  }
  if (scaled.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Status::error(StatusCode::LimitExceeded, "derated power exceeds the 64-bit range");
  }
  return Power(static_cast<std::int64_t>(scaled.value()));
}

}  // namespace power_capacity
