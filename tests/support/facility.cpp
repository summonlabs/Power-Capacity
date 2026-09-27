#include "facility.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <utility>

#include "test_harness.hpp"

namespace pc_test {

using power_capacity::AuthorityRef;
using power_capacity::DomainId;
using power_capacity::EvidenceId;
using power_capacity::EvidenceRef;
using power_capacity::Generation;
using power_capacity::GroupId;
using power_capacity::LoadId;
using power_capacity::MutationContext;
using power_capacity::Power;
using power_capacity::PresenceExpectation;
using power_capacity::Ratio;
using power_capacity::ResourceLimits;
using power_capacity::Revision;
using power_capacity::SourceId;
using power_capacity::Tick;

namespace {

std::filesystem::path make_temp_directory() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  static std::uint64_t counter = 0;
  ++counter;
  std::filesystem::path base = std::filesystem::temp_directory_path();
  base /= "power-capacity-tests";
  std::filesystem::path candidate =
      base / ("run-" + std::to_string(stamp) + "-" + std::to_string(counter));
  std::error_code error;
  std::filesystem::create_directories(candidate, error);
  if (error) {
    throw std::runtime_error("could not create a temporary directory: " + error.message());
  }
  return candidate;
}

}  // namespace

TempDir::TempDir() : path_(make_temp_directory()) {}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path TempDir::file(const std::string& name) const { return path_ / name; }

std::size_t TempDir::entry_count() const {
  std::error_code error;
  std::size_t count = 0;
  for (std::filesystem::directory_iterator iterator(path_, error), end; !error && iterator != end;
       iterator.increment(error)) {
    ++count;
  }
  return count;
}

std::vector<std::string> TempDir::entry_names() const {
  std::vector<std::string> names;
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(path_, error), end; !error && iterator != end;
       iterator.increment(error)) {
    names.push_back(iterator->path().filename().string());
  }
  return names;
}

DomainBuilder::DomainBuilder(std::string id) {
  const auto parsed = DomainId::parse(id, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed.ok()) {
    domain_.id = parsed.value();
  }
  domain_.kind = power_capacity::DomainKind::Pdu;
  domain_.derate_ratio = Ratio::full();
  domain_.degradation_ratio = Ratio::full();
  domain_.nominal_capacity = Power(0);
  domain_.usable_capacity = Power(0);
  EvidenceRef evidence;
  const auto evidence_id = EvidenceId::parse(evidence_id_, 128);
  if (evidence_id.ok()) {
    evidence.id = evidence_id.value();
  }
  const auto source = SourceId::parse("source.test", 128);
  if (source.ok()) {
    evidence.source = source.value();
  }
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000);
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  domain_.evidence = evidence;
}

DomainBuilder& DomainBuilder::kind(power_capacity::DomainKind kind) {
  domain_.kind = kind;
  return *this;
}

DomainBuilder& DomainBuilder::label(std::string label) {
  domain_.label = std::move(label);
  return *this;
}

DomainBuilder& DomainBuilder::parent(std::string parent) {
  const auto parsed = DomainId::parse(parent, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed.ok()) {
    domain_.parent = parsed.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::nominal_watts(std::int64_t watts) {
  const auto value = Power::from_watts(watts);
  if (value.ok()) {
    domain_.nominal_capacity = value.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::usable_watts(std::int64_t watts) {
  const auto value = Power::from_watts(watts);
  if (value.ok()) {
    domain_.usable_capacity = value.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::derate_bp(std::int32_t basis_points) {
  const auto value = Ratio::from_basis_points(basis_points);
  if (value.ok()) {
    domain_.derate_ratio = value.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::degradation_bp(std::int32_t basis_points) {
  const auto value = Ratio::from_basis_points(basis_points);
  if (value.ok()) {
    domain_.degradation_ratio = value.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::reserve_watts(std::int64_t watts) {
  const auto value = Power::from_watts(watts);
  if (value.ok()) {
    domain_.reserve.mode = power_capacity::ReservePolicy::Mode::Absolute;
    domain_.reserve.absolute = value.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::reserve_bp(std::int32_t basis_points) {
  const auto value = Ratio::from_basis_points(basis_points);
  if (value.ok()) {
    domain_.reserve.mode = power_capacity::ReservePolicy::Mode::Ratio;
    domain_.reserve.ratio = value.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::reserve_greater_of(std::int64_t watts, std::int32_t basis_points) {
  const auto value = Power::from_watts(watts);
  const auto ratio = Ratio::from_basis_points(basis_points);
  if (value.ok() && ratio.ok()) {
    domain_.reserve.mode = power_capacity::ReservePolicy::Mode::GreaterOfAbsoluteAndRatio;
    domain_.reserve.absolute = value.value();
    domain_.reserve.ratio = ratio.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::available() {
  domain_.state = power_capacity::OperationalState::Available;
  domain_.state_cause = power_capacity::StateCause::None;
  domain_.degradation_ratio = Ratio::full();
  return *this;
}

DomainBuilder& DomainBuilder::degraded(power_capacity::StateCause cause,
                                       std::int32_t degradation_bp) {
  domain_.state = power_capacity::OperationalState::Degraded;
  domain_.state_cause = cause;
  const auto ratio = Ratio::from_basis_points(degradation_bp);
  if (ratio.ok()) {
    domain_.degradation_ratio = ratio.value();
  }
  return *this;
}

DomainBuilder& DomainBuilder::unavailable(power_capacity::StateCause cause) {
  domain_.state = power_capacity::OperationalState::Unavailable;
  domain_.state_cause = cause;
  domain_.degradation_ratio = Ratio::full();
  return *this;
}

DomainBuilder& DomainBuilder::evidence(Tick observed, Tick valid_until, std::uint64_t revision) {
  EvidenceRef reference;
  const auto evidence_id = EvidenceId::parse(evidence_id_, 128);
  if (evidence_id.ok()) {
    reference.id = evidence_id.value();
  }
  const auto source = SourceId::parse("source.test", 128);
  if (source.ok()) {
    reference.source = source.value();
  }
  reference.observed_at = observed;
  reference.valid_until = valid_until;
  reference.revision = Revision(revision);
  reference.source_generation = Generation(1);
  domain_.evidence = reference;
  return *this;
}

DomainBuilder& DomainBuilder::no_evidence() {
  domain_.evidence.reset();
  return *this;
}

DomainBuilder& DomainBuilder::source_generation(const std::string& source, std::uint64_t generation) {
  const auto parsed = SourceId::parse(source, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed.ok()) {
    domain_.source_generations[parsed.value()] = Generation(generation);
  }
  return *this;
}

power_capacity::PowerDomain DomainBuilder::build() const { return domain_; }

power_capacity::LoadRecord make_load(const std::string& id, const std::string& domain,
                                     std::int64_t watts, power_capacity::LoadClass load_class,
                                     const std::string& authority) {
  power_capacity::LoadRecord load;
  const auto parsed_id = LoadId::parse(id, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed_id.ok()) {
    load.id = parsed_id.value();
  }
  const auto parsed_domain = DomainId::parse(domain, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed_domain.ok()) {
    load.domain = parsed_domain.value();
  }
  const auto parsed_authority =
      AuthorityRef::parse(authority, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed_authority.ok()) {
    load.authority = parsed_authority.value();
  }
  const auto value = Power::from_watts(watts);
  if (value.ok()) {
    load.load = value.value();
  }
  load.load_class = load_class;
  EvidenceRef evidence;
  const auto evidence_id = EvidenceId::parse("ev.load", 128);
  if (evidence_id.ok()) {
    evidence.id = evidence_id.value();
  }
  const auto source = SourceId::parse("source.test", 128);
  if (source.ok()) {
    evidence.source = source.value();
  }
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000);
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  load.evidence = evidence;
  return load;
}

power_capacity::RedundancyGroup make_group(const std::string& id,
                                           const std::vector<std::string>& members,
                                           std::uint32_t tolerance,
                                           power_capacity::RedundancyClass redundancy_class,
                                           const std::string& authority) {
  power_capacity::RedundancyGroup group;
  const auto parsed_id = GroupId::parse(id, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed_id.ok()) {
    group.id = parsed_id.value();
  }
  for (const std::string& member : members) {
    const auto parsed = DomainId::parse(member, ResourceLimits::defaults().max_identifier_bytes);
    if (parsed.ok()) {
      group.members.push_back(parsed.value());
    }
  }
  group.required_simultaneous_failures = tolerance;
  group.declared_class = redundancy_class;
  const auto parsed_authority =
      AuthorityRef::parse(authority, ResourceLimits::defaults().max_identifier_bytes);
  if (parsed_authority.ok()) {
    group.policy_authority = parsed_authority.value();
  }
  return group;
}

Facility::Facility(const std::filesystem::path& store_path, bool read_only, Tick initial_tick) {
  power_capacity::EngineOptions options;
  if (!store_path.empty()) {
    options.store_path = store_path;
  }
  options.read_only = read_only;
  options.create_if_missing = true;
  options.initial_tick = initial_tick;
  auto engine = power_capacity::CapacityEngine::open(options);
  if (!engine.ok()) {
    throw std::runtime_error("facility could not open an engine: " + engine.status().to_string());
  }
  engine_ = std::move(engine.value());
  as_of_ = initial_tick;
  const auto state = engine_->state();
  if (!state.ok()) {
    throw std::runtime_error("facility could not read state: " + state.status().to_string());
  }
  epoch_ = state.value()->epoch();
  incarnation_ = state.value()->incarnation();
}

std::shared_ptr<const power_capacity::CapacityState> Facility::require_state() const {
  const auto live = engine_->state();
  if (!live.ok()) {
    throw std::runtime_error("facility could not read state: " + live.status().to_string());
  }
  return live.value();
}

std::shared_ptr<const power_capacity::CapacityState> Facility::state() const {
  return require_state();
}

Generation Facility::generation() const { return require_state()->generation(); }

MutationContext Facility::context(std::optional<Generation> expected) const {
  MutationContext context;
  context.as_of = as_of_;
  context.epoch = epoch_;
  context.incarnation = incarnation_;
  context.expected_generation = expected.has_value() ? expected : require_state()->generation();
  return context;
}

void Facility::add_domain(const power_capacity::PowerDomain& domain) {
  const auto committed = engine_->put_domain(domain, PresenceExpectation::MustNotExist, context());
  if (!committed.ok()) {
    throw std::runtime_error("put_domain failed: " + committed.status().to_string());
  }
}

void Facility::update_domain(const power_capacity::PowerDomain& domain) {
  const auto committed = engine_->put_domain(domain, PresenceExpectation::MustExist, context());
  if (!committed.ok()) {
    throw std::runtime_error("put_domain (update) failed: " + committed.status().to_string());
  }
}

void Facility::remove_domain(const std::string& id) {
  const auto parsed = DomainId::parse(id, ResourceLimits::defaults().max_identifier_bytes);
  if (!parsed.ok()) {
    throw std::runtime_error("bad domain id in test");
  }
  const auto committed = engine_->erase_domain(parsed.value(), context());
  if (!committed.ok()) {
    throw std::runtime_error("erase_domain failed: " + committed.status().to_string());
  }
}

void Facility::add_load(const power_capacity::LoadRecord& load) {
  const auto committed = engine_->put_load(load, PresenceExpectation::MustNotExist, context());
  if (!committed.ok()) {
    throw std::runtime_error("put_load failed: " + committed.status().to_string());
  }
}

void Facility::remove_load(const std::string& id) {
  const auto parsed = LoadId::parse(id, ResourceLimits::defaults().max_identifier_bytes);
  if (!parsed.ok()) {
    throw std::runtime_error("bad load id in test");
  }
  const auto committed = engine_->erase_load(parsed.value(), context());
  if (!committed.ok()) {
    throw std::runtime_error("erase_load failed: " + committed.status().to_string());
  }
}

void Facility::add_group(const power_capacity::RedundancyGroup& group) {
  const auto committed = engine_->put_group(group, PresenceExpectation::MustNotExist, context());
  if (!committed.ok()) {
    throw std::runtime_error("put_group failed: " + committed.status().to_string());
  }
}

void Facility::remove_group(const std::string& id) {
  const auto parsed = GroupId::parse(id, ResourceLimits::defaults().max_identifier_bytes);
  if (!parsed.ok()) {
    throw std::runtime_error("bad group id in test");
  }
  const auto committed = engine_->erase_group(parsed.value(), context());
  if (!committed.ok()) {
    throw std::runtime_error("erase_group failed: " + committed.status().to_string());
  }
}

void Facility::advance_source(const std::string& source, std::uint64_t generation) {
  const auto parsed = SourceId::parse(source, ResourceLimits::defaults().max_identifier_bytes);
  if (!parsed.ok()) {
    throw std::runtime_error("bad source id in test");
  }
  MutationContext mutation = context();
  const Generation current = require_state()->find_source_generation(parsed.value())
                                 .value_or(Generation(0));
  mutation.expected_source_generations[parsed.value()] = current;
  const auto committed = engine_->advance_source_generation(parsed.value(), Generation(generation),
                                                            mutation);
  if (!committed.ok()) {
    throw std::runtime_error("advance_source_generation failed: " +
                             committed.status().to_string());
  }
}

power_capacity::DomainAssessment Facility::assess(const std::string& domain) {
  return assess(domain, as_of_);
}

power_capacity::DomainAssessment Facility::assess(const std::string& domain, Tick as_of) {
  const auto parsed = DomainId::parse(domain, ResourceLimits::defaults().max_identifier_bytes);
  if (!parsed.ok()) {
    throw std::runtime_error("bad domain id in test");
  }
  power_capacity::CapacityQuery query;
  query.domain = parsed.value();
  query.as_of = as_of;
  const auto assessment = engine_->assess(query);
  if (!assessment.ok()) {
    throw std::runtime_error("assess failed: " + assessment.status().to_string());
  }
  return assessment.value();
}

power_capacity::CandidateEvaluation Facility::evaluate(const std::string& load_id,
                                                       const std::string& domain,
                                                       std::int64_t watts) {
  return evaluate_batch({power_capacity::CandidateLoad{
                            PC_REQUIRE_OK(LoadId::parse(load_id, 128)),
                            PC_REQUIRE_OK(DomainId::parse(domain, 128)), power_capacity::LoadClass::Committed,
                            PC_REQUIRE_OK(Power::from_watts(watts))}})
      .front();
}

power_capacity::CandidateEvaluation Facility::evaluate_protected(const std::string& load_id,
                                                                 const std::string& domain,
                                                                 std::int64_t watts) {
  return evaluate_batch({power_capacity::CandidateLoad{
                            PC_REQUIRE_OK(LoadId::parse(load_id, 128)),
                            PC_REQUIRE_OK(DomainId::parse(domain, 128)), power_capacity::LoadClass::Protected,
                            PC_REQUIRE_OK(Power::from_watts(watts))}})
      .front();
}

std::vector<power_capacity::CandidateEvaluation> Facility::evaluate_batch(
    const std::vector<power_capacity::CandidateLoad>& candidates) {
  power_capacity::EvaluationContext context;
  context.as_of = as_of_;
  const auto evaluations = engine_->evaluate_batch(candidates, context);
  if (!evaluations.ok()) {
    throw std::runtime_error("evaluate_batch failed: " + evaluations.status().to_string());
  }
  return evaluations.value();
}

power_capacity::RevalidationOutcome Facility::revalidate(
    const power_capacity::AssessmentToken& token) {
  power_capacity::RevalidationRequest request;
  request.as_of = as_of_;
  request.epoch = epoch_;
  request.incarnation = incarnation_;
  const auto outcome = engine_->revalidate(token, request);
  if (!outcome.ok()) {
    throw std::runtime_error("revalidate failed: " + outcome.status().to_string());
  }
  return outcome.value();
}

std::string describe(const power_capacity::Status& status) { return status.to_string(); }

}  // namespace pc_test
