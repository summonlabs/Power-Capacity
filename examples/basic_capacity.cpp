// Example: the core capacity question for one power domain.
//
//   derated     = floor(usable * derate_ratio)
//   operational = derated while the domain is available
//   reserve     = per the declared reserve policy
//   safe        = operational - reserve
//   carryable   = safe - protected load
//   allocatable = carryable - committed load
//
// Every value is exact and every clamp is reported.

#include <cstdint>
#include <iostream>
#include <string>

#include "power_capacity/engine.hpp"

namespace {

using namespace power_capacity;

Result<PowerDomain> make_domain(const std::string& id, std::int64_t nominal_watts,
                                std::int64_t usable_watts, std::int32_t derate_bp,
                                std::int32_t reserve_bp, const char* parent = nullptr) {
  PowerDomain domain;
  const auto parsed = DomainId::parse(id, 128);
  if (!parsed.ok()) {
    return parsed.status();
  }
  domain.id = parsed.value();
  const auto nominal = Power::from_watts(nominal_watts);
  const auto usable = Power::from_watts(usable_watts);
  const auto derate = Ratio::from_basis_points(derate_bp);
  const auto reserve = Ratio::from_basis_points(reserve_bp);
  if (!nominal.ok()) {
    return nominal.status();
  }
  if (!usable.ok()) {
    return usable.status();
  }
  if (!derate.ok()) {
    return derate.status();
  }
  if (!reserve.ok()) {
    return reserve.status();
  }
  domain.nominal_capacity = nominal.value();
  domain.usable_capacity = usable.value();
  domain.derate_ratio = derate.value();
  domain.reserve.mode = ReservePolicy::Mode::Ratio;
  domain.reserve.ratio = reserve.value();
  if (parent != nullptr) {
    const auto parsed_parent = DomainId::parse(parent, 128);
    if (!parsed_parent.ok()) {
      return parsed_parent.status();
    }
    domain.parent = parsed_parent.value();
  }
  EvidenceRef evidence;
  evidence.id = EvidenceId::parse("ev.example", 128).value();
  evidence.source = SourceId::parse("source.electrical", 128).value();
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000);
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  domain.evidence = evidence;
  return domain;
}

Result<LoadRecord> make_load(const std::string& id, const std::string& domain,
                             std::int64_t watts, LoadClass load_class) {
  LoadRecord load;
  const auto parsed_id = LoadId::parse(id, 128);
  const auto parsed_domain = DomainId::parse(domain, 128);
  const auto value = Power::from_watts(watts);
  if (!parsed_id.ok()) {
    return parsed_id.status();
  }
  if (!parsed_domain.ok()) {
    return parsed_domain.status();
  }
  if (!value.ok()) {
    return value.status();
  }
  load.id = parsed_id.value();
  load.domain = parsed_domain.value();
  load.load = value.value();
  load.load_class = load_class;
  load.authority = AuthorityRef::parse("authority.reservation", 128).value();
  EvidenceRef evidence;
  evidence.id = EvidenceId::parse("ev.example.load", 128).value();
  evidence.source = SourceId::parse("source.electrical", 128).value();
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000);
  evidence.revision = Revision(1);
  evidence.source_generation = Generation(1);
  load.evidence = evidence;
  return load;
}

void print_assessment(const DomainAssessment& assessment) {
  std::cout << "domain                 : " << assessment.domain.value() << "\n";
  std::cout << "known                  : " << (assessment.known ? "true" : "false") << "\n";
  std::cout << "reason                 : " << to_string(assessment.reason) << "\n";
  if (!assessment.derivation.has_value()) {
    return;
  }
  const CapacityDerivation& d = *assessment.derivation;
  std::cout << "nominal                : " << d.nominal.to_watts_string() << " W\n";
  std::cout << "usable                 : " << d.usable.to_watts_string() << " W\n";
  std::cout << "derated                : " << d.derated.to_watts_string() << " W\n";
  std::cout << "operational            : " << d.operational.to_watts_string() << " W\n";
  std::cout << "reserve                : " << d.reserve.to_watts_string() << " W\n";
  std::cout << "protected load         : " << d.protected_load.to_watts_string() << " W\n";
  std::cout << "committed load         : " << d.committed_load.to_watts_string() << " W\n";
  std::cout << "safe capacity          : " << d.safe_capacity.to_watts_string() << " W\n";
  std::cout << "carryable capacity     : " << d.carryable_capacity.to_watts_string() << " W\n";
  std::cout << "allocatable headroom   : " << d.allocatable_headroom.to_watts_string() << " W\n";
  std::cout << "clamped (r/p/c)        : " << d.reserve_clamped << "/" << d.protected_clamped << "/"
            << d.committed_clamped << "\n";
}

}  // namespace

int main() {
  using namespace power_capacity;

  EngineOptions options;
  auto engine = CapacityEngine::open(options);
  if (!engine.ok()) {
    std::cerr << "open failed: " << engine.status().to_string() << "\n";
    return 1;
  }
  CapacityEngine& facility = *engine.value();

  const auto utility = make_domain("utility.feed.a", 12'000'000, 11'400'000, 9000, 1500);
  if (!utility.ok()) {
    return 1;
  }
  MutationContext context;
  context.as_of = Tick(10);
  context.expected_generation = facility.state().value()->generation();
  auto committed = facility.put_domain(utility.value(), PresenceExpectation::MustNotExist, context);
  if (!committed.ok()) {
    std::cerr << "put_domain failed: " << committed.status().to_string() << "\n";
    return 1;
  }

  const auto cooling = make_load("load.cooling", "utility.feed.a", 1'200'000, LoadClass::Protected);
  const auto tenant = make_load("load.tenant.a", "utility.feed.a", 4'000'000, LoadClass::Committed);
  if (!cooling.ok() || !tenant.ok()) {
    return 1;
  }
  context.expected_generation = committed.value().generation;
  committed = facility.put_load(cooling.value(), PresenceExpectation::MustNotExist, context);
  if (!committed.ok()) {
    std::cerr << "put_load failed: " << committed.status().to_string() << "\n";
    return 1;
  }
  context.expected_generation = committed.value().generation;
  committed = facility.put_load(tenant.value(), PresenceExpectation::MustNotExist, context);
  if (!committed.ok()) {
    std::cerr << "put_load failed: " << committed.status().to_string() << "\n";
    return 1;
  }

  CapacityQuery query;
  query.domain = DomainId::parse("utility.feed.a", 128).value();
  query.as_of = Tick(100);
  const auto assessment = facility.assess(query);
  if (!assessment.ok()) {
    std::cerr << "assess failed: " << assessment.status().to_string() << "\n";
    return 1;
  }
  print_assessment(assessment.value());

  std::cout << "\n-- the same question asked after the evidence window closes --\n";
  CapacityQuery expired = query;
  expired.as_of = Tick(2'000'000);
  const auto stale = facility.assess(expired);
  if (!stale.ok()) {
    std::cerr << "assess failed: " << stale.status().to_string() << "\n";
    return 1;
  }
  print_assessment(stale.value());
  std::cout << "unknown is reported as unknown, never as zero: known=" << stale.value().known
            << "\n";

  return 0;
}
