#include "detail/serialization.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "detail/codec.hpp"
#include "detail/crc32c.hpp"

namespace power_capacity::detail {
namespace {

constexpr std::array<std::byte, 8> kHeaderMagic = {
    std::byte{'P'}, std::byte{'W'}, std::byte{'C'}, std::byte{'A'},
    std::byte{'P'}, std::byte{'S'}, std::byte{'T'}, std::byte{0}};
constexpr std::array<std::byte, 8> kFooterMagic = {
    std::byte{'P'}, std::byte{'W'}, std::byte{'C'}, std::byte{'A'},
    std::byte{'P'}, std::byte{'E'}, std::byte{'N'}, std::byte{'D'}};
constexpr std::uint32_t kEndianMarker = 0x01020304u;
constexpr std::uint32_t kHeaderCrcOffset = 36;

enum class Section : std::uint32_t {
  Meta = 1,
  Domains = 2,
  Loads = 3,
  Groups = 4,
  Sources = 5,
  AppliedOperations = 6,
};

Status encode_evidence(ByteWriter& writer, const std::optional<EvidenceRef>& evidence,
                       const ResourceLimits& limits) {
  if (!evidence.has_value()) {
    writer.u8(0);
    return Status::success();
  }
  writer.u8(1);
  Status status = writer.text(evidence->id.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
  if (!status.ok()) {
    return status;
  }
  status = writer.text(evidence->source.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
  if (!status.ok()) {
    return status;
  }
  writer.i64(evidence->observed_at.value());
  writer.i64(evidence->valid_until.value());
  writer.u64(evidence->revision.value());
  writer.u64(evidence->source_generation.value());
  return Status::success();
}

Result<EvidenceRef> decode_evidence(ByteReader& reader, const ResourceLimits& limits) {
  const Result<std::string> id = reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
  if (!id.ok()) {
    return id.status();
  }
  const Result<std::string> source =
      reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
  if (!source.ok()) {
    return source.status();
  }
  const Result<std::int64_t> observed = reader.i64();
  if (!observed.ok()) {
    return observed.status();
  }
  const Result<std::int64_t> valid_until = reader.i64();
  if (!valid_until.ok()) {
    return valid_until.status();
  }
  const Result<std::uint64_t> revision = reader.u64();
  if (!revision.ok()) {
    return revision.status();
  }
  const Result<std::uint64_t> source_generation = reader.u64();
  if (!source_generation.ok()) {
    return source_generation.status();
  }
  const Result<EvidenceId> parsed_id =
      EvidenceId::parse(id.value(), limits.max_identifier_bytes);
  if (!parsed_id.ok()) {
    return parsed_id.status();
  }
  const Result<SourceId> parsed_source =
      SourceId::parse(source.value(), limits.max_identifier_bytes);
  if (!parsed_source.ok()) {
    return parsed_source.status();
  }
  EvidenceRef evidence;
  evidence.id = parsed_id.value();
  evidence.source = parsed_source.value();
  evidence.observed_at = Tick(observed.value());
  evidence.valid_until = Tick(valid_until.value());
  evidence.revision = Revision(revision.value());
  evidence.source_generation = Generation(source_generation.value());
  return evidence;
}

Status encode_source_generations(ByteWriter& writer,
                                 const std::map<SourceId, Generation>& generations,
                                 const ResourceLimits& limits) {
  if (generations.size() > limits.max_sources) {
    return Status::error(StatusCode::LimitExceeded, "too many source generations to encode");
  }
  writer.u32(static_cast<std::uint32_t>(generations.size()));
  for (const auto& entry : generations) {
    const Status status =
        writer.text(entry.first.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    writer.u64(entry.second.value());
  }
  return Status::success();
}

Status decode_source_generations(ByteReader& reader, const ResourceLimits& limits,
                                 std::map<SourceId, Generation>& generations) {
  const Result<std::uint32_t> count = reader.u32();
  if (!count.ok()) {
    return count.status();
  }
  if (count.value() > limits.max_sources) {
    return Status::error(StatusCode::LimitExceeded,
                         "declared source count " + std::to_string(count.value()) +
                             " exceeds the limit of " + std::to_string(limits.max_sources));
  }
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    const Result<std::string> source =
        reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!source.ok()) {
      return source.status();
    }
    const Result<std::uint64_t> generation = reader.u64();
    if (!generation.ok()) {
      return generation.status();
    }
    const Result<SourceId> parsed = SourceId::parse(source.value(), limits.max_identifier_bytes);
    if (!parsed.ok()) {
      return Status::error(StatusCode::Corruption, "invalid source id: " + parsed.status().message());
    }
    if (!generations.emplace(parsed.value(), Generation(generation.value())).second) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "source '" + source.value() + "' appears more than once");
    }
  }
  return Status::success();
}

Status encode_meta(ByteWriter& writer, const CapacityContent& content, const ResourceLimits& limits) {
  Status status = writer.text(content.store_identity.to_hex(), 32);
  if (!status.ok()) {
    return status;
  }
  status = writer.text(content.created_path, static_cast<std::uint32_t>(limits.max_path_bytes));
  if (!status.ok()) {
    return status;
  }
  writer.u64(content.generation.value());
  writer.u64(content.epoch.value());
  writer.u64(content.incarnation.value());
  writer.i64(content.created_at.value());
  writer.i64(content.updated_at.value());
  writer.i64(content.last_revalidated_at.value());
  return Status::success();
}

Status encode_domains(ByteWriter& writer, const CapacityContent& content,
                      const ResourceLimits& limits) {
  writer.u32(static_cast<std::uint32_t>(content.domains.size()));
  for (const auto& entry : content.domains) {
    const PowerDomain& domain = entry.second;
    Status status =
        writer.text(domain.id.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    writer.u8(static_cast<std::uint8_t>(domain.kind));
    status = writer.text(domain.label, static_cast<std::uint32_t>(limits.max_label_bytes));
    if (!status.ok()) {
      return status;
    }
    if (domain.parent.has_value()) {
      writer.u8(1);
      status =
          writer.text(domain.parent->value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
      if (!status.ok()) {
        return status;
      }
    } else {
      writer.u8(0);
    }
    writer.i64(domain.nominal_capacity.milliwatts());
    writer.i64(domain.usable_capacity.milliwatts());
    writer.u32(static_cast<std::uint32_t>(domain.derate_ratio.basis_points()));
    writer.u32(static_cast<std::uint32_t>(domain.degradation_ratio.basis_points()));
    writer.u8(static_cast<std::uint8_t>(domain.reserve.mode));
    writer.i64(domain.reserve.absolute.milliwatts());
    writer.u32(static_cast<std::uint32_t>(domain.reserve.ratio.basis_points()));
    writer.u8(static_cast<std::uint8_t>(domain.state));
    writer.u8(static_cast<std::uint8_t>(domain.state_cause));
    status = encode_evidence(writer, domain.evidence, limits);
    if (!status.ok()) {
      return status;
    }
    status = encode_source_generations(writer, domain.source_generations, limits);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

Status encode_loads(ByteWriter& writer, const CapacityContent& content, const ResourceLimits& limits) {
  writer.u32(static_cast<std::uint32_t>(content.loads.size()));
  for (const auto& entry : content.loads) {
    const LoadRecord& load = entry.second;
    Status status =
        writer.text(load.id.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    status = writer.text(load.domain.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    writer.u8(static_cast<std::uint8_t>(load.load_class));
    writer.i64(load.load.milliwatts());
    status = writer.text(load.authority.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    status = encode_evidence(writer, load.evidence, limits);
    if (!status.ok()) {
      return status;
    }
    status = writer.text(load.label, static_cast<std::uint32_t>(limits.max_label_bytes));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

Status encode_groups(ByteWriter& writer, const CapacityContent& content, const ResourceLimits& limits) {
  writer.u32(static_cast<std::uint32_t>(content.groups.size()));
  for (const auto& entry : content.groups) {
    const RedundancyGroup& group = entry.second;
    Status status =
        writer.text(group.id.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    writer.u8(static_cast<std::uint8_t>(group.declared_class));
    writer.u32(static_cast<std::uint32_t>(group.members.size()));
    for (const DomainId& member : group.members) {
      status = writer.text(member.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
      if (!status.ok()) {
        return status;
      }
    }
    writer.u32(group.required_simultaneous_failures);
    status = encode_evidence(writer, group.evidence, limits);
    if (!status.ok()) {
      return status;
    }
    status = writer.text(group.policy_authority.value(),
                         static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    status = encode_source_generations(writer, group.source_generations, limits);
    if (!status.ok()) {
      return status;
    }
    status = writer.text(group.label, static_cast<std::uint32_t>(limits.max_label_bytes));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

Status encode_sources(ByteWriter& writer, const CapacityContent& content,
                      const ResourceLimits& limits) {
  if (content.source_generations.size() > limits.max_sources) {
    return Status::error(StatusCode::LimitExceeded, "too many sources to encode");
  }
  writer.u32(static_cast<std::uint32_t>(content.source_generations.size()));
  for (const auto& entry : content.source_generations) {
    const Status status =
        writer.text(entry.first.value(), static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!status.ok()) {
      return status;
    }
    writer.u64(entry.second.value());
  }
  return Status::success();
}

Status encode_applied(ByteWriter& writer, const CapacityContent& content,
                      const ResourceLimits& limits) {
  if (content.applied_operations.size() > limits.max_applied_operations) {
    return Status::error(StatusCode::LimitExceeded, "too many applied operations to encode");
  }
  writer.u32(static_cast<std::uint32_t>(content.applied_operations.size()));
  for (const AppliedOperation& operation : content.applied_operations) {
    const Status status = writer.text(operation.key.value(),
                                      static_cast<std::uint32_t>(limits.max_idempotency_key_bytes));
    if (!status.ok()) {
      return status;
    }
    writer.u64(operation.generation.value());
  }
  return Status::success();
}

void write_section(ByteWriter& payload, Section section, const std::vector<std::byte>& body) {
  payload.u32(static_cast<std::uint32_t>(section));
  payload.u64(static_cast<std::uint64_t>(body.size()));
  payload.raw(body);
}

Status expect_section(ByteReader& reader, Section expected) {
  const Result<std::uint32_t> tag = reader.u32();
  if (!tag.ok()) {
    return tag.status();
  }
  if (tag.value() != static_cast<std::uint32_t>(expected)) {
    return Status::error(StatusCode::Corruption,
                         "artifact section order is wrong: expected section " +
                             std::to_string(static_cast<std::uint32_t>(expected)) + ", found " +
                             std::to_string(tag.value()));
  }
  return Status::success();
}

Result<std::span<const std::byte>> read_section(ByteReader& reader, const ResourceLimits& limits) {
  const Result<std::uint64_t> length = reader.u64();
  if (!length.ok()) {
    return length.status();
  }
  if (length.value() > limits.max_payload_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact section declares " + std::to_string(length.value()) +
                             " bytes, above the payload limit of " +
                             std::to_string(limits.max_payload_bytes));
  }
  if (length.value() > reader.remaining()) {
    return Status::error(StatusCode::Corruption,
                         "artifact section declares " + std::to_string(length.value()) +
                             " bytes but only " + std::to_string(reader.remaining()) + " remain");
  }
  return reader.raw(static_cast<std::size_t>(length.value()));
}

Result<CapacityContent> decode_meta(ByteReader& reader, const ResourceLimits& limits) {
  CapacityContent content;
  const Result<std::string> identity = reader.text(32);
  if (!identity.ok()) {
    return identity.status();
  }
  const Result<StoreIdentity> parsed_identity = StoreIdentity::parse(identity.value());
  if (!parsed_identity.ok()) {
    return Status::error(StatusCode::Corruption,
                         "artifact store identity is invalid: " + parsed_identity.status().message());
  }
  content.store_identity = parsed_identity.value();

  const Result<std::string> path = reader.text(static_cast<std::uint32_t>(limits.max_path_bytes));
  if (!path.ok()) {
    return path.status();
  }
  content.created_path = path.value();

  const Result<std::uint64_t> generation = reader.u64();
  if (!generation.ok()) {
    return generation.status();
  }
  const Result<std::uint64_t> epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.status();
  }
  const Result<std::uint64_t> incarnation = reader.u64();
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  const Result<std::int64_t> created_at = reader.i64();
  if (!created_at.ok()) {
    return created_at.status();
  }
  const Result<std::int64_t> updated_at = reader.i64();
  if (!updated_at.ok()) {
    return updated_at.status();
  }
  const Result<std::int64_t> last_revalidated_at = reader.i64();
  if (!last_revalidated_at.ok()) {
    return last_revalidated_at.status();
  }
  content.generation = Generation(generation.value());
  content.epoch = Epoch(epoch.value());
  content.incarnation = Incarnation(incarnation.value());
  content.created_at = Tick(created_at.value());
  content.updated_at = Tick(updated_at.value());
  content.last_revalidated_at = Tick(last_revalidated_at.value());
  return content;
}

Result<DomainKind> decode_domain_kind(ByteReader& reader) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > static_cast<std::uint8_t>(DomainKind::Other)) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares unknown domain kind " + std::to_string(raw.value()));
  }
  return static_cast<DomainKind>(raw.value());
}

Result<OperationalState> decode_operational_state(ByteReader& reader) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > static_cast<std::uint8_t>(OperationalState::Unavailable)) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares unknown operational state " +
                             std::to_string(raw.value()));
  }
  return static_cast<OperationalState>(raw.value());
}

Result<StateCause> decode_state_cause(ByteReader& reader) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > static_cast<std::uint8_t>(StateCause::Decommissioned)) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares unknown state cause " + std::to_string(raw.value()));
  }
  return static_cast<StateCause>(raw.value());
}

Result<ReservePolicy::Mode> decode_reserve_mode(ByteReader& reader) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > static_cast<std::uint8_t>(ReservePolicy::Mode::GreaterOfAbsoluteAndRatio)) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares unknown reserve mode " + std::to_string(raw.value()));
  }
  return static_cast<ReservePolicy::Mode>(raw.value());
}

Result<LoadClass> decode_load_class(ByteReader& reader) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > static_cast<std::uint8_t>(LoadClass::Protected)) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares unknown load class " + std::to_string(raw.value()));
  }
  return static_cast<LoadClass>(raw.value());
}

Result<RedundancyClass> decode_redundancy_class(ByteReader& reader) {
  const Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.status();
  }
  if (raw.value() > static_cast<std::uint8_t>(RedundancyClass::Other)) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares unknown redundancy class " +
                             std::to_string(raw.value()));
  }
  return static_cast<RedundancyClass>(raw.value());
}

Result<Ratio> decode_ratio(std::uint32_t basis_points) {
  if (basis_points > static_cast<std::uint32_t>(Ratio::scale)) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares ratio " + std::to_string(basis_points) +
                             " basis points, above 10000");
  }
  return Ratio::from_basis_points(static_cast<std::int32_t>(basis_points));
}

Status decode_domains(ByteReader& reader, const ResourceLimits& limits, CapacityContent& content) {
  const Result<std::uint32_t> count = reader.u32();
  if (!count.ok()) {
    return count.status();
  }
  if (count.value() > limits.max_domains) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact declares " + std::to_string(count.value()) +
                             " domains, above the limit of " + std::to_string(limits.max_domains));
  }
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    const Result<std::string> id = reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!id.ok()) {
      return id.status();
    }
    const Result<DomainId> parsed_id = DomainId::parse(id.value(), limits.max_identifier_bytes);
    if (!parsed_id.ok()) {
      return Status::error(StatusCode::Corruption, "invalid domain id: " + parsed_id.status().message());
    }
    PowerDomain domain;
    domain.id = parsed_id.value();

    const Result<DomainKind> kind = decode_domain_kind(reader);
    if (!kind.ok()) {
      return kind.status();
    }
    domain.kind = kind.value();

    const Result<std::string> label = reader.text(static_cast<std::uint32_t>(limits.max_label_bytes));
    if (!label.ok()) {
      return label.status();
    }
    domain.label = label.value();

    const Result<std::uint8_t> has_parent = reader.u8();
    if (!has_parent.ok()) {
      return has_parent.status();
    }
    if (has_parent.value() > 1) {
      return Status::error(StatusCode::Corruption, "artifact declares an invalid parent flag");
    }
    if (has_parent.value() == 1) {
      const Result<std::string> parent =
          reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
      if (!parent.ok()) {
        return parent.status();
      }
      const Result<DomainId> parsed_parent =
          DomainId::parse(parent.value(), limits.max_identifier_bytes);
      if (!parsed_parent.ok()) {
        return Status::error(StatusCode::Corruption,
                             "invalid parent domain id: " + parsed_parent.status().message());
      }
      domain.parent = parsed_parent.value();
    }

    const Result<std::int64_t> nominal = reader.i64();
    if (!nominal.ok()) {
      return nominal.status();
    }
    const Result<std::int64_t> usable = reader.i64();
    if (!usable.ok()) {
      return usable.status();
    }
    domain.nominal_capacity = Power(nominal.value());
    domain.usable_capacity = Power(usable.value());

    const Result<std::uint32_t> derate = reader.u32();
    if (!derate.ok()) {
      return derate.status();
    }
    const Result<Ratio> derate_ratio = decode_ratio(derate.value());
    if (!derate_ratio.ok()) {
      return derate_ratio.status();
    }
    domain.derate_ratio = derate_ratio.value();

    const Result<std::uint32_t> degradation = reader.u32();
    if (!degradation.ok()) {
      return degradation.status();
    }
    const Result<Ratio> degradation_ratio = decode_ratio(degradation.value());
    if (!degradation_ratio.ok()) {
      return degradation_ratio.status();
    }
    domain.degradation_ratio = degradation_ratio.value();

    const Result<ReservePolicy::Mode> mode = decode_reserve_mode(reader);
    if (!mode.ok()) {
      return mode.status();
    }
    domain.reserve.mode = mode.value();

    const Result<std::int64_t> reserve_absolute = reader.i64();
    if (!reserve_absolute.ok()) {
      return reserve_absolute.status();
    }
    domain.reserve.absolute = Power(reserve_absolute.value());

    const Result<std::uint32_t> reserve_ratio = reader.u32();
    if (!reserve_ratio.ok()) {
      return reserve_ratio.status();
    }
    const Result<Ratio> reserve_ratio_value = decode_ratio(reserve_ratio.value());
    if (!reserve_ratio_value.ok()) {
      return reserve_ratio_value.status();
    }
    domain.reserve.ratio = reserve_ratio_value.value();

    const Result<OperationalState> state = decode_operational_state(reader);
    if (!state.ok()) {
      return state.status();
    }
    domain.state = state.value();

    const Result<StateCause> cause = decode_state_cause(reader);
    if (!cause.ok()) {
      return cause.status();
    }
    domain.state_cause = cause.value();

    const Result<std::uint8_t> has_evidence = reader.u8();
    if (!has_evidence.ok()) {
      return has_evidence.status();
    }
    if (has_evidence.value() > 1) {
      return Status::error(StatusCode::Corruption, "artifact declares an invalid evidence flag");
    }
    if (has_evidence.value() == 1) {
      const Result<EvidenceRef> evidence = decode_evidence(reader, limits);
      if (!evidence.ok()) {
        return evidence.status();
      }
      domain.evidence = evidence.value();
    }

    const Status sources = decode_source_generations(reader, limits, domain.source_generations);
    if (!sources.ok()) {
      return sources;
    }

    if (!content.domains.emplace(domain.id, std::move(domain)).second) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "artifact lists domain '" + id.value() + "' more than once");
    }
  }
  return Status::success();
}

Status decode_loads(ByteReader& reader, const ResourceLimits& limits, CapacityContent& content) {
  const Result<std::uint32_t> count = reader.u32();
  if (!count.ok()) {
    return count.status();
  }
  if (count.value() > limits.max_loads) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact declares " + std::to_string(count.value()) +
                             " loads, above the limit of " + std::to_string(limits.max_loads));
  }
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    const Result<std::string> id = reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!id.ok()) {
      return id.status();
    }
    const Result<LoadId> parsed_id = LoadId::parse(id.value(), limits.max_identifier_bytes);
    if (!parsed_id.ok()) {
      return Status::error(StatusCode::Corruption, "invalid load id: " + parsed_id.status().message());
    }
    LoadRecord load;
    load.id = parsed_id.value();

    const Result<std::string> domain = reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!domain.ok()) {
      return domain.status();
    }
    const Result<DomainId> parsed_domain = DomainId::parse(domain.value(), limits.max_identifier_bytes);
    if (!parsed_domain.ok()) {
      return Status::error(StatusCode::Corruption,
                           "invalid load attachment domain id: " + parsed_domain.status().message());
    }
    load.domain = parsed_domain.value();

    const Result<LoadClass> load_class = decode_load_class(reader);
    if (!load_class.ok()) {
      return load_class.status();
    }
    load.load_class = load_class.value();

    const Result<std::int64_t> watts = reader.i64();
    if (!watts.ok()) {
      return watts.status();
    }
    load.load = Power(watts.value());

    const Result<std::string> authority =
        reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!authority.ok()) {
      return authority.status();
    }
    const Result<AuthorityRef> parsed_authority =
        AuthorityRef::parse(authority.value(), limits.max_identifier_bytes);
    if (!parsed_authority.ok()) {
      return Status::error(StatusCode::Corruption,
                           "invalid load authority reference: " + parsed_authority.status().message());
    }
    load.authority = parsed_authority.value();

    const Result<std::uint8_t> has_evidence = reader.u8();
    if (!has_evidence.ok()) {
      return has_evidence.status();
    }
    if (has_evidence.value() > 1) {
      return Status::error(StatusCode::Corruption, "artifact declares an invalid evidence flag");
    }
    if (has_evidence.value() == 1) {
      const Result<EvidenceRef> evidence = decode_evidence(reader, limits);
      if (!evidence.ok()) {
        return evidence.status();
      }
      load.evidence = evidence.value();
    }

    const Result<std::string> label = reader.text(static_cast<std::uint32_t>(limits.max_label_bytes));
    if (!label.ok()) {
      return label.status();
    }
    load.label = label.value();

    if (!content.loads.emplace(load.id, std::move(load)).second) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "artifact lists load '" + id.value() + "' more than once");
    }
  }
  return Status::success();
}

Status decode_groups(ByteReader& reader, const ResourceLimits& limits, CapacityContent& content) {
  const Result<std::uint32_t> count = reader.u32();
  if (!count.ok()) {
    return count.status();
  }
  if (count.value() > limits.max_groups) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact declares " + std::to_string(count.value()) +
                             " redundancy groups, above the limit of " +
                             std::to_string(limits.max_groups));
  }
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    const Result<std::string> id = reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!id.ok()) {
      return id.status();
    }
    const Result<GroupId> parsed_id = GroupId::parse(id.value(), limits.max_identifier_bytes);
    if (!parsed_id.ok()) {
      return Status::error(StatusCode::Corruption,
                           "invalid redundancy group id: " + parsed_id.status().message());
    }
    RedundancyGroup group;
    group.id = parsed_id.value();

    const Result<RedundancyClass> declared_class = decode_redundancy_class(reader);
    if (!declared_class.ok()) {
      return declared_class.status();
    }
    group.declared_class = declared_class.value();

    const Result<std::uint32_t> member_count = reader.u32();
    if (!member_count.ok()) {
      return member_count.status();
    }
    if (member_count.value() > limits.max_members_per_group) {
      return Status::error(StatusCode::LimitExceeded,
                           "artifact declares " + std::to_string(member_count.value()) +
                               " members for one group, above the limit of " +
                               std::to_string(limits.max_members_per_group));
    }
    group.members.reserve(member_count.value());
    for (std::uint32_t member_index = 0; member_index < member_count.value(); ++member_index) {
      const Result<std::string> member =
          reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
      if (!member.ok()) {
        return member.status();
      }
      const Result<DomainId> parsed_member =
          DomainId::parse(member.value(), limits.max_identifier_bytes);
      if (!parsed_member.ok()) {
        return Status::error(StatusCode::Corruption,
                             "invalid redundancy group member id: " +
                                 parsed_member.status().message());
      }
      group.members.push_back(parsed_member.value());
    }

    const Result<std::uint32_t> tolerance = reader.u32();
    if (!tolerance.ok()) {
      return tolerance.status();
    }
    group.required_simultaneous_failures = tolerance.value();

    const Result<std::uint8_t> has_evidence = reader.u8();
    if (!has_evidence.ok()) {
      return has_evidence.status();
    }
    if (has_evidence.value() > 1) {
      return Status::error(StatusCode::Corruption, "artifact declares an invalid evidence flag");
    }
    if (has_evidence.value() == 1) {
      const Result<EvidenceRef> evidence = decode_evidence(reader, limits);
      if (!evidence.ok()) {
        return evidence.status();
      }
      group.evidence = evidence.value();
    }

    const Result<std::string> authority =
        reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!authority.ok()) {
      return authority.status();
    }
    const Result<AuthorityRef> parsed_authority =
        AuthorityRef::parse(authority.value(), limits.max_identifier_bytes);
    if (!parsed_authority.ok()) {
      return Status::error(StatusCode::Corruption, "invalid group policy authority reference: " +
                                                       parsed_authority.status().message());
    }
    group.policy_authority = parsed_authority.value();

    const Status sources = decode_source_generations(reader, limits, group.source_generations);
    if (!sources.ok()) {
      return sources;
    }

    const Result<std::string> label = reader.text(static_cast<std::uint32_t>(limits.max_label_bytes));
    if (!label.ok()) {
      return label.status();
    }
    group.label = label.value();

    if (!content.groups.emplace(group.id, std::move(group)).second) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "artifact lists redundancy group '" + id.value() + "' more than once");
    }
  }
  return Status::success();
}

Status decode_sources(ByteReader& reader, const ResourceLimits& limits, CapacityContent& content) {
  const Result<std::uint32_t> count = reader.u32();
  if (!count.ok()) {
    return count.status();
  }
  if (count.value() > limits.max_sources) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact declares " + std::to_string(count.value()) +
                             " sources, above the limit of " + std::to_string(limits.max_sources));
  }
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    const Result<std::string> source =
        reader.text(static_cast<std::uint32_t>(limits.max_identifier_bytes));
    if (!source.ok()) {
      return source.status();
    }
    const Result<std::uint64_t> generation = reader.u64();
    if (!generation.ok()) {
      return generation.status();
    }
    const Result<SourceId> parsed = SourceId::parse(source.value(), limits.max_identifier_bytes);
    if (!parsed.ok()) {
      return Status::error(StatusCode::Corruption, "invalid source id: " + parsed.status().message());
    }
    if (!content.source_generations.emplace(parsed.value(), Generation(generation.value())).second) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "artifact lists source '" + source.value() + "' more than once");
    }
  }
  return Status::success();
}

Status decode_applied(ByteReader& reader, const ResourceLimits& limits, CapacityContent& content) {
  const Result<std::uint32_t> count = reader.u32();
  if (!count.ok()) {
    return count.status();
  }
  if (count.value() > limits.max_applied_operations) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact declares " + std::to_string(count.value()) +
                             " applied operations, above the limit of " +
                             std::to_string(limits.max_applied_operations));
  }
  content.applied_operations.reserve(count.value());
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    const Result<std::string> key =
        reader.text(static_cast<std::uint32_t>(limits.max_idempotency_key_bytes));
    if (!key.ok()) {
      return key.status();
    }
    const Result<std::uint64_t> generation = reader.u64();
    if (!generation.ok()) {
      return generation.status();
    }
    const Result<IdempotencyKey> parsed =
        IdempotencyKey::parse(key.value(), limits.max_idempotency_key_bytes);
    if (!parsed.ok()) {
      return Status::error(StatusCode::Corruption,
                           "invalid idempotency key: " + parsed.status().message());
    }
    AppliedOperation operation;
    operation.key = parsed.value();
    operation.generation = Generation(generation.value());
    content.applied_operations.push_back(std::move(operation));
  }
  return Status::success();
}

}  // namespace

Result<EncodedArtifact> encode_artifact(const CapacityContent& content,
                                        const ResourceLimits& limits) {
  ByteWriter payload;
  {
    ByteWriter body;
    const Status status = encode_meta(body, content, limits);
    if (!status.ok()) {
      return status;
    }
    write_section(payload, Section::Meta, body.buffer());
  }
  {
    ByteWriter body;
    const Status status = encode_domains(body, content, limits);
    if (!status.ok()) {
      return status;
    }
    write_section(payload, Section::Domains, body.buffer());
  }
  {
    ByteWriter body;
    const Status status = encode_loads(body, content, limits);
    if (!status.ok()) {
      return status;
    }
    write_section(payload, Section::Loads, body.buffer());
  }
  {
    ByteWriter body;
    const Status status = encode_groups(body, content, limits);
    if (!status.ok()) {
      return status;
    }
    write_section(payload, Section::Groups, body.buffer());
  }
  {
    ByteWriter body;
    const Status status = encode_sources(body, content, limits);
    if (!status.ok()) {
      return status;
    }
    write_section(payload, Section::Sources, body.buffer());
  }
  {
    ByteWriter body;
    const Status status = encode_applied(body, content, limits);
    if (!status.ok()) {
      return status;
    }
    write_section(payload, Section::AppliedOperations, body.buffer());
  }

  const std::span<const std::byte> payload_bytes(payload.buffer().data(), payload.buffer().size());
  const std::uint32_t payload_crc = crc32c(payload_bytes);

  ByteWriter header;
  header.raw(kHeaderMagic);
  header.u32(kArtifactFormatVersion);
  header.u32(static_cast<std::uint32_t>(kArtifactHeaderBytes));
  header.u32(kEndianMarker);
  header.u32(0);
  header.u64(static_cast<std::uint64_t>(payload_bytes.size()));
  header.u32(payload_crc);
  // The header CRC covers everything before it, including the payload CRC.
  const std::span<const std::byte> header_prefix(header.buffer().data(), kHeaderCrcOffset);
  const std::uint32_t header_crc = crc32c(header_prefix);
  ByteWriter header_final;
  header_final.raw(header_prefix);
  header_final.u32(header_crc);
  header_final.u64(content.generation.value());
  header_final.u64(content.epoch.value());
  header_final.u64(content.incarnation.value());
  header_final.u64(0);

  ByteWriter footer;
  footer.raw(kFooterMagic);
  footer.u32(payload_crc);
  const std::span<const std::byte> footer_prefix(footer.buffer().data(), 12);
  footer.u32(crc32c(footer_prefix));

  EncodedArtifact encoded;
  encoded.bytes.reserve(header_final.size() + payload_bytes.size() + kArtifactFooterBytes);
  encoded.bytes.insert(encoded.bytes.end(), header_final.buffer().begin(),
                       header_final.buffer().end());
  encoded.bytes.insert(encoded.bytes.end(), payload_bytes.begin(), payload_bytes.end());
  encoded.bytes.insert(encoded.bytes.end(), footer.buffer().begin(), footer.buffer().end());
  encoded.payload_crc32c = payload_crc;
  return encoded;
}

Result<DecodedArtifact> decode_artifact(std::span<const std::byte> artifact,
                                        const ResourceLimits& limits) {
  if (artifact.size() < kArtifactHeaderBytes + kArtifactFooterBytes) {
    return Status::error(StatusCode::Corruption,
                         "artifact is " + std::to_string(artifact.size()) +
                             " bytes, shorter than the " +
                             std::to_string(kArtifactHeaderBytes + kArtifactFooterBytes) +
                             "-byte envelope");
  }
  if (artifact.size() > limits.max_store_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact is " + std::to_string(artifact.size()) +
                             " bytes, above the configured limit of " +
                             std::to_string(limits.max_store_bytes));
  }

  const std::span<const std::byte> header = artifact.first(kArtifactHeaderBytes);
  if (!std::equal(kHeaderMagic.begin(), kHeaderMagic.end(), header.begin())) {
    return Status::error(StatusCode::Corruption, "artifact magic does not match the store format");
  }
  ByteReader header_reader(header.subspan(kHeaderMagic.size()));
  const Result<std::uint32_t> version = header_reader.u32();
  const Result<std::uint32_t> header_bytes = header_reader.u32();
  const Result<std::uint32_t> endian_marker = header_reader.u32();
  const Result<std::uint32_t> flags = header_reader.u32();
  const Result<std::uint64_t> payload_bytes = header_reader.u64();
  const Result<std::uint32_t> payload_crc = header_reader.u32();
  const Result<std::uint32_t> header_crc = header_reader.u32();
  const Result<std::uint64_t> header_generation = header_reader.u64();
  const Result<std::uint64_t> header_epoch = header_reader.u64();
  const Result<std::uint64_t> header_incarnation = header_reader.u64();
  const Result<std::uint64_t> reserved = header_reader.u64();
  for (const Status* status : {&version.status(), &header_bytes.status(), &endian_marker.status(),
                               &flags.status(), &payload_bytes.status(), &payload_crc.status(),
                               &header_crc.status(), &header_generation.status(),
                               &header_epoch.status(), &header_incarnation.status(),
                               &reserved.status()}) {
    if (!status->ok()) {
      return *status;
    }
  }
  if (endian_marker.value() != kEndianMarker) {
    if (endian_marker.value() == 0x04030201u) {
      return Status::error(StatusCode::EndianMismatch,
                           "artifact was written in the opposite byte order");
    }
    return Status::error(StatusCode::Corruption, "artifact endian marker is invalid");
  }
  if (version.value() != kArtifactFormatVersion) {
    return Status::error(StatusCode::IncompatibleVersion,
                         "artifact format version " + std::to_string(version.value()) +
                             " is not supported; this build writes and reads version " +
                             std::to_string(kArtifactFormatVersion));
  }
  if (header_bytes.value() != kArtifactHeaderBytes) {
    return Status::error(StatusCode::Corruption,
                         "artifact header size " + std::to_string(header_bytes.value()) +
                             " does not match the expected " +
                             std::to_string(kArtifactHeaderBytes));
  }
  if (flags.value() != 0) {
    return Status::error(StatusCode::IncompatibleVersion,
                         "artifact declares unsupported flags 0x" + std::to_string(flags.value()));
  }
  const std::span<const std::byte> header_prefix = header.first(kHeaderCrcOffset);
  if (crc32c(header_prefix) != header_crc.value()) {
    return Status::error(StatusCode::Corruption, "artifact header checksum mismatch");
  }
  if (reserved.value() != 0) {
    return Status::error(StatusCode::Corruption, "artifact reserved header field is not zero");
  }

  const std::uint64_t expected_payload =
      static_cast<std::uint64_t>(artifact.size() - kArtifactHeaderBytes - kArtifactFooterBytes);
  if (payload_bytes.value() != expected_payload) {
    return Status::error(StatusCode::Corruption,
                         "artifact declares " + std::to_string(payload_bytes.value()) +
                             " payload bytes but the envelope holds " +
                             std::to_string(expected_payload));
  }
  if (payload_bytes.value() > limits.max_payload_bytes) {
    return Status::error(StatusCode::LimitExceeded,
                         "artifact payload of " + std::to_string(payload_bytes.value()) +
                             " bytes exceeds the configured limit of " +
                             std::to_string(limits.max_payload_bytes));
  }

  const std::span<const std::byte> payload =
      artifact.subspan(kArtifactHeaderBytes, static_cast<std::size_t>(payload_bytes.value()));
  if (crc32c(payload) != payload_crc.value()) {
    return Status::error(StatusCode::Corruption, "artifact payload checksum mismatch");
  }

  const std::span<const std::byte> footer = artifact.last(kArtifactFooterBytes);
  if (!std::equal(kFooterMagic.begin(), kFooterMagic.end(), footer.begin())) {
    return Status::error(StatusCode::Corruption, "artifact footer magic is invalid");
  }
  ByteReader footer_reader(footer.subspan(kFooterMagic.size()));
  const Result<std::uint32_t> footer_payload_crc = footer_reader.u32();
  const Result<std::uint32_t> footer_crc = footer_reader.u32();
  if (!footer_payload_crc.ok() || !footer_crc.ok()) {
    return Status::error(StatusCode::Corruption, "artifact footer is truncated");
  }
  if (footer_payload_crc.value() != payload_crc.value()) {
    return Status::error(StatusCode::Corruption,
                         "artifact footer payload checksum disagrees with the header");
  }
  if (crc32c(footer.first(12)) != footer_crc.value()) {
    return Status::error(StatusCode::Corruption, "artifact footer checksum mismatch");
  }

  ByteReader reader(payload);
  Status status = expect_section(reader, Section::Meta);
  if (!status.ok()) {
    return status;
  }
  const Result<std::span<const std::byte>> meta_section = read_section(reader, limits);
  if (!meta_section.ok()) {
    return meta_section.status();
  }
  ByteReader meta_reader(meta_section.value());
  const Result<CapacityContent> meta = decode_meta(meta_reader, limits);
  if (!meta.ok()) {
    return meta.status();
  }
  if (!meta_reader.at_end()) {
    return Status::error(StatusCode::Corruption, "artifact meta section has trailing bytes");
  }
  CapacityContent content = meta.value();
  if (content.generation.value() != header_generation.value() ||
      content.epoch.value() != header_epoch.value() ||
      content.incarnation.value() != header_incarnation.value()) {
    return Status::error(StatusCode::Corruption,
                         "artifact header generation, epoch, or incarnation disagrees with the "
                         "payload metadata");
  }

  status = expect_section(reader, Section::Domains);
  if (!status.ok()) {
    return status;
  }
  const Result<std::span<const std::byte>> domains_section = read_section(reader, limits);
  if (!domains_section.ok()) {
    return domains_section.status();
  }
  ByteReader domains_reader(domains_section.value());
  status = decode_domains(domains_reader, limits, content);
  if (!status.ok()) {
    return status;
  }
  if (!domains_reader.at_end()) {
    return Status::error(StatusCode::Corruption, "artifact domain section has trailing bytes");
  }

  status = expect_section(reader, Section::Loads);
  if (!status.ok()) {
    return status;
  }
  const Result<std::span<const std::byte>> loads_section = read_section(reader, limits);
  if (!loads_section.ok()) {
    return loads_section.status();
  }
  ByteReader loads_reader(loads_section.value());
  status = decode_loads(loads_reader, limits, content);
  if (!status.ok()) {
    return status;
  }
  if (!loads_reader.at_end()) {
    return Status::error(StatusCode::Corruption, "artifact load section has trailing bytes");
  }

  status = expect_section(reader, Section::Groups);
  if (!status.ok()) {
    return status;
  }
  const Result<std::span<const std::byte>> groups_section = read_section(reader, limits);
  if (!groups_section.ok()) {
    return groups_section.status();
  }
  ByteReader groups_reader(groups_section.value());
  status = decode_groups(groups_reader, limits, content);
  if (!status.ok()) {
    return status;
  }
  if (!groups_reader.at_end()) {
    return Status::error(StatusCode::Corruption, "artifact group section has trailing bytes");
  }

  status = expect_section(reader, Section::Sources);
  if (!status.ok()) {
    return status;
  }
  const Result<std::span<const std::byte>> sources_section = read_section(reader, limits);
  if (!sources_section.ok()) {
    return sources_section.status();
  }
  ByteReader sources_reader(sources_section.value());
  status = decode_sources(sources_reader, limits, content);
  if (!status.ok()) {
    return status;
  }
  if (!sources_reader.at_end()) {
    return Status::error(StatusCode::Corruption, "artifact source section has trailing bytes");
  }

  status = expect_section(reader, Section::AppliedOperations);
  if (!status.ok()) {
    return status;
  }
  const Result<std::span<const std::byte>> applied_section = read_section(reader, limits);
  if (!applied_section.ok()) {
    return applied_section.status();
  }
  ByteReader applied_reader(applied_section.value());
  status = decode_applied(applied_reader, limits, content);
  if (!status.ok()) {
    return status;
  }
  if (!applied_reader.at_end()) {
    return Status::error(StatusCode::Corruption, "artifact applied-operation section has trailing bytes");
  }

  if (!reader.at_end()) {
    return Status::error(StatusCode::Corruption,
                         "artifact payload has " + std::to_string(reader.remaining()) +
                             " trailing bytes after the last section");
  }

  DecodedArtifact decoded;
  decoded.content = std::move(content);
  decoded.format_version = version.value();
  decoded.payload_crc32c = payload_crc.value();
  decoded.file_bytes = static_cast<std::uint64_t>(artifact.size());
  return decoded;
}

}  // namespace power_capacity::detail
