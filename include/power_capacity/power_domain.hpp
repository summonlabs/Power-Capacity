#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "power_capacity/evidence.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/status.hpp"
#include "power_capacity/units.hpp"

namespace power_capacity {

/// Classification of a power domain, consumed from evidence.
///
/// The kind is descriptive metadata. Redundancy, capacity, and authority are
/// never inferred from a kind, a label, or a naming convention.
enum class DomainKind : std::uint8_t {
  UtilityFeed = 0,
  Transformer = 1,
  Switchgear = 2,
  TransferSwitch = 3,
  Generator = 4,
  UpsSystem = 5,
  UpsModule = 6,
  Busway = 7,
  Pdu = 8,
  RackPdu = 9,
  Other = 10,
};

const char* to_string(DomainKind kind) noexcept;
Result<DomainKind> parse_domain_kind(std::string_view text);

/// Operational state of a power domain, consumed from evidence or maintenance
/// authority. It is never inferred from the absence of a fault report.
enum class OperationalState : std::uint8_t {
  Available = 0,
  Degraded = 1,
  Unavailable = 2,
};

const char* to_string(OperationalState state) noexcept;
Result<OperationalState> parse_operational_state(std::string_view text);

/// Why a domain is degraded or unavailable, consumed from evidence.
enum class StateCause : std::uint8_t {
  None = 0,
  Maintenance = 1,
  Fault = 2,
  EvidenceUnknown = 3,
  Decommissioned = 4,
};

const char* to_string(StateCause cause) noexcept;
Result<StateCause> parse_state_cause(std::string_view text);

/// How much of the operational capacity a domain must hold back.
struct ReservePolicy {
  enum class Mode : std::uint8_t {
    /// No reserve is held.
    None = 0,
    /// A fixed quantity is held back.
    Absolute = 1,
    /// A fraction of the operational capacity is held back.
    Ratio = 2,
    /// The greater of a fixed quantity and a fraction is held back.
    GreaterOfAbsoluteAndRatio = 3,
  };

  Mode mode = Mode::None;
  Power absolute{};
  Ratio ratio = Ratio::zero();
};

const char* to_string(ReservePolicy::Mode mode) noexcept;
Result<ReservePolicy::Mode> parse_reserve_mode(std::string_view text);

/// One capacity-bearing node in the electrical-capacity model.
///
/// A domain owns its own capacity declarations and at most one immediate
/// upstream domain. The containment structure is therefore a forest, which is
/// what makes rollups free of double counting. Dual-corded delivery, where a load
/// can be served from two independent sources, is expressed with a redundancy
/// group rather than with a second parent.
struct PowerDomain {
  DomainId id;

  /// Descriptive metadata only.
  DomainKind kind = DomainKind::Other;
  std::string label;

  /// Immediate upstream domain. Absent for a root domain.
  std::optional<DomainId> parent;

  /// Nameplate capacity declared for this domain.
  Power nominal_capacity{};
  /// Capacity available after fixed conversion losses. Must not exceed the
  /// nameplate value.
  Power usable_capacity{};
  /// Derating applied to the usable capacity, from policy or evidence.
  Ratio derate_ratio = Ratio::full();
  /// Additional reduction that applies while the domain is degraded. Must be
  /// `Ratio::full()` unless `state == OperationalState::Degraded`, and must be
  /// below `Ratio::full()` when the domain is degraded.
  Ratio degradation_ratio = Ratio::full();

  ReservePolicy reserve;

  OperationalState state = OperationalState::Available;
  StateCause state_cause = StateCause::None;

  /// Evidence that justifies the capacity and state declarations above.
  std::optional<EvidenceRef> evidence;

  /// Which source generations this record was derived from, for provenance.
  std::map<SourceId, Generation> source_generations;
};

/// Validates a domain declaration against the configured bounds.
Status validate_domain(const PowerDomain& domain, const ResourceLimits& limits);

}  // namespace power_capacity
