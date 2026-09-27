// Adversarial tests: malformed, oversized, contradictory, cyclic, and
// deliberately well-formed-but-semantically-broken input.
//
// The last category matters most. A valid checksum proves only that the bytes
// were not corrupted in transit; it says nothing about whether the payload obeys
// the model. These tests craft artifacts whose envelope and checksums are
// perfect and whose contents are impossible, and require the reader to refuse
// them anyway.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "detail/codec.hpp"
#include "detail/crc32c.hpp"
#include "detail/serialization.hpp"
#include "power_capacity/engine.hpp"
#include "power_capacity/store.hpp"
#include "support/facility.hpp"
#include "support/test_harness.hpp"

using namespace power_capacity;
using pc_test::DomainBuilder;
using pc_test::Facility;
using pc_test::make_group;
using pc_test::make_load;
using pc_test::TempDir;

namespace {

constexpr std::size_t kHeaderBytes = 72;
constexpr std::size_t kFooterBytes = 16;

std::uint32_t read_u32(const std::vector<std::byte>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(index)])
             << (8 * index);
  }
  return value;
}

std::uint64_t read_u64(const std::vector<std::byte>& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)])
             << (8 * index);
  }
  return value;
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::byte>((value >> (8 * index)) & 0xFFu);
  }
}

/// Recomputes the payload CRC, the header CRC, and the footer CRCs after the body
/// has been edited. The result is an artifact that passes every integrity check
/// and can only be refused on its semantics.
void reseal_envelope(std::vector<std::byte>& bytes) {
  const std::size_t payload_bytes = bytes.size() - kHeaderBytes - kFooterBytes;
  const std::span<const std::byte> payload(bytes.data() + kHeaderBytes, payload_bytes);
  const std::uint32_t payload_crc = detail::crc32c(payload);
  write_u32(bytes, 32, payload_crc);

  const std::span<const std::byte> header_prefix(bytes.data(), 36);
  write_u32(bytes, 36, detail::crc32c(header_prefix));

  const std::size_t footer = bytes.size() - kFooterBytes;
  write_u32(bytes, footer + 8, payload_crc);
  const std::span<const std::byte> footer_prefix(bytes.data() + footer, 12);
  write_u32(bytes, footer + 12, detail::crc32c(footer_prefix));
}

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

/// Rebuilds a complete, checksum-correct artifact from modified content. This is
/// what an attacker with write access to the file can trivially produce, and it
/// is what the semantic checks have to survive.
std::vector<std::byte> reseal(const CapacityContent& content) {
  const auto encoded = detail::encode_artifact(content, ResourceLimits::defaults());
  if (!encoded.ok()) {
    return {};
  }
  return encoded.value().bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.flush();
}

CapacityContent load_content(const std::filesystem::path& store) {
  const auto decoded = detail::decode_artifact(read_bytes(store), ResourceLimits::defaults());
  if (!decoded.ok()) {
    throw std::runtime_error("test could not decode its own store");
  }
  return decoded.value().content;
}

CapacityContent base_content(const std::filesystem::path& directory) {
  const std::filesystem::path store = directory / "base.pcstore";
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("busway.main").nominal_watts(100000).usable_watts(100000));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(50000).usable_watts(50000)
                            .parent("busway.main"));
    facility.add_domain(DomainBuilder("pdu.b").nominal_watts(50000).usable_watts(50000)
                            .parent("busway.main"));
    facility.add_load(make_load("load.a", "pdu.a", 1000));
    facility.add_group(make_group("group.pdu", {"pdu.a", "pdu.b"}, 1));
  }
  return load_content(store);
}

}  // namespace

PC_TEST(adversarial, cyclic_containment_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  // Make the root a child of one of its own children.
  auto root = content.domains.find(PC_REQUIRE_OK(DomainId::parse("busway.main", 128)));
  PC_CHECK(root != content.domains.end());
  root->second.parent = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));

  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::InvariantViolation);
}

PC_TEST(adversarial, a_longer_containment_cycle_is_also_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  // busway -> pdu.a -> busway, expressed as a three-node cycle.
  auto pdu_a = content.domains.find(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)));
  auto pdu_b = content.domains.find(PC_REQUIRE_OK(DomainId::parse("pdu.b", 128)));
  PC_CHECK(pdu_a != content.domains.end());
  PC_CHECK(pdu_b != content.domains.end());
  pdu_a->second.parent = PC_REQUIRE_OK(DomainId::parse("pdu.b", 128));
  pdu_b->second.parent = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::InvariantViolation);
}

PC_TEST(adversarial, containment_deeper_than_the_limit_is_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("deep.pcstore");
  const std::uint32_t limit = ResourceLimits::defaults().max_tree_depth;
  {
    Facility facility(store, false, Tick(0));
    for (std::uint32_t index = 0; index <= limit; ++index) {
      DomainBuilder builder("depth." + std::to_string(index));
      builder.nominal_watts(1000).usable_watts(1000);
      if (index > 0) {
        builder.parent("depth." + std::to_string(index - 1));
      }
      facility.add_domain(builder.build());
    }
  }
  const auto state = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(state.ok());
  if (state.ok()) {
    PC_CHECK_EQ(state.value()->depth_of(PC_REQUIRE_OK(DomainId::parse("depth." + std::to_string(limit),
                                                                      128))),
                limit);
  }

  // One level deeper than the limit must be refused by the same read path.
  CapacityContent content = load_content(store);
  PowerDomain extra = DomainBuilder("depth.extra").nominal_watts(1000).usable_watts(1000)
                          .parent("depth." + std::to_string(limit))
                          .build();
  content.domains.emplace(extra.id, extra);
  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::LimitExceeded);
}

PC_TEST(adversarial, a_missing_parent_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  auto pdu_a = content.domains.find(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)));
  pdu_a->second.parent = PC_REQUIRE_OK(DomainId::parse("does.not.exist", 128));
  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::NotFound);
}

PC_TEST(adversarial, a_load_on_an_unknown_domain_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  LoadRecord orphan = make_load("load.orphan", "pdu.a", 100);
  orphan.domain = PC_REQUIRE_OK(DomainId::parse("ghost", 128));
  content.loads.emplace(orphan.id, orphan);
  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::NotFound);
}

PC_TEST(adversarial, a_group_member_that_does_not_exist_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  auto group = content.groups.find(PC_REQUIRE_OK(GroupId::parse("group.pdu", 128)));
  PC_CHECK(group != content.groups.end());
  // "zzghost" keeps the canonical ascending order, so the only defect left in the
  // artifact is the missing member.
  group->second.members.back() = PC_REQUIRE_OK(DomainId::parse("zzghost", 128));
  std::sort(group->second.members.begin(), group->second.members.end());
  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::NotFound);
}

PC_TEST(adversarial, a_resealed_artifact_with_an_impossible_capacity_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  auto pdu_a = content.domains.find(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)));
  PC_CHECK(pdu_a != content.domains.end());
  // Usable capacity above the nameplate value: the checksums stay valid because
  // the artifact is re-sealed, so only a semantic check can catch this.
  pdu_a->second.usable_capacity = Power(pdu_a->second.nominal_capacity.milliwatts() + 1);

  const std::vector<std::byte> sealed = reseal(content);
  PC_CHECK(!sealed.empty());
  const std::filesystem::path broken = directory.file("impossible.pcstore");
  write_bytes(broken, sealed);
  PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::InvalidArgument);
}

PC_TEST(adversarial, a_resealed_artifact_with_a_degraded_domain_without_a_ratio_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  auto pdu_a = content.domains.find(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)));
  pdu_a->second.state = OperationalState::Degraded;
  pdu_a->second.state_cause = StateCause::Fault;
  pdu_a->second.degradation_ratio = Ratio::full();
  const std::filesystem::path broken = directory.file("degraded.pcstore");
  write_bytes(broken, reseal(content));
  PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::InvalidArgument);
}

PC_TEST(adversarial, a_resealed_artifact_with_a_group_that_is_not_independent_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  auto group = content.groups.find(PC_REQUIRE_OK(GroupId::parse("group.pdu", 128)));
  PC_CHECK(group != content.groups.end());
  // busway.main is an ancestor of pdu.a, so this group would double count.
  group->second.members = {PC_REQUIRE_OK(DomainId::parse("busway.main", 128)),
                           PC_REQUIRE_OK(DomainId::parse("pdu.a", 128))};
  const std::filesystem::path broken = directory.file("overlap.pcstore");
  write_bytes(broken, reseal(content));
  PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::InvariantViolation);
}

PC_TEST(adversarial, a_resealed_artifact_with_a_duplicate_identity_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  // A second record under an existing identity can only be produced by editing
  // the payload, so the artifact now names the same domain twice.
  const DomainId pdu_a = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  PowerDomain duplicate = content.domains.at(pdu_a);
  content.domains.emplace(PC_REQUIRE_OK(DomainId::parse("pdu.zzz", 128)), duplicate);

  const std::filesystem::path broken = directory.file("duplicate.pcstore");
  write_bytes(broken, reseal(content));
  PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::DuplicateIdentity);
}

PC_TEST(adversarial, a_map_key_that_disagrees_with_its_record_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  // A caller that builds content directly can key a record differently from the
  // identity the record carries. The state builder must refuse it rather than
  // silently adopting whichever of the two it happened to read first.
  const DomainId pdu_a = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
  const DomainId alias = PC_REQUIRE_OK(DomainId::parse("pdu.alias", 128));
  PowerDomain misplaced = content.domains.at(pdu_a);
  content.domains.erase(pdu_a);
  content.domains.emplace(alias, misplaced);

  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::InvariantViolation);
}

PC_TEST(adversarial, a_resealed_artifact_that_exceeds_a_limit_is_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  const std::filesystem::path broken = directory.file("many.pcstore");
  write_bytes(broken, reseal(content));

  ResourceLimits limits = ResourceLimits::defaults();
  limits.max_domains = 2;
  const auto state = CapacityStore::read_file(broken, StoreReadOptions{limits});
  PC_CHECK(!state.ok());
  PC_CHECK_EQ(state.status().code(), StatusCode::LimitExceeded);

  limits = ResourceLimits::defaults();
  limits.max_groups = 0;
  PC_REQUIRE_STATUS(CapacityStore::read_file(broken, StoreReadOptions{limits}).status(), StatusCode::LimitExceeded);

  limits = ResourceLimits::defaults();
  limits.max_loads = 0;
  PC_REQUIRE_STATUS(CapacityStore::read_file(broken, StoreReadOptions{limits}).status(), StatusCode::LimitExceeded);

  limits = ResourceLimits::defaults();
  limits.max_identifier_bytes = 4;
  PC_REQUIRE_STATUS(CapacityStore::read_file(broken, StoreReadOptions{limits}).status(), StatusCode::LimitExceeded);
}

PC_TEST(adversarial, a_resealed_artifact_with_an_impossible_declared_count_is_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("base-count.pcstore");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  }
  std::vector<std::byte> bytes = read_bytes(store);

  // Section layout: 72-byte header, then the meta section (tag, length, body),
  // then the domain section whose first field is the declared domain count.
  const std::uint64_t meta_length = read_u64(bytes, kHeaderBytes + 4);
  const std::size_t domain_body = kHeaderBytes + 12 + static_cast<std::size_t>(meta_length) + 12;
  PC_CHECK(domain_body + 4 <= bytes.size() - kFooterBytes);

  write_u32(bytes, domain_body, 0x7FFFFFFFu);
  reseal_envelope(bytes);
  const std::filesystem::path hostile = directory.file("hostile.pcstore");
  write_bytes(hostile, bytes);

  // The envelope and both checksums are valid, so only the declared-count bound
  // can refuse it, and it must refuse before allocating for the claim.
  PC_REQUIRE_STATUS(CapacityStore::verify_file(hostile), StatusCode::LimitExceeded);

  ResourceLimits inflated = ResourceLimits::defaults();
  inflated.max_domains = 0x7FFFFFFFu;
  const auto state = CapacityStore::read_file(hostile, StoreReadOptions{inflated});
  PC_CHECK(!state.ok());
  PC_CHECK_EQ(state.status().code(), StatusCode::Corruption);
}

PC_TEST(adversarial, a_resealed_artifact_with_an_oversized_section_is_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("base-section.pcstore");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  }
  std::vector<std::byte> bytes = read_bytes(store);
  const std::uint64_t meta_length = read_u64(bytes, kHeaderBytes + 4);
  const std::size_t domain_length = kHeaderBytes + 12 + static_cast<std::size_t>(meta_length) + 4;
  PC_CHECK(read_u64(bytes, domain_length) > 0);

  // A declared section length beyond the configured payload bound is refused by
  // the bound, before any allocation is attempted.
  {
    std::vector<std::byte> hostile = bytes;
    for (int index = 0; index < 8; ++index) {
      hostile[domain_length + static_cast<std::size_t>(index)] = std::byte{0xFF};
    }
    reseal_envelope(hostile);
    const std::filesystem::path path = directory.file("oversized-section.pcstore");
    write_bytes(path, hostile);
    PC_REQUIRE_STATUS(CapacityStore::verify_file(path), StatusCode::LimitExceeded);
  }

  // A declared length inside the bound but beyond the bytes that are actually
  // present is refuse as corruption rather than as a truncation of the model.
  {
    std::vector<std::byte> hostile = bytes;
    const std::uint64_t overstated = bytes.size();
    for (int index = 0; index < 8; ++index) {
      hostile[domain_length + static_cast<std::size_t>(index)] =
          static_cast<std::byte>((overstated >> (8 * index)) & 0xFFu);
    }
    reseal_envelope(hostile);
    const std::filesystem::path path = directory.file("overstated-section.pcstore");
    write_bytes(path, hostile);
    PC_REQUIRE_STATUS(CapacityStore::verify_file(path), StatusCode::Corruption);
  }
}

PC_TEST(adversarial, oversized_component_and_aggregate_declarations_are_refused) {
  TempDir directory;
  CapacityContent content = base_content(directory.path());
  auto pdu_a = content.domains.find(PC_REQUIRE_OK(DomainId::parse("pdu.a", 128)));
  pdu_a->second.nominal_capacity = Power(ResourceLimits::defaults().max_component_milliwatts + 1);
  pdu_a->second.usable_capacity = pdu_a->second.nominal_capacity;
  const std::filesystem::path broken = directory.file("oversized.pcstore");
  write_bytes(broken, reseal(content));
  PC_REQUIRE_STATUS(CapacityStore::verify_file(broken), StatusCode::LimitExceeded);
}

PC_TEST(adversarial, an_aggregate_rollup_beyond_the_limit_is_refused) {
  CapacityContent content;
  content.store_identity = StoreIdentity::generate();
  content.generation = Generation(1);
  const std::int64_t component = ResourceLimits::defaults().max_component_milliwatts;

  PowerDomain root = DomainBuilder("big.root").nominal_watts(component / 1000)
                         .usable_watts(component / 1000)
                         .build();
  content.domains.emplace(root.id, root);
  for (int index = 0; index < 11; ++index) {
    const std::string id = "big." + std::to_string(index);
    PowerDomain child = DomainBuilder(id)
                            .nominal_watts(component / 1000)
                            .usable_watts(component / 1000)
                            .parent("big.root")
                            .build();
    content.domains.emplace(child.id, child);
    LoadRecord load = make_load("load.big." + std::to_string(index), id, component / 1000);
    content.loads.emplace(load.id, load);
  }
  // Eleven 100 GW loads roll up to 1.1 PW through the root, above the 1 PW bound,
  // even though every individual declaration is legal.
  const auto built = CapacityState::build(std::move(content), ResourceLimits::defaults());
  PC_CHECK(!built.ok());
  PC_CHECK_EQ(built.status().code(), StatusCode::LimitExceeded);
}

PC_TEST(adversarial, invalid_identifiers_and_labels_are_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.file("identifiers.pcstore");
  Facility facility(store, false, Tick(0));

  const auto with_id = [&](const std::string& id) {
    PowerDomain domain = DomainBuilder("placeholder").nominal_watts(1000).usable_watts(1000).build();
    const auto parsed = DomainId::parse(id, 128);
    if (!parsed.ok()) {
      return parsed.status();
    }
    domain.id = parsed.value();
    return facility.engine()
        .put_domain(domain, PresenceExpectation::MustNotExist, facility.context())
        .status();
  };
  PC_REQUIRE_STATUS(with_id(""), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(with_id("has space"), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(with_id("../escape"), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(with_id("a/b"), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(with_id(std::string(200, 'x')), StatusCode::LimitExceeded);
  PC_REQUIRE_STATUS(with_id("\xff\xfe"), StatusCode::InvalidArgument);

  PowerDomain domain = DomainBuilder("pdu.label").nominal_watts(1000).usable_watts(1000).build();
  domain.label = std::string("bad\0label", 9);
  PC_REQUIRE_STATUS(
      facility.engine().put_domain(domain, PresenceExpectation::MustNotExist, facility.context())
          .status(),
      StatusCode::InvalidArgument);

  domain.label = std::string(4096, 'L');
  PC_REQUIRE_STATUS(
      facility.engine().put_domain(domain, PresenceExpectation::MustNotExist, facility.context())
          .status(),
      StatusCode::LimitExceeded);

  // A control character in a label is refused even though it is valid UTF-8.
  domain.label = "line\nbreak";
  PC_REQUIRE_STATUS(
      facility.engine().put_domain(domain, PresenceExpectation::MustNotExist, facility.context())
          .status(),
      StatusCode::InvalidArgument);
}

PC_TEST(adversarial, contradictory_presence_expectations_are_refused) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  PC_REQUIRE_STATUS(facility.engine()
                        .put_domain(DomainBuilder("pdu.a").nominal_watts(1).usable_watts(1).build(),
                                    PresenceExpectation::MustNotExist, facility.context())
                        .status(),
                    StatusCode::AlreadyExists);
  PC_REQUIRE_STATUS(facility.engine()
                        .put_domain(DomainBuilder("pdu.b").nominal_watts(1).usable_watts(1).build(),
                                    PresenceExpectation::MustExist, facility.context())
                        .status(),
                    StatusCode::NotFound);
}

PC_TEST(adversarial, path_manipulation_through_a_nested_store_path) {
  TempDir directory;
  const std::filesystem::path nested = directory.path() / "a" / "b" / "c";
  std::error_code error;
  std::filesystem::create_directories(nested, error);
  PC_CHECK(!error);
  const std::filesystem::path store = nested / ".." / ".." / ".." / "escape.pcstore";
  StoreOpenOptions options;
  options.path = store;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(opened.ok());
  if (!opened.ok()) {
    return;
  }
  // The store is created at the resolved location, and the recorded binding is
  // normalized, so the same store reopens through either spelling.
  PC_CHECK(opened.value()->close().ok());
  const std::filesystem::path resolved = directory.path() / "escape.pcstore";
  PC_CHECK(std::filesystem::exists(resolved));
  PC_CHECK(CapacityStore::read_file(resolved, StoreReadOptions{.enforce_path_binding = true}).ok());
  PC_CHECK(CapacityStore::read_file(store, StoreReadOptions{.enforce_path_binding = true}).ok());
}

PC_TEST(adversarial, a_parent_directory_that_does_not_exist_is_refused) {
  TempDir directory;
  const std::filesystem::path store = directory.path() / "missing-dir" / "store.pcstore";
  StoreOpenOptions options;
  options.path = store;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(!opened.ok());
  PC_CHECK_EQ(opened.status().code(), StatusCode::NotFound);
  PC_CHECK(!std::filesystem::exists(store));
}

PC_TEST(adversarial, a_store_path_without_a_file_name_is_refused) {
  TempDir directory;
  StoreOpenOptions options;
  options.path = directory.path();
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  PC_REQUIRE_STATUS(CapacityStore::open(options).status(), StatusCode::InvalidArgument);

  StoreOpenOptions empty;
  empty.access = StoreAccess::ReadWrite;
  PC_REQUIRE_STATUS(CapacityStore::open(empty).status(), StatusCode::InvalidArgument);
}

PC_TEST(adversarial, a_symbolic_link_in_place_of_a_store_is_refused) {
  TempDir directory;
  const std::filesystem::path real = directory.file("real.pcstore");
  const std::filesystem::path link = directory.file("link.pcstore");
  {
    Facility facility(real, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  }

  std::error_code error;
  std::filesystem::create_symlink(real, link, error);
  if (error) {
    // Creating a symbolic link needs a privilege or Developer Mode on Windows.
    // Where the platform refuses, the refusal of a linked store path cannot be
    // exercised here and is not claimed; the directory case above still covers the
    // "not a regular file" branch of the same check.
    std::cout << "  (symbolic link creation unavailable on this host: " << error.message()
              << "; symlink refusal not exercised)\\n";
    return;
  }

  PC_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(link)));
  StoreOpenOptions options;
  options.path = link;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  const auto opened = CapacityStore::open(options);
  PC_CHECK(!opened.ok());
  PC_CHECK_EQ(opened.status().code(), StatusCode::InvalidArgument);
  PC_REQUIRE_STATUS(CapacityStore::verify_file(link), StatusCode::InvalidArgument);
  // The real store behind the link is untouched.
  PC_CHECK(CapacityStore::verify_file(real).ok());
}

PC_TEST(adversarial, an_older_but_valid_copy_of_a_store_is_refused_by_a_generation_floor) {
  TempDir directory;
  const std::filesystem::path store = directory.file("floor.pcstore");
  const std::filesystem::path snapshot = directory.file("snapshot.pcstore");

  Generation early(0);
  Generation late(0);
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
    early = facility.generation();
    std::error_code error;
    std::filesystem::copy_file(store, snapshot, std::filesystem::copy_options::overwrite_existing,
                               error);
    PC_CHECK(!error);
    facility.add_domain(DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000));
    late = facility.generation();
  }
  PC_CHECK(early < late);

  // The rollback: an earlier, internally perfect artifact is restored.
  std::error_code error;
  std::filesystem::copy_file(snapshot, store, std::filesystem::copy_options::overwrite_existing,
                             error);
  PC_CHECK(!error);

  // Nothing about the artifact itself reveals the rollback: its envelope, both
  // checksums, and every invariant are valid.
  PC_CHECK(CapacityStore::verify_file(store).ok());
  PC_CHECK_EQ(
      PC_REQUIRE_OK(CapacityStore::read_file(store, StoreReadOptions{}))->generation().value(),
      early.value());

  // The durable floor is what detects it.
  StoreReadOptions floored;
  floored.minimum_generation = late;
  const auto refused = CapacityStore::read_file(store, floored);
  PC_CHECK(!refused.ok());
  PC_CHECK_EQ(refused.status().code(), StatusCode::StaleGeneration);

  StoreOpenOptions open_options;
  open_options.path = store;
  open_options.access = StoreAccess::ReadWrite;
  open_options.minimum_generation = late;
  PC_REQUIRE_STATUS(CapacityStore::open(open_options).status(), StatusCode::StaleGeneration);

  EngineOptions engine_options;
  engine_options.store_path = store;
  engine_options.create_if_missing = false;
  engine_options.minimum_generation = late;
  PC_REQUIRE_STATUS(CapacityEngine::open(engine_options).status(), StatusCode::StaleGeneration);

  // A floor at or below the artifact's generation still reads.
  StoreReadOptions satisfied;
  satisfied.minimum_generation = early;
  PC_CHECK(CapacityStore::read_file(store, satisfied).ok());
}

PC_TEST(adversarial, a_read_only_store_file_cannot_be_replaced_by_a_commit) {
  TempDir directory;
  const std::filesystem::path store = directory.file("readonly.pcstore");
  {
    Facility facility(store, false, Tick(0));
    facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  }
  const auto before = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(before.ok());
  if (!before.ok()) {
    return;
  }

  std::error_code error;
  std::filesystem::permissions(store, std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    std::cout << "  (could not mark the store read-only: " << error.message() << ")\\n";
    return;
  }

  EngineOptions options;
  options.store_path = store;
  options.create_if_missing = false;
  const auto engine = CapacityEngine::open(options);
  if (engine.ok()) {
    RevalidationRequest request;
    request.as_of = Tick(0);
    PC_CHECK(engine.value()->revalidate_recovered_state(request).ok());
    MutationContext context;
    context.as_of = Tick(0);
    context.expected_generation = engine.value()->state().value()->generation();
    const auto committed = engine.value()->put_domain(
        DomainBuilder("pdu.b").nominal_watts(1000).usable_watts(1000).build(),
        PresenceExpectation::MustNotExist, context);
    // The publish cannot replace a read-only file, and the failure must be
    // reported rather than silently dropping the commit.
    PC_CHECK(!committed.ok());
    PC_CHECK(engine.value()->close().ok());
  }

  std::filesystem::permissions(store, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  // Whatever happened, the committed state is whole and unchanged.
  const auto after = CapacityStore::read_file(store, StoreReadOptions{});
  PC_CHECK(after.ok());
  if (after.ok()) {
    PC_CHECK_EQ(after.value()->domain_count(), before.value()->domain_count());
    PC_CHECK_EQ(after.value()->generation().value(), before.value()->generation().value());
  }
  for (const std::string& name : directory.entry_names()) {
    PC_CHECK_MSG(name.find(".stg-") == std::string::npos,
                 "staging residue left after a failed publish: " + name);
  }
}

PC_TEST(adversarial, a_cycle_cannot_be_created_through_the_engine) {
  Facility facility;
  facility.add_domain(DomainBuilder("a.root").nominal_watts(1000).usable_watts(1000));
  facility.add_domain(
      DomainBuilder("b.mid").nominal_watts(1000).usable_watts(1000).parent("a.root"));
  facility.add_domain(
      DomainBuilder("c.leaf").nominal_watts(1000).usable_watts(1000).parent("b.mid"));

  // Re-parenting the root under its own descendant must be refused, and the
  // published model must be unchanged afterwards.
  const Generation before = facility.generation();
  const auto committed = facility.engine().put_domain(
      DomainBuilder("a.root").nominal_watts(1000).usable_watts(1000).parent("c.leaf").build(),
      PresenceExpectation::MustExist, facility.context());
  PC_CHECK(!committed.ok());
  PC_CHECK_EQ(committed.status().code(), StatusCode::InvariantViolation);
  PC_CHECK_EQ(facility.generation().value(), before.value());
  PC_CHECK(!facility.state()
                ->find_domain(PC_REQUIRE_OK(DomainId::parse("a.root", 128)))
                ->parent.has_value());

  // A shorter cycle between existing domains is refused the same way.
  const auto pair = facility.engine().put_domain(
      DomainBuilder("b.mid").nominal_watts(1000).usable_watts(1000).parent("c.leaf").build(),
      PresenceExpectation::MustExist, facility.context());
  PC_CHECK(!pair.ok());
  PC_CHECK_EQ(pair.status().code(), StatusCode::InvariantViolation);
}

PC_TEST(adversarial, a_self_parent_is_refused_through_the_engine) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));
  const auto committed = facility.engine().put_domain(
      DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000).parent("pdu.a").build(),
      PresenceExpectation::MustExist, facility.context());
  PC_CHECK(!committed.ok());
  PC_CHECK_EQ(committed.status().code(), StatusCode::InvalidArgument);
}

PC_TEST(adversarial, a_regressing_authority_binding_is_refused) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(1000).usable_watts(1000));

  RevalidationRequest forward;
  forward.as_of = Tick(0);
  forward.epoch = Epoch(5);
  forward.incarnation = Incarnation(3);
  PC_CHECK(facility.engine().revalidate_recovered_state(forward).ok());

  RevalidationRequest older_epoch;
  older_epoch.as_of = Tick(0);
  older_epoch.epoch = Epoch(4);
  older_epoch.incarnation = Incarnation(9);
  PC_REQUIRE_STATUS(facility.engine().revalidate_recovered_state(older_epoch),
                    StatusCode::StaleAuthority);

  RevalidationRequest older_incarnation;
  older_incarnation.as_of = Tick(0);
  older_incarnation.epoch = Epoch(5);
  older_incarnation.incarnation = Incarnation(2);
  PC_REQUIRE_STATUS(facility.engine().revalidate_recovered_state(older_incarnation),
                    StatusCode::StaleAuthority);

  PC_CHECK(facility.engine().revalidate_recovered_state(forward).ok());

  RevalidationRequest newer;
  newer.as_of = Tick(0);
  newer.epoch = Epoch(5);
  newer.incarnation = Incarnation(4);
  PC_CHECK(facility.engine().revalidate_recovered_state(newer).ok());
}

PC_TEST(adversarial, a_candidate_batch_that_overflows_the_overlay_is_refused_not_wrapped) {
  Facility facility;
  facility.add_domain(DomainBuilder("pdu.a").nominal_watts(100000000).usable_watts(100000000));
  const std::int64_t component = ResourceLimits::defaults().max_component_milliwatts / 1000;

  std::vector<CandidateLoad> candidates;
  for (int index = 0; index < 40; ++index) {
    CandidateLoad candidate;
    candidate.id = PC_REQUIRE_OK(LoadId::parse("cand." + std::to_string(index), 128));
    candidate.domain = PC_REQUIRE_OK(DomainId::parse("pdu.a", 128));
    candidate.load = PC_REQUIRE_OK(Power::from_watts(component));
    candidates.push_back(candidate);
  }
  const std::vector<CandidateEvaluation> results = facility.evaluate_batch(candidates);
  PC_CHECK_EQ(results.size(), candidates.size());
  // The overlay saturates at the aggregate bound instead of wrapping, so once a
  // candidate is refused every later candidate is refused too.
  bool saw_refusal = false;
  for (const CandidateEvaluation& evaluation : results) {
    if (evaluation.verdict != CandidateVerdict::Admissible) {
      saw_refusal = true;
    } else {
      PC_CHECK_MSG(!saw_refusal, "a candidate became admissible again after a refusal");
    }
  }
  PC_CHECK(saw_refusal);
}