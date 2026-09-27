// Example: persistence, recovery, refusal of stale authority, and revalidation.
//
// Runs two lifecycles over the same store. The second lifecycle proves that
// committed state survives a close/reopen, that recovered state may not answer
// capacity questions until it has been revalidated against current evidence, and
// that an assessment issued before a change is superseded rather than silently
// reused.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "power_capacity/engine.hpp"
#include "power_capacity/store.hpp"

namespace {

using namespace power_capacity;

PowerDomain make_domain(const std::string& id, std::int64_t watts, const char* parent,
                        Tick observed, Tick valid_until) {
  PowerDomain domain;
  domain.id = DomainId::parse(id, 128).value();
  domain.nominal_capacity = Power::from_watts(watts).value();
  domain.usable_capacity = domain.nominal_capacity;
  if (parent != nullptr) {
    domain.parent = DomainId::parse(parent, 128).value();
  }
  EvidenceRef evidence;
  evidence.id = EvidenceId::parse("ev." + id, 128).value();
  evidence.source = SourceId::parse("source.electrical", 128).value();
  evidence.observed_at = observed;
  evidence.valid_until = valid_until;
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  domain.evidence = evidence;
  return domain;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace power_capacity;

  const std::filesystem::path store =
      argc > 1 ? std::filesystem::path(argv[1]) / "facility.pcstore"
               : std::filesystem::path("example-store") / "facility.pcstore";
  std::error_code error;
  std::filesystem::remove_all(store.parent_path(), error);
  // The library deliberately does not create directories as a side effect of
  // opening a store: a missing directory is reported as not found rather than
  // conjured. The operator creates it.
  std::filesystem::create_directories(store.parent_path(), error);
  if (error) {
    std::cerr << "could not create " << store.parent_path().string() << ": " << error.message()
              << "\n";
    return 1;
  }

  AssessmentToken token;
  StoreIdentity identity;

  // --- lifecycle one: create, commit, assess -------------------------------
  {
    EngineOptions options;
    options.store_path = store;
    options.create_if_missing = true;
    options.initial_tick = Tick(0);
    auto engine = CapacityEngine::open(options);
    if (!engine.ok()) {
      std::cerr << "open failed: " << engine.status().to_string() << "\n";
      return 1;
    }
    std::cout << "lifecycle one state : "
              << to_string(engine.value()->lifecycle()) << "\n";

    MutationContext context;
    context.as_of = Tick(10);
    context.expected_generation = engine.value()->state().value()->generation();
    auto committed = engine.value()->put_domain(
        make_domain("busway.main", 900'000, nullptr, Tick(0), Tick(1000)),
        PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      std::cerr << "put_domain failed: " << committed.status().to_string() << "\n";
      return 1;
    }
    context.expected_generation = committed.value().generation;
    committed = engine.value()->put_domain(
        make_domain("pdu.a", 400'000, "busway.main", Tick(0), Tick(1000)),
        PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      std::cerr << "put_domain failed: " << committed.status().to_string() << "\n";
      return 1;
    }

    CapacityQuery query;
    query.domain = DomainId::parse("pdu.a", 128).value();
    query.as_of = Tick(500);
    const auto assessment = engine.value()->assess(query);
    if (!assessment.ok()) {
      std::cerr << "assess failed: " << assessment.status().to_string() << "\n";
      return 1;
    }
    token = assessment.value().token();
    identity = assessment.value().store;
    std::cout << "generation one      : " << assessment.value().generation.value() << "\n";
    std::cout << "allocatable         : "
              << assessment.value().derivation->allocatable_headroom.to_watts_string() << " W\n";
    std::cout << "store identity      : " << identity.to_hex() << "\n";
  }

  // --- lifecycle two: reopen and refuse to answer until revalidated --------
  {
    EngineOptions options;
    options.store_path = store;
    options.create_if_missing = false;
    options.expected_store_identity = identity;
    auto engine = CapacityEngine::open(options);
    if (!engine.ok()) {
      std::cerr << "reopen failed: " << engine.status().to_string() << "\n";
      return 1;
    }
    std::cout << "\nlifecycle two state : "
              << to_string(engine.value()->lifecycle()) << "\n";

    CapacityQuery query;
    query.domain = DomainId::parse("pdu.a", 128).value();
    query.as_of = Tick(500);
    const auto before = engine.value()->assess(query);
    std::cout << "assess before revalidation : " << before.status().to_string() << "\n";

    RevalidationRequest request;
    request.as_of = Tick(500);
    const auto report = engine.value()->revalidate_recovered_state(request);
    if (!report.ok()) {
      std::cerr << "revalidate failed: " << report.status().to_string() << "\n";
      return 1;
    }
    std::cout << "revalidated " << report.value().domains_current << " of "
              << report.value().domains_total << " domains against current evidence\n";

    const auto after = engine.value()->assess(query);
    if (!after.ok()) {
      std::cerr << "assess failed: " << after.status().to_string() << "\n";
      return 1;
    }
    std::cout << "allocatable after reopen : "
              << after.value().derivation->allocatable_headroom.to_watts_string() << " W\n";

    // The token issued in lifecycle one cannot be confirmed: it belongs to a
    // session that ended when the store was closed.
    const auto stale = engine.value()->revalidate(token, request);
    if (!stale.ok()) {
      std::cerr << "revalidate failed: " << stale.status().to_string() << "\n";
      return 1;
    }
    std::cout << "token from lifecycle one : " << to_string(stale.value().verdict) << " ("
              << to_string(stale.value().reason) << ")\n";

    // A freshly issued token revalidates as long as nothing changes.
    const auto fresh = engine.value()->assess(query);
    const auto confirmed = engine.value()->revalidate(fresh.value().token(), request);
    std::cout << "fresh token              : " << to_string(confirmed.value().verdict) << "\n";

    // Once the evidence window closes, the same question becomes unknown rather
    // than zero.
    CapacityQuery expired = query;
    expired.as_of = Tick(5000);
    const auto unknown = engine.value()->assess(expired);
    std::cout << "after the evidence window closes: known=" << unknown.value().known
              << " reason=" << to_string(unknown.value().reason) << "\n";
  }

  // --- integrity verification of the on-disk artifact ----------------------
  const Status verified = CapacityStore::verify_file(store);
  std::cout << "\nverify store        : " << verified.to_string() << "\n";
  const auto reopened = CapacityStore::read_file(store, StoreReadOptions{.expected_store_identity = identity});
  if (!reopened.ok()) {
    std::cerr << "read failed: " << reopened.status().to_string() << "\n";
    return 1;
  }
  std::cout << "domains in store    : " << reopened.value()->domain_count() << "\n";
  std::cout << "final generation    : " << reopened.value()->generation().value() << "\n";
  return 0;
}
