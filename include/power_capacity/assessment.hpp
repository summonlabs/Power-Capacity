#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "power_capacity/capacity.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/load.hpp"
#include "power_capacity/status.hpp"
#include "power_capacity/units.hpp"

namespace power_capacity {

/// A question about one domain's allocatable capacity at one logical instant.
struct CapacityQuery {
  DomainId domain;
  Tick as_of;
  std::optional<Generation> expected_generation;
  std::optional<Epoch> expected_epoch;
  std::optional<Incarnation> expected_incarnation;
};

/// One domain on the path from the queried domain up to the root.
struct PathNode {
  DomainId domain;
  /// 0 is the queried domain; each step toward the root adds one.
  std::uint32_t depth = 0;
  CapacityDerivation derivation{};
  std::optional<GroupId> redundancy_group;
  std::optional<Power> group_effective_headroom;
  /// The lesser of this domain's own allocatable headroom and the effective
  /// headroom of the redundancy group it belongs to.
  Power effective_headroom{};
  ReasonCode reason = ReasonCode::Ok;
};

/// The token a caller can present later to ask whether an assessment still holds.
struct AssessmentToken {
  StoreIdentity store;
  SessionId session;
  Generation generation;
  Epoch epoch;
  Incarnation incarnation;
  Tick as_of;
  DomainId domain;
  std::uint64_t digest = 0;
};

/// The answer to a capacity query.
///
/// A capacity that cannot be justified by current evidence is reported with
/// `known == false` and a reason. It is never reported as zero.
struct DomainAssessment {
  DomainId domain;
  StoreIdentity store;
  SessionId session;

  Tick as_of;
  Generation generation;
  Epoch epoch;
  Incarnation incarnation;

  bool known = false;
  ReasonCode reason = ReasonCode::Unknown;
  std::uint32_t depth = 0;

  std::optional<CapacityDerivation> derivation;
  /// The redundancy group this domain belongs to, if any.
  std::optional<GroupId> redundancy_group;
  /// The effective headroom of that group, before this domain's own bound.
  std::optional<Power> group_effective_headroom;
  /// The allocatable headroom for a *new* commitment at this domain: the least of
  /// this domain's own allocatable headroom, the effective headroom of its
  /// redundancy group, and the allocatable headroom of every ancestor on its
  /// path. A commitment at this domain consumes capacity at every ancestor, so
  /// this is the quantity to compare a proposed load against. Equal to
  /// `bottleneck_headroom`.
  std::optional<Power> effective_allocatable_headroom;
  /// The most restrictive domain on the path, when the query succeeded.
  std::optional<DomainId> bottleneck;
  std::optional<Power> bottleneck_headroom;
  /// True when any domain on the path, including the queried one, is degraded.
  bool path_degraded = false;

  /// Index 0 is the queried domain; the last element is the root.
  std::vector<PathNode> path;
  /// Evidence records that justified the answer, ordered by path position.
  std::vector<EvidenceRef> evidence;

  /// Deterministic change-detection digest over the authoritative fields above.
  /// It detects change; it is not a cryptographic commitment and does not resist
  /// deliberate tampering.
  std::uint64_t digest = 0;

  AssessmentToken token() const;
};

/// A proposed load. Evaluation is advisory: Power Capacity reports whether the
/// load fits under current authority and evidence, and does not grant it.
struct CandidateLoad {
  LoadId id;
  DomainId domain;
  LoadClass load_class = LoadClass::Committed;
  Power load{};
};

enum class CandidateVerdict : std::uint8_t {
  /// The load fits at the requested instant under current authority and evidence.
  Admissible = 0,
  /// The load does not fit, or a precondition is violated.
  Refused = 1,
  /// The value cannot be determined; the caller must refresh evidence or
  /// authority. Distinct from `Refused`.
  Indeterminate = 2,
};

const char* to_string(CandidateVerdict verdict) noexcept;

struct CandidateEvaluation {
  LoadId id;
  DomainId domain;
  LoadClass load_class = LoadClass::Committed;
  CandidateVerdict verdict = CandidateVerdict::Indeterminate;
  ReasonCode reason = ReasonCode::Unknown;
  StatusCode cause = StatusCode::Unknown;
  Power requested{};
  /// The most restrictive domain on the path.
  std::optional<DomainId> bottleneck;
  /// Headroom at the bottleneck after this load is accounted for.
  std::optional<Power> remaining_at_bottleneck;
  /// True when the binding constraint was a redundancy group rather than a
  /// domain's own headroom.
  bool redundancy_limited = false;
  std::string explanation;
};

enum class RevalidationVerdict : std::uint8_t {
  /// The assessment still holds unchanged.
  StillValid = 0,
  /// The assessment no longer holds.
  Superseded = 1,
  /// The assessment cannot be confirmed or denied from current evidence.
  Indeterminate = 2,
};

const char* to_string(RevalidationVerdict verdict) noexcept;

struct RevalidationOutcome {
  RevalidationVerdict verdict = RevalidationVerdict::Indeterminate;
  ReasonCode reason = ReasonCode::Unknown;
  StatusCode cause = StatusCode::Unknown;
  std::string explanation;
  /// The freshly computed assessment when one could be produced.
  std::optional<DomainAssessment> current;
};

/// The result of qualifying recovered state against current evidence.
struct RevalidationReport {
  Tick as_of;
  Generation generation;
  Epoch epoch;
  Incarnation incarnation;
  std::size_t domains_total = 0;
  std::size_t domains_current = 0;
  std::size_t domains_stale = 0;
  std::size_t domains_missing_evidence = 0;
  /// Bounded, sorted samples of the domains that are not current.
  std::vector<DomainId> stale_domains;
  std::vector<DomainId> missing_evidence_domains;
  bool all_evidence_current = false;
};

}  // namespace power_capacity
