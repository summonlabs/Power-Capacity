// Concurrency: immutable snapshots, lock-free reads, and mutation serialization.
//
// The documented lock order is exercised here from the outside: readers take no
// engine lock at all, and a mutation either publishes a wholly validated state or
// publishes nothing.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "power_capacity/engine.hpp"
#include "support/facility.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_group;
using pc_test::make_load;

PC_TEST(concurrency, a_snapshot_never_changes_after_it_is_published) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000).usable_watts(100000));
  const std::shared_ptr<const CapacityState> snapshot = facility.state();
  const Generation generation = snapshot->generation();
  const std::int64_t headroom =
      snapshot->derivation_of(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)))
          ->allocatable_headroom.milliwatts();

  for (int index = 0; index < 8; ++index) {
    facility.add_load(make_load("load." + std::to_string(index), "pdu.a", 1000));
  }
  PC_CHECK_EQ(snapshot->generation().value(), generation.value());
  PC_CHECK_EQ(snapshot->domain_count(), std::size_t{1});
  PC_CHECK_EQ(snapshot->load_count(), std::size_t{0});
  PC_CHECK_EQ(snapshot->derivation_of(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)))
                  ->allocatable_headroom.milliwatts(),
              headroom);
  // The engine moved on; the held snapshot did not.
  PC_CHECK_EQ(facility.state()->load_count(), std::size_t{8});
}

PC_TEST(concurrency, readers_always_observe_a_whole_state_while_a_writer_commits) {
  Facility facility;
  facility.add_domain(DomainBuilder("busway.main").nominal_watts(1000000).usable_watts(1000000));
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(500000).usable_watts(500000)
                          .parent("busway.main"));
  facility.add_domain(DomainBuilder("pdu.b").nominal_watts(500000).usable_watts(500000)
                          .parent("busway.main"));
  facility.add_group(make_group("group.pdu", {"pdu.a", "pdu.b"}, 1, RedundancyClass::NPlusOne));

  constexpr int kWriterCommits = 120;
  constexpr int kReaders = 4;
  /// Longest bounded wait for one reader to inspect one published state.
  constexpr int kInterleaveWait = 5000;
  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<int> unobserved{0};
  std::atomic<std::uint64_t> observations{0};

  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int index = 0; index < kReaders; ++index) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        const auto snapshot = facility.engine().state();
        if (!snapshot.ok()) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const CapacityState& state = *snapshot.value();
        // The published state is self-consistent: the rollup at the root equals
        // the sum of every load, and every derivation satisfies its ordering.
        std::int64_t total = 0;
        for (const auto& entry : state.loads()) {
          total += entry.second.load.milliwatts();
        }
        const DomainId root = PC_REQUIRE_OK(DomainId::parse("busway.main", 128));
        if (state.rolled_up_committed_of(root).milliwatts() != total) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        for (const auto& entry : state.domains()) {
          const CapacityDerivation* derivation = state.derivation_of(entry.first);
          if (derivation == nullptr ||
              derivation->allocatable_headroom > derivation->carryable_capacity ||
              derivation->carryable_capacity > derivation->safe_capacity ||
              derivation->safe_capacity > derivation->operational ||
              derivation->operational > derivation->derated ||
              derivation->derated > derivation->usable ||
              derivation->usable > derivation->nominal) {
            failures.fetch_add(1, std::memory_order_relaxed);
          }
        }
        const GroupDerivation* group =
            state.group_derivation_of(PC_REQUIRE_OK(GroupId::parse("group.pdu", 128)));
        if (group == nullptr || group->effective_headroom > group->survivable_capacity) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        // A query that takes only the published snapshot must agree with the
        // snapshot it was read from, or with a strictly newer one: the engine
        // never runs the published state backwards. The query deliberately cites
        // no expected generation, because the model is moving underneath it.
        CapacityQuery query;
        query.domain = root;
        query.as_of = Tick(0);
        const auto assessment = facility.engine().assess(query);
        if (!assessment.ok() ||
            assessment.value().generation.value() < state.generation().value()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        observations.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int index = 0; index < kWriterCommits; ++index) {
    const std::uint64_t before = observations.load(std::memory_order_relaxed);
    facility.add_load(make_load("load." + std::to_string(index), index % 2 == 0 ? "pdu.a" : "pdu.b",
                                1000));
    // Wait until at least one reader has inspected a state published after this
    // commit. Without this the writer can finish before the readers are ever
    // scheduled, which would let the suite pass without exercising anything. The
    // wait can only fail on exhaustion: it never reports interleaving that did not
    // happen.
    int attempts = 0;
    while (observations.load(std::memory_order_relaxed) == before && attempts < kInterleaveWait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ++attempts;
    }
    if (observations.load(std::memory_order_relaxed) == before) {
      unobserved.fetch_add(1, std::memory_order_relaxed);
    }
  }
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& reader : readers) {
    reader.join();
  }

  PC_CHECK_EQ(failures.load(), 0);
  PC_CHECK_MSG(unobserved.load() == 0,
               std::to_string(unobserved.load()) +
                   " commits were never observed by a reader thread");
  // Every commit was observed at least once, and the readers also ran before the
  // first one, so the invariants above were checked against a moving model.
  PC_CHECK(observations.load() > static_cast<std::uint64_t>(kWriterCommits));
  PC_CHECK_EQ(facility.state()->load_count(), static_cast<std::size_t>(kWriterCommits));
}

PC_TEST(concurrency, concurrent_mutations_serialize_without_losing_a_commit) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(10000000).usable_watts(10000000));

  // Four threads contend on the mutation path. Every mutation that depends on
  // the current generation carries an explicit precondition read from the
  // engine, so a lost race is refused rather than merged, and exactly one winner
  // per generation succeeds.
  constexpr int kThreads = 4;
  constexpr int kAttempts = 200;
  std::atomic<int> committed{0};
  std::atomic<int> refused_as_stale{0};
  std::atomic<int> unexpected{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&, thread_index] {
      for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const auto live = facility.engine().state();
        if (!live.ok()) {
          unexpected.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        MutationContext context;
        context.as_of = facility.as_of();
        context.epoch = facility.epoch();
        context.incarnation = facility.incarnation();
        context.expected_generation = live.value()->generation();
        const std::string id = "load." + std::to_string(thread_index) + "." + std::to_string(attempt);
        const auto result = facility.engine().put_load(make_load(id, "pdu.a", 1),
                                                       PresenceExpectation::MustNotExist, context);
        if (result.ok()) {
          committed.fetch_add(1, std::memory_order_relaxed);
        } else if (result.status().code() == StatusCode::StaleGeneration) {
          refused_as_stale.fetch_add(1, std::memory_order_relaxed);
        } else {
          unexpected.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  PC_CHECK_EQ(unexpected.load(), 0);
  PC_CHECK_EQ(committed.load(), static_cast<int>(facility.state()->load_count()));
  PC_CHECK(committed.load() + refused_as_stale.load() == kThreads * kAttempts);
  PC_CHECK(committed.load() > 0);
  // Generation one is the empty model and generation two added the one domain,
  // so the generation advanced exactly once per committed load and never for a
  // refused one.
  PC_CHECK_EQ(facility.generation().value(),
              static_cast<std::uint64_t>(committed.load()) + 2);
}

PC_TEST(concurrency, a_closed_engine_refuses_every_operation) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  PC_CHECK(facility.engine().close().ok());
  PC_CHECK_EQ(facility.engine().lifecycle(), EngineLifecycle::Closed);

  CapacityQuery query;
  query.domain = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  query.as_of = Tick(0);
  PC_REQUIRE_STATUS(facility.engine().assess(query), StatusCode::Closed);
  PC_REQUIRE_STATUS(facility.engine().state().status(), StatusCode::Closed);
  PC_REQUIRE_STATUS(facility.engine().status().status(), StatusCode::Closed);
  PC_REQUIRE_STATUS(
      facility.engine().put_domain(DomainBuilder("pdu.b").nominal_watts(1).usable_watts(1).build(),
                                   PresenceExpectation::MustNotExist, MutationContext{})
          .status(),
      StatusCode::Closed);
  PC_CHECK(facility.engine().close().ok());
}

PC_TEST(concurrency, closing_twice_is_idempotent) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  PC_CHECK(facility.engine().close().ok());
  PC_CHECK(facility.engine().close().ok());
}
