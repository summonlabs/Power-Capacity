#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "power_capacity/evidence.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/status.hpp"

namespace power_capacity {

/// Descriptive label for the redundancy arrangement of a group.
///
/// The label is never used to derive an obligation. The obligation is the
/// explicit `required_simultaneous_failures` count, which the group's policy
/// authority must state.
enum class RedundancyClass : std::uint8_t {
  Unspecified = 0,
  N = 1,
  NPlusOne = 2,
  NPlusTwo = 3,
  TwoN = 4,
  TwoNPlusOne = 5,
  TwoNPlusTwo = 6,
  Other = 7,
};

const char* to_string(RedundancyClass redundancy_class) noexcept;
Result<RedundancyClass> parse_redundancy_class(std::string_view text);

/// A set of independent power domains that jointly carry a load and jointly
/// absorb failures.
///
/// Members must be pairwise independent: no member may be an ancestor or a
/// descendant of another member. That is what makes the group's arithmetic free
/// of double counting, and the model refuses a group that violates it.
struct RedundancyGroup {
  GroupId id;
  RedundancyClass declared_class = RedundancyClass::Unspecified;

  /// Canonical form: sorted ascending, unique, at least two entries.
  std::vector<DomainId> members;

  /// How many members the group must be able to lose simultaneously and still
  /// serve its load. Stated explicitly by `policy_authority`; never inferred from
  /// `declared_class`, from a member name, or from the number of members.
  std::uint32_t required_simultaneous_failures = 0;

  /// Evidence justifying the membership and tolerance.
  std::optional<EvidenceRef> evidence;

  /// The authority that asserts this redundancy obligation.
  AuthorityRef policy_authority;

  std::map<SourceId, Generation> source_generations;
  std::string label;
};

/// Validates a redundancy group declaration against the configured bounds.
Status validate_group(const RedundancyGroup& group, const ResourceLimits& limits);

/// Sorts members ascending and rejects duplicates with
/// `StatusCode::DuplicateIdentity`. `validate_group` requires the canonical
/// sorted-unique form, so callers canonicalize once before validating.
Status canonicalize_group_members(std::vector<DomainId>& members);

}  // namespace power_capacity
