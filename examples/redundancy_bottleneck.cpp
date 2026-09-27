// Example: redundancy, degradation, and bottleneck analysis.
//
// Two UPS modules feed two PDUs under one busway. The busway is the shared
// upstream, the modules form an N+1 group, and one module is degraded. The
// example shows that the group's allocatable capacity is not the sum of its
// members, that the shared upstream is accounted once, and that the binding
// constraint is named explicitly.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "power_capacity/engine.hpp"

namespace {

using namespace power_capacity;

PowerDomain domain(const std::string& id, std::int64_t watts, const char* parent,
                   OperationalState state, StateCause cause, std::int32_t degradation_bp) {
  PowerDomain value;
  value.id = DomainId::parse(id, 128).value();
  value.nominal_capacity = Power::from_watts(watts).value();
  value.usable_capacity = value.nominal_capacity;
  if (parent != nullptr) {
    value.parent = DomainId::parse(parent, 128).value();
  }
  value.state = state;
  value.state_cause = cause;
  if (state == OperationalState::Degraded) {
    value.degradation_ratio = Ratio::from_basis_points(degradation_bp).value();
  }
  EvidenceRef evidence;
  evidence.id = EvidenceId::parse("ev.topology", 128).value();
  evidence.source = SourceId::parse("source.electrical", 128).value();
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000);
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  value.evidence = evidence;
  return value;
}

LoadRecord load(const std::string& id, const std::string& attached, std::int64_t watts) {
  LoadRecord value;
  value.id = LoadId::parse(id, 128).value();
  value.domain = DomainId::parse(attached, 128).value();
  value.load = Power::from_watts(watts).value();
  value.authority = AuthorityRef::parse("authority.reservation", 128).value();
  EvidenceRef evidence;
  evidence.id = EvidenceId::parse("ev.load", 128).value();
  evidence.source = SourceId::parse("source.electrical", 128).value();
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000);
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  value.evidence = evidence;
  return value;
}

}  // namespace

int main() {
  using namespace power_capacity;

  EngineOptions options;
  auto engine = CapacityEngine::open(options);
  if (!engine.ok()) {
    std::cerr << "open failed: " << engine.status().to_string() << "\n";
    return 1;
  }
  CapacityEngine& facility = *engine.value();

  MutationContext context;
  context.as_of = Tick(0);
  const auto commit = [&](const auto& record, PresenceExpectation presence) {
    context.expected_generation = facility.state().value()->generation();
    const auto result = facility.put_domain(record, presence, context);
    if (!result.ok()) {
      std::cerr << "put_domain failed: " << result.status().to_string() << "\n";
      std::exit(1);
    }
  };
  const auto commit_load = [&](const LoadRecord& record) {
    context.expected_generation = facility.state().value()->generation();
    const auto result = facility.put_load(record, PresenceExpectation::MustNotExist, context);
    if (!result.ok()) {
      std::cerr << "put_load failed: " << result.status().to_string() << "\n";
      std::exit(1);
    }
  };

  commit(domain("busway.main", 900'000, nullptr, OperationalState::Available, StateCause::None, 0),
         PresenceExpectation::MustNotExist);
  commit(domain("ups.a", 600'000, "busway.main", OperationalState::Available, StateCause::None, 0),
         PresenceExpectation::MustNotExist);
  commit(domain("ups.b", 600'000, "busway.main", OperationalState::Degraded, StateCause::Fault, 5000),
         PresenceExpectation::MustNotExist);
  commit(domain("pdu.a", 500'000, "ups.a", OperationalState::Available, StateCause::None, 0),
         PresenceExpectation::MustNotExist);
  commit(domain("pdu.b", 500'000, "ups.b", OperationalState::Available, StateCause::None, 0),
         PresenceExpectation::MustNotExist);

  RedundancyGroup group;
  group.id = GroupId::parse("group.ups", 128).value();
  group.members = {DomainId::parse("ups.a", 128).value(), DomainId::parse("ups.b", 128).value()};
  group.required_simultaneous_failures = 1;
  group.declared_class = RedundancyClass::NPlusOne;
  group.policy_authority = AuthorityRef::parse("authority.policy.redundancy", 128).value();
  {
    EvidenceRef evidence;
    evidence.id = EvidenceId::parse("ev.redundancy", 128).value();
    evidence.source = SourceId::parse("source.policy", 128).value();
    evidence.observed_at = Tick(0);
    evidence.valid_until = Tick(1000000);
    evidence.revision = Revision(1);
    evidence.source_generation = Generation(1);
    group.evidence = evidence;
  }
  context.expected_generation = facility.state().value()->generation();
  const auto committed = facility.put_group(group, PresenceExpectation::MustNotExist, context);
  if (!committed.ok()) {
    std::cerr << "put_group failed: " << committed.status().to_string() << "\n";
    return 1;
  }

  commit_load(load("load.tenant.a", "pdu.a", 200'000));
  commit_load(load("load.tenant.b", "pdu.b", 100'000));

  CapacityQuery query;
  query.domain = DomainId::parse("pdu.a", 128).value();
  query.as_of = Tick(10);
  const auto assessment = facility.assess(query);
  if (!assessment.ok()) {
    std::cerr << "assess failed: " << assessment.status().to_string() << "\n";
    return 1;
  }

  std::cout << "path from the queried domain to the root:\n";
  for (const PathNode& node : assessment.value().path) {
    std::cout << "  depth " << node.depth << "  " << node.domain.value() << "  own "
              << node.derivation.allocatable_headroom.to_watts_string() << " W  effective "
              << node.effective_headroom.to_watts_string() << " W  reason "
              << to_string(node.reason) << "\n";
  }
  std::cout << "\nallocatable at the queried domain : "
            << assessment.value().derivation->allocatable_headroom.to_watts_string() << " W\n";
  std::cout << "effective after its group cap   : "
            << assessment.value().effective_allocatable_headroom->to_watts_string() << " W\n";
  std::cout << "binding constraint               : "
            << assessment.value().bottleneck->value() << " at "
            << assessment.value().bottleneck_headroom->to_watts_string() << " W\n";
  std::cout << "path contains a degraded domain  : "
            << (assessment.value().path_degraded ? "true" : "false") << "\n";

  std::cout << "\ncandidate loads against " << query.domain.value() << ":\n";
  std::vector<CandidateLoad> candidates;
  for (const std::int64_t watts : {std::int64_t{100'000}, std::int64_t{300'000}}) {
    CandidateLoad candidate;
    candidate.id = LoadId::parse("candidate." + std::to_string(watts), 128).value();
    candidate.domain = query.domain;
    candidate.load = Power::from_watts(watts).value();
    candidates.push_back(candidate);
  }
  EvaluationContext evaluation_context;
  evaluation_context.as_of = Tick(10);
  const auto evaluations = facility.evaluate_batch(candidates, evaluation_context);
  if (!evaluations.ok()) {
    std::cerr << "evaluate failed: " << evaluations.status().to_string() << "\n";
    return 1;
  }
  for (const CandidateEvaluation& evaluation : evaluations.value()) {
    std::cout << "  " << evaluation.requested.to_watts_string() << " W -> "
              << to_string(evaluation.verdict) << " (" << to_string(evaluation.reason) << "): "
              << evaluation.explanation << "\n";
  }

  // The group is at its tolerance limit because one member is degraded to the
  // point where it can no longer carry the group's load, so the refusal names the
  // obligation rather than the arithmetic.
  const GroupDerivation& derivation = *facility.state()
                                           .value()
                                           ->group_derivation_of(
                                               GroupId::parse("group.ups", 128).value());
  std::cout << "\ngroup arithmetic:\n";
  std::cout << "  member carrying ability total : "
            << derivation.member_carryable_total.to_watts_string() << " W\n";
  std::cout << "  tolerated simultaneous loss   : "
            << derivation.tolerated_loss.to_watts_string() << " W\n";
  std::cout << "  survivable capacity           : "
            << derivation.survivable_capacity.to_watts_string() << " W\n";
  std::cout << "  effective headroom            : "
            << derivation.effective_headroom.to_watts_string() << " W\n";
  if (derivation.shared_upstream_limiter.has_value()) {
    std::cout << "  shared upstream limiter       : "
              << derivation.shared_upstream_limiter->value() << " at "
              << derivation.shared_upstream_headroom->to_watts_string() << " W\n";
  }
  std::cout << "  reason                        : " << to_string(derivation.reason) << "\n";
  return 0;
}
