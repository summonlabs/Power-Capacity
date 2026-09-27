#pragma once

// Test fixtures and builders. Not installed.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "power_capacity/engine.hpp"
#include "power_capacity/state.hpp"

namespace pc_test {

/// A uniquely named temporary directory that removes itself on destruction.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const noexcept { return path_; }
  std::filesystem::path file(const std::string& name) const;
  std::size_t entry_count() const;
  std::vector<std::string> entry_names() const;

 private:
  std::filesystem::path path_;
};

/// Fluent builder for a power domain declaration.
class DomainBuilder {
 public:
  explicit DomainBuilder(std::string id);

  DomainBuilder& kind(power_capacity::DomainKind kind);
  DomainBuilder& label(std::string label);
  DomainBuilder& parent(std::string parent);
  DomainBuilder& nominal_watts(std::int64_t watts);
  DomainBuilder& usable_watts(std::int64_t watts);
  DomainBuilder& derate_bp(std::int32_t basis_points);
  DomainBuilder& degradation_bp(std::int32_t basis_points);
  DomainBuilder& reserve_watts(std::int64_t watts);
  DomainBuilder& reserve_bp(std::int32_t basis_points);
  DomainBuilder& reserve_greater_of(std::int64_t watts, std::int32_t basis_points);
  DomainBuilder& available();
  DomainBuilder& degraded(power_capacity::StateCause cause, std::int32_t degradation_bp);
  DomainBuilder& unavailable(power_capacity::StateCause cause);
  DomainBuilder& evidence(power_capacity::Tick observed, power_capacity::Tick valid_until,
                          std::uint64_t revision = 1);
  DomainBuilder& no_evidence();
  DomainBuilder& source_generation(const std::string& source, std::uint64_t generation);

  power_capacity::PowerDomain build() const;
  operator power_capacity::PowerDomain() const { return build(); }  // NOLINT

 private:
  power_capacity::PowerDomain domain_;
  std::string evidence_id_ = "ev.default";
};

power_capacity::LoadRecord make_load(const std::string& id, const std::string& domain,
                                     std::int64_t watts,
                                     power_capacity::LoadClass load_class =
                                         power_capacity::LoadClass::Committed,
                                     const std::string& authority = "authority.test");

power_capacity::RedundancyGroup make_group(
    const std::string& id, const std::vector<std::string>& members, std::uint32_t tolerance,
    power_capacity::RedundancyClass redundancy_class = power_capacity::RedundancyClass::NPlusOne,
    const std::string& authority = "authority.policy");

/// A small facility that manages the generation, instant, and authority binding
/// boilerplate so that tests can state only what they are actually testing.
class Facility {
 public:
  /// `store_path` empty means an in-memory engine.
  explicit Facility(const std::filesystem::path& store_path = {}, bool read_only = false,
                    power_capacity::Tick initial_tick = power_capacity::Tick(0));

  power_capacity::CapacityEngine& engine() { return *engine_; }
  const power_capacity::CapacityEngine& engine() const { return *engine_; }
  /// The currently published state, read live from the engine so that a test
  /// which mutates the engine directly never observes a cached generation.
  std::shared_ptr<const power_capacity::CapacityState> state() const;

  power_capacity::Tick as_of() const noexcept { return as_of_; }
  void set_as_of(power_capacity::Tick tick) noexcept { as_of_ = tick; }
  power_capacity::Epoch epoch() const noexcept { return epoch_; }
  power_capacity::Incarnation incarnation() const noexcept { return incarnation_; }
  power_capacity::Generation generation() const;

  void add_domain(const power_capacity::PowerDomain& domain);
  void update_domain(const power_capacity::PowerDomain& domain);
  void remove_domain(const std::string& id);
  void add_load(const power_capacity::LoadRecord& load);
  void remove_load(const std::string& id);
  void add_group(const power_capacity::RedundancyGroup& group);
  void remove_group(const std::string& id);
  void advance_source(const std::string& source, std::uint64_t generation);

  power_capacity::DomainAssessment assess(const std::string& domain);
  power_capacity::DomainAssessment assess(const std::string& domain, power_capacity::Tick as_of);
  power_capacity::CandidateEvaluation evaluate(const std::string& load_id, const std::string& domain,
                                               std::int64_t watts);
  power_capacity::CandidateEvaluation evaluate_protected(const std::string& load_id,
                                                         const std::string& domain,
                                                         std::int64_t watts);
  std::vector<power_capacity::CandidateEvaluation> evaluate_batch(
      const std::vector<power_capacity::CandidateLoad>& candidates);
  power_capacity::RevalidationOutcome revalidate(const power_capacity::AssessmentToken& token);

  power_capacity::MutationContext context(std::optional<power_capacity::Generation> expected =
                                              std::nullopt) const;

 private:
  std::shared_ptr<const power_capacity::CapacityState> require_state() const;

  std::unique_ptr<power_capacity::CapacityEngine> engine_;
  power_capacity::Tick as_of_{};
  power_capacity::Epoch epoch_;
  power_capacity::Incarnation incarnation_;
};

std::string describe(const power_capacity::Status& status);

}  // namespace pc_test
