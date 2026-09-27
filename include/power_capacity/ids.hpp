#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "power_capacity/status.hpp"

namespace power_capacity {

/// Identity families. Each family is a distinct C++ type; there is no implicit
/// conversion between them, and no implicit conversion to or from the underlying
/// string.
enum class IdFamily { Domain, Load, Group, Evidence, Source, Authority, Idempotency };

/// Validates the textual form of an identifier.
///
/// Accepted: 1..`max_bytes` bytes drawn from `A-Z a-z 0-9 . _ - : @`. Rejected:
/// empty, over-long, NUL, whitespace, control characters, path separators,
/// non-ASCII bytes, and every other punctuation character.
Status validate_identifier(std::string_view text, std::size_t max_bytes);

/// Validates free-text metadata such as a domain label. Accepted: up to
/// `max_bytes` bytes of UTF-8 without NUL, C0 controls, or DEL.
Status validate_label(std::string_view text, std::size_t max_bytes);

/// A strongly typed identifier.
template <IdFamily Family>
class BasicId {
 public:
  BasicId() = default;

  static Result<BasicId> parse(std::string_view text, std::size_t max_bytes = 128) {
    const Status status = validate_identifier(text, max_bytes);
    if (!status.ok()) {
      return status;
    }
    BasicId id;
    id.value_.assign(text);
    return id;
  }

  bool empty() const noexcept { return value_.empty(); }
  const std::string& value() const noexcept { return value_; }
  std::string_view view() const noexcept { return value_; }

  friend bool operator==(const BasicId&, const BasicId&) = default;
  friend std::strong_ordering operator<=>(const BasicId&, const BasicId&) = default;

 private:
  std::string value_{};
};

using DomainId = BasicId<IdFamily::Domain>;
using LoadId = BasicId<IdFamily::Load>;
using GroupId = BasicId<IdFamily::Group>;
using EvidenceId = BasicId<IdFamily::Evidence>;
using SourceId = BasicId<IdFamily::Source>;
using AuthorityRef = BasicId<IdFamily::Authority>;
using IdempotencyKey = BasicId<IdFamily::Idempotency>;

/// A 128-bit opaque fingerprint. Used for store identity and engine session
/// identity. A zero fingerprint means "unset" and is never a valid identity.
enum class FingerprintFamily { StoreIdentity, SessionId };

template <FingerprintFamily Family>
class BasicFingerprint {
 public:
  BasicFingerprint() = default;

  /// Non-deterministic 128-bit identity drawn from the platform entropy source.
  static BasicFingerprint generate();

  /// Parses exactly 32 lowercase hexadecimal characters.
  static Result<BasicFingerprint> parse(std::string_view hex);

  /// Builds a fingerprint from its two halves. Used by the serializer and by the
  /// generator; it performs no validation beyond rejecting the all-zero value.
  static Result<BasicFingerprint> from_components(std::uint64_t high, std::uint64_t low);

  bool is_zero() const noexcept { return high_ == 0 && low_ == 0; }
  std::uint64_t high() const noexcept { return high_; }
  std::uint64_t low() const noexcept { return low_; }
  std::string to_hex() const;

  friend bool operator==(const BasicFingerprint&, const BasicFingerprint&) = default;
  friend std::strong_ordering operator<=>(const BasicFingerprint&, const BasicFingerprint&) = default;

 private:
  std::uint64_t high_ = 0;
  std::uint64_t low_ = 0;
};

using StoreIdentity = BasicFingerprint<FingerprintFamily::StoreIdentity>;
using SessionId = BasicFingerprint<FingerprintFamily::SessionId>;

// ---------------------------------------------------------------------------
// Ordinals
// ---------------------------------------------------------------------------

struct GenerationTag {};
struct EpochTag {};
struct IncarnationTag {};
struct RevisionTag {};
struct AttemptTag {};
struct TickTag {};

/// A strongly typed monotonically increasing ordinal.
template <typename Tag, typename Rep>
class BasicOrdinal {
 public:
  using rep_type = Rep;

  constexpr BasicOrdinal() = default;
  explicit constexpr BasicOrdinal(Rep value) noexcept : value_(value) {}

  constexpr Rep value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == Rep{0}; }

  friend constexpr bool operator==(BasicOrdinal, BasicOrdinal) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(BasicOrdinal, BasicOrdinal) noexcept = default;

 private:
  Rep value_ = 0;
};

/// Capacity generation. Generation 0 is the empty, never-committed state.
using Generation = BasicOrdinal<GenerationTag, std::uint64_t>;
/// Consumed control-plane epoch from the Control Plane Epoch runtime.
using Epoch = BasicOrdinal<EpochTag, std::uint64_t>;
/// Consumed controller incarnation within an epoch.
using Incarnation = BasicOrdinal<IncarnationTag, std::uint64_t>;
/// Source-side revision of one evidence record.
using Revision = BasicOrdinal<RevisionTag, std::uint64_t>;
/// Commit attempt identifier, unique per engine session.
using AttemptId = BasicOrdinal<AttemptTag, std::uint64_t>;
/// A logical instant. Power Capacity never reads a wall clock for authoritative
/// decisions; callers supply the instant explicitly.
using Tick = BasicOrdinal<TickTag, std::int64_t>;

}  // namespace power_capacity
