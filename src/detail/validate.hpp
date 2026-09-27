#pragma once

// Internal validation helpers shared by the model types. Not installed.

#include <map>
#include <optional>
#include <string>

#include "power_capacity/evidence.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/status.hpp"

namespace power_capacity::detail {

/// Validates an optional evidence reference: identifier forms, non-negative
/// observation instant, and a non-empty validity window.
inline Status validate_evidence_ref(const std::optional<EvidenceRef>& evidence,
                                    const ResourceLimits& limits, const char* owner) {
  if (!evidence.has_value()) {
    return Status::success();
  }
  Status status = validate_identifier(evidence->id.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), std::string(owner) + " evidence id: " + status.message());
  }
  status = validate_identifier(evidence->source.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), std::string(owner) + " evidence source: " + status.message());
  }
  if (evidence->observed_at.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string(owner) + " evidence observed_at must not be negative");
  }
  if (evidence->valid_until.value() <= evidence->observed_at.value()) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string(owner) +
                             " evidence validity window must end after it begins: valid_until "
                             "must be strictly greater than observed_at");
  }
  return Status::success();
}

/// Validates the provenance map of a record: bounded size and well-formed source
/// identifiers.
inline Status validate_source_generations(const std::map<SourceId, Generation>& generations,
                                          const ResourceLimits& limits, const char* owner) {
  if (generations.size() > limits.max_sources) {
    return Status::error(StatusCode::LimitExceeded,
                         std::string(owner) + " declares " + std::to_string(generations.size()) +
                             " source generations, limit is " + std::to_string(limits.max_sources));
  }
  for (const auto& entry : generations) {
    const Status status = validate_identifier(entry.first.value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), std::string(owner) + " source id: " + status.message());
    }
  }
  return Status::success();
}

}  // namespace power_capacity::detail
