#include "power_capacity/engine.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "detail/digest.hpp"

namespace power_capacity {

const char* to_string(EngineLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case EngineLifecycle::RecoveredPendingRevalidation:
      return "recovered_pending_revalidation";
    case EngineLifecycle::Current:
      return "current";
    case EngineLifecycle::Closed:
      return "closed";
  }
  return "closed";
}

const char* to_string(CandidateVerdict verdict) noexcept {
  switch (verdict) {
    case CandidateVerdict::Admissible:
      return "admissible";
    case CandidateVerdict::Refused:
      return "refused";
    case CandidateVerdict::Indeterminate:
      return "indeterminate";
  }
  return "indeterminate";
}

const char* to_string(RevalidationVerdict verdict) noexcept {
  switch (verdict) {
    case RevalidationVerdict::StillValid:
      return "still_valid";
    case RevalidationVerdict::Superseded:
      return "superseded";
    case RevalidationVerdict::Indeterminate:
      return "indeterminate";
  }
  return "indeterminate";
}

AssessmentToken DomainAssessment::token() const {
  AssessmentToken issued;
  issued.store = store;
  issued.session = session;
  issued.generation = generation;
  issued.epoch = epoch;
  issued.incarnation = incarnation;
  issued.as_of = as_of;
  issued.domain = domain;
  issued.digest = digest;
  return issued;
}

struct CapacityEngine::Impl {
  mutable std::mutex mutation_mutex;
  /// Guards the accepted authority binding. Lock order: `mutation_mutex` is
  /// always acquired before `binding_mutex`, and `binding_mutex` is never held
  /// while taking any other lock.
  mutable std::mutex binding_mutex;
  Epoch bound_epoch;
  Incarnation bound_incarnation;

  std::atomic<std::shared_ptr<const CapacityState>> published;
  std::atomic<EngineLifecycle> lifecycle{EngineLifecycle::RecoveredPendingRevalidation};

  std::shared_ptr<CapacityStore> store;
  EngineOptions options;
  SessionId session;
  Tick qualified_at;
  std::uint64_t local_attempt = 0;

  void bind(Epoch epoch, Incarnation incarnation) {
    const std::lock_guard<std::mutex> guard(binding_mutex);
    bound_epoch = epoch;
    bound_incarnation = incarnation;
  }

  EngineLifecycle current_lifecycle() const noexcept { return lifecycle.load(); }

  std::shared_ptr<const CapacityState> snapshot() const noexcept { return published.load(); }

  ReasonCode evidence_reason(EvidenceState state) const noexcept {
    switch (state) {
      case EvidenceState::Missing:
        return ReasonCode::EvidenceMissing;
      case EvidenceState::NotYetValid:
        return ReasonCode::EvidenceFromFuture;
      case EvidenceState::Stale:
        return ReasonCode::EvidenceStale;
      case EvidenceState::Current:
        break;
    }
    return ReasonCode::Ok;
  }

  static bool unavailable_reason(ReasonCode reason) noexcept {
    switch (reason) {
      case ReasonCode::DomainUnavailableMaintenance:
      case ReasonCode::DomainUnavailableFault:
      case ReasonCode::DomainUnavailableDecommissioned:
      case ReasonCode::DomainUnavailableUnknown:
        return true;
      default:
        return false;
    }
  }

  DomainAssessment assess_domain(const CapacityState& state, const DomainId& id, Tick as_of,
                                 Epoch epoch, Incarnation incarnation) const;
  CandidateEvaluation evaluate_one(const CapacityState& state, const CandidateLoad& candidate,
                                   Tick as_of, std::map<DomainId, Power>& extra) const;

  template <typename Fn>
  Result<CommitResult> apply(const MutationContext& context, Fn&& mutate);
};

namespace {

void update_digest(detail::Digest64& digest, const CapacityDerivation& derivation) {
  digest.update_i64(derivation.nominal.milliwatts());
  digest.update_i64(derivation.usable.milliwatts());
  digest.update_i64(derivation.derated.milliwatts());
  digest.update_i64(derivation.operational.milliwatts());
  digest.update_i64(derivation.reserve.milliwatts());
  digest.update_i64(derivation.protected_load.milliwatts());
  digest.update_i64(derivation.committed_load.milliwatts());
  digest.update_i64(derivation.safe_capacity.milliwatts());
  digest.update_i64(derivation.carryable_capacity.milliwatts());
  digest.update_i64(derivation.allocatable_headroom.milliwatts());
  digest.update_u64(static_cast<std::uint64_t>(derivation.reason));
  digest.update_bool(derivation.reserve_clamped);
  digest.update_bool(derivation.protected_clamped);
  digest.update_bool(derivation.committed_clamped);
  digest.update_bool(derivation.load_on_unavailable);
  digest.update_bool(derivation.degraded);
}

std::uint64_t compute_digest(const CapacityState& state, const DomainAssessment& assessment) {
  detail::Digest64 digest;
  digest.update_text(state.store_identity().to_hex());
  digest.update_u64(state.generation().value());
  digest.update_u64(assessment.epoch.value());
  digest.update_u64(assessment.incarnation.value());
  digest.update_text(assessment.domain.value());
  digest.update_i64(assessment.as_of.value());
  digest.update_bool(assessment.known);
  digest.update_u64(static_cast<std::uint64_t>(assessment.reason));
  if (assessment.derivation.has_value()) {
    update_digest(digest, *assessment.derivation);
  }
  digest.update_bool(assessment.redundancy_group.has_value());
  if (assessment.redundancy_group.has_value()) {
    digest.update_text(assessment.redundancy_group->value());
  }
  digest.update_bool(assessment.group_effective_headroom.has_value());
  if (assessment.group_effective_headroom.has_value()) {
    digest.update_i64(assessment.group_effective_headroom->milliwatts());
  }
  digest.update_bool(assessment.effective_allocatable_headroom.has_value());
  if (assessment.effective_allocatable_headroom.has_value()) {
    digest.update_i64(assessment.effective_allocatable_headroom->milliwatts());
  }
  digest.update_bool(assessment.bottleneck.has_value());
  if (assessment.bottleneck.has_value()) {
    digest.update_text(assessment.bottleneck->value());
  }
  digest.update_bool(assessment.bottleneck_headroom.has_value());
  if (assessment.bottleneck_headroom.has_value()) {
    digest.update_i64(assessment.bottleneck_headroom->milliwatts());
  }
  digest.update_bool(assessment.path_degraded);
  digest.update_u64(static_cast<std::uint64_t>(assessment.path.size()));
  for (const PathNode& node : assessment.path) {
    digest.update_text(node.domain.value());
    digest.update_u64(node.depth);
    update_digest(digest, node.derivation);
    digest.update_bool(node.redundancy_group.has_value());
    if (node.redundancy_group.has_value()) {
      digest.update_text(node.redundancy_group->value());
    }
    digest.update_i64(node.effective_headroom.milliwatts());
  }
  digest.update_u64(static_cast<std::uint64_t>(assessment.evidence.size()));
  for (const EvidenceRef& evidence : assessment.evidence) {
    digest.update_text(evidence.id.value());
    digest.update_text(evidence.source.value());
    digest.update_i64(evidence.observed_at.value());
    digest.update_i64(evidence.valid_until.value());
    digest.update_u64(evidence.revision.value());
  }
  return digest.value();
}

}  // namespace

DomainAssessment CapacityEngine::Impl::assess_domain(const CapacityState& state, const DomainId& id,
                                                     Tick as_of, Epoch epoch,
                                                     Incarnation incarnation) const {
  DomainAssessment assessment;
  assessment.domain = id;
  assessment.store = state.store_identity();
  assessment.session = session;
  assessment.as_of = as_of;
  assessment.epoch = epoch;
  assessment.incarnation = incarnation;
  assessment.generation = state.generation();

  const PowerDomain* domain = state.find_domain(id);
  if (domain == nullptr) {
    assessment.known = false;
    assessment.reason = ReasonCode::DomainNotFound;
    assessment.digest = compute_digest(state, assessment);
    return assessment;
  }

  const std::vector<DomainId> path = state.ancestry_of(id);
  assessment.depth = state.depth_of(id);

  // Evidence freshness is evaluated from the queried domain outward. The first
  // node that cannot justify a value decides the reason, so the reported reason
  // is always the one closest to the question that was asked.
  for (const DomainId& node : path) {
    const PowerDomain* node_domain = state.find_domain(node);
    const EvidenceState evidence_state = evaluate_evidence(node_domain->evidence, as_of);
    if (evidence_state != EvidenceState::Current) {
      assessment.known = false;
      assessment.reason = evidence_reason(evidence_state);
      assessment.digest = compute_digest(state, assessment);
      return assessment;
    }
    assessment.evidence.push_back(*node_domain->evidence);
  }

  bool path_degraded = false;
  Power lowest = Power(0);
  std::size_t lowest_index = 0;
  bool have_lowest = false;

  for (std::size_t index = 0; index < path.size(); ++index) {
    const DomainId& node = path[index];
    const CapacityDerivation* derivation = state.derivation_of(node);
    PathNode path_node;
    path_node.domain = node;
    path_node.depth = static_cast<std::uint32_t>(index);
    path_node.derivation = *derivation;
    path_node.reason = derivation->reason;
    path_node.redundancy_group = state.group_of(node);

    Power effective = derivation->allocatable_headroom;
    if (path_node.redundancy_group.has_value()) {
      const GroupDerivation* group = state.group_derivation_of(*path_node.redundancy_group);
      path_node.group_effective_headroom = group->effective_headroom;
      effective = min_power(effective, group->effective_headroom);
    }
    path_node.effective_headroom = effective;

    if (derivation->degraded) {
      path_degraded = true;
    }
    if (!have_lowest || effective < lowest ||
        (effective == lowest && index > lowest_index)) {
      lowest = effective;
      lowest_index = index;
      have_lowest = true;
    }
    assessment.path.push_back(std::move(path_node));
  }

  assessment.known = true;
  assessment.derivation = *state.derivation_of(id);
  assessment.reason = assessment.derivation->reason;
  assessment.redundancy_group = state.group_of(id);
  if (assessment.redundancy_group.has_value()) {
    const GroupDerivation* group = state.group_derivation_of(*assessment.redundancy_group);
    assessment.group_effective_headroom = group->effective_headroom;
  }
  assessment.effective_allocatable_headroom = lowest;
  assessment.bottleneck = path[lowest_index];
  assessment.bottleneck_headroom = lowest;
  assessment.path_degraded = path_degraded;
  assessment.digest = compute_digest(state, assessment);
  return assessment;
}

CandidateEvaluation CapacityEngine::Impl::evaluate_one(const CapacityState& state,
                                                       const CandidateLoad& candidate, Tick as_of,
                                                       std::map<DomainId, Power>& extra) const {
  CandidateEvaluation evaluation;
  evaluation.id = candidate.id;
  evaluation.domain = candidate.domain;
  evaluation.load_class = candidate.load_class;
  evaluation.requested = candidate.load;

  const PowerDomain* domain = state.find_domain(candidate.domain);
  if (domain == nullptr) {
    evaluation.verdict = CandidateVerdict::Refused;
    evaluation.reason = ReasonCode::DomainNotFound;
    evaluation.cause = StatusCode::NotFound;
    evaluation.explanation = "domain '" + candidate.domain.value() + "' is not in the model";
    return evaluation;
  }
  if (state.find_load(candidate.id) != nullptr) {
    evaluation.verdict = CandidateVerdict::Refused;
    evaluation.reason = ReasonCode::LoadAlreadyCommitted;
    evaluation.cause = StatusCode::AlreadyExists;
    evaluation.explanation = "load '" + candidate.id.value() + "' is already committed";
    return evaluation;
  }

  const std::vector<DomainId> path = state.ancestry_of(candidate.domain);

  for (const DomainId& node : path) {
    const PowerDomain* node_domain = state.find_domain(node);
    const EvidenceState evidence_state = evaluate_evidence(node_domain->evidence, as_of);
    if (evidence_state != EvidenceState::Current) {
      evaluation.verdict = CandidateVerdict::Indeterminate;
      evaluation.reason = evidence_reason(evidence_state);
      evaluation.cause = evidence_state == EvidenceState::NotYetValid
                             ? StatusCode::Indeterminate
                             : (evidence_state == EvidenceState::Missing ? StatusCode::EvidenceMissing
                                                                        : StatusCode::EvidenceStale);
      evaluation.bottleneck = node;
      evaluation.explanation = "domain '" + node.value() + "' evidence is " +
                               to_string(evidence_state) + " at instant " +
                               std::to_string(as_of.value());
      return evaluation;
    }
  }

  for (const DomainId& node : path) {
    const CapacityDerivation* derivation = state.derivation_of(node);
    if (unavailable_reason(derivation->reason)) {
      evaluation.verdict = CandidateVerdict::Refused;
      evaluation.reason = derivation->reason;
      evaluation.cause = StatusCode::Unavailable;
      evaluation.bottleneck = node;
      evaluation.explanation = "domain '" + node.value() + "' is unavailable (" +
                               to_string(derivation->reason) + ")";
      return evaluation;
    }
  }

  bool path_degraded = false;
  for (const DomainId& node : path) {
    const CapacityDerivation* derivation = state.derivation_of(node);
    if (derivation->degraded) {
      path_degraded = true;
    }
    const std::optional<GroupId> group_id = state.group_of(node);
    if (!group_id.has_value()) {
      continue;
    }
    const GroupDerivation* group = state.group_derivation_of(*group_id);
    if (group->tolerance_exceeded || (group->tolerance_consumed && group->tolerance > 0)) {
      evaluation.verdict = CandidateVerdict::Refused;
      evaluation.reason = group->tolerance_exceeded ? ReasonCode::RedundancyToleranceExceeded
                                                    : ReasonCode::RedundancyToleranceAtLimit;
      evaluation.cause = StatusCode::RedundancyViolated;
      evaluation.bottleneck = node;
      evaluation.redundancy_limited = true;
      evaluation.explanation = "redundancy group '" + group_id->value() + "' has " +
                               std::to_string(group->unavailable_members) +
                               " unavailable members against a stated tolerance of " +
                               std::to_string(group->tolerance);
      return evaluation;
    }
  }

  Power lowest = Power(0);
  DomainId lowest_node;
  std::size_t lowest_index = 0;
  bool lowest_is_group = false;
  bool have_lowest = false;

  for (std::size_t index = 0; index < path.size(); ++index) {
    const DomainId& node = path[index];
    Power own = state.derivation_of(node)->allocatable_headroom;
    const auto extra_at_node = extra.find(node);
    if (extra_at_node != extra.end()) {
      own = subtract_power_clamped(own, extra_at_node->second).value;
    }
    Power effective = own;
    bool group_limited = false;
    const std::optional<GroupId> group_id = state.group_of(node);
    if (group_id.has_value()) {
      Power group_headroom = state.group_derivation_of(*group_id)->effective_headroom;
      // A domain belongs to at most one group and group members are pairwise
      // independent, so at most one member of a group can lie on this path. The
      // group's headroom therefore falls by exactly the extra load attributed to
      // that member, and never twice for one load.
      if (extra_at_node != extra.end()) {
        group_headroom = subtract_power_clamped(group_headroom, extra_at_node->second).value;
      }
      if (group_headroom < effective) {
        effective = group_headroom;
        group_limited = true;
      }
    }
    if (!have_lowest || effective < lowest || (effective == lowest && index > lowest_index)) {
      lowest = effective;
      lowest_node = node;
      lowest_index = index;
      lowest_is_group = group_limited;
      have_lowest = true;
    }
  }

  evaluation.bottleneck = lowest_node;
  evaluation.redundancy_limited = lowest_is_group;

  if (lowest < candidate.load) {
    evaluation.verdict = CandidateVerdict::Refused;
    evaluation.reason = ReasonCode::InsufficientHeadroom;
    evaluation.cause = StatusCode::CapacityExceeded;
    evaluation.remaining_at_bottleneck = lowest;
    evaluation.explanation =
        "requested " + candidate.load.to_watts_string() + " W but only " +
        lowest.to_watts_string() + " W is available at '" + lowest_node.value() + "'" +
        (lowest_is_group ? " (limited by its redundancy group)" : "");
    return evaluation;
  }

  for (const DomainId& node : path) {
    auto& accumulated = extra[node];
    const Result<Power> total = add_power(accumulated, candidate.load);
    if (!total.ok() || total.value().milliwatts() > options.limits.max_aggregate_milliwatts) {
      // Saturate the overlay so that every later candidate in the batch is
      // refused rather than evaluated against an unrepresentable projection.
      accumulated = Power(options.limits.max_aggregate_milliwatts);
    } else {
      accumulated = total.value();
    }
  }

  evaluation.verdict = CandidateVerdict::Admissible;
  evaluation.remaining_at_bottleneck =
      subtract_power_clamped(lowest, candidate.load).value;
  if (path_degraded) {
    evaluation.reason = ReasonCode::PathContainsDegradedDomain;
  } else if (lowest_node != candidate.domain) {
    evaluation.reason = ReasonCode::BottleneckUpstream;
  } else {
    evaluation.reason = ReasonCode::Admissible;
  }
  evaluation.explanation = "fits; binding constraint is '" + lowest_node.value() + "' with " +
                           lowest.to_watts_string() + " W available" +
                           (lowest_is_group ? " (limited by its redundancy group)" : "");
  return evaluation;
}

template <typename Fn>
Result<CommitResult> CapacityEngine::Impl::apply(const MutationContext& context, Fn&& mutate) {
  const std::lock_guard<std::mutex> guard(mutation_mutex);

  const EngineLifecycle current = lifecycle.load();
  if (current == EngineLifecycle::Closed) {
    return Status::error(StatusCode::Closed, "engine is closed");
  }
  if (current != EngineLifecycle::Current) {
    return Status::error(StatusCode::NotRevalidated,
                         "engine state has not been revalidated against current evidence and "
                         "authority; call revalidate_recovered_state first");
  }
  if (options.read_only) {
    return Status::error(StatusCode::ReadOnly, "engine was opened read-only");
  }
  if (context.as_of.value() < 0) {
    return Status::error(StatusCode::InvalidArgument, "mutation instant must not be negative");
  }
  if (!context.expected_generation.has_value()) {
    return Status::error(StatusCode::InvalidArgument,
                         "every mutation must carry an explicit expected_generation precondition");
  }

  const std::shared_ptr<const CapacityState> snapshot = published.load();
  if (!snapshot) {
    return Status::error(StatusCode::Closed, "engine has no published state");
  }

  Epoch bound_epoch_value;
  Incarnation bound_incarnation_value;
  {
    const std::lock_guard<std::mutex> binding_guard(binding_mutex);
    bound_epoch_value = bound_epoch;
    bound_incarnation_value = bound_incarnation;
  }
  if (context.epoch != bound_epoch_value || context.incarnation != bound_incarnation_value) {
    return Status::error(StatusCode::StaleAuthority,
                         "mutation cites epoch " + std::to_string(context.epoch.value()) +
                             " incarnation " + std::to_string(context.incarnation.value()) +
                             " but this engine is bound to epoch " +
                             std::to_string(bound_epoch_value.value()) + " incarnation " +
                             std::to_string(bound_incarnation_value.value()));
  }
  // An idempotent replay is answered before the generation precondition is
  // compared. A caller that retries an operation whose response was lost still
  // cites the generation it read before the first attempt, which is by then
  // stale; refusing the retry would make the key useless for exactly the case it
  // exists for. The authority binding and the presence of an explicit
  // precondition are still required, and the replay never touches the model.
  if (context.idempotency_key.has_value()) {
    for (const AppliedOperation& operation : snapshot->applied_operations()) {
      if (operation.key == *context.idempotency_key) {
        CommitResult replay;
        replay.generation = operation.generation;
        replay.attempt = AttemptId(0);
        replay.already_applied = true;
        return replay;
      }
    }
  }

  if (context.expected_generation->value() != snapshot->generation().value()) {
    return Status::error(StatusCode::StaleGeneration,
                         "mutation expects generation " +
                             std::to_string(context.expected_generation->value()) +
                             " but the current generation is " +
                             std::to_string(snapshot->generation().value()));
  }
  if (context.as_of.value() < snapshot->created_at().value()) {
    return Status::error(StatusCode::InvalidArgument,
                         "mutation instant " + std::to_string(context.as_of.value()) +
                             " precedes the store creation instant " +
                             std::to_string(snapshot->created_at().value()));
  }

  for (const auto& entry : context.expected_source_generations) {
    const Generation current_source =
        snapshot->find_source_generation(entry.first).value_or(Generation(0));
    if (current_source.value() != entry.second.value()) {
      return Status::error(StatusCode::StaleSourceGeneration,
                           "source '" + entry.first.value() + "' was read at generation " +
                               std::to_string(entry.second.value()) +
                               " but the current generation is " +
                               std::to_string(current_source.value()));
    }
  }
  for (const auto& entry : context.advanced_source_generations) {
    const auto read = context.expected_source_generations.find(entry.first);
    if (read == context.expected_source_generations.end()) {
      return Status::error(StatusCode::InvalidArgument,
                           "source '" + entry.first.value() +
                               "' is advanced without being declared as read");
    }
    if (entry.second.value() <= read->second.value()) {
      return Status::error(StatusCode::InvalidArgument,
                           "source '" + entry.first.value() +
                               "' may only advance: the new generation " +
                               std::to_string(entry.second.value()) +
                               " is not above the generation read " +
                               std::to_string(read->second.value()));
    }
  }

  CapacityContent content = snapshot->content();
  const Status mutated = mutate(content);
  if (!mutated.ok()) {
    return mutated;
  }

  content.generation = Generation(snapshot->generation().value() + 1);
  // The recorded instant is monotonic: an older caller instant never moves the
  // audit trail backwards.
  if (context.as_of.value() > snapshot->updated_at().value()) {
    content.updated_at = context.as_of;
  }
  for (const auto& entry : context.advanced_source_generations) {
    content.source_generations[entry.first] = entry.second;
  }
  if (context.idempotency_key.has_value()) {
    AppliedOperation operation;
    operation.key = *context.idempotency_key;
    operation.generation = content.generation;
    content.applied_operations.push_back(std::move(operation));
    while (content.applied_operations.size() > options.limits.max_applied_operations) {
      content.applied_operations.erase(content.applied_operations.begin());
    }
  }

  const Result<std::shared_ptr<const CapacityState>> built =
      CapacityState::build(std::move(content), options.limits);
  if (!built.ok()) {
    return built.status();
  }

  CommitResult result;
  result.generation = built.value()->generation();
  result.already_applied = false;
  result.attempt = AttemptId(++local_attempt);
  if (store) {
    const Result<CommitResult> committed = store->commit(*built.value());
    if (!committed.ok()) {
      return committed.status();
    }
    result.attempt = committed.value().attempt;
    result.metrics = committed.value().metrics;
  }
  // The engine publishes only after the durable commit succeeded, so the
  // in-memory state never runs ahead of the store.
  published.store(built.value());
  return result;
}

CapacityEngine::~CapacityEngine() { (void)close(); }

Result<std::unique_ptr<CapacityEngine>> CapacityEngine::open(const EngineOptions& options) {
  auto impl = std::make_shared<Impl>();
  impl->options = options;
  impl->session = SessionId::generate();

  bool created = false;
  if (options.store_path.has_value()) {
    StoreOpenOptions store_options;
    store_options.path = *options.store_path;
    store_options.access = options.read_only ? StoreAccess::ReadOnly : StoreAccess::ReadWrite;
    store_options.create_if_missing = options.create_if_missing && !options.read_only;
    store_options.expected_store_identity = options.expected_store_identity;
    store_options.minimum_generation = options.minimum_generation;
    store_options.enforce_path_binding = options.enforce_path_binding;
    store_options.created_at = options.initial_tick;
    store_options.epoch = options.epoch;
    store_options.incarnation = options.incarnation;
    store_options.limits = options.limits;

    const Result<std::shared_ptr<CapacityStore>> store = CapacityStore::open(store_options);
    if (!store.ok()) {
      return store.status();
    }
    impl->store = store.value();
    created = store.value()->info().created;

    // The store already read and described one exact snapshot; reusing it means the
    // published state and the store metadata can never disagree about the generation.
    const std::shared_ptr<const CapacityState>& state = store.value()->opened_state();
    if (!state) {
      return Status::error(StatusCode::InvariantViolation,
                           "store reported success without reading a state");
    }
    impl->published.store(state);
    impl->bind(state->epoch(), state->incarnation());
    impl->qualified_at = state->last_revalidated_at();
  } else {
    CapacityContent content;
    content.store_identity = StoreIdentity::generate();
    content.generation = Generation(1);
    content.epoch = options.epoch;
    content.incarnation = options.incarnation;
    content.created_at = options.initial_tick;
    content.updated_at = options.initial_tick;
    content.last_revalidated_at = options.initial_tick;
    const Result<std::shared_ptr<const CapacityState>> state =
        CapacityState::build(std::move(content), options.limits);
    if (!state.ok()) {
      return state.status();
    }
    impl->published.store(state.value());
    impl->bind(options.epoch, options.incarnation);
    impl->qualified_at = options.initial_tick;
    created = true;
  }

  // Only state recovered from an existing store needs qualification. A store
  // this engine just created, and an in-memory engine, have no recovered
  // evidence to revalidate.
  impl->lifecycle.store(created ? EngineLifecycle::Current
                                : EngineLifecycle::RecoveredPendingRevalidation);

  auto engine = std::unique_ptr<CapacityEngine>(new CapacityEngine());
  engine->impl_ = std::move(impl);
  return engine;
}

EngineLifecycle CapacityEngine::lifecycle() const noexcept {
  return impl_ ? impl_->current_lifecycle() : EngineLifecycle::Closed;
}

Result<EngineStatus> CapacityEngine::status() const {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  EngineStatus status;
  status.lifecycle = impl_->current_lifecycle();
  status.durable = impl_->store != nullptr;
  status.read_only = impl_->options.read_only;
  status.session = impl_->session;
  {
    const std::lock_guard<std::mutex> guard(impl_->binding_mutex);
    status.epoch = impl_->bound_epoch;
    status.incarnation = impl_->bound_incarnation;
  }
  status.qualified_at = impl_->qualified_at;
  if (impl_->store) {
    status.store = impl_->store->info();
  }
  const std::shared_ptr<const CapacityState> snapshot = impl_->snapshot();
  if (snapshot) {
    status.domain_count = snapshot->domain_count();
    status.load_count = snapshot->load_count();
    status.group_count = snapshot->group_count();
    status.source_count = snapshot->source_count();
    status.applied_operation_count = snapshot->applied_operations().size();
  }
  return status;
}

Result<RevalidationReport> CapacityEngine::revalidate_recovered_state(
    const RevalidationRequest& request) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  const std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  const EngineLifecycle current = impl_->current_lifecycle();
  if (current == EngineLifecycle::Closed) {
    return Status::error(StatusCode::Closed, "engine is closed");
  }
  if (request.as_of.value() < 0) {
    return Status::error(StatusCode::InvalidArgument, "revalidation instant must not be negative");
  }

  const std::shared_ptr<const CapacityState> snapshot = impl_->snapshot();
  if (!snapshot) {
    return Status::error(StatusCode::Closed, "engine has no published state");
  }
  {
    const std::lock_guard<std::mutex> binding_guard(impl_->binding_mutex);
    if (request.epoch < impl_->bound_epoch ||
        (request.epoch == impl_->bound_epoch && request.incarnation < impl_->bound_incarnation)) {
      return Status::error(StatusCode::StaleAuthority,
                           "revalidation cites epoch " + std::to_string(request.epoch.value()) +
                               " incarnation " + std::to_string(request.incarnation.value()) +
                               " which is behind the bound epoch " +
                               std::to_string(impl_->bound_epoch.value()) + " incarnation " +
                               std::to_string(impl_->bound_incarnation.value()));
    }
  }
  if (request.expected_generation.has_value() &&
      request.expected_generation->value() != snapshot->generation().value()) {
    return Status::error(StatusCode::StaleGeneration,
                         "revalidation expects generation " +
                             std::to_string(request.expected_generation->value()) +
                             " but the current generation is " +
                             std::to_string(snapshot->generation().value()));
  }

  RevalidationReport report;
  report.as_of = request.as_of;
  report.generation = snapshot->generation();
  report.epoch = request.epoch;
  report.incarnation = request.incarnation;
  report.domains_total = snapshot->domain_count();
  for (const auto& entry : snapshot->domains()) {
    const EvidenceState evidence_state = evaluate_evidence(entry.second.evidence, request.as_of);
    switch (evidence_state) {
      case EvidenceState::Current:
        ++report.domains_current;
        break;
      case EvidenceState::Missing:
        ++report.domains_missing_evidence;
        if (report.missing_evidence_domains.size() < request.max_reported_domains) {
          report.missing_evidence_domains.push_back(entry.first);
        }
        break;
      case EvidenceState::Stale:
      case EvidenceState::NotYetValid:
        ++report.domains_stale;
        if (report.stale_domains.size() < request.max_reported_domains) {
          report.stale_domains.push_back(entry.first);
        }
        break;
    }
  }
  report.all_evidence_current = report.domains_stale == 0 && report.domains_missing_evidence == 0;

  // Re-qualifying under the authority binding the store already records cannot
  // change any authoritative value, so it does not commit and does not advance
  // the generation. A new binding does commit, because the persisted binding is
  // itself an authoritative fact. Evidence freshness is never persisted, so a
  // restarted process always has to qualify recovered state again.
  bool binding_changed = false;
  {
    const std::lock_guard<std::mutex> binding_guard(impl_->binding_mutex);
    binding_changed = request.epoch != impl_->bound_epoch ||
                      request.incarnation != impl_->bound_incarnation;
  }

  const bool writable = impl_->store && impl_->store->writable();
  if (writable && binding_changed) {
    CapacityContent content = snapshot->content();
    content.epoch = request.epoch;
    content.incarnation = request.incarnation;
    content.last_revalidated_at = request.as_of;
    content.updated_at = request.as_of.value() > content.updated_at.value()
                             ? request.as_of
                             : content.updated_at;
    content.generation = Generation(snapshot->generation().value() + 1);
    const Result<std::shared_ptr<const CapacityState>> built =
        CapacityState::build(std::move(content), impl_->options.limits);
    if (!built.ok()) {
      return built.status();
    }
    const Result<CommitResult> committed = impl_->store->commit(*built.value());
    if (!committed.ok()) {
      return committed.status();
    }
    impl_->published.store(built.value());
    report.generation = built.value()->generation();
    // committed_generation is reported through the bound state below.
  }

  impl_->bind(request.epoch, request.incarnation);
  impl_->qualified_at = request.as_of;
  impl_->lifecycle.store(EngineLifecycle::Current);
  return report;
}

Result<CommitResult> CapacityEngine::put_domain(const PowerDomain& domain,
                                                PresenceExpectation presence,
                                                const MutationContext& context) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  return impl_->apply(context, [&domain, presence](CapacityContent& content) -> Status {
    const bool exists = content.domains.find(domain.id) != content.domains.end();
    if (presence == PresenceExpectation::MustNotExist && exists) {
      return Status::error(StatusCode::AlreadyExists,
                           "domain '" + domain.id.value() + "' already exists");
    }
    if (presence == PresenceExpectation::MustExist && !exists) {
      return Status::error(StatusCode::NotFound,
                           "domain '" + domain.id.value() + "' does not exist");
    }
    if (domain.parent.has_value()) {
      if (*domain.parent == domain.id) {
        return Status::error(StatusCode::InvalidArgument,
                             "domain '" + domain.id.value() + "' cannot be its own parent");
      }
      if (content.domains.find(*domain.parent) == content.domains.end()) {
        return Status::error(StatusCode::NotFound,
                             "domain '" + domain.id.value() + "' cites unknown parent '" +
                                 domain.parent->value() + "'");
      }
    }
    content.domains[domain.id] = domain;
    return Status::success();
  });
}

Result<CommitResult> CapacityEngine::erase_domain(const DomainId& id,
                                                  const MutationContext& context) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  return impl_->apply(context, [&id](CapacityContent& content) -> Status {
    if (content.domains.find(id) == content.domains.end()) {
      return Status::error(StatusCode::NotFound, "domain '" + id.value() + "' does not exist");
    }
    for (const auto& entry : content.domains) {
      if (entry.second.parent.has_value() && *entry.second.parent == id) {
        return Status::error(StatusCode::Conflict,
                             "domain '" + id.value() + "' still contains domain '" +
                                 entry.first.value() + "'");
      }
    }
    for (const auto& entry : content.loads) {
      if (entry.second.domain == id) {
        return Status::error(StatusCode::Conflict,
                             "domain '" + id.value() + "' still carries load '" +
                                 entry.first.value() + "'");
      }
    }
    for (const auto& entry : content.groups) {
      for (const DomainId& member : entry.second.members) {
        if (member == id) {
          return Status::error(StatusCode::Conflict,
                               "domain '" + id.value() + "' is a member of redundancy group '" +
                                   entry.first.value() + "'");
        }
      }
    }
    content.domains.erase(id);
    return Status::success();
  });
}

Result<CommitResult> CapacityEngine::put_load(const LoadRecord& load, PresenceExpectation presence,
                                              const MutationContext& context) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  return impl_->apply(context, [&load, presence](CapacityContent& content) -> Status {
    const bool exists = content.loads.find(load.id) != content.loads.end();
    if (presence == PresenceExpectation::MustNotExist && exists) {
      return Status::error(StatusCode::AlreadyExists,
                           "load '" + load.id.value() + "' already exists");
    }
    if (presence == PresenceExpectation::MustExist && !exists) {
      return Status::error(StatusCode::NotFound, "load '" + load.id.value() + "' does not exist");
    }
    if (content.domains.find(load.domain) == content.domains.end()) {
      return Status::error(StatusCode::NotFound,
                           "load '" + load.id.value() + "' cites unknown domain '" +
                               load.domain.value() + "'");
    }
    content.loads[load.id] = load;
    return Status::success();
  });
}

Result<CommitResult> CapacityEngine::erase_load(const LoadId& id, const MutationContext& context) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  return impl_->apply(context, [&id](CapacityContent& content) -> Status {
    if (content.loads.find(id) == content.loads.end()) {
      return Status::error(StatusCode::NotFound, "load '" + id.value() + "' does not exist");
    }
    content.loads.erase(id);
    return Status::success();
  });
}

Result<CommitResult> CapacityEngine::put_group(const RedundancyGroup& group,
                                               PresenceExpectation presence,
                                               const MutationContext& context) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  return impl_->apply(context, [&group, presence](CapacityContent& content) -> Status {
    RedundancyGroup canonical = group;
    const Status canonicalized = canonicalize_group_members(canonical.members);
    if (!canonicalized.ok()) {
      return canonicalized;
    }
    const bool exists = content.groups.find(canonical.id) != content.groups.end();
    if (presence == PresenceExpectation::MustNotExist && exists) {
      return Status::error(StatusCode::AlreadyExists,
                           "redundancy group '" + canonical.id.value() + "' already exists");
    }
    if (presence == PresenceExpectation::MustExist && !exists) {
      return Status::error(StatusCode::NotFound,
                           "redundancy group '" + canonical.id.value() + "' does not exist");
    }
    for (const DomainId& member : canonical.members) {
      if (content.domains.find(member) == content.domains.end()) {
        return Status::error(StatusCode::NotFound,
                             "redundancy group '" + canonical.id.value() + "' cites unknown member '" +
                                 member.value() + "'");
      }
    }
    content.groups[canonical.id] = std::move(canonical);
    return Status::success();
  });
}

Result<CommitResult> CapacityEngine::erase_group(const GroupId& id,
                                                 const MutationContext& context) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  return impl_->apply(context, [&id](CapacityContent& content) -> Status {
    if (content.groups.find(id) == content.groups.end()) {
      return Status::error(StatusCode::NotFound,
                           "redundancy group '" + id.value() + "' does not exist");
    }
    content.groups.erase(id);
    return Status::success();
  });
}

Result<CommitResult> CapacityEngine::advance_source_generation(const SourceId& source,
                                                               Generation next,
                                                               const MutationContext& context) {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  return impl_->apply(context, [&source, next](CapacityContent& content) -> Status {
    const auto found = content.source_generations.find(source);
    const Generation current =
        found == content.source_generations.end() ? Generation(0) : found->second;
    if (next.value() <= current.value()) {
      return Status::error(StatusCode::InvalidArgument,
                           "source '" + source.value() + "' may only advance: the new generation " +
                               std::to_string(next.value()) + " is not above the current " +
                               std::to_string(current.value()));
    }
    content.source_generations[source] = next;
    return Status::success();
  });
}

Result<DomainAssessment> CapacityEngine::assess(const CapacityQuery& query) const {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  const EngineLifecycle current = impl_->current_lifecycle();
  if (current == EngineLifecycle::Closed) {
    return Status::error(StatusCode::Closed, "engine is closed");
  }
  if (current != EngineLifecycle::Current) {
    return Status::error(StatusCode::NotRevalidated,
                         "engine state has not been revalidated against current evidence and "
                         "authority; call revalidate_recovered_state first");
  }
  if (query.domain.empty()) {
    return Status::error(StatusCode::InvalidArgument, "query must name a domain");
  }
  if (query.as_of.value() < 0) {
    return Status::error(StatusCode::InvalidArgument, "query instant must not be negative");
  }
  Epoch bound_epoch;
  Incarnation bound_incarnation;
  {
    const std::lock_guard<std::mutex> guard(impl_->binding_mutex);
    bound_epoch = impl_->bound_epoch;
    bound_incarnation = impl_->bound_incarnation;
  }
  if (query.expected_epoch.has_value() && *query.expected_epoch != bound_epoch) {
    return Status::error(StatusCode::StaleAuthority,
                         "query expects epoch " +
                             std::to_string(query.expected_epoch->value()) +
                             " but this engine is bound to epoch " +
                             std::to_string(bound_epoch.value()));
  }
  if (query.expected_incarnation.has_value() && *query.expected_incarnation != bound_incarnation) {
    return Status::error(StatusCode::StaleAuthority,
                         "query expects incarnation " +
                             std::to_string(query.expected_incarnation->value()) +
                             " but this engine is bound to incarnation " +
                             std::to_string(bound_incarnation.value()));
  }
  const std::shared_ptr<const CapacityState> snapshot = impl_->snapshot();
  if (!snapshot) {
    return Status::error(StatusCode::Closed, "engine has no published state");
  }
  if (query.expected_generation.has_value() &&
      query.expected_generation->value() != snapshot->generation().value()) {
    return Status::error(StatusCode::StaleGeneration,
                         "query expects generation " +
                             std::to_string(query.expected_generation->value()) +
                             " but the current generation is " +
                             std::to_string(snapshot->generation().value()));
  }
  if (snapshot->find_domain(query.domain) == nullptr) {
    return Status::error(StatusCode::NotFound,
                         "domain '" + query.domain.value() + "' is not in the model");
  }
  return impl_->assess_domain(*snapshot, query.domain, query.as_of, bound_epoch, bound_incarnation);
}

Result<std::vector<CandidateEvaluation>> CapacityEngine::evaluate_batch(
    const std::vector<CandidateLoad>& candidates, const EvaluationContext& context) const {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  const EngineLifecycle current = impl_->current_lifecycle();
  if (current == EngineLifecycle::Closed) {
    return Status::error(StatusCode::Closed, "engine is closed");
  }
  if (current != EngineLifecycle::Current) {
    return Status::error(StatusCode::NotRevalidated,
                         "engine state has not been revalidated against current evidence and "
                         "authority; call revalidate_recovered_state first");
  }
  if (context.as_of.value() < 0) {
    return Status::error(StatusCode::InvalidArgument, "evaluation instant must not be negative");
  }
  if (candidates.size() > impl_->options.limits.max_loads) {
    return Status::error(StatusCode::LimitExceeded,
                         "candidate batch of " + std::to_string(candidates.size()) +
                             " exceeds the load limit");
  }
  std::set<LoadId> seen;
  for (const CandidateLoad& candidate : candidates) {
    if (candidate.id.empty()) {
      return Status::error(StatusCode::InvalidArgument, "candidate load must name an identity");
    }
    if (candidate.domain.empty()) {
      return Status::error(StatusCode::InvalidArgument,
                           "candidate load '" + candidate.id.value() + "' must name a domain");
    }
    if (candidate.load.is_zero() || candidate.load.is_negative()) {
      return Status::error(StatusCode::InvalidArgument,
                           "candidate load '" + candidate.id.value() +
                               "' must declare a strictly positive load");
    }
    if (candidate.load.milliwatts() > impl_->options.limits.max_component_milliwatts) {
      return Status::error(StatusCode::LimitExceeded,
                           "candidate load '" + candidate.id.value() +
                               "' exceeds the per-component limit");
    }
    if (!seen.insert(candidate.id).second) {
      return Status::error(StatusCode::DuplicateIdentity,
                           "candidate load '" + candidate.id.value() +
                               "' appears more than once in the batch");
    }
  }

  const std::shared_ptr<const CapacityState> snapshot = impl_->snapshot();
  if (!snapshot) {
    return Status::error(StatusCode::Closed, "engine has no published state");
  }
  if (context.expected_generation.has_value() &&
      context.expected_generation->value() != snapshot->generation().value()) {
    return Status::error(StatusCode::StaleGeneration,
                         "evaluation expects generation " +
                             std::to_string(context.expected_generation->value()) +
                             " but the current generation is " +
                             std::to_string(snapshot->generation().value()));
  }

  // Candidates are applied cumulatively to an overlay, so a batch answers
  // whether the whole set fits, not whether each candidate fits in isolation.
  // The overlay never mutates the committed state.
  std::map<DomainId, Power> extra;
  std::vector<CandidateEvaluation> results;
  results.reserve(candidates.size());
  for (const CandidateLoad& candidate : candidates) {
    results.push_back(impl_->evaluate_one(*snapshot, candidate, context.as_of, extra));
  }
  return results;
}

Result<CandidateEvaluation> CapacityEngine::evaluate(const CandidateLoad& candidate,
                                                     const EvaluationContext& context) const {
  const Result<std::vector<CandidateEvaluation>> batch =
      evaluate_batch(std::vector<CandidateLoad>{candidate}, context);
  if (!batch.ok()) {
    return batch.status();
  }
  return batch.value().front();
}

Result<RevalidationOutcome> CapacityEngine::revalidate(const AssessmentToken& token,
                                                       const RevalidationRequest& request) const {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  const EngineLifecycle current = impl_->current_lifecycle();
  if (current == EngineLifecycle::Closed) {
    return Status::error(StatusCode::Closed, "engine is closed");
  }
  if (current != EngineLifecycle::Current) {
    return Status::error(StatusCode::NotRevalidated,
                         "engine state has not been revalidated against current evidence and "
                         "authority; call revalidate_recovered_state first");
  }
  if (token.domain.empty()) {
    return Status::error(StatusCode::InvalidArgument, "token must name a domain");
  }
  if (request.as_of.value() < 0) {
    return Status::error(StatusCode::InvalidArgument, "revalidation instant must not be negative");
  }
  const std::shared_ptr<const CapacityState> snapshot = impl_->snapshot();
  if (!snapshot) {
    return Status::error(StatusCode::Closed, "engine has no published state");
  }
  Epoch bound_epoch;
  Incarnation bound_incarnation;
  {
    const std::lock_guard<std::mutex> guard(impl_->binding_mutex);
    bound_epoch = impl_->bound_epoch;
    bound_incarnation = impl_->bound_incarnation;
  }

  RevalidationOutcome outcome;
  if (token.store != snapshot->store_identity()) {
    outcome.verdict = RevalidationVerdict::Superseded;
    outcome.reason = ReasonCode::StoreIdentityMismatch;
    outcome.cause = StatusCode::StaleAuthority;
    outcome.explanation = "the token was issued against store " + token.store.to_hex() +
                          " but this engine holds " + snapshot->store_identity().to_hex();
    return outcome;
  }
  if (token.session != impl_->session) {
    outcome.verdict = RevalidationVerdict::Superseded;
    outcome.reason = ReasonCode::StoreReopened;
    outcome.cause = StatusCode::StaleAuthority;
    outcome.explanation = "the token was issued by a previous engine session";
    return outcome;
  }
  if (token.epoch != bound_epoch || token.incarnation != bound_incarnation) {
    outcome.verdict = RevalidationVerdict::Superseded;
    outcome.reason = ReasonCode::AuthorityEpochChanged;
    outcome.cause = StatusCode::StaleAuthority;
    outcome.explanation = "control-plane authority moved from epoch " +
                          std::to_string(token.epoch.value()) + " incarnation " +
                          std::to_string(token.incarnation.value()) + " to epoch " +
                          std::to_string(bound_epoch.value()) + " incarnation " +
                          std::to_string(bound_incarnation.value());
    return outcome;
  }
  if (token.generation != snapshot->generation()) {
    outcome.verdict = RevalidationVerdict::Superseded;
    outcome.reason = ReasonCode::GenerationAdvanced;
    outcome.cause = StatusCode::StaleGeneration;
    outcome.explanation = "the token was issued at generation " +
                          std::to_string(token.generation.value()) +
                          " but the current generation is " +
                          std::to_string(snapshot->generation().value());
    return outcome;
  }
  if (snapshot->find_domain(token.domain) == nullptr) {
    outcome.verdict = RevalidationVerdict::Superseded;
    outcome.reason = ReasonCode::DomainNotFound;
    outcome.cause = StatusCode::NotFound;
    outcome.explanation = "domain '" + token.domain.value() + "' is no longer in the model";
    return outcome;
  }

  DomainAssessment fresh =
      impl_->assess_domain(*snapshot, token.domain, request.as_of, bound_epoch, bound_incarnation);
  if (!fresh.known) {
    outcome.verdict = RevalidationVerdict::Indeterminate;
    outcome.reason = fresh.reason;
    outcome.cause = StatusCode::Indeterminate;
    outcome.explanation = "the assessment cannot be confirmed or denied at instant " +
                          std::to_string(request.as_of.value()) + ": " + to_string(fresh.reason);
    outcome.current = std::move(fresh);
    return outcome;
  }
  if (fresh.digest != token.digest) {
    outcome.verdict = RevalidationVerdict::Superseded;
    outcome.reason = ReasonCode::EvidenceRevisionChanged;
    outcome.cause = StatusCode::Conflict;
    outcome.explanation = "the authoritative inputs behind the assessment changed";
    outcome.current = std::move(fresh);
    return outcome;
  }
  outcome.verdict = RevalidationVerdict::StillValid;
  outcome.reason = ReasonCode::Ok;
  outcome.cause = StatusCode::Ok;
  outcome.explanation = "the assessment still holds at instant " +
                        std::to_string(request.as_of.value());
  outcome.current = std::move(fresh);
  return outcome;
}

Result<std::shared_ptr<const CapacityState>> CapacityEngine::state() const {
  if (!impl_) {
    return Status::error(StatusCode::Closed, "engine is not open");
  }
  const EngineLifecycle current = impl_->current_lifecycle();
  if (current == EngineLifecycle::Closed) {
    return Status::error(StatusCode::Closed, "engine is closed");
  }
  const std::shared_ptr<const CapacityState> snapshot = impl_->snapshot();
  if (!snapshot) {
    return Status::error(StatusCode::Closed, "engine has no published state");
  }
  return snapshot;
}

Status CapacityEngine::close() {
  if (!impl_) {
    return Status::success();
  }
  impl_->lifecycle.store(EngineLifecycle::Closed);
  Status status = Status::success();
  if (impl_->store) {
    status = impl_->store->close();
    impl_->store.reset();
  }
  impl_.reset();
  return status;
}

}  // namespace power_capacity
