#include "power_capacity/redundancy.hpp"

#include <algorithm>
#include <string>

#include "detail/validate.hpp"

namespace power_capacity {

const char* to_string(RedundancyClass redundancy_class) noexcept {
  switch (redundancy_class) {
    case RedundancyClass::Unspecified:
      return "unspecified";
    case RedundancyClass::N:
      return "n";
    case RedundancyClass::NPlusOne:
      return "n_plus_one";
    case RedundancyClass::NPlusTwo:
      return "n_plus_two";
    case RedundancyClass::TwoN:
      return "two_n";
    case RedundancyClass::TwoNPlusOne:
      return "two_n_plus_one";
    case RedundancyClass::TwoNPlusTwo:
      return "two_n_plus_two";
    case RedundancyClass::Other:
      return "other";
  }
  return "unspecified";
}

Result<RedundancyClass> parse_redundancy_class(std::string_view text) {
  if (text == "unspecified") {
    return RedundancyClass::Unspecified;
  }
  if (text == "n") {
    return RedundancyClass::N;
  }
  if (text == "n_plus_one") {
    return RedundancyClass::NPlusOne;
  }
  if (text == "n_plus_two") {
    return RedundancyClass::NPlusTwo;
  }
  if (text == "two_n") {
    return RedundancyClass::TwoN;
  }
  if (text == "two_n_plus_one") {
    return RedundancyClass::TwoNPlusOne;
  }
  if (text == "two_n_plus_two") {
    return RedundancyClass::TwoNPlusTwo;
  }
  if (text == "other") {
    return RedundancyClass::Other;
  }
  return Status::error(StatusCode::InvalidArgument,
                       "unknown redundancy class: '" + std::string(text) + "'");
}

Status canonicalize_group_members(std::vector<DomainId>& members) {
  std::sort(members.begin(), members.end());
  for (std::size_t index = 1; index < members.size(); ++index) {
    if (members[index] == members[index - 1]) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "redundancy group lists member '" + members[index].value() +
                               "' more than once");
    }
  }
  return Status::success();
}

Status validate_group(const RedundancyGroup& group, const ResourceLimits& limits) {
  Status status = validate_identifier(group.id.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "redundancy group id: " + status.message());
  }
  status = validate_identifier(group.policy_authority.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(),
                         "redundancy group '" + group.id.value() +
                             "' must cite the authority that asserts its obligation: " +
                             status.message());
  }
  status = validate_label(group.label, limits.max_label_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "redundancy group label: " + status.message());
  }
  if (group.members.size() < 2) {
    return Status::error(StatusCode::InvalidArgument,
                         "redundancy group '" + group.id.value() +
                             "' must list at least two members");
  }
  if (group.members.size() > limits.max_members_per_group) {
    return Status::error(StatusCode::LimitExceeded,
                         "redundancy group '" + group.id.value() + "' lists " +
                             std::to_string(group.members.size()) + " members, limit is " +
                             std::to_string(limits.max_members_per_group));
  }
  for (std::size_t index = 0; index < group.members.size(); ++index) {
    status = validate_identifier(group.members[index].value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "redundancy group member id: " + status.message());
    }
    if (index > 0 && !(group.members[index - 1] < group.members[index])) {
      return Status::error(StatusCode::InvalidArgument,
                           "redundancy group '" + group.id.value() +
                               "' members must be sorted ascending and unique");
    }
  }
  // Bounds are checked before relationships: a declaration that exceeds a
  // configured limit is refused as out of range before any reasoning about what
  // it would mean.
  if (group.required_simultaneous_failures > limits.max_redundancy_tolerance) {
    return Status::error(StatusCode::LimitExceeded,
                         "redundancy group '" + group.id.value() + "' tolerance " +
                             std::to_string(group.required_simultaneous_failures) + " exceeds " +
                             std::to_string(limits.max_redundancy_tolerance));
  }
  if (group.required_simultaneous_failures >= group.members.size()) {
    return Status::error(StatusCode::InvalidArgument,
                         "redundancy group '" + group.id.value() +
                             "' tolerates losing every member; the tolerance must be smaller than "
                             "the member count");
  }
  status = detail::validate_evidence_ref(group.evidence, limits, "redundancy group");
  if (!status.ok()) {
    return status;
  }
  return detail::validate_source_generations(group.source_generations, limits,
                                             "redundancy group");
}

}  // namespace power_capacity
