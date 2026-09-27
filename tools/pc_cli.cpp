// Power Capacity administration and inspection tool.
//
// Every command is deterministic: it reads only the named store, the named
// instant, and the authority binding given on the command line, and it never
// consults a wall clock for an authoritative decision.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cli_json.hpp"
#include "power_capacity/engine.hpp"
#include "power_capacity/store.hpp"
#include "power_capacity/version.hpp"

namespace {

using power_capacity::CapacityEngine;
using power_capacity::CapacityQuery;
using power_capacity::CandidateLoad;
using power_capacity::DomainId;
using power_capacity::EngineLifecycle;
using power_capacity::EngineOptions;
using power_capacity::Epoch;
using power_capacity::EvaluationContext;
using power_capacity::EvidenceId;
using power_capacity::EvidenceRef;
using power_capacity::Generation;
using power_capacity::GroupId;
using power_capacity::IdempotencyKey;
using power_capacity::Incarnation;
using power_capacity::LoadId;
using power_capacity::LoadRecord;
using power_capacity::MutationContext;
using power_capacity::Power;
using power_capacity::PowerDomain;
using power_capacity::Ratio;
using power_capacity::ReasonCode;
using power_capacity::RedundancyGroup;
using power_capacity::ResourceLimits;
using power_capacity::Revision;
using power_capacity::SourceId;
using power_capacity::Status;
using power_capacity::StatusCode;
using power_capacity::StoreIdentity;
using power_capacity::StoreReadOptions;
using power_capacity::Tick;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

class Arguments {
 public:
  static Arguments parse(int argc, char** argv) {
    Arguments arguments;
    for (int index = 0; index < argc; ++index) {
      std::string token = argv[index];
      if (token.rfind("--", 0) != 0) {
        arguments.positional_.push_back(std::move(token));
        continue;
      }
      token.erase(0, 2);
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        arguments.values_[token.substr(0, equals)].push_back(token.substr(equals + 1));
        continue;
      }
      if (index + 1 < argc && std::string_view(argv[index + 1]).rfind("--", 0) != 0) {
        arguments.values_[token].push_back(argv[index + 1]);
        ++index;
      } else {
        arguments.flags_.insert(std::move(token));
      }
    }
    return arguments;
  }

  bool has(std::string_view name) const {
    return values_.find(std::string(name)) != values_.end() ||
           flags_.find(std::string(name)) != flags_.end();
  }
  bool flag(std::string_view name) const { return flags_.find(std::string(name)) != flags_.end(); }

  std::optional<std::string> get(std::string_view name) const {
    const auto found = values_.find(std::string(name));
    if (found == values_.end() || found->second.empty()) {
      return std::nullopt;
    }
    return found->second.back();
  }

  std::vector<std::string> get_all(std::string_view name) const {
    const auto found = values_.find(std::string(name));
    if (found == values_.end()) {
      return {};
    }
    return found->second;
  }

  std::string require(std::string_view name) const {
    const std::optional<std::string> value = get(name);
    return value.has_value() ? *value : std::string();
  }

  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::map<std::string, std::vector<std::string>> values_;
  std::set<std::string> flags_;
  std::vector<std::string> positional_;
};

Status parse_i64(std::string_view name, const std::string& text, std::int64_t& out) {
  if (text.empty()) {
    return Status::error(StatusCode::InvalidArgument, std::string(name) + " must not be empty");
  }
  std::size_t consumed = 0;
  try {
    const long long value = std::stoll(text, &consumed, 10);
    if (consumed != text.size()) {
      throw std::invalid_argument("trailing");
    }
    out = static_cast<std::int64_t>(value);
  } catch (const std::exception&) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string(name) + " must be a decimal integer, got '" + text + "'");
  }
  return Status::success();
}

Status parse_u64(std::string_view name, const std::string& text, std::uint64_t& out) {
  std::int64_t signed_value = 0;
  const Status status = parse_i64(name, text, signed_value);
  if (!status.ok()) {
    return status;
  }
  if (signed_value < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string(name) + " must not be negative");
  }
  out = static_cast<std::uint64_t>(signed_value);
  return Status::success();
}

Status required_flag(const Arguments& arguments, std::string_view name, std::string& out) {
  if (!arguments.has(name)) {
    return Status::error(StatusCode::InvalidArgument, "missing required option --" + std::string(name));
  }
  out = arguments.require(name);
  return Status::success();
}

Status required_tick(const Arguments& arguments, std::string_view name, Tick& out) {
  std::string text;
  const Status status = required_flag(arguments, name, text);
  if (!status.ok()) {
    return status;
  }
  std::int64_t value = 0;
  const Status parsed = parse_i64(name, text, value);
  if (!parsed.ok()) {
    return parsed;
  }
  if (value < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string(name) + " must not be negative");
  }
  out = Tick(value);
  return Status::success();
}

Status required_generation(const Arguments& arguments, std::string_view name, Generation& out) {
  std::string text;
  const Status status = required_flag(arguments, name, text);
  if (!status.ok()) {
    return status;
  }
  std::uint64_t value = 0;
  const Status parsed = parse_u64(name, text, value);
  if (!parsed.ok()) {
    return parsed;
  }
  out = Generation(value);
  return Status::success();
}

Status required_watts(const Arguments& arguments, std::string_view name, Power& out) {
  std::string text;
  const Status status = required_flag(arguments, name, text);
  if (!status.ok()) {
    return status;
  }
  std::int64_t value = 0;
  const Status parsed = parse_i64(name, text, value);
  if (!parsed.ok()) {
    return parsed;
  }
  const power_capacity::Result<Power> watts = Power::from_watts(value);
  if (!watts.ok()) {
    return watts.status();
  }
  out = watts.value();
  return Status::success();
}

Status optional_ratio(const Arguments& arguments, std::string_view name, std::int32_t fallback,
                      Ratio& out) {
  if (!arguments.has(name)) {
    const power_capacity::Result<Ratio> ratio = Ratio::from_basis_points(fallback);
    if (!ratio.ok()) {
      return ratio.status();
    }
    out = ratio.value();
    return Status::success();
  }
  std::int64_t value = 0;
  const Status parsed = parse_i64(name, arguments.require(name), value);
  if (!parsed.ok()) {
    return parsed;
  }
  if (value < 0 || value > Ratio::scale) {
    return Status::error(StatusCode::InvalidArgument,
                         std::string(name) + " must be in [0, 10000]");
  }
  const power_capacity::Result<Ratio> ratio = Ratio::from_basis_points(static_cast<std::int32_t>(value));
  if (!ratio.ok()) {
    return ratio.status();
  }
  out = ratio.value();
  return Status::success();
}

void emit(const power_capacity::cli::Report& report, bool json) {
  std::cout << (json ? report.render_json() : report.render_text());
}

void emit_error(const Status& status, bool json) {
  power_capacity::cli::Report report;
  report.boolean("ok", false);
  report.text("error", power_capacity::to_string(status.code()));
  report.text("message", status.message());
  emit(report, json);
}

void emit_ok(const power_capacity::cli::Report& report, bool json) {
  power_capacity::cli::Report combined;
  combined.boolean("ok", true);
  combined.append(report);
  emit(combined, json);
}

std::filesystem::path store_path(const Arguments& arguments) {
  return std::filesystem::path(arguments.require("store"));
}

std::optional<StoreIdentity> expected_identity(const Arguments& arguments) {
  if (!arguments.has("store-id")) {
    return std::nullopt;
  }
  const power_capacity::Result<StoreIdentity> identity =
      StoreIdentity::parse(arguments.require("store-id"));
  if (!identity.ok()) {
    return std::nullopt;
  }
  return identity.value();
}

Status apply_authority_options(const Arguments& arguments, Epoch& epoch, Incarnation& incarnation) {
  if (arguments.has("epoch")) {
    std::uint64_t value = 0;
    const Status status = parse_u64("epoch", arguments.require("epoch"), value);
    if (!status.ok()) {
      return status;
    }
    epoch = Epoch(value);
  }
  if (arguments.has("incarnation")) {
    std::uint64_t value = 0;
    const Status status = parse_u64("incarnation", arguments.require("incarnation"), value);
    if (!status.ok()) {
      return status;
    }
    incarnation = Incarnation(value);
  }
  return Status::success();
}

/// Qualifies recovered state for the authority binding the store already
/// records.
///
/// Implicit qualification never moves the binding. A caller that supplies a
/// different `--epoch` or `--incarnation` is told so and pointed at the
/// `revalidate` command, because moving the binding is itself an authoritative
/// act that advances the generation and must be stated explicitly.
Status qualify(CapacityEngine& engine, const Arguments& arguments) {
  if (engine.lifecycle() == EngineLifecycle::Current) {
    return Status::success();
  }
  if (engine.lifecycle() == EngineLifecycle::Closed) {
    return Status::error(StatusCode::Closed, "engine is closed");
  }
  const power_capacity::Result<power_capacity::EngineStatus> current = engine.status();
  if (!current.ok()) {
    return current.status();
  }
  Epoch requested_epoch = current.value().epoch;
  Incarnation requested_incarnation = current.value().incarnation;
  const Status parsed = apply_authority_options(arguments, requested_epoch, requested_incarnation);
  if (!parsed.ok()) {
    return parsed;
  }
  if (requested_epoch != current.value().epoch ||
      requested_incarnation != current.value().incarnation) {
    return Status::error(
        StatusCode::StaleAuthority,
        "the store is bound to epoch " + std::to_string(current.value().epoch.value()) +
            " incarnation " + std::to_string(current.value().incarnation.value()) +
            "; moving the binding is an authoritative act, so use the revalidate command");
  }

  Tick as_of;
  Status status = required_tick(arguments, "as-of-tick", as_of);
  if (!status.ok()) {
    return Status::error(status.code(),
                         "opening recovered state requires --as-of-tick: " + status.message());
  }
  power_capacity::RevalidationRequest request;
  request.as_of = as_of;
  request.epoch = requested_epoch;
  request.incarnation = requested_incarnation;
  if (arguments.has("expected-generation")) {
    Generation generation;
    status = required_generation(arguments, "expected-generation", generation);
    if (!status.ok()) {
      return status;
    }
    request.expected_generation = generation;
  }
  const power_capacity::Result<power_capacity::RevalidationReport> report =
      engine.revalidate_recovered_state(request);
  if (!report.ok()) {
    return report.status();
  }
  return Status::success();
}

power_capacity::Result<std::unique_ptr<CapacityEngine>> open_engine(const Arguments& arguments,
                                                                    bool read_only) {
  EngineOptions options;
  options.store_path = store_path(arguments);
  options.create_if_missing = arguments.flag("create");
  options.read_only = read_only;
  options.enforce_path_binding = arguments.flag("enforce-path-binding");
  options.expected_store_identity = expected_identity(arguments);
  if (arguments.has("tick")) {
    std::int64_t value = 0;
    const Status status = parse_i64("tick", arguments.require("tick"), value);
    if (!status.ok()) {
      return status;
    }
    options.initial_tick = Tick(value);
  }
  Epoch epoch;
  Incarnation incarnation;
  Status status = apply_authority_options(arguments, epoch, incarnation);
  if (!status.ok()) {
    return status;
  }
  options.epoch = epoch;
  options.incarnation = incarnation;

  power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = CapacityEngine::open(options);
  if (!engine.ok()) {
    return engine.status();
  }
  const Status qualified = qualify(*engine.value(), arguments);
  if (!qualified.ok()) {
    return qualified;
  }
  return engine;
}

/// Builds a mutation context. The authority binding defaults to the binding this
/// engine currently accepts, so a caller only states it when it means to assert
/// one; an assertion that disagrees is refused by the engine.
Status build_mutation_context(const Arguments& arguments, const CapacityEngine& engine,
                              MutationContext& context) {
  Status status = required_tick(arguments, "as-of-tick", context.as_of);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<power_capacity::EngineStatus> current = engine.status();
  if (!current.ok()) {
    return current.status();
  }
  context.epoch = current.value().epoch;
  context.incarnation = current.value().incarnation;
  status = apply_authority_options(arguments, context.epoch, context.incarnation);
  if (!status.ok()) {
    return status;
  }
  Generation generation;
  status = required_generation(arguments, "expected-generation", generation);
  if (!status.ok()) {
    return status;
  }
  context.expected_generation = generation;

  for (const std::string& entry : arguments.get_all("expect-source")) {
    const std::size_t equals = entry.find('=');
    if (equals == std::string::npos) {
      return Status::error(StatusCode::InvalidArgument,
                           "--expect-source expects <id>=<generation>, got '" + entry + "'");
    }
    const power_capacity::Result<SourceId> source =
        SourceId::parse(entry.substr(0, equals), ResourceLimits::defaults().max_identifier_bytes);
    if (!source.ok()) {
      return source.status();
    }
    std::uint64_t value = 0;
    status = parse_u64("expect-source generation", entry.substr(equals + 1), value);
    if (!status.ok()) {
      return status;
    }
    context.expected_source_generations[source.value()] = Generation(value);
  }
  for (const std::string& entry : arguments.get_all("advance-source")) {
    const std::size_t equals = entry.find('=');
    if (equals == std::string::npos) {
      return Status::error(StatusCode::InvalidArgument,
                           "--advance-source expects <id>=<generation>, got '" + entry + "'");
    }
    const power_capacity::Result<SourceId> source =
        SourceId::parse(entry.substr(0, equals), ResourceLimits::defaults().max_identifier_bytes);
    if (!source.ok()) {
      return source.status();
    }
    std::uint64_t value = 0;
    status = parse_u64("advance-source generation", entry.substr(equals + 1), value);
    if (!status.ok()) {
      return status;
    }
    context.advanced_source_generations[source.value()] = Generation(value);
  }
  if (arguments.has("idempotency-key")) {
    const power_capacity::Result<IdempotencyKey> key = IdempotencyKey::parse(
        arguments.require("idempotency-key"), ResourceLimits::defaults().max_idempotency_key_bytes);
    if (!key.ok()) {
      return key.status();
    }
    context.idempotency_key = key.value();
  }
  return Status::success();
}

Status build_evidence(const Arguments& arguments, std::optional<EvidenceRef>& out) {
  if (!arguments.has("evidence-id")) {
    return Status::success();
  }
  const ResourceLimits limits = ResourceLimits::defaults();
  const power_capacity::Result<EvidenceId> id =
      EvidenceId::parse(arguments.require("evidence-id"), limits.max_identifier_bytes);
  if (!id.ok()) {
    return id.status();
  }
  std::string source_text;
  Status status = required_flag(arguments, "evidence-source", source_text);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<SourceId> source =
      SourceId::parse(source_text, limits.max_identifier_bytes);
  if (!source.ok()) {
    return source.status();
  }
  Tick observed;
  status = required_tick(arguments, "observed-tick", observed);
  if (!status.ok()) {
    return status;
  }
  Tick valid_until;
  status = required_tick(arguments, "valid-until-tick", valid_until);
  if (!status.ok()) {
    return status;
  }
  std::uint64_t revision = 0;
  if (arguments.has("revision")) {
    status = parse_u64("revision", arguments.require("revision"), revision);
    if (!status.ok()) {
      return status;
    }
  }
  std::uint64_t source_generation = 0;
  if (arguments.has("evidence-generation")) {
    status = parse_u64("evidence-generation", arguments.require("evidence-generation"),
                       source_generation);
    if (!status.ok()) {
      return status;
    }
  }
  EvidenceRef evidence;
  evidence.id = id.value();
  evidence.source = source.value();
  evidence.observed_at = observed;
  evidence.valid_until = valid_until;
  evidence.revision = Revision(revision);
  evidence.source_generation = Generation(source_generation);
  out = evidence;
  return Status::success();
}

Status build_domain(const Arguments& arguments, PowerDomain& domain) {
  const ResourceLimits limits = ResourceLimits::defaults();
  std::string id;
  Status status = required_flag(arguments, "id", id);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<DomainId> parsed =
      DomainId::parse(id, limits.max_identifier_bytes);
  if (!parsed.ok()) {
    return parsed.status();
  }
  domain.id = parsed.value();

  if (arguments.has("kind")) {
    const power_capacity::Result<power_capacity::DomainKind> kind =
        power_capacity::parse_domain_kind(arguments.require("kind"));
    if (!kind.ok()) {
      return kind.status();
    }
    domain.kind = kind.value();
  }
  if (arguments.has("label")) {
    domain.label = arguments.require("label");
  }
  if (arguments.has("parent")) {
    const power_capacity::Result<DomainId> parent =
        DomainId::parse(arguments.require("parent"), limits.max_identifier_bytes);
    if (!parent.ok()) {
      return parent.status();
    }
    domain.parent = parent.value();
  }
  status = required_watts(arguments, "nominal-w", domain.nominal_capacity);
  if (!status.ok()) {
    return status;
  }
  status = required_watts(arguments, "usable-w", domain.usable_capacity);
  if (!status.ok()) {
    return status;
  }
  status = optional_ratio(arguments, "derate-bp", Ratio::scale, domain.derate_ratio);
  if (!status.ok()) {
    return status;
  }
  status = optional_ratio(arguments, "degrade-bp", Ratio::scale, domain.degradation_ratio);
  if (!status.ok()) {
    return status;
  }
  if (arguments.has("reserve-mode")) {
    const power_capacity::Result<power_capacity::ReservePolicy::Mode> mode =
        power_capacity::parse_reserve_mode(arguments.require("reserve-mode"));
    if (!mode.ok()) {
      return mode.status();
    }
    domain.reserve.mode = mode.value();
  }
  if (arguments.has("reserve-w")) {
    status = required_watts(arguments, "reserve-w", domain.reserve.absolute);
    if (!status.ok()) {
      return status;
    }
  }
  if (arguments.has("reserve-bp")) {
    Ratio ratio = Ratio::zero();
    status = optional_ratio(arguments, "reserve-bp", 0, ratio);
    if (!status.ok()) {
      return status;
    }
    domain.reserve.ratio = ratio;
  }
  if (arguments.has("state")) {
    const power_capacity::Result<power_capacity::OperationalState> state =
        power_capacity::parse_operational_state(arguments.require("state"));
    if (!state.ok()) {
      return state.status();
    }
    domain.state = state.value();
  }
  if (arguments.has("cause")) {
    const power_capacity::Result<power_capacity::StateCause> cause =
        power_capacity::parse_state_cause(arguments.require("cause"));
    if (!cause.ok()) {
      return cause.status();
    }
    domain.state_cause = cause.value();
  }
  status = build_evidence(arguments, domain.evidence);
  if (!status.ok()) {
    return status;
  }
  return Status::success();
}

Status build_group(const Arguments& arguments, RedundancyGroup& group) {
  const ResourceLimits limits = ResourceLimits::defaults();
  std::string id;
  Status status = required_flag(arguments, "id", id);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<GroupId> parsed = GroupId::parse(id, limits.max_identifier_bytes);
  if (!parsed.ok()) {
    return parsed.status();
  }
  group.id = parsed.value();

  const std::vector<std::string> members = arguments.get_all("member");
  if (members.size() < 2) {
    return Status::error(StatusCode::InvalidArgument,
                         "--member must be given at least twice for a redundancy group");
  }
  for (const std::string& member : members) {
    const power_capacity::Result<DomainId> domain =
        DomainId::parse(member, limits.max_identifier_bytes);
    if (!domain.ok()) {
      return domain.status();
    }
    group.members.push_back(domain.value());
  }
  if (arguments.has("class")) {
    const power_capacity::Result<power_capacity::RedundancyClass> declared =
        power_capacity::parse_redundancy_class(arguments.require("class"));
    if (!declared.ok()) {
      return declared.status();
    }
    group.declared_class = declared.value();
  }
  std::string tolerance_text;
  status = required_flag(arguments, "tolerance", tolerance_text);
  if (!status.ok()) {
    return status;
  }
  std::uint64_t tolerance = 0;
  status = parse_u64("tolerance", tolerance_text, tolerance);
  if (!status.ok()) {
    return status;
  }
  group.required_simultaneous_failures = static_cast<std::uint32_t>(tolerance);

  std::string authority;
  status = required_flag(arguments, "policy-authority", authority);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<power_capacity::AuthorityRef> parsed_authority =
      power_capacity::AuthorityRef::parse(authority, limits.max_identifier_bytes);
  if (!parsed_authority.ok()) {
    return parsed_authority.status();
  }
  group.policy_authority = parsed_authority.value();
  status = build_evidence(arguments, group.evidence);
  if (!status.ok()) {
    return status;
  }
  if (arguments.has("label")) {
    group.label = arguments.require("label");
  }
  return Status::success();
}

void report_commit(const power_capacity::CommitResult& commit, power_capacity::cli::Report& report) {
  report.unsigned_integer("generation", commit.generation.value());
  report.unsigned_integer("attempt", commit.attempt.value());
  report.boolean("already_applied", commit.already_applied);
  report.unsigned_integer("bytes_written", commit.metrics.bytes_written);
  report.unsigned_integer("commit_total_nanos", commit.metrics.total_nanos);
  report.unsigned_integer("commit_flush_nanos", commit.metrics.staging_write_nanos);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Status command_init(const Arguments& arguments, power_capacity::cli::Report& report) {
  power_capacity::StoreOpenOptions options;
  options.path = store_path(arguments);
  options.create_if_missing = true;
  options.access = power_capacity::StoreAccess::ReadWrite;
  options.expected_store_identity = expected_identity(arguments);
  Tick tick;
  const Status status = required_tick(arguments, "tick", tick);
  if (!status.ok()) {
    return status;
  }
  options.created_at = tick;
  Epoch epoch;
  Incarnation incarnation;
  const Status authority = apply_authority_options(arguments, epoch, incarnation);
  if (!authority.ok()) {
    return authority;
  }
  options.epoch = epoch;
  options.incarnation = incarnation;

  const power_capacity::Result<std::shared_ptr<power_capacity::CapacityStore>> store =
      power_capacity::CapacityStore::open(options);
  if (!store.ok()) {
    return store.status();
  }
  const power_capacity::StoreInfo& info = store.value()->info();
  report.text("store", power_capacity::path_to_utf8(info.path));
  report.text("store_id", info.store_identity.to_hex());
  report.unsigned_integer("generation", info.generation.value());
  report.unsigned_integer("epoch", info.epoch.value());
  report.unsigned_integer("incarnation", info.incarnation.value());
  report.boolean("created", info.created);
  report.unsigned_integer("file_bytes", info.file_bytes);
  report.unsigned_integer("format_version", info.format_version);
  report.boolean("path_binding_matches", info.path_binding_matches);
  return store.value()->close();
}

Status command_verify(const Arguments& arguments, power_capacity::cli::Report& report) {
  const Status status =
      power_capacity::CapacityStore::verify_file(store_path(arguments));
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<std::shared_ptr<const power_capacity::CapacityState>> state =
      power_capacity::CapacityStore::read_file(store_path(arguments), StoreReadOptions{});
  if (!state.ok()) {
    return state.status();
  }
  report.text("store", power_capacity::path_to_utf8(store_path(arguments)));
  report.text("store_id", state.value()->store_identity().to_hex());
  report.unsigned_integer("generation", state.value()->generation().value());
  report.unsigned_integer("domains", state.value()->domain_count());
  report.unsigned_integer("loads", state.value()->load_count());
  report.unsigned_integer("groups", state.value()->group_count());
  report.unsigned_integer("sources", state.value()->source_count());
  report.boolean("integrity_verified", true);
  return Status::success();
}

Status command_inspect(const Arguments& arguments, power_capacity::cli::Report& report) {
  power_capacity::StoreOpenOptions options;
  options.path = store_path(arguments);
  options.access = power_capacity::StoreAccess::ReadOnly;
  options.expected_store_identity = expected_identity(arguments);
  options.enforce_path_binding = arguments.flag("enforce-path-binding");
  const power_capacity::Result<std::shared_ptr<power_capacity::CapacityStore>> store =
      power_capacity::CapacityStore::open(options);
  if (!store.ok()) {
    return store.status();
  }
  const power_capacity::StoreInfo& info = store.value()->info();
  report.text("store", power_capacity::path_to_utf8(info.path));
  report.text("store_id", info.store_identity.to_hex());
  report.unsigned_integer("generation", info.generation.value());
  report.unsigned_integer("epoch", info.epoch.value());
  report.unsigned_integer("incarnation", info.incarnation.value());
  report.integer("created_at", info.created_at.value());
  report.integer("updated_at", info.updated_at.value());
  report.integer("last_revalidated_at", info.last_revalidated_at.value());
  report.text("recorded_path", info.recorded_path);
  report.boolean("path_binding_matches", info.path_binding_matches);
  report.boolean("writable", info.writable);
  report.unsigned_integer("format_version", info.format_version);
  report.unsigned_integer("file_bytes", info.file_bytes);
  report.unsigned_integer("payload_crc32c", info.payload_crc32c);
  report.unsigned_integer("domains", info.domain_count);
  report.unsigned_integer("loads", info.load_count);
  report.unsigned_integer("groups", info.group_count);
  report.unsigned_integer("sources", info.source_count);
  report.unsigned_integer("applied_operations", info.applied_operation_count);
  return store.value()->close();
}

Status command_put_domain(const Arguments& arguments, power_capacity::cli::Report& report) {
  PowerDomain domain;
  Status status = build_domain(arguments, domain);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  MutationContext context;
  status = build_mutation_context(arguments, *engine.value(), context);
  if (!status.ok()) {
    return status;
  }
  power_capacity::PresenceExpectation presence = power_capacity::PresenceExpectation::Any;
  if (arguments.flag("must-exist")) {
    presence = power_capacity::PresenceExpectation::MustExist;
  } else if (arguments.flag("must-not-exist")) {
    presence = power_capacity::PresenceExpectation::MustNotExist;
  }
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->put_domain(domain, presence, context);
  if (!committed.ok()) {
    return committed.status();
  }
  report.text("domain", domain.id.value());
  report_commit(committed.value(), report);
  return Status::success();
}

Status command_erase_domain(const Arguments& arguments, power_capacity::cli::Report& report) {
  const power_capacity::Result<DomainId> id = DomainId::parse(
      arguments.require("id"), ResourceLimits::defaults().max_identifier_bytes);
  if (!id.ok()) {
    return id.status();
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  MutationContext context;
  Status status = build_mutation_context(arguments, *engine.value(), context);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->erase_domain(id.value(), context);
  if (!committed.ok()) {
    return committed.status();
  }
  report.text("domain", id.value().value());
  report_commit(committed.value(), report);
  return Status::success();
}

Status command_put_load(const Arguments& arguments, power_capacity::cli::Report& report) {
  const ResourceLimits limits = ResourceLimits::defaults();
  LoadRecord load;
  const power_capacity::Result<LoadId> id =
      LoadId::parse(arguments.require("id"), limits.max_identifier_bytes);
  if (!id.ok()) {
    return id.status();
  }
  load.id = id.value();
  const power_capacity::Result<DomainId> domain =
      DomainId::parse(arguments.require("domain"), limits.max_identifier_bytes);
  if (!domain.ok()) {
    return domain.status();
  }
  load.domain = domain.value();
  if (arguments.has("class")) {
    const power_capacity::Result<power_capacity::LoadClass> load_class =
        power_capacity::parse_load_class(arguments.require("class"));
    if (!load_class.ok()) {
      return load_class.status();
    }
    load.load_class = load_class.value();
  }
  Status status = required_watts(arguments, "load-w", load.load);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<power_capacity::AuthorityRef> authority = power_capacity::AuthorityRef::parse(
      arguments.require("authority"), limits.max_identifier_bytes);
  if (!authority.ok()) {
    return authority.status();
  }
  load.authority = authority.value();
  status = build_evidence(arguments, load.evidence);
  if (!status.ok()) {
    return status;
  }
  if (arguments.has("label")) {
    load.label = arguments.require("label");
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  MutationContext context;
  status = build_mutation_context(arguments, *engine.value(), context);
  if (!status.ok()) {
    return status;
  }
  power_capacity::PresenceExpectation presence = power_capacity::PresenceExpectation::Any;
  if (arguments.flag("must-exist")) {
    presence = power_capacity::PresenceExpectation::MustExist;
  } else if (arguments.flag("must-not-exist")) {
    presence = power_capacity::PresenceExpectation::MustNotExist;
  }
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->put_load(load, presence, context);
  if (!committed.ok()) {
    return committed.status();
  }
  report.text("load", load.id.value());
  report.text("domain", load.domain.value());
  report_commit(committed.value(), report);
  return Status::success();
}

Status command_erase_load(const Arguments& arguments, power_capacity::cli::Report& report) {
  const power_capacity::Result<LoadId> id = LoadId::parse(
      arguments.require("id"), ResourceLimits::defaults().max_identifier_bytes);
  if (!id.ok()) {
    return id.status();
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  MutationContext context;
  Status status = build_mutation_context(arguments, *engine.value(), context);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->erase_load(id.value(), context);
  if (!committed.ok()) {
    return committed.status();
  }
  report.text("load", id.value().value());
  report_commit(committed.value(), report);
  return Status::success();
}

Status command_put_group(const Arguments& arguments, power_capacity::cli::Report& report) {
  RedundancyGroup group;
  Status status = build_group(arguments, group);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  MutationContext context;
  status = build_mutation_context(arguments, *engine.value(), context);
  if (!status.ok()) {
    return status;
  }
  power_capacity::PresenceExpectation presence = power_capacity::PresenceExpectation::Any;
  if (arguments.flag("must-exist")) {
    presence = power_capacity::PresenceExpectation::MustExist;
  } else if (arguments.flag("must-not-exist")) {
    presence = power_capacity::PresenceExpectation::MustNotExist;
  }
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->put_group(group, presence, context);
  if (!committed.ok()) {
    return committed.status();
  }
  report.text("group", group.id.value());
  report.unsigned_integer("members", group.members.size());
  report.unsigned_integer("tolerance", group.required_simultaneous_failures);
  report_commit(committed.value(), report);
  return Status::success();
}

Status command_erase_group(const Arguments& arguments, power_capacity::cli::Report& report) {
  const power_capacity::Result<GroupId> id = GroupId::parse(
      arguments.require("id"), ResourceLimits::defaults().max_identifier_bytes);
  if (!id.ok()) {
    return id.status();
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  MutationContext context;
  Status status = build_mutation_context(arguments, *engine.value(), context);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->erase_group(id.value(), context);
  if (!committed.ok()) {
    return committed.status();
  }
  report.text("group", id.value().value());
  report_commit(committed.value(), report);
  return Status::success();
}

Status command_advance_source(const Arguments& arguments, power_capacity::cli::Report& report) {
  const power_capacity::Result<SourceId> source = SourceId::parse(
      arguments.require("source"), ResourceLimits::defaults().max_identifier_bytes);
  if (!source.ok()) {
    return source.status();
  }
  std::uint64_t next = 0;
  Status status = parse_u64("to-generation", arguments.require("to-generation"), next);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  MutationContext context;
  status = build_mutation_context(arguments, *engine.value(), context);
  if (!status.ok()) {
    return status;
  }
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->advance_source_generation(source.value(), Generation(next), context);
  if (!committed.ok()) {
    return committed.status();
  }
  report.text("source", source.value().value());
  report.unsigned_integer("generation", next);
  report_commit(committed.value(), report);
  return Status::success();
}

Status command_revalidate(const Arguments& arguments, power_capacity::cli::Report& report) {
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(arguments, false);
  if (!engine.ok()) {
    return engine.status();
  }
  Tick as_of;
  Status status = required_tick(arguments, "as-of-tick", as_of);
  if (!status.ok()) {
    return status;
  }
  power_capacity::RevalidationRequest request;
  request.as_of = as_of;
  status = apply_authority_options(arguments, request.epoch, request.incarnation);
  if (!status.ok()) {
    return status;
  }
  if (arguments.has("expected-generation")) {
    Generation generation;
    status = required_generation(arguments, "expected-generation", generation);
    if (!status.ok()) {
      return status;
    }
    request.expected_generation = generation;
  }
  const power_capacity::Result<power_capacity::RevalidationReport> revalidated =
      engine.value()->revalidate_recovered_state(request);
  if (!revalidated.ok()) {
    return revalidated.status();
  }
  report.integer("as_of", revalidated.value().as_of.value());
  report.unsigned_integer("generation", revalidated.value().generation.value());
  report.unsigned_integer("epoch", revalidated.value().epoch.value());
  report.unsigned_integer("incarnation", revalidated.value().incarnation.value());
  report.unsigned_integer("domains_total", revalidated.value().domains_total);
  report.unsigned_integer("domains_current", revalidated.value().domains_current);
  report.unsigned_integer("domains_stale", revalidated.value().domains_stale);
  report.unsigned_integer("domains_missing_evidence", revalidated.value().domains_missing_evidence);
  report.boolean("all_evidence_current", revalidated.value().all_evidence_current);
  return Status::success();
}

void report_assessment(const power_capacity::DomainAssessment& assessment,
                       power_capacity::cli::Report& report) {
  report.text("domain", assessment.domain.value());
  report.text("store_id", assessment.store.to_hex());
  report.integer("as_of", assessment.as_of.value());
  report.unsigned_integer("generation", assessment.generation.value());
  report.unsigned_integer("epoch", assessment.epoch.value());
  report.unsigned_integer("incarnation", assessment.incarnation.value());
  report.boolean("known", assessment.known);
  report.text("reason", power_capacity::to_string(assessment.reason));
  report.unsigned_integer("depth", assessment.depth);
  report.boolean("path_degraded", assessment.path_degraded);
  report.unsigned_integer("digest", assessment.digest);
  if (assessment.derivation.has_value()) {
    const power_capacity::CapacityDerivation& derivation = *assessment.derivation;
    report.watts("nominal_w", derivation.nominal.milliwatts());
    report.watts("usable_w", derivation.usable.milliwatts());
    report.watts("derated_w", derivation.derated.milliwatts());
    report.watts("operational_w", derivation.operational.milliwatts());
    report.watts("reserve_w", derivation.reserve.milliwatts());
    report.watts("protected_w", derivation.protected_load.milliwatts());
    report.watts("committed_w", derivation.committed_load.milliwatts());
    report.watts("safe_w", derivation.safe_capacity.milliwatts());
    report.watts("carryable_w", derivation.carryable_capacity.milliwatts());
    report.watts("allocatable_w", derivation.allocatable_headroom.milliwatts());
    report.boolean("reserve_clamped", derivation.reserve_clamped);
    report.boolean("protected_clamped", derivation.protected_clamped);
    report.boolean("committed_clamped", derivation.committed_clamped);
    report.boolean("load_on_unavailable", derivation.load_on_unavailable);
  } else {
    report.unknown("nominal_w");
    report.unknown("allocatable_w");
  }
  if (assessment.effective_allocatable_headroom.has_value()) {
    report.watts("effective_allocatable_w",
                 assessment.effective_allocatable_headroom->milliwatts());
  } else {
    report.unknown("effective_allocatable_w");
  }
  if (assessment.redundancy_group.has_value()) {
    report.text("group", assessment.redundancy_group->value());
  } else {
    report.unknown("group");
  }
  if (assessment.group_effective_headroom.has_value()) {
    report.watts("group_effective_w", assessment.group_effective_headroom->milliwatts());
  } else {
    report.unknown("group_effective_w");
  }
  if (assessment.bottleneck.has_value()) {
    report.text("bottleneck", assessment.bottleneck->value());
  } else {
    report.unknown("bottleneck");
  }
  if (assessment.bottleneck_headroom.has_value()) {
    report.watts("bottleneck_w", assessment.bottleneck_headroom->milliwatts());
  } else {
    report.unknown("bottleneck_w");
  }
  report.unsigned_integer("path_length", assessment.path.size());
  for (std::size_t index = 0; index < assessment.path.size(); ++index) {
    const power_capacity::PathNode& node = assessment.path[index];
    const std::string prefix = "path." + std::to_string(index) + ".";
    report.text(prefix + "domain", node.domain.value());
    report.unsigned_integer(prefix + "depth", node.depth);
    report.watts(prefix + "allocatable_w", node.derivation.allocatable_headroom.milliwatts());
    report.watts(prefix + "effective_w", node.effective_headroom.milliwatts());
    report.text(prefix + "reason", power_capacity::to_string(node.reason));
    if (node.redundancy_group.has_value()) {
      report.text(prefix + "group", node.redundancy_group->value());
    }
  }
}

Status command_assess(const Arguments& arguments, power_capacity::cli::Report& report) {
  const power_capacity::Result<DomainId> domain = DomainId::parse(
      arguments.require("domain"), ResourceLimits::defaults().max_identifier_bytes);
  if (!domain.ok()) {
    return domain.status();
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine =
      open_engine(arguments, true);
  if (!engine.ok()) {
    return engine.status();
  }
  CapacityQuery query;
  query.domain = domain.value();
  Status status = required_tick(arguments, "as-of-tick", query.as_of);
  if (!status.ok()) {
    return status;
  }
  if (arguments.has("expected-generation")) {
    Generation generation;
    status = required_generation(arguments, "expected-generation", generation);
    if (!status.ok()) {
      return status;
    }
    query.expected_generation = generation;
  }
  const power_capacity::Result<power_capacity::DomainAssessment> assessment =
      engine.value()->assess(query);
  if (!assessment.ok()) {
    return assessment.status();
  }
  report_assessment(assessment.value(), report);
  return Status::success();
}

Status command_evaluate(const Arguments& arguments, power_capacity::cli::Report& report) {
  const ResourceLimits limits = ResourceLimits::defaults();
  CandidateLoad candidate;
  const power_capacity::Result<LoadId> id =
      LoadId::parse(arguments.require("id"), limits.max_identifier_bytes);
  if (!id.ok()) {
    return id.status();
  }
  candidate.id = id.value();
  const power_capacity::Result<DomainId> domain =
      DomainId::parse(arguments.require("domain"), limits.max_identifier_bytes);
  if (!domain.ok()) {
    return domain.status();
  }
  candidate.domain = domain.value();
  Status status = required_watts(arguments, "load-w", candidate.load);
  if (!status.ok()) {
    return status;
  }
  if (arguments.has("class")) {
    const power_capacity::Result<power_capacity::LoadClass> load_class =
        power_capacity::parse_load_class(arguments.require("class"));
    if (!load_class.ok()) {
      return load_class.status();
    }
    candidate.load_class = load_class.value();
  }
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine =
      open_engine(arguments, true);
  if (!engine.ok()) {
    return engine.status();
  }
  EvaluationContext context;
  status = required_tick(arguments, "as-of-tick", context.as_of);
  if (!status.ok()) {
    return status;
  }
  if (arguments.has("expected-generation")) {
    Generation generation;
    status = required_generation(arguments, "expected-generation", generation);
    if (!status.ok()) {
      return status;
    }
    context.expected_generation = generation;
  }
  const power_capacity::Result<power_capacity::CandidateEvaluation> evaluation =
      engine.value()->evaluate(candidate, context);
  if (!evaluation.ok()) {
    return evaluation.status();
  }
  report.text("load", evaluation.value().id.value());
  report.text("domain", evaluation.value().domain.value());
  report.text("class", power_capacity::to_string(evaluation.value().load_class));
  report.text("verdict", power_capacity::to_string(evaluation.value().verdict));
  report.text("reason", power_capacity::to_string(evaluation.value().reason));
  report.text("cause", power_capacity::to_string(evaluation.value().cause));
  report.watts("requested_w", evaluation.value().requested.milliwatts());
  if (evaluation.value().bottleneck.has_value()) {
    report.text("bottleneck", evaluation.value().bottleneck->value());
  } else {
    report.unknown("bottleneck");
  }
  if (evaluation.value().remaining_at_bottleneck.has_value()) {
    report.watts("remaining_w", evaluation.value().remaining_at_bottleneck->milliwatts());
  } else {
    report.unknown("remaining_w");
  }
  report.boolean("redundancy_limited", evaluation.value().redundancy_limited);
  report.text("explanation", evaluation.value().explanation);
  return Status::success();
}

Status command_status(const Arguments& arguments, power_capacity::cli::Report& report) {
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine =
      open_engine(arguments, !arguments.flag("writable"));
  if (!engine.ok()) {
    return engine.status();
  }
  const power_capacity::Result<power_capacity::EngineStatus> status = engine.value()->status();
  if (!status.ok()) {
    return status.status();
  }
  report.text("lifecycle", power_capacity::to_string(status.value().lifecycle));
  report.boolean("durable", status.value().durable);
  report.boolean("read_only", status.value().read_only);
  report.text("session", status.value().session.to_hex());
  report.unsigned_integer("epoch", status.value().epoch.value());
  report.unsigned_integer("incarnation", status.value().incarnation.value());
  report.integer("qualified_at", status.value().qualified_at.value());
  report.boolean("store_open", !status.value().store.path.empty());
  if (!status.value().store.path.empty()) {
    report.text("store_id", status.value().store.store_identity.to_hex());
    report.unsigned_integer("generation", status.value().store.generation.value());
    report.boolean("path_binding_matches", status.value().store.path_binding_matches);
  }
  report.unsigned_integer("domains", status.value().domain_count);
  report.unsigned_integer("loads", status.value().load_count);
  report.unsigned_integer("groups", status.value().group_count);
  report.unsigned_integer("sources", status.value().source_count);
  report.unsigned_integer("applied_operations", status.value().applied_operation_count);
  return Status::success();
}

void print_usage() {
  std::cout <<
      "power-capacity " POWER_CAPACITY_VERSION_STRING "\n"
      "\n"
      "Electrical capacity authority for the Data Center Control Plane.\n"
      "\n"
      "Usage: power-capacity <command> [options]\n"
      "\n"
      "Commands:\n"
      "  version             Print the library version.\n"
      "  init                Create a store and print its identity.\n"
      "  verify              Verify a store artifact end to end.\n"
      "  inspect             Print committed store metadata.\n"
      "  status              Open an engine and print its lifecycle and binding.\n"
      "  put-domain          Commit a power domain.\n"
      "  erase-domain        Remove a power domain that has no dependents.\n"
      "  put-load            Commit a load attributed to one domain.\n"
      "  erase-load          Remove a load.\n"
      "  put-group           Commit a redundancy group.\n"
      "  erase-group         Remove a redundancy group.\n"
      "  advance-source      Record that a source generation advanced.\n"
      "  revalidate          Qualify recovered state against current evidence.\n"
      "  assess              Answer the capacity question for one domain.\n"
      "  evaluate            Evaluate a proposed load against current authority.\n"
      "\n"
      "Common options:\n"
      "  --store <path>            Store file. Required by every command but version.\n"
      "  --store-id <hex>          Expected store identity; a mismatch is refused.\n"
      "  --json                    Emit one JSON object instead of key=value lines.\n"
      "  --tick <n>                Creation instant. init only.\n"
      "  --epoch <n>               Control-plane epoch. Default 0.\n"
      "  --incarnation <n>         Controller incarnation. Default 0.\n"
      "  --as-of-tick <n>          Logical instant of the question or the evidence.\n"
      "  --expected-generation <n> Required precondition on every mutation.\n"
      "  --expect-source <id>=<n>  Source generation the caller read. Repeatable.\n"
      "  --advance-source <id>=<n> Source generation to record. Repeatable.\n"
      "  --idempotency-key <key>   Bounded replay key for a mutation.\n"
      "  --read-only               assess and evaluate only: read without the writer lock.\n"
      "\n"
      "Evidence options (all required together, or none):\n"
      "  --evidence-id, --evidence-source, --observed-tick, --valid-until-tick,\n"
      "  --revision, --evidence-generation\n"
      "\n"
      "Exit codes: 0 success, 1 typed error, 2 usage error.\n";
}

}  // namespace

int main(int argc, char** argv) {
  const Arguments arguments = Arguments::parse(argc - 1, argv + 1);
  const std::vector<std::string>& positional = arguments.positional();
  if (positional.empty()) {
    print_usage();
    return kExitUsage;
  }
  const std::string command = positional.front();
  const bool json = arguments.flag("json");

  if (command == "version" || command == "--version") {
    power_capacity::cli::Report report;
    report.text("name", "power-capacity");
    report.text("version", POWER_CAPACITY_VERSION_STRING);
    report.integer("version_major", power_capacity::Version::major);
    report.integer("version_minor", power_capacity::Version::minor);
    report.integer("version_patch", power_capacity::Version::patch);
    emit_ok(report, json);
    return kExitOk;
  }
  if (command == "help" || command == "-h" || command == "--help") {
    print_usage();
    return kExitOk;
  }
  if (command != "init" && !arguments.has("store")) {
    emit_error(Status::error(StatusCode::InvalidArgument,
                             "command '" + command + "' requires --store <path>"),
               json);
    return kExitUsage;
  }

  power_capacity::cli::Report report;
  Status status = Status::error(StatusCode::Unsupported, "unknown command '" + command + "'");
  if (command == "init") {
    status = command_init(arguments, report);
  } else if (command == "verify") {
    status = command_verify(arguments, report);
  } else if (command == "inspect") {
    status = command_inspect(arguments, report);
  } else if (command == "status") {
    status = command_status(arguments, report);
  } else if (command == "put-domain") {
    status = command_put_domain(arguments, report);
  } else if (command == "erase-domain") {
    status = command_erase_domain(arguments, report);
  } else if (command == "put-load") {
    status = command_put_load(arguments, report);
  } else if (command == "erase-load") {
    status = command_erase_load(arguments, report);
  } else if (command == "put-group") {
    status = command_put_group(arguments, report);
  } else if (command == "erase-group") {
    status = command_erase_group(arguments, report);
  } else if (command == "advance-source") {
    status = command_advance_source(arguments, report);
  } else if (command == "revalidate") {
    status = command_revalidate(arguments, report);
  } else if (command == "assess") {
    status = command_assess(arguments, report);
  } else if (command == "evaluate") {
    status = command_evaluate(arguments, report);
  }

  if (!status.ok()) {
    emit_error(status, json);
    return status.code() == StatusCode::Unsupported && command != "help" && command != "version"
               ? kExitUsage
               : kExitFailure;
  }
  emit_ok(report, json);
  return kExitOk;
}
