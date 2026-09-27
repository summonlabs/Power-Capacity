#pragma once

#include <cstdint>
#include <optional>

#include "power_capacity/ids.hpp"

namespace power_capacity {

/// Freshness classification of an evidence record at a requested instant.
enum class EvidenceState : std::uint8_t {
  /// No record was supplied. Never interpreted as a zero value.
  Missing = 0,
  /// The record claims an observation instant later than the requested instant,
  /// so it is not yet authoritative.
  NotYetValid = 1,
  /// `observed_at <= as_of < valid_until`.
  Current = 2,
  /// The record is present but its validity window closed at or before `as_of`.
  Stale = 3,
};

const char* to_string(EvidenceState state) noexcept;

/// Provenance and freshness of one externally supplied fact.
///
/// Power Capacity does not observe electrical state itself. Every capacity,
/// load, derating, state, and redundancy fact arrives as evidence from a named
/// source with an explicit validity window. Nothing here is a wall clock: the
/// instants are caller-supplied logical ticks, so evaluation is deterministic and
/// replayable.
struct EvidenceRef {
  EvidenceId id;
  SourceId source;
  Tick observed_at;
  /// Exclusive upper bound: the record is current while `as_of < valid_until`.
  Tick valid_until;
  Revision revision;
  /// Generation of the source that produced the record, as declared by the
  /// caller when the record was accepted.
  Generation source_generation;
};

/// Classifies a record against a requested instant.
EvidenceState evaluate_evidence(const std::optional<EvidenceRef>& ref, Tick as_of) noexcept;

/// True when the record is present and current at `as_of`.
bool evidence_is_current(const std::optional<EvidenceRef>& ref, Tick as_of) noexcept;

}  // namespace power_capacity
