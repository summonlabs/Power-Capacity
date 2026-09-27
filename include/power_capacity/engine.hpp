#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "power_capacity/assessment.hpp"
#include "power_capacity/capacity.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/limits.hpp"
#include "power_capacity/load.hpp"
#include "power_capacity/power_domain.hpp"
#include "power_capacity/redundancy.hpp"
#include "power_capacity/state.hpp"
#include "power_capacity/status.hpp"
#include "power_capacity/store.hpp"

namespace power_capacity {

/// Lifecycle of one engine instance.
enum class EngineLifecycle : std::uint8_t {
  /// State was recovered from a store and has not yet been revalidated against
  /// current evidence and authority. Queries are refused in this state.
  RecoveredPendingRevalidation = 0,
  /// The engine may answer capacity questions.
  Current = 1,
  /// `close` was called.
  Closed = 2,
};

const char* to_string(EngineLifecycle lifecycle) noexcept;

struct EngineOptions {
  /// Absent means an in-memory engine. An in-memory engine has no durable
  /// authority and is intended for modelling, tests, and examples.
  std::optional<std::filesystem::path> store_path;
  bool create_if_missing = true;
  bool read_only = false;
  bool enforce_path_binding = false;
  std::optional<StoreIdentity> expected_store_identity;
  /// When present, the store must be committed at or above this generation. A
  /// store file can be replaced by an older but valid copy of itself, which no
  /// checksum detects; a durable floor is what detects it.
  std::optional<Generation> minimum_generation;
  ResourceLimits limits = ResourceLimits::defaults();
  /// Authority binding recorded when this engine creates a new store or runs in
  /// memory. An engine that opens an existing store takes the binding from the
  /// store and must revalidate before it may answer questions.
  Epoch epoch;
  Incarnation incarnation;
  Tick initial_tick;
};

enum class PresenceExpectation : std::uint8_t {
  /// Create or replace.
  Any = 0,
  /// The identity must already exist.
  MustExist = 1,
  /// The identity must not exist yet.
  MustNotExist = 2,
};

/// Explicit preconditions for one mutation.
///
/// Every mutation that depends on current state carries the generation, the
/// control-plane authority binding, and the source generations the caller read.
/// A mismatch is refused; a stale mutation is never merged.
struct MutationContext {
  /// The logical instant at which the caller's evidence was read.
  Tick as_of;
  Epoch epoch;
  Incarnation incarnation;
  /// Required. The generation the caller believes is current.
  std::optional<Generation> expected_generation;
  /// Source generations the caller read and depends on. Each must equal the
  /// generation currently recorded for that source.
  std::map<SourceId, Generation> expected_source_generations;
  /// Source generations to record on success. Every source named here must also
  /// appear in `expected_source_generations`.
  std::map<SourceId, Generation> advanced_source_generations;
  /// Bounded idempotency key. Re-applying a recorded key returns the previously
  /// recorded generation without applying the mutation again.
  std::optional<IdempotencyKey> idempotency_key;
};

/// Binds recovered state to the current evidence instant and control-plane
/// authority. This is the qualification step that turns recovered persisted state
/// into state that may answer capacity questions.
struct RevalidationRequest {
  Tick as_of;
  Epoch epoch;
  Incarnation incarnation;
  std::optional<Generation> expected_generation;
  /// How many offending domains to name in the report. The counts are exact; the
  /// samples are bounded.
  std::size_t max_reported_domains = 32;
};

struct EngineStatus {
  EngineLifecycle lifecycle = EngineLifecycle::RecoveredPendingRevalidation;
  bool durable = false;
  bool read_only = false;
  SessionId session;
  StoreInfo store;
  /// The authority binding this engine currently accepts.
  Epoch epoch;
  Incarnation incarnation;
  /// The instant at which the binding and the evidence behind it were last
  /// accepted. Zero means the engine has not been qualified yet.
  Tick qualified_at;
  std::size_t domain_count = 0;
  std::size_t load_count = 0;
  std::size_t group_count = 0;
  std::size_t source_count = 0;
  std::size_t applied_operation_count = 0;
};

struct EvaluationContext {
  Tick as_of;
  std::optional<Generation> expected_generation;
};

/// The electrical-capacity authority of one facility control-plane site.
///
/// Concurrency: the engine holds an immutable state snapshot behind an atomic
/// pointer. Queries read the published snapshot without taking any lock.
/// Mutations serialize on a single mutation mutex, which is acquired before the
/// store's file lock and never while a file lock is held. See the concurrency
/// section of the README for the full lock order.
class CapacityEngine {
 public:
  CapacityEngine() = default;
  ~CapacityEngine();
  CapacityEngine(const CapacityEngine&) = delete;
  CapacityEngine& operator=(const CapacityEngine&) = delete;

  static Result<std::unique_ptr<CapacityEngine>> open(const EngineOptions& options);

  EngineLifecycle lifecycle() const noexcept;
  Result<EngineStatus> status() const;

  /// Qualifies recovered state against current evidence and the caller's
  /// authority binding. Required once after opening an existing store, before any
  /// query. The call commits a new generation only when the authority binding
  /// changes; evidence freshness is never persisted, so recovered state is always
  /// re-qualified against the instant supplied here rather than inheriting the
  /// freshness it had when it was written.
  Result<RevalidationReport> revalidate_recovered_state(const RevalidationRequest& request);

  // -- mutations ------------------------------------------------------------

  Result<CommitResult> put_domain(const PowerDomain& domain, PresenceExpectation presence,
                                  const MutationContext& context);
  Result<CommitResult> erase_domain(const DomainId& id, const MutationContext& context);
  Result<CommitResult> put_load(const LoadRecord& load, PresenceExpectation presence,
                                const MutationContext& context);
  Result<CommitResult> erase_load(const LoadId& id, const MutationContext& context);
  Result<CommitResult> put_group(const RedundancyGroup& group, PresenceExpectation presence,
                                 const MutationContext& context);
  Result<CommitResult> erase_group(const GroupId& id, const MutationContext& context);

  /// Records that a source advanced to a new generation without changing the
  /// capacity model. The caller must have read the previous generation.
  Result<CommitResult> advance_source_generation(const SourceId& source, Generation next,
                                                 const MutationContext& context);

  // -- queries --------------------------------------------------------------

  Result<DomainAssessment> assess(const CapacityQuery& query) const;

  /// Evaluates one proposed load against current authority and evidence. The
  /// result is advisory: it reports whether the load fits. Committing a load is
  /// a separate mutation that requires the authority reference that granted it.
  Result<CandidateEvaluation> evaluate(const CandidateLoad& candidate,
                                       const EvaluationContext& context) const;

  /// Evaluates a set of proposed loads together. Each candidate sees the effect
  /// of the candidates accepted before it, in the order given, so a batch answers
  /// "can all of these fit together" rather than "does each fit alone".
  Result<std::vector<CandidateEvaluation>> evaluate_batch(
      const std::vector<CandidateLoad>& candidates, const EvaluationContext& context) const;

  Result<RevalidationOutcome> revalidate(const AssessmentToken& token,
                                         const RevalidationRequest& request) const;

  /// The currently published immutable state. Safe to hold; it never changes.
  Result<std::shared_ptr<const CapacityState>> state() const;

  Status close();

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace power_capacity
