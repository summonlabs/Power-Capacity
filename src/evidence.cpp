#include "power_capacity/evidence.hpp"

namespace power_capacity {

const char* to_string(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Missing:
      return "missing";
    case EvidenceState::NotYetValid:
      return "not_yet_valid";
    case EvidenceState::Current:
      return "current";
    case EvidenceState::Stale:
      return "stale";
  }
  return "unknown";
}

EvidenceState evaluate_evidence(const std::optional<EvidenceRef>& ref, Tick as_of) noexcept {
  if (!ref.has_value()) {
    return EvidenceState::Missing;
  }
  if (ref->observed_at.value() > as_of.value()) {
    return EvidenceState::NotYetValid;
  }
  if (as_of.value() >= ref->valid_until.value()) {
    return EvidenceState::Stale;
  }
  return EvidenceState::Current;
}

bool evidence_is_current(const std::optional<EvidenceRef>& ref, Tick as_of) noexcept {
  return evaluate_evidence(ref, as_of) == EvidenceState::Current;
}

}  // namespace power_capacity
