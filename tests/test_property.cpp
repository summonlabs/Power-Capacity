// Deterministic property and seeded randomized tests against an independent
// reference model.
//
// The reference model recomputes rollups by recursive descent from the roots and
// re-derives every capacity stage from first principles. It shares no code with
// the engine's bottom-up ordering, so agreement between the two is evidence about
// the algorithm rather than about a shared helper.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "power_capacity/capacity.hpp"
#include "power_capacity/engine.hpp"
#include "support/facility.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_group;
using pc_test::make_load;

namespace {

struct ReferenceDomain {
  std::string id;
  std::optional<std::string> parent;
  std::int64_t nominal_mw = 0;
  std::int64_t usable_mw = 0;
  std::int32_t derate_bp = 10000;
  std::int32_t degradation_bp = 10000;
  int reserve_mode = 0;
  std::int64_t reserve_absolute_mw = 0;
  std::int32_t reserve_bp = 0;
  bool unavailable = false;
  bool degraded = false;
};

struct ReferenceLoad {
  std::string id;
  std::string domain;
  std::int64_t milliwatts = 0;
  bool is_protected = false;
};

struct ReferenceGroup {
  std::string id;
  std::vector<std::string> members;
  std::uint32_t tolerance = 0;
};

struct ReferenceModel {
  std::vector<ReferenceDomain> domains;
  std::vector<ReferenceLoad> loads;
  std::vector<ReferenceGroup> groups;

  const ReferenceDomain* find(const std::string& id) const {
    for (const ReferenceDomain& domain : domains) {
      if (domain.id == id) {
        return &domain;
      }
    }
    return nullptr;
  }

  std::vector<std::string> ancestry(const std::string& id) const {
    std::vector<std::string> chain;
    const ReferenceDomain* domain = find(id);
    while (domain != nullptr) {
      chain.push_back(domain->id);
      if (!domain->parent.has_value()) {
        break;
      }
      domain = find(*domain->parent);
    }
    return chain;
  }

  std::pair<std::int64_t, std::int64_t> rollup(const std::string& id) const {
    std::int64_t committed = 0;
    std::int64_t protected_load = 0;
    for (const ReferenceLoad& load : loads) {
      if (load.domain == id) {
        if (load.is_protected) {
          protected_load += load.milliwatts;
        } else {
          committed += load.milliwatts;
        }
      }
    }
    for (const ReferenceDomain& child : domains) {
      if (child.parent.has_value() && *child.parent == id) {
        const auto nested = rollup(child.id);
        committed += nested.first;
        protected_load += nested.second;
      }
    }
    return {committed, protected_load};
  }

  std::int64_t safe_capacity(const ReferenceDomain& domain) const {
    std::int64_t operational = 0;
    if (!domain.unavailable) {
      operational = static_cast<std::int64_t>(
          (static_cast<unsigned long long>(domain.usable_mw) * static_cast<unsigned long long>(domain.derate_bp)) /
          10000ULL);
      operational = static_cast<std::int64_t>(
          (static_cast<unsigned long long>(operational) *
           static_cast<unsigned long long>(domain.degradation_bp)) /
          10000ULL);
    }
    std::int64_t reserve = 0;
    if (domain.reserve_mode == 1) {
      reserve = domain.reserve_absolute_mw;
    } else if (domain.reserve_mode == 2) {
      reserve = static_cast<std::int64_t>(
          (static_cast<unsigned long long>(operational) *
           static_cast<unsigned long long>(domain.reserve_bp)) /
          10000ULL);
    } else if (domain.reserve_mode == 3) {
      const std::int64_t ratio_reserve = static_cast<std::int64_t>(
          (static_cast<unsigned long long>(operational) *
           static_cast<unsigned long long>(domain.reserve_bp)) /
          10000ULL);
      reserve = std::max(domain.reserve_absolute_mw, ratio_reserve);
    }
    return operational - reserve;
  }

  std::int64_t allocatable(const std::string& id) const {
    const ReferenceDomain* domain = find(id);
    if (domain == nullptr) {
      return 0;
    }
    const auto rolled = rollup(id);
    const std::int64_t safe = safe_capacity(*domain);
    const std::int64_t carryable = std::max<std::int64_t>(0, safe - rolled.second);
    return std::max<std::int64_t>(0, carryable - rolled.first);
  }

  std::int64_t carryable(const std::string& id) const {
    const ReferenceDomain* domain = find(id);
    if (domain == nullptr) {
      return 0;
    }
    const auto rolled = rollup(id);
    return std::max<std::int64_t>(0, safe_capacity(*domain) - rolled.second);
  }

  /// The group headroom, recomputed independently of the closed form.
  std::int64_t group_headroom(const ReferenceGroup& group) const {
    std::vector<std::int64_t> carrying;
    std::int64_t load = 0;
    std::size_t unavailable = 0;
    for (const std::string& member : group.members) {
      const ReferenceDomain* domain = find(member);
      if (domain == nullptr) {
        continue;
      }
      carrying.push_back(carryable(member));
      load += rollup(member).first;
      if (domain->unavailable) {
        ++unavailable;
      }
    }
    if (unavailable > group.tolerance) {
      return 0;
    }
    if (group.tolerance > 0 && unavailable == group.tolerance) {
      return 0;
    }
    const std::size_t effective = static_cast<std::size_t>(group.tolerance) - unavailable;
    std::sort(carrying.begin(), carrying.end(), std::greater<std::int64_t>());
    std::int64_t survivable = 0;
    for (std::size_t index = 0; index < carrying.size(); ++index) {
      if (index >= effective) {
        survivable += carrying[index];
      }
    }
    std::int64_t headroom = std::max<std::int64_t>(0, survivable - load);

    // The shared upstream bound: every common ancestor of every member.
    std::vector<std::string> common = ancestry(group.members.front());
    common.erase(common.begin());
    for (std::size_t index = 1; index < group.members.size(); ++index) {
      const std::vector<std::string> other = ancestry(group.members[index]);
      std::vector<std::string> intersection;
      for (const std::string& candidate : common) {
        if (std::find(other.begin(), other.end(), candidate) != other.end()) {
          intersection.push_back(candidate);
        }
      }
      common = intersection;
    }
    for (const std::string& ancestor : common) {
      headroom = std::min(headroom, allocatable(ancestor));
    }
    return headroom;
  }
};

std::int64_t random_range(std::mt19937_64& generator, std::int64_t low, std::int64_t high) {
  std::uniform_int_distribution<std::int64_t> distribution(low, high);
  return distribution(generator);
}

ReferenceModel make_reference_model(std::uint64_t seed) {
  std::mt19937_64 generator(seed);
  ReferenceModel model;

  const std::int64_t domain_count = random_range(generator, 6, 26);
  for (std::int64_t index = 0; index < domain_count; ++index) {
    ReferenceDomain domain;
    domain.id = "d." + std::to_string(index);
    if (index > 0) {
      // Parents always come from earlier indices, so the structure is a forest by
      // construction and can never contain a cycle.
      const std::int64_t parent_index = random_range(generator, 0, index - 1);
      if (random_range(generator, 0, 9) < 7) {
        domain.parent = "d." + std::to_string(parent_index);
      }
    }
    domain.nominal_mw = random_range(generator, 100, 20000) * 1000;
    const std::int64_t usable_percent = random_range(generator, 60, 100);
    // Everything the reference model states is an exact whole number of watts, so
    // the two models can be compared without any rounding ambiguity.
    domain.usable_mw = (domain.nominal_mw / 1000) * usable_percent / 100 * 1000;
    domain.derate_bp = static_cast<std::int32_t>(random_range(generator, 5000, 10000));
    const std::int64_t reserve_choice = random_range(generator, 0, 3);
    if (reserve_choice == 1) {
      domain.reserve_mode = 1;
      domain.reserve_absolute_mw = random_range(generator, 0, domain.usable_mw / 4000 + 1) * 1000;
    } else if (reserve_choice == 2) {
      domain.reserve_mode = 2;
      domain.reserve_bp = static_cast<std::int32_t>(random_range(generator, 1, 3000));
    } else if (reserve_choice == 3) {
      domain.reserve_mode = 3;
      domain.reserve_absolute_mw = random_range(generator, 0, domain.usable_mw / 4000 + 1) * 1000;
      domain.reserve_bp = static_cast<std::int32_t>(random_range(generator, 1, 3000));
    }
    const std::int64_t state_choice = random_range(generator, 0, 9);
    if (state_choice == 8) {
      domain.unavailable = true;
    } else if (state_choice == 7) {
      domain.degraded = true;
      domain.degradation_bp = static_cast<std::int32_t>(random_range(generator, 2000, 9000));
    }
    model.domains.push_back(domain);
  }

  const std::int64_t load_count = random_range(generator, 0, 30);
  for (std::int64_t index = 0; index < load_count; ++index) {
    ReferenceLoad load;
    load.id = "l." + std::to_string(index);
    const std::int64_t domain_index = random_range(generator, 0, domain_count - 1);
    load.domain = "d." + std::to_string(domain_index);
    load.milliwatts = random_range(generator, 1, 500) * 1000;
    load.is_protected = random_range(generator, 0, 9) < 2;
    model.loads.push_back(load);
  }

  // Groups are built over domains that share a parent, which makes them pairwise
  // independent by construction.
  std::map<std::string, std::vector<std::string>> by_parent;
  for (const ReferenceDomain& domain : model.domains) {
    by_parent[domain.parent.value_or("<root>")].push_back(domain.id);
  }
  std::int64_t group_index = 0;
  for (const auto& entry : by_parent) {
    if (entry.second.size() < 2 || random_range(generator, 0, 9) < 5) {
      continue;
    }
    ReferenceGroup group;
    group.id = "g." + std::to_string(group_index++);
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(entry.second.size()),
                               random_range(generator, 2, 3)));
    for (std::size_t index = 0; index < take; ++index) {
      group.members.push_back(entry.second[index]);
    }
    group.tolerance = static_cast<std::uint32_t>(random_range(generator, 0, static_cast<std::int64_t>(take) - 1));
    model.groups.push_back(group);
  }
  return model;
}

Facility build_facility(const ReferenceModel& model, Tick as_of) {
  Facility facility;
  for (const ReferenceDomain& reference : model.domains) {
    DomainBuilder builder(reference.id);
    builder.nominal_watts(reference.nominal_mw / 1000).usable_watts(reference.usable_mw / 1000);
    builder.derate_bp(reference.derate_bp);
    if (reference.parent.has_value()) {
      builder.parent(*reference.parent);
    }
    if (reference.unavailable) {
      builder.unavailable(StateCause::Fault);
    } else if (reference.degraded) {
      builder.degraded(StateCause::Fault, reference.degradation_bp);
    }
    if (reference.reserve_mode == 1) {
      builder.reserve_watts(reference.reserve_absolute_mw / 1000);
    } else if (reference.reserve_mode == 2) {
      builder.reserve_bp(reference.reserve_bp);
    } else if (reference.reserve_mode == 3) {
      builder.reserve_greater_of(reference.reserve_absolute_mw / 1000, reference.reserve_bp);
    }
    builder.evidence(Tick(0), Tick(as_of.value() + 1000));
    facility.add_domain(builder.build());
  }
  for (const ReferenceLoad& reference : model.loads) {
    facility.add_load(make_load(reference.id, reference.domain, reference.milliwatts / 1000,
                                reference.is_protected ? LoadClass::Protected : LoadClass::Committed));
  }
  for (const ReferenceGroup& reference : model.groups) {
    facility.add_group(make_group(reference.id, reference.members, reference.tolerance));
  }
  return facility;
}

}  // namespace

PC_TEST(property, engine_agrees_with_the_reference_model) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    const ReferenceModel model = make_reference_model(seed * 7919);
    const Tick as_of(5000);
    Facility facility = build_facility(model, as_of);
    const auto& state = facility.state();

    for (const ReferenceDomain& reference : model.domains) {
      const DomainId id = PC_REQUIRE_OK(DomainId::parse(reference.id, 128));
      const CapacityDerivation* derivation = state->derivation_of(id);
      PC_CHECK_MSG(derivation != nullptr, "seed " + std::to_string(seed) + " domain " + reference.id);
      if (derivation == nullptr) {
        continue;
      }
      const auto rolled = model.rollup(reference.id);
      const std::int64_t expected_safe = std::max<std::int64_t>(0, model.safe_capacity(reference));
      const std::int64_t expected_carryable =
          std::max<std::int64_t>(0, expected_safe - rolled.second);
      const std::int64_t expected_allocatable =
          std::max<std::int64_t>(0, expected_carryable - rolled.first);
      const std::string context =
          " seed " + std::to_string(seed) + " domain " + reference.id;

      PC_CHECK_MSG(derivation->committed_load.milliwatts() == rolled.first,
                   "committed" + context);
      PC_CHECK_MSG(derivation->protected_load.milliwatts() == rolled.second,
                   "protected" + context);
      if (!reference.unavailable) {
        PC_CHECK_MSG(derivation->safe_capacity.milliwatts() == expected_safe, "safe" + context);
        PC_CHECK_MSG(derivation->carryable_capacity.milliwatts() == expected_carryable,
                     "carryable" + context);
        PC_CHECK_MSG(derivation->allocatable_headroom.milliwatts() == expected_allocatable,
                     "allocatable" + context);
      } else {
        PC_CHECK_MSG(derivation->allocatable_headroom.milliwatts() == 0, "unavailable" + context);
      }

      // Invariants that must hold for every domain in every model.
      PC_CHECK(derivation->usable <= derivation->nominal);
      PC_CHECK(derivation->derated <= derivation->usable);
      PC_CHECK(derivation->operational <= derivation->derated);
      PC_CHECK(derivation->safe_capacity <= derivation->operational);
      PC_CHECK(derivation->carryable_capacity <= derivation->safe_capacity);
      PC_CHECK(derivation->allocatable_headroom <= derivation->carryable_capacity);
      if (!derivation->reserve_clamped && !derivation->protected_clamped &&
          !derivation->committed_clamped) {
        // Exact accounting closure when nothing was clamped away.
        PC_CHECK_EQ(derivation->operational.milliwatts(),
                    derivation->reserve.milliwatts() + derivation->protected_load.milliwatts() +
                        derivation->committed_load.milliwatts() +
                        derivation->allocatable_headroom.milliwatts());
      }
    }

    for (const ReferenceGroup& reference : model.groups) {
      const GroupId id = PC_REQUIRE_OK(GroupId::parse(reference.id, 128));
      const GroupDerivation* derivation = state->group_derivation_of(id);
      PC_CHECK(derivation != nullptr);
      if (derivation == nullptr) {
        continue;
      }
      PC_CHECK_MSG(derivation->effective_headroom.milliwatts() == model.group_headroom(reference),
                   "seed " + std::to_string(seed) + " group " + reference.id);
    }

    // Every assessment over a current-evidence model must be known and must match
    // the state's own derivation exactly.
    for (const ReferenceDomain& reference : model.domains) {
      const DomainAssessment assessment = facility.assess(reference.id, as_of);
      PC_CHECK(assessment.known);
      if (!assessment.known) {
        continue;
      }
      PC_CHECK_EQ(assessment.derivation->allocatable_headroom.milliwatts(),
                  state->derivation_of(PC_REQUIRE_OK(DomainId::parse(reference.id, 128)))
                      ->allocatable_headroom.milliwatts());
    }
  }
}

PC_TEST(property, candidate_evaluation_matches_the_reference_headroom) {
  for (std::uint64_t seed = 1; seed <= 16; ++seed) {
    const ReferenceModel model = make_reference_model(seed * 104729 + 13);
    const Tick as_of(5000);
    Facility facility = build_facility(model, as_of);

    for (const ReferenceDomain& reference : model.domains) {
      const std::vector<std::string> path = model.ancestry(reference.id);
      std::int64_t lowest = std::numeric_limits<std::int64_t>::max();
      for (const std::string& node : path) {
        lowest = std::min(lowest, model.allocatable(node));
        for (const ReferenceGroup& group : model.groups) {
          if (std::find(group.members.begin(), group.members.end(), node) != group.members.end()) {
            lowest = std::min(lowest, model.group_headroom(group));
          }
        }
      }
      if (lowest == std::numeric_limits<std::int64_t>::max()) {
        continue;
      }
      // Candidate loads are whole watts. The fitting candidate is one watt below
      // the floor of the headroom, and the refused candidate is one watt above
      // it, so the comparison is exact in both directions.
      const std::int64_t lowest_watts = lowest / 1000;
      if (lowest_watts < 2) {
        continue;
      }
      const CandidateEvaluation fits =
          facility.evaluate("cand.fit." + reference.id, reference.id, lowest_watts - 1);
      PC_CHECK_MSG(fits.verdict == CandidateVerdict::Admissible,
                   "seed " + std::to_string(seed) + " domain " + reference.id + " headroom " +
                       std::to_string(lowest_watts) + " W");

      const CandidateEvaluation refused =
          facility.evaluate("cand.big." + reference.id, reference.id, lowest_watts + 1);
      PC_CHECK_MSG(refused.verdict != CandidateVerdict::Admissible,
                   "seed " + std::to_string(seed) + " domain " + reference.id);
    }
  }
}

PC_TEST(property, generation_advances_exactly_once_per_commit) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    const ReferenceModel model = make_reference_model(seed * 31 + 5);
    Facility facility;
    std::uint64_t commits = 0;
    for (const ReferenceDomain& reference : model.domains) {
      DomainBuilder builder(reference.id);
      builder.nominal_watts(1000).usable_watts(1000);
      facility.add_domain(builder.build());
      ++commits;
      PC_CHECK_EQ(facility.generation().value(), commits + 1);
    }
    for (const ReferenceLoad& reference : model.loads) {
      facility.add_load(make_load(reference.id, reference.domain, 1));
      ++commits;
      PC_CHECK_EQ(facility.generation().value(), commits + 1);
    }
    for (const ReferenceGroup& reference : model.groups) {
      facility.add_group(make_group(reference.id, reference.members, reference.tolerance));
      ++commits;
      PC_CHECK_EQ(facility.generation().value(), commits + 1);
    }
    PC_CHECK_EQ(facility.state()->domain_count(), model.domains.size());
    PC_CHECK_EQ(facility.state()->load_count(), model.loads.size());
    PC_CHECK_EQ(facility.state()->group_count(), model.groups.size());
  }
}
