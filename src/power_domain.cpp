#include "power_capacity/power_domain.hpp"

#include <string>

#include "detail/validate.hpp"

namespace power_capacity {
namespace {

Status unknown_token(const char* what, std::string_view text) {
  return Status::error(StatusCode::InvalidArgument,
                       std::string("unknown ") + what + ": '" + std::string(text) + "'");
}

}  // namespace

const char* to_string(DomainKind kind) noexcept {
  switch (kind) {
    case DomainKind::UtilityFeed:
      return "utility_feed";
    case DomainKind::Transformer:
      return "transformer";
    case DomainKind::Switchgear:
      return "switchgear";
    case DomainKind::TransferSwitch:
      return "transfer_switch";
    case DomainKind::Generator:
      return "generator";
    case DomainKind::UpsSystem:
      return "ups_system";
    case DomainKind::UpsModule:
      return "ups_module";
    case DomainKind::Busway:
      return "busway";
    case DomainKind::Pdu:
      return "pdu";
    case DomainKind::RackPdu:
      return "rack_pdu";
    case DomainKind::Other:
      return "other";
  }
  return "other";
}

Result<DomainKind> parse_domain_kind(std::string_view text) {
  if (text == "utility_feed") {
    return DomainKind::UtilityFeed;
  }
  if (text == "transformer") {
    return DomainKind::Transformer;
  }
  if (text == "switchgear") {
    return DomainKind::Switchgear;
  }
  if (text == "transfer_switch") {
    return DomainKind::TransferSwitch;
  }
  if (text == "generator") {
    return DomainKind::Generator;
  }
  if (text == "ups_system") {
    return DomainKind::UpsSystem;
  }
  if (text == "ups_module") {
    return DomainKind::UpsModule;
  }
  if (text == "busway") {
    return DomainKind::Busway;
  }
  if (text == "pdu") {
    return DomainKind::Pdu;
  }
  if (text == "rack_pdu") {
    return DomainKind::RackPdu;
  }
  if (text == "other") {
    return DomainKind::Other;
  }
  return unknown_token("domain kind", text);
}

const char* to_string(OperationalState state) noexcept {
  switch (state) {
    case OperationalState::Available:
      return "available";
    case OperationalState::Degraded:
      return "degraded";
    case OperationalState::Unavailable:
      return "unavailable";
  }
  return "unknown";
}

Result<OperationalState> parse_operational_state(std::string_view text) {
  if (text == "available") {
    return OperationalState::Available;
  }
  if (text == "degraded") {
    return OperationalState::Degraded;
  }
  if (text == "unavailable") {
    return OperationalState::Unavailable;
  }
  return unknown_token("operational state", text);
}

const char* to_string(StateCause cause) noexcept {
  switch (cause) {
    case StateCause::None:
      return "none";
    case StateCause::Maintenance:
      return "maintenance";
    case StateCause::Fault:
      return "fault";
    case StateCause::EvidenceUnknown:
      return "evidence_unknown";
    case StateCause::Decommissioned:
      return "decommissioned";
  }
  return "unknown";
}

Result<StateCause> parse_state_cause(std::string_view text) {
  if (text == "none") {
    return StateCause::None;
  }
  if (text == "maintenance") {
    return StateCause::Maintenance;
  }
  if (text == "fault") {
    return StateCause::Fault;
  }
  if (text == "evidence_unknown") {
    return StateCause::EvidenceUnknown;
  }
  if (text == "decommissioned") {
    return StateCause::Decommissioned;
  }
  return unknown_token("state cause", text);
}

const char* to_string(ReservePolicy::Mode mode) noexcept {
  switch (mode) {
    case ReservePolicy::Mode::None:
      return "none";
    case ReservePolicy::Mode::Absolute:
      return "absolute";
    case ReservePolicy::Mode::Ratio:
      return "ratio";
    case ReservePolicy::Mode::GreaterOfAbsoluteAndRatio:
      return "greater_of_absolute_and_ratio";
  }
  return "unknown";
}

Result<ReservePolicy::Mode> parse_reserve_mode(std::string_view text) {
  if (text == "none") {
    return ReservePolicy::Mode::None;
  }
  if (text == "absolute") {
    return ReservePolicy::Mode::Absolute;
  }
  if (text == "ratio") {
    return ReservePolicy::Mode::Ratio;
  }
  if (text == "greater_of_absolute_and_ratio") {
    return ReservePolicy::Mode::GreaterOfAbsoluteAndRatio;
  }
  return unknown_token("reserve mode", text);
}

Status validate_domain(const PowerDomain& domain, const ResourceLimits& limits) {
  Status status = validate_identifier(domain.id.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "domain id: " + status.message());
  }
  status = validate_label(domain.label, limits.max_label_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "domain label: " + status.message());
  }
  if (domain.parent.has_value()) {
    status = validate_identifier(domain.parent->value(), limits.max_identifier_bytes);
    if (!status.ok()) {
      return Status::error(status.code(), "domain parent id: " + status.message());
    }
    if (*domain.parent == domain.id) {
      return Status::error(StatusCode::InvalidArgument,
                           "domain '" + domain.id.value() + "' declares itself as its own parent");
    }
  }
  if (domain.nominal_capacity.is_negative()) {
    return Status::error(StatusCode::InvalidArgument,
                         "domain '" + domain.id.value() + "' has a negative nominal capacity");
  }
  if (domain.usable_capacity.is_negative()) {
    return Status::error(StatusCode::InvalidArgument,
                         "domain '" + domain.id.value() + "' has a negative usable capacity");
  }
  if (domain.usable_capacity > domain.nominal_capacity) {
    return Status::error(StatusCode::InvalidArgument,
                         "domain '" + domain.id.value() + "' declares usable capacity " +
                             domain.usable_capacity.to_watts_string() +
                             " W above its nominal capacity " +
                             domain.nominal_capacity.to_watts_string() + " W");
  }
  if (domain.nominal_capacity.milliwatts() > limits.max_component_milliwatts ||
      domain.usable_capacity.milliwatts() > limits.max_component_milliwatts) {
    return Status::error(StatusCode::LimitExceeded,
                         "domain '" + domain.id.value() + "' declares a capacity above the " +
                             std::to_string(limits.max_component_milliwatts) +
                             " mW per-component limit");
  }

  switch (domain.state) {
    case OperationalState::Available:
      if (domain.state_cause != StateCause::None) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' is available but declares a state cause");
      }
      if (!domain.degradation_ratio.is_full()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' is available but declares a degradation ratio below 100%");
      }
      break;
    case OperationalState::Degraded:
      if (domain.state_cause != StateCause::Maintenance &&
          domain.state_cause != StateCause::Fault &&
          domain.state_cause != StateCause::EvidenceUnknown) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' is degraded and must declare a maintenance, fault, or "
                                 "evidence_unknown cause");
      }
      if (domain.degradation_ratio.is_full()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' is degraded but declares a degradation ratio of 100%, which "
                                 "would make the degraded state meaningless");
      }
      break;
    case OperationalState::Unavailable:
      if (domain.state_cause == StateCause::None) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' is unavailable and must declare a state cause");
      }
      if (!domain.degradation_ratio.is_full()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' is unavailable; degradation_ratio is ignored and must be "
                                 "100%");
      }
      break;
  }

  switch (domain.reserve.mode) {
    case ReservePolicy::Mode::None:
      if (!domain.reserve.absolute.is_zero() || !domain.reserve.ratio.is_zero()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' declares reserve mode 'none' together with a reserve value");
      }
      break;
    case ReservePolicy::Mode::Absolute:
      if (domain.reserve.absolute.is_negative()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() + "' has a negative absolute reserve");
      }
      if (!domain.reserve.ratio.is_zero()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' declares reserve mode 'absolute' together with a ratio");
      }
      break;
    case ReservePolicy::Mode::Ratio:
      if (domain.reserve.ratio.is_zero()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' declares reserve mode 'ratio' with a zero ratio");
      }
      if (!domain.reserve.absolute.is_zero()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' declares reserve mode 'ratio' together with an absolute value");
      }
      break;
    case ReservePolicy::Mode::GreaterOfAbsoluteAndRatio:
      if (domain.reserve.absolute.is_negative()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() + "' has a negative absolute reserve");
      }
      if (domain.reserve.ratio.is_zero()) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() +
                                 "' declares 'greater_of_absolute_and_ratio' with a zero ratio");
      }
      break;
  }
  if (domain.reserve.absolute.milliwatts() > limits.max_component_milliwatts) {
    return Status::error(StatusCode::LimitExceeded,
                         "domain '" + domain.id.value() +
                             "' declares an absolute reserve above the per-component limit");
  }

  status = detail::validate_evidence_ref(domain.evidence, limits, "domain");
  if (!status.ok()) {
    return status;
  }
  return detail::validate_source_generations(domain.source_generations, limits, "domain");
}

}  // namespace power_capacity
