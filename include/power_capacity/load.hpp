#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "power_capacity/evidence.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/status.hpp"
#include "power_capacity/units.hpp"

namespace power_capacity {

/// Classification of a load attached to a power domain.
enum class LoadClass : std::uint8_t {
  /// Load that is under capacity management. It consumes allocatable headroom.
  Committed = 0,
  /// Load that must remain served, for example life safety, controls, or heat
  /// removal. It consumes capacity and is never available to new commitments.
  /// Power Capacity classifies protected load; it does not shed load.
  Protected = 1,
};

const char* to_string(LoadClass load_class) noexcept;
Result<LoadClass> parse_load_class(std::string_view text);

/// One load attributed to exactly one power domain.
///
/// A load is attached at a single point and rolls up through that domain's
/// ancestors. Attaching the same physical load at more than one domain would
/// double count it, so every load carries its own identity and the model rejects
/// a duplicate identity outright.
struct LoadRecord {
  LoadId id;
  /// The single domain this load is attached to.
  DomainId domain;
  LoadClass load_class = LoadClass::Committed;
  Power load{};
  /// The authority outside Power Capacity that granted this commitment.
  /// Power Capacity accounts for committed load; it does not grant it, and it
  /// refuses a load record that does not cite the authority that granted it.
  AuthorityRef authority;
  std::optional<EvidenceRef> evidence;
  std::string label;
};

/// Validates a load declaration against the configured bounds.
Status validate_load(const LoadRecord& load, const ResourceLimits& limits);

}  // namespace power_capacity
