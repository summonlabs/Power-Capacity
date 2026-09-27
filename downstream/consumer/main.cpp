// Independent consumer of the installed Power Capacity package.
//
// It exercises the public API only: it never includes an internal header and it
// never reaches into the build tree. The lifecycle it runs is the same one a
// facility operator would run: create a durable store, commit a model, close it,
// reopen it in a new engine, revalidate recovered state against current
// evidence, ask the capacity question, and evaluate a proposed load.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "power_capacity/engine.hpp"
#include "power_capacity/version.hpp"

namespace {

int fail(const std::string& message) {
  std::cerr << "consumer-failure: " << message << "\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace power_capacity;

  if (argc < 2) {
    return fail("usage: consumer <store-path>");
  }
  const std::filesystem::path store_path(argv[1]);
  std::error_code error;
  std::filesystem::remove_all(store_path.parent_path(), error);
  // Opening a store never creates a directory as a side effect: a missing parent
  // is reported, not conjured. The consumer creates its own working directory.
  std::filesystem::create_directories(store_path.parent_path(), error);
  if (error) {
    return fail("could not create the consumer working directory: " + error.message());
  }

  std::cout << "power_capacity_version=" << POWER_CAPACITY_VERSION_STRING << "\n";
  std::cout << "linked_version=" << Version::string << "\n";

  constexpr Tick kCreated(0);
  constexpr Tick kAsOf(100);
  constexpr Tick kValidUntil(1000);

  const auto make_domain = [](const std::string& id, std::int64_t watts,
                              const char* parent) -> Result<PowerDomain> {
    PowerDomain domain;
    const auto parsed = DomainId::parse(id, 128);
    if (!parsed.ok()) {
      return parsed.status();
    }
    domain.id = parsed.value();
    const auto nominal = Power::from_watts(watts);
    if (!nominal.ok()) {
      return nominal.status();
    }
    domain.nominal_capacity = nominal.value();
    domain.usable_capacity = nominal.value();
    if (parent != nullptr) {
      const auto parsed_parent = DomainId::parse(parent, 128);
      if (!parsed_parent.ok()) {
        return parsed_parent.status();
      }
      domain.parent = parsed_parent.value();
    }
    EvidenceRef evidence;
    const auto evidence_id = EvidenceId::parse("ev.consumer", 128);
    const auto source = SourceId::parse("source.consumer", 128);
    if (!evidence_id.ok()) {
      return evidence_id.status();
    }
    if (!source.ok()) {
      return source.status();
    }
    evidence.id = evidence_id.value();
    evidence.source = source.value();
    evidence.observed_at = kCreated;
    evidence.valid_until = kValidUntil;
    evidence.revision = Revision(1);
    evidence.source_generation = Generation(1);
    domain.evidence = evidence;
    return domain;
  };

  // --- first lifecycle: create and commit ---------------------------------
  {
    EngineOptions options;
    options.store_path = store_path;
    options.create_if_missing = true;
    options.initial_tick = kCreated;
    auto engine = CapacityEngine::open(options);
    if (!engine.ok()) {
      return fail("open: " + engine.status().to_string());
    }
    if (engine.value()->lifecycle() != EngineLifecycle::Current) {
      return fail("a freshly created store must be current, not pending revalidation");
    }

    const auto ups = make_domain("ups.a", 400000, nullptr);
    if (!ups.ok()) {
      return fail("domain: " + ups.status().to_string());
    }
    const auto rack = make_domain("rackpdu.a1", 100000, "ups.a");
    if (!rack.ok()) {
      return fail("domain: " + rack.status().to_string());
    }

    MutationContext context;
    context.as_of = kAsOf;
    context.expected_generation = engine.value()->state().value()->generation();
    auto committed = engine.value()->put_domain(ups.value(), PresenceExpectation::MustNotExist,
                                                context);
    if (!committed.ok()) {
      return fail("put_domain: " + committed.status().to_string());
    }
    context.expected_generation = committed.value().generation;
    committed = engine.value()->put_domain(rack.value(), PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      return fail("put_domain: " + committed.status().to_string());
    }

    LoadRecord load;
    load.id = LoadId::parse("load.tenant", 128).value();
    load.domain = DomainId::parse("rackpdu.a1", 128).value();
    load.load = Power::from_watts(25000).value();
    load.authority = AuthorityRef::parse("reservation.authority", 128).value();
    load.evidence = rack.value().evidence;
    context.expected_generation = committed.value().generation;
    committed = engine.value()->put_load(load, PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      return fail("put_load: " + committed.status().to_string());
    }
    std::cout << "committed_generation=" << committed.value().generation.value() << "\n";
    std::cout << "committed_bytes=" << committed.value().metrics.bytes_written << "\n";
  }

  // --- second lifecycle: reopen, revalidate, query -------------------------
  {
    EngineOptions options;
    options.store_path = store_path;
    options.create_if_missing = false;
    auto engine = CapacityEngine::open(options);
    if (!engine.ok()) {
      return fail("reopen: " + engine.status().to_string());
    }
    if (engine.value()->lifecycle() != EngineLifecycle::RecoveredPendingRevalidation) {
      return fail("recovered state must require revalidation");
    }

    CapacityQuery refused;
    refused.domain = DomainId::parse("rackpdu.a1", 128).value();
    refused.as_of = kAsOf;
    if (engine.value()->assess(refused).ok()) {
      return fail("a query before revalidation must be refused");
    }

    RevalidationRequest request;
    request.as_of = kAsOf;
    auto report = engine.value()->revalidate_recovered_state(request);
    if (!report.ok()) {
      return fail("revalidate: " + report.status().to_string());
    }
    std::cout << "revalidated_domains=" << report.value().domains_total << "\n";
    std::cout << "all_evidence_current=" << (report.value().all_evidence_current ? "true" : "false")
              << "\n";

    CapacityQuery query;
    query.domain = DomainId::parse("rackpdu.a1", 128).value();
    query.as_of = kAsOf;
    auto assessment = engine.value()->assess(query);
    if (!assessment.ok()) {
      return fail("assess: " + assessment.status().to_string());
    }
    if (!assessment.value().known) {
      return fail("assessment must be known: " + std::string(to_string(assessment.value().reason)));
    }
    std::cout << "allocatable_w="
              << assessment.value().derivation->allocatable_headroom.to_watts_string() << "\n";
    std::cout << "committed_w="
              << assessment.value().derivation->committed_load.to_watts_string() << "\n";

    CandidateLoad candidate;
    candidate.id = LoadId::parse("load.candidate", 128).value();
    candidate.domain = DomainId::parse("rackpdu.a1", 128).value();
    candidate.load = Power::from_watts(50000).value();
    EvaluationContext evaluation_context;
    evaluation_context.as_of = kAsOf;
    auto evaluation = engine.value()->evaluate(candidate, evaluation_context);
    if (!evaluation.ok()) {
      return fail("evaluate: " + evaluation.status().to_string());
    }
    std::cout << "candidate_verdict=" << to_string(evaluation.value().verdict) << "\n";
    if (evaluation.value().verdict != CandidateVerdict::Admissible) {
      return fail("a 50 MW candidate under a 75 MW headroom must be admissible");
    }
    std::cout << "bottleneck=" << evaluation.value().bottleneck->value() << "\n";

    // The capacity answer must be exactly the documented derivation: nameplate
    // minus the committed load, with no derating and no reserve declared.
    const std::int64_t expected =
        100000 * Power::milliwatts_per_watt - 25000 * Power::milliwatts_per_watt;
    if (assessment.value().derivation->allocatable_headroom.milliwatts() != expected) {
      return fail("allocatable headroom is " +
                  std::to_string(assessment.value().derivation->allocatable_headroom.milliwatts()) +
                  " mW, expected " + std::to_string(expected));
    }
    std::cout << "allocatable_milliwatts=" << expected << "\n";
  }

  std::cout << "consumer-ok\n";
  return 0;
}
