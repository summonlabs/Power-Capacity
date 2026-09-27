// Completed-operation benchmark for Power Capacity.
//
// What is measured, and how:
//
//   * Every reported number is the cost of a *completed* operation. Nothing is
//     timed in submission: an assessment is timed around the call that returns
//     the assessment, and a durable commit is timed around the call that returns
//     only after the staging write, the flush, the verification, and the atomic
//     publish have all finished.
//   * The durable commit row therefore includes the flush and publish cost. The
//     per-phase breakdown is reported next to it so the durability cost is
//     visible rather than hidden.
//   * Each row runs `kRepeats` alternating rounds of `kIterations` operations and
//     the *median* round is reported, which removes the effect of a single slow
//     round without publishing a before/after pair that the methodology could not
//     isolate.
//   * The workload is synthetic and is labelled as such. It is a generated
//     facility model, not measured facility telemetry, and no hardware behaviour
//     is claimed.
//   * The store the benchmark creates is removed afterwards and the final state
//     is verified before exit.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "power_capacity/engine.hpp"
#include "power_capacity/store.hpp"

namespace {

using namespace power_capacity;

constexpr int kRepeats = 7;
constexpr int kWarmup = 1;

std::uint64_t now_nanos() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 == 1) {
    return values[middle];
  }
  return (values[middle - 1] + values[middle]) / 2.0;
}

void report(const char* label, double nanos_per_operation, double operations) {
  const double per_second = nanos_per_operation > 0.0 ? 1e9 / nanos_per_operation : 0.0;
  std::printf("%-46s %12.1f ns/op %14.0f op/s   (%.0f operations)\n", label, nanos_per_operation,
              per_second, operations);
}

DomainId domain_id(const std::string& text) { return DomainId::parse(text, 128).value(); }

PowerDomain make_domain(const std::string& id, const char* parent, std::int64_t watts,
                        std::int32_t derate_bp) {
  PowerDomain domain;
  domain.id = domain_id(id);
  domain.nominal_capacity = Power::from_watts(watts).value();
  domain.usable_capacity = domain.nominal_capacity;
  domain.derate_ratio = Ratio::from_basis_points(derate_bp).value();
  if (parent != nullptr) {
    domain.parent = domain_id(parent);
  }
  EvidenceRef evidence;
  evidence.id = EvidenceId::parse("ev.bench", 128).value();
  evidence.source = SourceId::parse("source.bench", 128).value();
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000000);
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  domain.evidence = evidence;
  return domain;
}

/// A synthetic three-level facility: one root, `rows` PDUs, and `leaves` rack
/// PDUs under each of them, with one committed load on every leaf.
struct Model {
  std::vector<PowerDomain> domains;
  std::vector<LoadRecord> loads;
  RedundancyGroup group;
  std::vector<DomainId> leaves;
  DomainId root;
};

Model build_model(int rows, int leaves_per_row) {
  Model model;
  model.domains.push_back(make_domain("bench.root", nullptr, 40'000'000, 9500));
  model.root = model.domains.back().id;
  model.group.id = GroupId::parse("bench.group", 128).value();
  model.group.policy_authority = AuthorityRef::parse("authority.bench", 128).value();
  model.group.required_simultaneous_failures = 1;
  model.group.declared_class = RedundancyClass::NPlusOne;

  for (int row = 0; row < rows; ++row) {
    const std::string pdu = "bench.pdu." + std::to_string(row);
    model.domains.push_back(make_domain(pdu, "bench.root", 600'000, 9700));
    model.group.members.push_back(model.domains.back().id);
    for (int leaf = 0; leaf < leaves_per_row; ++leaf) {
      const std::string rack = pdu + ".rackpdu." + std::to_string(leaf);
      model.domains.push_back(make_domain(rack, pdu.c_str(), 40'000, 9800));
      model.leaves.push_back(model.domains.back().id);
      LoadRecord load;
      load.id = LoadId::parse("bench.load." + std::to_string(row) + "." + std::to_string(leaf), 128)
                    .value();
      load.domain = model.domains.back().id;
      load.load = Power::from_watts(5000 + leaf).value();
      load.authority = AuthorityRef::parse("authority.bench", 128).value();
      load.evidence = model.domains.back().evidence;
      model.loads.push_back(load);
    }
  }
  if (model.group.members.size() < 2) {
    model.group.members.clear();
  }
  return model;
}

/// Commits the whole model, one commit per record, exactly as a control plane
/// would, and returns the number of commits.
std::uint64_t load_model(CapacityEngine& engine, const Model& model, Tick as_of) {
  std::uint64_t commits = 0;
  MutationContext context;
  context.as_of = as_of;
  for (const PowerDomain& domain : model.domains) {
    context.expected_generation = engine.state().value()->generation();
    const auto committed = engine.put_domain(domain, PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      std::cerr << "benchmark setup failed: " << committed.status().to_string() << "\n";
      std::exit(1);
    }
    ++commits;
  }
  for (const LoadRecord& load : model.loads) {
    context.expected_generation = engine.state().value()->generation();
    const auto committed = engine.put_load(load, PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      std::cerr << "benchmark setup failed: " << committed.status().to_string() << "\n";
      std::exit(1);
    }
    ++commits;
  }
  if (!model.group.members.empty()) {
    context.expected_generation = engine.state().value()->generation();
    const auto committed = engine.put_group(model.group, PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      std::cerr << "benchmark setup failed: " << committed.status().to_string() << "\n";
      std::exit(1);
    }
    ++commits;
  }
  return commits;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path scratch =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() / "power-capacity-benchmark";
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  std::filesystem::create_directories(scratch, error);
  const std::filesystem::path store_path = scratch / "bench.pcstore";

  constexpr int kRows = 40;
  constexpr int kLeavesPerRow = 12;
  const Model model = build_model(kRows, kLeavesPerRow);

  std::printf("Power Capacity completed-operation benchmark\n");
  std::printf("workload: SYNTHETIC. %zu domains, %zu loads, 1 redundancy group.\n",
              model.domains.size(), model.loads.size());
  std::printf("method: median of %d alternating rounds; warmup rounds discarded.\n\n", kRepeats);

  // ------------------------------------------------------------------
  // Setup: one durable commit per record.
  // ------------------------------------------------------------------
  {
    EngineOptions options;
    options.store_path = store_path;
    options.create_if_missing = true;
    auto engine = CapacityEngine::open(options);
    if (!engine.ok()) {
      std::cerr << "open failed: " << engine.status().to_string() << "\n";
      return 1;
    }
    const std::uint64_t setup_start = now_nanos();
    const std::uint64_t commits = load_model(*engine.value(), model, Tick(10));
    const std::uint64_t setup_end = now_nanos();
    std::printf("%-46s %12.1f ns/op %14s   (%llu commits)\n",
                "durable commit, model setup (flush included)",
                static_cast<double>(setup_end - setup_start) / static_cast<double>(commits), "",
                static_cast<unsigned long long>(commits));
    engine.value()->close();
  }

  // ------------------------------------------------------------------
  // Query and evaluation throughput on an in-memory engine over the same model.
  // ------------------------------------------------------------------
  EngineOptions memory_options;
  auto memory = CapacityEngine::open(memory_options);
  if (!memory.ok()) {
    std::cerr << "open failed: " << memory.status().to_string() << "\n";
    return 1;
  }
  const std::uint64_t model_commits = load_model(*memory.value(), model, Tick(10));
  std::printf("in-memory model built with %llu commits\n\n",
              static_cast<unsigned long long>(model_commits));

  const Tick as_of(20);
  {
    std::vector<double> rounds;
    std::uint64_t operations = 0;
    for (int round = 0; round < kRepeats + kWarmup; ++round) {
      const std::uint64_t start = now_nanos();
      std::uint64_t count = 0;
      for (const DomainId& leaf : model.leaves) {
        CapacityQuery query;
        query.domain = leaf;
        query.as_of = as_of;
        const auto assessment = memory.value()->assess(query);
        if (!assessment.ok() || !assessment.value().known) {
          std::cerr << "assessment failed during the benchmark\n";
          return 1;
        }
        ++count;
      }
      const std::uint64_t elapsed = now_nanos() - start;
      if (round >= kWarmup) {
        rounds.push_back(static_cast<double>(elapsed) / static_cast<double>(count));
        operations += count;
      }
    }
    report("assess leaf (path walk to the root)", median(rounds),
           static_cast<double>(operations) / static_cast<double>(kRepeats));
  }

  {
    std::vector<double> rounds;
    std::uint64_t operations = 0;
    for (int round = 0; round < kRepeats + kWarmup; ++round) {
      const std::uint64_t start = now_nanos();
      std::uint64_t count = 0;
      for (const DomainId& leaf : model.leaves) {
        CandidateLoad candidate;
        candidate.id = LoadId::parse("bench.candidate", 128).value();
        candidate.domain = leaf;
        candidate.load = Power::from_watts(1).value();
        EvaluationContext context;
        context.as_of = as_of;
        const auto evaluation = memory.value()->evaluate(candidate, context);
        if (!evaluation.ok()) {
          std::cerr << "evaluation failed during the benchmark\n";
          return 1;
        }
        ++count;
      }
      const std::uint64_t elapsed = now_nanos() - start;
      if (round >= kWarmup) {
        rounds.push_back(static_cast<double>(elapsed) / static_cast<double>(count));
        operations += count;
      }
    }
    report("evaluate candidate at a leaf", median(rounds),
           static_cast<double>(operations) / static_cast<double>(kRepeats));
  }

  {
    constexpr int kRootIterations = 200;
    std::vector<double> rounds;
    std::uint64_t operations = 0;
    for (int round = 0; round < kRepeats + kWarmup; ++round) {
      const std::uint64_t start = now_nanos();
      for (int index = 0; index < kRootIterations; ++index) {
        CapacityQuery query;
        query.domain = model.root;
        query.as_of = as_of;
        const auto assessment = memory.value()->assess(query);
        if (!assessment.ok()) {
          std::cerr << "assessment failed during the benchmark\n";
          return 1;
        }
      }
      const std::uint64_t elapsed = now_nanos() - start;
      if (round >= kWarmup) {
        rounds.push_back(static_cast<double>(elapsed) / kRootIterations);
        operations += kRootIterations;
      }
    }
    report("assess root (rollup read over the whole model)", median(rounds),
           static_cast<double>(operations) / static_cast<double>(kRepeats));
  }

  memory.value()->close();

  // ------------------------------------------------------------------
  // Durable commit of a single small change against the full model, with the
  // per-phase breakdown so the durability cost is visible.
  // ------------------------------------------------------------------
  {
    EngineOptions options;
    options.store_path = store_path;
    options.create_if_missing = false;
    auto engine = CapacityEngine::open(options);
    if (!engine.ok()) {
      std::cerr << "reopen failed: " << engine.status().to_string() << "\n";
      return 1;
    }
    RevalidationRequest request;
    request.as_of = Tick(20);
    if (!engine.value()->revalidate_recovered_state(request).ok()) {
      std::cerr << "revalidation failed\n";
      return 1;
    }

    constexpr int kCommitIterations = 40;
    std::vector<double> total_rounds;
    std::vector<double> flush_rounds;
    std::vector<double> verify_rounds;
    std::uint64_t bytes = 0;
    std::uint64_t commits = 0;
    for (int round = 0; round < kRepeats + kWarmup; ++round) {
      const std::uint64_t start = now_nanos();
      std::uint64_t flush_nanos = 0;
      std::uint64_t verify_nanos = 0;
      for (int index = 0; index < kCommitIterations; ++index) {
        MutationContext context;
        context.as_of = Tick(20);
        context.expected_generation = engine.value()->state().value()->generation();
        const std::string id = "bench.extra." + std::to_string(round) + "." + std::to_string(index);
        const auto committed = engine.value()->put_domain(
            make_domain(id, "bench.root", 10'000, 10000), PresenceExpectation::MustNotExist,
            context);
        if (!committed.ok()) {
          std::cerr << "commit failed during the benchmark: " << committed.status().to_string()
                    << "\n";
          return 1;
        }
        flush_nanos += committed.value().metrics.staging_write_nanos;
        verify_nanos += committed.value().metrics.verify_nanos;
        bytes = committed.value().metrics.bytes_written;
        ++commits;
      }
      const std::uint64_t elapsed = now_nanos() - start;
      if (round >= kWarmup) {
        total_rounds.push_back(static_cast<double>(elapsed) / kCommitIterations);
        flush_rounds.push_back(static_cast<double>(flush_nanos) / kCommitIterations);
        verify_rounds.push_back(static_cast<double>(verify_nanos) / kCommitIterations);
      }
    }
    report("durable commit (encode, flush, verify, publish)", median(total_rounds),
           static_cast<double>(commits));
    report("  of which staging write and device flush", median(flush_rounds),
           static_cast<double>(commits));
    report("  of which staged-artifact verification", median(verify_rounds),
           static_cast<double>(commits));
    std::printf("%-46s %12llu bytes\n", "  artifact size per commit",
                static_cast<unsigned long long>(bytes));

    // Verify the state the benchmark produced, then close and remove it.
    const auto state = engine.value()->state();
    const std::size_t expected_domains = model.domains.size() + static_cast<std::size_t>(commits);
    if (!state.ok() || state.value()->domain_count() != expected_domains) {
      std::cerr << "benchmark produced " << (state.ok() ? state.value()->domain_count() : 0)
                << " domains, expected " << expected_domains << "\n";
      return 1;
    }
    engine.value()->close();

    const Status verified = CapacityStore::verify_file(store_path);
    if (!verified.ok()) {
      std::cerr << "benchmark store failed verification: " << verified.to_string() << "\n";
      return 1;
    }
    std::printf("\nfinal store verified: %s\n", verified.to_string().c_str());
    const auto final_state = CapacityStore::read_file(store_path, StoreReadOptions{});
    if (!final_state.ok()) {
      std::cerr << "benchmark store could not be read back\n";
      return 1;
    }
    std::printf("final generation: %llu, domains: %zu\n",
                static_cast<unsigned long long>(final_state.value()->generation().value()),
                final_state.value()->domain_count());
  }

  std::filesystem::remove_all(scratch, error);
  std::printf("residue removed: %s\n", std::filesystem::exists(scratch) ? "no" : "yes");
  return std::filesystem::exists(scratch) ? 1 : 0;
}
