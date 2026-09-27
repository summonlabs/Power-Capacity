// Persistence: the artifact envelope, integrity checks, strict read paths,
// identity and path binding, and close/reopen.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "power_capacity/engine.hpp"
#include "power_capacity/store.hpp"
#include "support/facility.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_load;
using pc_test::TempDir;

namespace {

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const std::streamsize count = stream.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
    }
  }
  return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.flush();
}

/// A store with two domains and one load, ready for reopening.
void build_store(const std::filesystem::path& path) {
  Facility facility(path, false, Tick(100));
  facility.set_as_of(Tick(120));
  facility.add_domain(DomainBuilder("busway.main").nominal_watts(400000).usable_watts(400000)
                          .evidence(Tick(0), Tick(100000)));
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(200000).usable_watts(200000)
                          .parent("busway.main")
                          .evidence(Tick(0), Tick(100000)));
  facility.add_load(make_load("load.a", "pdu.a", 10000));
}

}  // namespace

PC_TEST(persistence, create_close_and_reopen_preserves_the_model) {
  TempDir directory;
  const std::filesystem::path store = directory.file("facility.pcstore");
  build_store(store);

  StoreOpenOptions options;
  options.path = store;
  options.access = StoreAccess::ReadOnly;
  const auto reopened = CapacityStore::open(options);
  PC_CHECK(reopened.ok());
  if (!reopened.ok()) {
    return;
  }
  const StoreInfo& info = reopened.value()->info();
  PC_CHECK_EQ(info.domain_count, std::size_t{2});
  PC_CHECK_EQ(info.load_count, std::size_t{1});
  PC_CHECK(!info.writable);
  PC_CHECK(info.path_binding_matches);

  const auto state = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(state.ok());
  PC_CHECK_EQ(state.value()->rolled_up_committed_of(
                  PC_REQUIRE_OK(DomainId::parse("busway.main", 128)))
                  .milliwatts(),
              10000000);
  PC_CHECK_EQ(state.value()->derivation_of(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)))
                  ->allocatable_headroom.milliwatts(),
              190000000);
  PC_CHECK(reopened.value()->close().ok());
}

PC_TEST(persistence, a_fresh_store_starts_at_generation_one_with_an_empty_model) {
  TempDir directory;
  const std::filesystem::path store = directory.file("fresh.pcstore");
  Facility facility(store, false, Tick(5));
  PC_CHECK_EQ(facility.generation().value(), std::uint64_t{1});
  PC_CHECK_EQ(facility.state()->domain_count(), std::size_t{0});
  PC_CHECK_EQ(facility.state()->store_identity().is_zero(), false);
}

PC_TEST(persistence, a_reopened_store_requires_revalidation_before_answering) {
  TempDir directory;
  const std::filesystem::path store = directory.file("recover.pcstore");
  build_store(store);

  EngineOptions options;
  options.store_path = store;
  options.create_if_missing = false;
  const auto engine = CapacityEngine::open(options);
  PC_CHECK(engine.ok());
  if (!engine.ok()) {
    return;
  }
  PC_CHECK_EQ(engine.value()->lifecycle(), EngineLifecycle::RecoveredPendingRevalidation);

  CapacityQuery query;
  query.domain = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  query.as_of = Tick(120);
  PC_REQUIRE_STATUS(engine.value()->assess(query), StatusCode::NotRevalidated);

  const auto refused = engine.value()->put_domain(
      DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
      PresenceExpectation::MustNotExist,
      MutationContext{Tick(120), Epoch(0), Incarnation(0), Generation(1), {}, {}, std::nullopt});
  PC_REQUIRE_STATUS(refused.status(), StatusCode::NotRevalidated);

  RevalidationRequest request;
  request.as_of = Tick(120);
  const auto report = engine.value()->revalidate_recovered_state(request);
  PC_CHECK(report.ok());
  if (!report.ok()) {
    return;
  }
  PC_CHECK_EQ(report.value().domains_total, std::size_t{2});
  PC_CHECK_EQ(report.value().domains_current, std::size_t{2});
  PC_CHECK(report.value().all_evidence_current);
  PC_CHECK_EQ(engine.value()->lifecycle(), EngineLifecycle::Current);
  PC_CHECK(engine.value()->assess(query).ok());
}

PC_TEST(persistence, revalidation_reports_stale_domains_without_hiding_them) {
  TempDir directory;
  const std::filesystem::path store = directory.file("stale.pcstore");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.fresh").nominal_watts(1000).usable_watts(1000)
                            .evidence(Tick(0), Tick(100000)));
    facility.add_domain(DomainBuilder("pdu.stale").nominal_watts(1000).usable_watts(1000)
                            .evidence(Tick(0), Tick(50)));
    facility.add_domain(DomainBuilder("pdu.none").nominal_watts(1000).usable_watts(1000).no_evidence());
  }
  EngineOptions options;
  options.store_path = store;
  const auto engine = CapacityEngine::open(options);
  PC_CHECK(engine.ok());
  if (!engine.ok()) {
    return;
  }
  RevalidationRequest request;
  request.as_of = Tick(100);
  const auto report = engine.value()->revalidate_recovered_state(request);
  PC_CHECK(report.ok());
  if (!report.ok()) {
    return;
  }
  PC_CHECK_EQ(report.value().domains_total, std::size_t{3});
  PC_CHECK_EQ(report.value().domains_current, std::size_t{1});
  PC_CHECK_EQ(report.value().domains_stale, std::size_t{1});
  PC_CHECK_EQ(report.value().domains_missing_evidence, std::size_t{1});
  PC_CHECK(!report.value().all_evidence_current);
  PC_CHECK_EQ(report.value().stale_domains.size(), std::size_t{1});
  PC_CHECK_EQ(report.value().stale_domains.front().value(), std::string("pdu.stale"));
}

PC_TEST(persistence, a_truncated_artifact_is_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("truncated.pcstore");
  build_store(store);
  std::vector<std::byte> bytes = read_bytes(store);
  for (const std::size_t keep : {std::size_t{0}, std::size_t{8}, std::size_t{40},
                                 std::size_t{80}, bytes.size() - 1}) {
    const std::filesystem::path broken = directory.file("broken.pcstore");
    write_bytes(broken, std::vector<std::byte>(bytes.begin(),
                                               bytes.begin() + static_cast<std::ptrdiff_t>(keep)));
    PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::Corruption);
  }
}

PC_TEST(persistence, each_header_field_is_checked) {
  TempDir directory;
  const std::filesystem::path store = directory.file("fields.pcstore");
  build_store(store);
  const std::vector<std::byte> original = read_bytes(store);

  const auto flip = [&](std::size_t offset, std::byte value, StatusCode expected,
                        const char* label) {
    std::vector<std::byte> bytes = original;
    bytes[offset] = value;
    const std::filesystem::path broken = directory.file("field.pcstore");
    write_bytes(broken, bytes);
    const Status status = CapacityStore::verify_file(broken);
    PC_CHECK_MSG(status.code() == expected,
                 std::string(label) + ": expected " + to_string(expected) + " but got " +
                     status.to_string());
  };

  flip(0, std::byte{'X'}, StatusCode::Corruption, "magic");
  // Format version lives at byte 8, little endian.
  flip(8, std::byte{9}, StatusCode::IncompatibleVersion, "version");
  // Flags live at byte 20.
  flip(20, std::byte{1}, StatusCode::IncompatibleVersion, "flags");
  // Payload length lives at byte 24.
  flip(24, std::byte{0xFF}, StatusCode::Corruption, "payload length");
  // Payload CRC lives at byte 32.
  flip(32, std::byte{0x5A}, StatusCode::Corruption, "payload crc");
  // Header CRC lives at byte 36.
  flip(36, std::byte{0x5A}, StatusCode::Corruption, "header crc");
  // Reserved header field lives at byte 64.
  flip(64, std::byte{1}, StatusCode::Corruption, "reserved");

  // A byte-swapped endian marker is the one mutation that must be reported as an
  // endianness problem rather than as generic corruption.
  {
    std::vector<std::byte> bytes = original;
    bytes[16] = std::byte{0x01};
    bytes[17] = std::byte{0x02};
    bytes[18] = std::byte{0x03};
    bytes[19] = std::byte{0x04};
    const std::filesystem::path broken = directory.file("endian.pcstore");
    write_bytes(broken, bytes);
    PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::EndianMismatch);
  }
  // Any other endian marker value is generic corruption.
  flip(16, std::byte{0x07}, StatusCode::Corruption, "endian marker");
}

PC_TEST(persistence, payload_corruption_is_detected) {
  TempDir directory;
  const std::filesystem::path store = directory.file("payload.pcstore");
  build_store(store);
  const std::vector<std::byte> original = read_bytes(store);
  // Sample the payload at several offsets rather than every byte, so the test
  // stays fast while still covering the whole body.
  for (std::size_t offset = 72; offset + 1 < original.size() - 16; offset += 17) {
    std::vector<std::byte> bytes = original;
    bytes[offset] = static_cast<std::byte>(static_cast<unsigned>(bytes[offset]) ^ 0x40u);
    const std::filesystem::path broken = directory.file("payload-broken.pcstore");
    write_bytes(broken, bytes);
    PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::Corruption);
  }
}

PC_TEST(persistence, trailing_bytes_are_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("trailing.pcstore");
  build_store(store);
  std::vector<std::byte> bytes = read_bytes(store);
  bytes.push_back(std::byte{0});
  const std::filesystem::path broken = directory.file("trailing-broken.pcstore");
  write_bytes(broken, bytes);
  PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::Corruption);
}

PC_TEST(persistence, an_oversized_artifact_is_refused_before_it_is_read) {
  TempDir directory;
  const std::filesystem::path store = directory.file("big.pcstore");
  build_store(store);
  ResourceLimits limits = ResourceLimits::defaults();
  limits.max_store_bytes = 64;
  const auto state = CapacityStore::read_file(store, StoreReadOptions{limits});
  PC_CHECK(!state.ok());
  PC_CHECK_EQ(state.status().code(), StatusCode::LimitExceeded);
  PC_REQUIRE_STATUS(CapacityStore::verify_file(store, StoreReadOptions{limits}), StatusCode::LimitExceeded);
}

PC_TEST(persistence, declared_counts_are_bounded_by_the_configured_limits) {
  TempDir directory;
  const std::filesystem::path store = directory.file("counts.pcstore");
  build_store(store);
  ResourceLimits limits = ResourceLimits::defaults();
  limits.max_domains = 1;
  const auto state = CapacityStore::read_file(store, StoreReadOptions{limits});
  PC_CHECK(!state.ok());
  PC_CHECK_EQ(state.status().code(), StatusCode::LimitExceeded);
}

PC_TEST(persistence, a_swapped_store_is_refused_by_identity) {
  TempDir directory;
  const std::filesystem::path first = directory.file("first.pcstore");
  const std::filesystem::path second = directory.file("second.pcstore");
  build_store(first);
  build_store(second);

  const auto first_state = CapacityStore::read_file(first, StoreReadOptions{});
  PC_CHECK(first_state.ok());
  const StoreIdentity identity = first_state.value()->store_identity();

  // The second store is copied over the first path: a swap, not a corruption.
  std::error_code error;
  std::filesystem::copy_file(second, first, std::filesystem::copy_options::overwrite_existing,
                             error);
  PC_CHECK(!error);

  const auto refused = CapacityStore::read_file(first, StoreReadOptions{.expected_store_identity = identity});
  PC_CHECK(!refused.ok());
  PC_CHECK_EQ(refused.status().code(), StatusCode::StaleAuthority);

  // Without an expected identity the store is still readable, which is exactly
  // why the caller has to supply one when it cares.
  PC_CHECK(CapacityStore::read_file(first, StoreReadOptions{}).ok());
}

PC_TEST(persistence, path_binding_can_be_enforced) {
  TempDir directory;
  const std::filesystem::path original = directory.file("original.pcstore");
  const std::filesystem::path moved = directory.file("moved.pcstore");
  build_store(original);
  std::error_code error;
  std::filesystem::copy_file(original, moved, error);
  PC_CHECK(!error);

  PC_CHECK(CapacityStore::read_file(moved, StoreReadOptions{}).ok());
  const auto refused = CapacityStore::read_file(moved, StoreReadOptions{.enforce_path_binding = true});
  PC_CHECK(!refused.ok());
  PC_CHECK_EQ(refused.status().code(), StatusCode::Conflict);

  StoreOpenOptions options;
  options.path = moved;
  options.access = StoreAccess::ReadOnly;
  options.enforce_path_binding = true;
  PC_REQUIRE_STATUS(CapacityStore::open(options).status(), StatusCode::Conflict);

  // The path the store was created at is still readable with the check on.
  PC_CHECK(CapacityStore::read_file(original, StoreReadOptions{.enforce_path_binding = true}).ok());
}

PC_TEST(persistence, a_directory_in_place_of_a_store_is_refused) {
  TempDir directory;
  const std::filesystem::path path = directory.file("directory.pcstore");
  std::error_code error;
  std::filesystem::create_directory(path, error);
  PC_CHECK(!error);

  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(!opened.ok());
  PC_CHECK_EQ(opened.status().code(), StatusCode::InvalidArgument);
  // The strict read path fails too; the exact code depends on how the platform
  // reports the size of a directory, so only the failure is asserted here.
  PC_CHECK(!CapacityStore::verify_file(path).ok());
}

PC_TEST(persistence, a_missing_store_is_not_found_and_is_never_created_by_a_read) {
  TempDir directory;
  const std::filesystem::path path = directory.file("missing.pcstore");
  PC_REQUIRE_STATUS(CapacityStore::verify_file(path), StatusCode::NotFound);

  StoreOpenOptions options;
  options.path = path;
  options.access = StoreAccess::ReadOnly;
  options.create_if_missing = true;
  PC_REQUIRE_STATUS(CapacityStore::open(options).status(), StatusCode::NotFound);
  PC_CHECK(!std::filesystem::exists(path));

  StoreOpenOptions writer;
  writer.path = path;
  writer.access = StoreAccess::ReadWrite;
  writer.create_if_missing = false;
  PC_REQUIRE_STATUS(CapacityStore::open(writer).status(), StatusCode::NotFound);
  PC_CHECK(!std::filesystem::exists(path));
}

PC_TEST(persistence, a_writer_excludes_a_second_writer_but_not_a_reader) {
  TempDir directory;
  const std::filesystem::path store = directory.file("exclusive.pcstore");
  build_store(store);

  StoreOpenOptions writer;
  writer.path = store;
  writer.access = StoreAccess::ReadWrite;
  const auto first = CapacityStore::open(writer);
  PC_CHECK(first.ok());

  const auto second = CapacityStore::open(writer);
  PC_CHECK(!second.ok());
  PC_CHECK_EQ(second.status().code(), StatusCode::LockConflict);

  StoreOpenOptions reader;
  reader.path = store;
  reader.access = StoreAccess::ReadOnly;
  const auto read_only = CapacityStore::open(reader);
  PC_CHECK(read_only.ok());
  if (read_only.ok()) {
    PC_CHECK(read_only.value()->close().ok());
  }
  PC_CHECK(first.value()->close().ok());

  // Once the writer released the lock, another writer may take it.
  const auto third = CapacityStore::open(writer);
  PC_CHECK(third.ok());
  if (third.ok()) {
    PC_CHECK(third.value()->close().ok());
  }
}

PC_TEST(persistence, opening_a_missing_store_with_create_publishes_a_complete_artifact) {
  TempDir directory;
  const std::filesystem::path store = directory.file("created.pcstore");
  StoreOpenOptions options;
  options.path = store;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  options.created_at = Tick(42);
  options.epoch = Epoch(3);
  options.incarnation = Incarnation(4);
  const auto opened = CapacityStore::open(options);
  PC_CHECK(opened.ok());
  if (!opened.ok()) {
    return;
  }
  PC_CHECK(opened.value()->info().created);
  PC_CHECK_EQ(opened.value()->info().generation.value(), std::uint64_t{1});
  PC_CHECK_EQ(opened.value()->info().epoch.value(), std::uint64_t{3});
  PC_CHECK_EQ(opened.value()->info().incarnation.value(), std::uint64_t{4});
  PC_CHECK(opened.value()->close().ok());

  const auto state = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(state.ok());
  PC_CHECK_EQ(state.value()->epoch().value(), std::uint64_t{3});
  PC_CHECK_EQ(state.value()->created_at().value(), std::int64_t{42});
}

PC_TEST(persistence, a_commit_that_skips_a_generation_is_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("generation.pcstore");
  StoreOpenOptions options;
  options.path = store;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(opened.ok());
  if (!opened.ok()) {
    return;
  }
  const auto current = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(current.ok());

  CapacityContent content = current.value()->content();
  content.generation = Generation(5);
  const auto skipped = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(skipped.ok());
  if (skipped.ok()) {
    const auto committed = opened.value()->commit(*skipped.value());
    PC_CHECK(!committed.ok());
    PC_CHECK_EQ(committed.status().code(), StatusCode::StaleGeneration);
  }

  CapacityContent other = current.value()->content();
  other.store_identity = StoreIdentity::generate();
  other.generation = Generation(current.value()->generation().value() + 1);
  const auto foreign = CapacityState::build(std::move(other), ResourceLimits::defaults());
  PC_CHECK(foreign.ok());
  if (foreign.ok()) {
    const auto committed = opened.value()->commit(*foreign.value());
    PC_CHECK(!committed.ok());
    PC_CHECK_EQ(committed.status().code(), StatusCode::StaleAuthority);
  }
  PC_CHECK(opened.value()->close().ok());
}

PC_TEST(persistence, staging_residue_from_an_interrupted_commit_is_swept) {
  TempDir directory;
  const std::filesystem::path store = directory.file("residue.pcstore");
  build_store(store);
  const auto before = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(before.ok());

  // Residue with the right shape and the wrong content, exactly what a process
  // killed between the staging write and the publish would leave behind.
  const std::filesystem::path residue = directory.file("residue.pcstore.stg-1234-deadbeef-1");
  write_bytes(residue, std::vector<std::byte>(512, std::byte{'R'}));
  PC_CHECK(std::filesystem::exists(residue));

  StoreOpenOptions options;
  options.path = store;
  options.access = StoreAccess::ReadWrite;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(opened.ok());
  if (!opened.ok()) {
    return;
  }
  PC_CHECK(!std::filesystem::exists(residue));
  PC_CHECK_EQ(opened.value()->info().generation.value(), before.value()->generation().value());
  PC_CHECK_EQ(opened.value()->info().domain_count, std::size_t{2});
  PC_CHECK(opened.value()->close().ok());
}

PC_TEST(persistence, the_strict_read_path_and_the_open_path_agree) {
  TempDir directory;
  const std::filesystem::path store = directory.file("agree.pcstore");
  build_store(store);
  std::vector<std::byte> bytes = read_bytes(store);
  bytes[100] = static_cast<std::byte>(static_cast<unsigned>(bytes[100]) ^ 0x20u);
  const std::filesystem::path broken = directory.file("agree-broken.pcstore");
  write_bytes(broken, bytes);

  const Status verified = CapacityStore::verify_file(broken);
  const auto read = CapacityStore::read_file(broken, StoreReadOptions{});
  StoreOpenOptions options;
  options.path = broken;
  options.access = StoreAccess::ReadOnly;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(!verified.ok());
  PC_CHECK(!read.ok());
  PC_CHECK(!opened.ok());
  PC_CHECK_EQ(verified.code(), read.status().code());
  PC_CHECK_EQ(verified.code(), opened.status().code());
  PC_CHECK_EQ(verified.to_string(), read.status().to_string());
}

PC_TEST(persistence, an_empty_file_and_a_short_envelope_are_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("empty.pcstore");
  write_bytes(store, {});
  PC_REQUIRE_STATUS(CapacityStore::verify_file(store), StatusCode::Corruption);
  write_bytes(store, std::vector<std::byte>(87, std::byte{0}));
  PC_REQUIRE_STATUS(CapacityStore::verify_file(store), StatusCode::Corruption);
  write_bytes(store, std::vector<std::byte>(88, std::byte{0}));
  PC_REQUIRE_STATUS(CapacityStore::verify_file(store), StatusCode::Corruption);
}

PC_TEST(persistence, a_unicode_store_path_round_trips) {
  TempDir directory;
  const std::filesystem::path store =
      directory.path() / std::filesystem::path(u8"capacit\u00e9-\u6d4b\u8bd5.pcstore");
  StoreOpenOptions options;
  options.path = store;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(opened.ok());
  if (!opened.ok()) {
    return;
  }
  PC_CHECK(opened.value()->close().ok());
  PC_CHECK(std::filesystem::exists(store));
  PC_CHECK(CapacityStore::verify_file(store).ok());
}
