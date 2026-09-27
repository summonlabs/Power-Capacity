// Real multiprocess authority and crash recovery.
//
// Every fact in this file is established with an independent operating-system
// process, never with a thread. The probe dies with an immediate process exit or
// is force-killed by the operating system; nothing here unwinds, flushes, or
// releases anything on the way out.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "power_capacity/engine.hpp"
#include "power_capacity/store.hpp"
#include "support/facility.hpp"
#include "support/proc.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_load;
using pc_test::probe_path;
using pc_test::TempDir;

namespace {

constexpr int kReadyAttempts = 4000;

StoreOpenOptions writer_options(const std::filesystem::path& path) {
  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = false;
  return options;
}

bool wait_ready(const std::filesystem::path& ready, std::string& error) {
  return pc_test::wait_for_file(ready, kReadyAttempts, error);
}

}  // namespace

PC_TEST(recovery, a_live_process_holds_writer_authority_against_this_process) {
  TempDir directory;
  const std::filesystem::path store = directory.file("authority.pcstore");
  const std::filesystem::path ready = directory.file("ready.txt");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  }

  std::string error;
  PC_CHECK_MSG(pc_test::spawn_nowait(probe_path(),
                                     {"hold-forever", "--store", store.string(), "--ready",
                                      ready.string(), "--tick", "0"},
                                     error),
               error);
  PC_CHECK_MSG(wait_ready(ready, error), error);
  const std::uint64_t pid = pc_test::read_pid(ready);
  PC_CHECK(pid != 0);

  // The other process holds the lock, so this process must be refused.
  const auto refused = CapacityStore::open(writer_options(store));
  PC_CHECK(!refused.ok());
  PC_CHECK_EQ(refused.status().code(), StatusCode::LockConflict);

  // A reader is not excluded: reading the committed artifact does not need
  // writer authority.
  StoreOpenOptions reader;
  reader.path = store;
  reader.access = StoreAccess::ReadOnly;
  const auto readable = CapacityStore::open(reader);
  PC_CHECK(readable.ok());
  if (readable.ok()) {
    PC_CHECK(readable.value()->close().ok());
  }

  PC_CHECK_MSG(pc_test::terminate_process(pid, error), error);

  // After an operating-system level force kill the authority must be free.
  bool acquired = false;
  std::shared_ptr<CapacityStore> reopened;
  for (int attempt = 0; attempt < 400 && !acquired; ++attempt) {
    const auto candidate = CapacityStore::open(writer_options(store));
    if (candidate.ok()) {
      reopened = candidate.value();
      acquired = true;
      break;
    }
    PC_CHECK_EQ(candidate.status().code(), StatusCode::LockConflict);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  PC_CHECK_MSG(acquired, "writer authority was not released after a force kill");
  if (acquired) {
    PC_CHECK(reopened->close().ok());
  }
}

PC_TEST(recovery, process_death_without_cleanup_relinquishes_writer_authority) {
  TempDir directory;
  const std::filesystem::path store = directory.file("death.pcstore");
  const std::filesystem::path ready = directory.file("death-ready.txt");

  int exit_code = 0;
  std::string error;
  PC_CHECK_MSG(pc_test::spawn_wait(probe_path(),
                                   {"hold-and-die", "--store", store.string(), "--ready",
                                    ready.string(), "--tick", "7"},
                                   exit_code, error),
               error);
  PC_CHECK_EQ(exit_code, 17);
  PC_CHECK(std::filesystem::exists(store));

  // The process that held the lock is gone with no unwinding at all. The
  // operating system must have released the lock.
  const auto reopened = CapacityStore::open(writer_options(store));
  PC_CHECK_MSG(reopened.ok(), reopened.ok() ? "" : reopened.status().to_string());
  if (reopened.ok()) {
    PC_CHECK_EQ(reopened.value()->info().generation.value(), std::uint64_t{1});
    PC_CHECK(reopened.value()->close().ok());
  }
}

PC_TEST(recovery, a_commit_survives_the_death_of_the_committing_process) {
  TempDir directory;
  const std::filesystem::path store = directory.file("durable.pcstore");
  const std::filesystem::path ready = directory.file("durable-ready.txt");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.base").nominal_watts(1000).usable_watts(1000));
  }

  int exit_code = 0;
  std::string error;
  PC_CHECK_MSG(pc_test::spawn_wait(probe_path(),
                                   {"commit-and-die", "--store", store.string(), "--ready",
                                    ready.string(), "--tick", "50", "--domain", "pdu.fromprobe",
                                    "--nominal-w", "250000"},
                                   exit_code, error),
               error);
  PC_CHECK_EQ(exit_code, 18);

  const auto committed = pc_test::read_key_values(ready);
  PC_CHECK(committed.find("generation") != committed.end());
  const auto read = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(read.ok());
  if (!read.ok()) {
    return;
  }
  const std::shared_ptr<const CapacityState> state = read.value();
  PC_CHECK(state->find_domain(PC_REQUIRE_OK(DomainId::parse("pdu.fromprobe", 128))) != nullptr);
  PC_CHECK_EQ(state->domain_count(), std::size_t{2});
  PC_CHECK_EQ(state->derivation_of(PC_REQUIRE_OK(DomainId::parse("pdu.fromprobe", 128)))
                  ->allocatable_headroom.milliwatts(),
              250000000);
  if (committed.find("generation") != committed.end()) {
    PC_CHECK_EQ(std::to_string(state->generation().value()), committed.at("generation"));
  }
}

PC_TEST(recovery, an_independent_process_reads_the_committed_state) {
  TempDir directory;
  const std::filesystem::path store = directory.file("crossproc.pcstore");
  const std::filesystem::path out = directory.file("crossproc-out.txt");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
    facility.add_domain(DomainBuilder("pdu.b").nominal_watts(2000).usable_watts(2000));
    facility.add_load(make_load("load.a", "pdu.a", 500));
  }
  const auto initial = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(initial.ok());
  if (!initial.ok()) {
    return;
  }
  const StoreIdentity identity = initial.value()->store_identity();

  int exit_code = 0;
  std::string error;
  PC_CHECK_MSG(pc_test::spawn_wait(probe_path(),
                                   {"read", "--store", store.string(), "--out", out.string()},
                                   exit_code, error),
               error);
  PC_CHECK_EQ(exit_code, 0);
  const auto values = pc_test::read_key_values(out);
  PC_CHECK_EQ(values.at("store_id"), identity.to_hex());
  PC_CHECK_EQ(values.at("domains"), std::string("2"));
  PC_CHECK_EQ(values.at("loads"), std::string("1"));
  PC_CHECK(values.find("domain") != values.end());
}

PC_TEST(recovery, a_force_kill_during_a_commit_leaves_a_valid_store) {
  TempDir directory;
  const std::filesystem::path store = directory.file("crashed.pcstore");
  const std::filesystem::path ready = directory.file("crashed-ready.txt");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.base").nominal_watts(1000).usable_watts(1000));
  }
  const Generation before =
      PC_REQUIRE_OK(CapacityStore::read_file(store, StoreReadOptions{}))->generation();

  std::string error;
  PC_CHECK_MSG(pc_test::spawn_nowait(probe_path(),
                                     {"commit-loop", "--store", store.string(), "--ready",
                                      ready.string(), "--tick", "10", "--start", "0"},
                                     error),
               error);
  PC_CHECK_MSG(wait_ready(ready, error), error);
  const std::uint64_t pid = pc_test::read_pid(ready);
  PC_CHECK(pid != 0);
  // Let the loop get well into committing before the kill lands.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  PC_CHECK_MSG(pc_test::terminate_process(pid, error), error);

  // The store must open and be structurally whole. It must never be a mixture of
  // the old and the new state, and it must never be unreadable.
  Status verified = Status::error(StatusCode::Corruption, "not attempted");
  std::shared_ptr<const CapacityState> state;
  for (int attempt = 0; attempt < 400; ++attempt) {
    verified = CapacityStore::verify_file(store);
    if (verified.ok()) {
      const auto read = CapacityStore::read_file(store, StoreReadOptions{});
      if (read.ok()) {
        state = read.value();
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  PC_CHECK_MSG(verified.ok(), verified.to_string());
  if (state == nullptr) {
    return;
  }
  PC_CHECK(state->generation() >= before);
  // One domain per committed generation after the empty generation one and the
  // parent's own setup commit, so the model and the generation can never drift.
  PC_CHECK_EQ(state->domain_count(), static_cast<std::size_t>(state->generation().value() - 1));
  for (const auto& entry : state->domains()) {
    const CapacityDerivation* derivation = state->derivation_of(entry.first);
    PC_CHECK(derivation != nullptr);
    if (derivation != nullptr) {
      PC_CHECK(derivation->allocatable_headroom <= derivation->carryable_capacity);
      PC_CHECK(derivation->carryable_capacity <= derivation->safe_capacity);
      PC_CHECK(derivation->safe_capacity <= derivation->operational);
    }
  }
}

PC_TEST(recovery, staging_residue_from_a_dead_process_is_swept_and_never_adopted) {
  TempDir directory;
  const std::filesystem::path store = directory.file("residue.pcstore");
  const std::filesystem::path other = directory.file("other.pcstore");
  const std::filesystem::path ready = directory.file("residue-ready.txt");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.original").nominal_watts(1000).usable_watts(1000));
  }
  {
    Facility facility(other, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.other").nominal_watts(9999).usable_watts(9999));
    facility.add_domain(DomainBuilder("pdu.other2").nominal_watts(9999).usable_watts(9999));
  }
  const Generation before =
      PC_REQUIRE_OK(CapacityStore::read_file(store, StoreReadOptions{}))->generation();

  // A different process copies a completely valid artifact to the staging path
  // of this store and dies. A crash between the staging write and the publish
  // leaves exactly this on disk.
  int exit_code = 0;
  std::string error;
  PC_CHECK_MSG(pc_test::spawn_wait(probe_path(),
                                   {"residue-and-die", "--store", store.string(), "--ready",
                                    ready.string(), "--source", other.string()},
                                   exit_code, error),
               error);
  PC_CHECK_EQ(exit_code, 19);

  const auto opened = CapacityStore::open(writer_options(store));
  PC_CHECK(opened.ok());
  if (!opened.ok()) {
    return;
  }
  // The residue was never adopted: the committed state is still the original.
  PC_CHECK_EQ(opened.value()->info().generation.value(), before.value());
  PC_CHECK_EQ(opened.value()->info().domain_count, std::size_t{1});
  const auto state = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(state.ok());
  if (state.ok()) {
    PC_CHECK(state.value()->find_domain(PC_REQUIRE_OK(DomainId::parse("pdu.original", 128))) !=
             nullptr);
    PC_CHECK(state.value()->find_domain(PC_REQUIRE_OK(DomainId::parse("pdu.other", 128))) ==
             nullptr);
  }
  PC_CHECK(opened.value()->close().ok());

  // The sweep removed the residue, and nothing else in the directory was touched.
  std::size_t staging_left = 0;
  for (const std::string& name : directory.entry_names()) {
    if (name.find(".stg-") != std::string::npos) {
      ++staging_left;
    }
  }
  PC_CHECK_EQ(staging_left, std::size_t{0});
  PC_CHECK(std::filesystem::exists(store));
  PC_CHECK(std::filesystem::exists(other));
}

PC_TEST(recovery, a_residue_of_garbage_never_becomes_the_committed_state) {
  TempDir directory;
  const std::filesystem::path store = directory.file("garbage.pcstore");
  const std::filesystem::path ready = directory.file("garbage-ready.txt");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  }
  int exit_code = 0;
  std::string error;
  PC_CHECK_MSG(pc_test::spawn_wait(probe_path(),
                                   {"residue-and-die", "--store", store.string(), "--ready",
                                    ready.string(), "--bytes", "5000"},
                                   exit_code, error),
               error);
  PC_CHECK_EQ(exit_code, 20);

  const auto opened = CapacityStore::open(writer_options(store));
  PC_CHECK(opened.ok());
  if (!opened.ok()) {
    return;
  }
  PC_CHECK_EQ(opened.value()->info().domain_count, std::size_t{1});
  PC_CHECK(opened.value()->close().ok());
}

PC_TEST(recovery, two_engines_cannot_both_hold_writer_authority) {
  TempDir directory;
  const std::filesystem::path store = directory.file("two.pcstore");
  const std::filesystem::path ready = directory.file("two-ready.txt");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  }
  std::string error;
  PC_CHECK_MSG(pc_test::spawn_nowait(probe_path(),
                                     {"hold-forever", "--store", store.string(), "--ready",
                                      ready.string(), "--tick", "0"},
                                     error),
               error);
  PC_CHECK_MSG(wait_ready(ready, error), error);
  const std::uint64_t pid = pc_test::read_pid(ready);

  EngineOptions options;
  options.store_path = store;
  options.create_if_missing = false;
  const auto engine = CapacityEngine::open(options);
  PC_CHECK(!engine.ok());
  PC_CHECK_EQ(engine.status().code(), StatusCode::LockConflict);

  PC_CHECK_MSG(pc_test::terminate_process(pid, error), error);
}
