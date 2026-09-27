#include "power_capacity/load.hpp"

#include <string>

#include "detail/validate.hpp"

namespace power_capacity {

const char* to_string(LoadClass load_class) noexcept {
  switch (load_class) {
    case LoadClass::Committed:
      return "committed";
    case LoadClass::Protected:
      return "protected";
  }
  return "unknown";
}

Result<LoadClass> parse_load_class(std::string_view text) {
  if (text == "committed") {
    return LoadClass::Committed;
  }
  if (text == "protected") {
    return LoadClass::Protected;
  }
  return Status::error(StatusCode::InvalidArgument, "unknown load class: '" + std::string(text) + "'");
}

Status validate_load(const LoadRecord& load, const ResourceLimits& limits) {
  Status status = validate_identifier(load.id.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "load id: " + status.message());
  }
  status = validate_identifier(load.domain.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "load attachment domain id: " + status.message());
  }
  status = validate_identifier(load.authority.value(), limits.max_identifier_bytes);
  if (!status.ok()) {
    return Status::error(
        status.code(),
        "load '" + load.id.value() +
            "' must cite the authority that granted the commitment: " + status.message());
  }
  status = validate_label(load.label, limits.max_label_bytes);
  if (!status.ok()) {
    return Status::error(status.code(), "load label: " + status.message());
  }
  if (load.load.is_negative() || load.load.is_zero()) {
    return Status::error(StatusCode::InvalidArgument,
                         "load '" + load.id.value() + "' must declare a strictly positive load");
  }
  if (load.load.milliwatts() > limits.max_component_milliwatts) {
    return Status::error(StatusCode::LimitExceeded,
                         "load '" + load.id.value() + "' declares " +
                             std::to_string(load.load.milliwatts()) +
                             " mW above the per-component limit");
  }
  return detail::validate_evidence_ref(load.evidence, limits, "load");
}

}  // namespace power_capacity
