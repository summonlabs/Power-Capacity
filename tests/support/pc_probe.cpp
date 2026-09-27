// Test-support probe.
//
// This program exists so that persistence, writer authority, and crash recovery
// can be proved with independent operating-system processes instead of threads.
// Every mode writes its result to a file rather than to standard output, so the
// proof never depends on capturing a child process's stdio.
//
// "hard" exit means immediate process termination with no unwinding and no
// cleanup: the process dies exactly as it would under an external force kill, and
// the operating system releases the file lock it held.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "power_capacity/engine.hpp"
#include "power_capacity/store.hpp"

namespace {

std::uint64_t probe_pid() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

using power_capacity::CapacityEngine;
using power_capacity::CapacityContent;
using power_capacity::DomainId;
using power_capacity::EngineLifecycle;
using power_capacity::EngineOptions;
using power_capacity::Epoch;
using power_capacity::Generation;
using power_capacity::Incarnation;
using power_capacity::MutationContext;
using power_capacity::Power;
using power_capacity::PowerDomain;
using power_capacity::PresenceExpectation;
using power_capacity::RevalidationRequest;
using power_capacity::Status;
using power_capacity::StatusCode;
using power_capacity::StoreIdentity;
using power_capacity::StoreOpenOptions;
using power_capacity::Tick;

struct Options {
  std::string mode;
  std::filesystem::path store;
  std::filesystem::path ready;
  std::filesystem::path out;
  std::filesystem::path source;
  std::uint64_t count = 0;
  std::uint64_t start = 0;
  std::int64_t tick = 0;
  std::uint64_t generation = 1;
  std::uint64_t bytes = 0;
  std::string domain;
  std::string kind = "pdu";
  std::int64_t nominal_watts = 100000;
};

void dump_argv(int argc, char** argv) {
  std::cerr << "probe argv (" << argc << "):";
  for (int index = 0; index < argc; ++index) {
    std::cerr << " [" << index << "]=" << argv[index];
  }
  std::cerr << "\n";
}

bool parse(int argc, char** argv, Options& options) {
  // The mode is the first token that is not an option, whichever position the
  // runtime places it in. Relying on a fixed index would make the probe depend on
  // how the spawning runtime populates argv[0].
  int index = 1;
  for (; index < argc; ++index) {
    const std::string token = argv[index];
    if (token.rfind("--", 0) != 0) {
      options.mode = token;
      ++index;
      break;
    }
  }
  if (options.mode.empty()) {
    dump_argv(argc, argv);
    return false;
  }

  for (; index < argc; ++index) {
    const std::string flag = argv[index];
    auto next = [&]() -> std::string {
      return index + 1 < argc ? std::string(argv[++index]) : std::string();
    };
    if (flag == "--store") {
      options.store = next();
    } else if (flag == "--ready") {
      options.ready = next();
    } else if (flag == "--out") {
      options.out = next();
    } else if (flag == "--source") {
      options.source = next();
    } else if (flag == "--count") {
      options.count = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--start") {
      options.start = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--tick") {
      options.tick = std::strtoll(next().c_str(), nullptr, 10);
    } else if (flag == "--generation") {
      options.generation = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--bytes") {
      options.bytes = std::strtoull(next().c_str(), nullptr, 10);
    } else if (flag == "--domain") {
      options.domain = next();
    } else if (flag == "--kind") {
      options.kind = next();
    } else if (flag == "--nominal-w") {
      options.nominal_watts = std::strtoll(next().c_str(), nullptr, 10);
    } else {
      std::cerr << "unknown probe option: " << flag << "\n";
      dump_argv(argc, argv);
      return false;
    }
  }
  return true;
}

void write_ready(const Options& options, const std::string& extra) {
  if (options.ready.empty()) {
    return;
  }
  std::ofstream stream(options.ready, std::ios::binary | std::ios::trunc);
  stream << "pid=" << probe_pid() << "\n";
  stream << extra;
  stream.flush();
}

Status write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return Status::error(StatusCode::IoFailure, "probe could not write its result file");
  }
  stream << text;
  stream.flush();
  if (!stream) {
    return Status::error(StatusCode::IoFailure, "probe failed to flush its result file");
  }
  return Status::success();
}

power_capacity::Result<std::unique_ptr<CapacityEngine>> open_engine(const Options& options,
                                                                    bool read_only) {
  EngineOptions engine_options;
  engine_options.store_path = options.store;
  engine_options.read_only = read_only;
  engine_options.create_if_missing = !read_only;
  engine_options.initial_tick = Tick(options.tick);
  power_capacity::Result<std::unique_ptr<CapacityEngine>> engine =
      CapacityEngine::open(engine_options);
  if (!engine.ok()) {
    return engine.status();
  }
  if (engine.value()->lifecycle() == EngineLifecycle::RecoveredPendingRevalidation) {
    RevalidationRequest request;
    request.as_of = Tick(options.tick);
    const power_capacity::Result<power_capacity::RevalidationReport> report =
        engine.value()->revalidate_recovered_state(request);
    if (!report.ok()) {
      return report.status();
    }
  }
  return engine;
}

PowerDomain make_domain(const std::string& id, std::uint64_t index, std::int64_t watts) {
  PowerDomain domain;
  auto parsed = DomainId::parse(id, 128);
  if (parsed.ok()) {
    domain.id = parsed.value();
  }
  auto kind = power_capacity::parse_domain_kind("pdu");
  if (kind.ok()) {
    domain.kind = kind.value();
  }
  auto nominal = Power::from_watts(watts);
  if (nominal.ok()) {
    domain.nominal_capacity = nominal.value();
    domain.usable_capacity = nominal.value();
  }
  power_capacity::EvidenceRef evidence;
  auto evidence_id = power_capacity::EvidenceId::parse("probe.evidence", 128);
  if (evidence_id.ok()) {
    evidence.id = evidence_id.value();
  }
  auto source = power_capacity::SourceId::parse("probe.source", 128);
  if (source.ok()) {
    evidence.source = source.value();
  }
  evidence.observed_at = Tick(0);
  evidence.valid_until = Tick(1000000000);
  evidence.revision = power_capacity::Revision(index + 1);
  evidence.source_generation = Generation(1);
  domain.evidence = evidence;
  return domain;
}

int mode_hold_and_die(const Options& options) {
  StoreOpenOptions store_options;
  store_options.path = options.store;
  store_options.access = power_capacity::StoreAccess::ReadWrite;
  store_options.create_if_missing = true;
  store_options.created_at = Tick(options.tick);
  const power_capacity::Result<std::shared_ptr<power_capacity::CapacityStore>> store =
      power_capacity::CapacityStore::open(store_options);
  if (!store.ok()) {
    write_ready(options, "error=" + store.status().to_string() + "\n");
    return 1;
  }
  write_ready(options, "generation=" + std::to_string(store.value()->info().generation.value()) + "\n");
  std::cout.flush();
  std::_Exit(17);
}

/// Holds writer authority and stays alive until an external force kill arrives.
int mode_hold_forever(const Options& options) {
  StoreOpenOptions store_options;
  store_options.path = options.store;
  store_options.access = power_capacity::StoreAccess::ReadWrite;
  store_options.create_if_missing = true;
  store_options.created_at = Tick(options.tick);
  const power_capacity::Result<std::shared_ptr<power_capacity::CapacityStore>> store =
      power_capacity::CapacityStore::open(store_options);
  if (!store.ok()) {
    write_ready(options, "error=" + store.status().to_string() + "\n");
    return 1;
  }
  write_ready(options, "generation=" + std::to_string(store.value()->info().generation.value()) + "\n");
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

int mode_commit_and_die(const Options& options) {
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(options, false);
  if (!engine.ok()) {
    write_ready(options, "error=" + engine.status().to_string() + "\n");
    return 1;
  }
  PowerDomain domain = make_domain(options.domain, options.start, options.nominal_watts);
  MutationContext context;
  context.as_of = Tick(options.tick);
  const power_capacity::Result<std::shared_ptr<const power_capacity::CapacityState>> state =
      engine.value()->state();
  if (!state.ok()) {
    write_ready(options, "error=" + state.status().to_string() + "\n");
    return 1;
  }
  context.expected_generation = state.value()->generation();
  const power_capacity::Result<power_capacity::CommitResult> committed =
      engine.value()->put_domain(domain, PresenceExpectation::MustNotExist, context);
  if (!committed.ok()) {
    write_ready(options, "error=" + committed.status().to_string() + "\n");
    return 1;
  }
  write_ready(options, "generation=" + std::to_string(committed.value().generation.value()) + "\n");
  std::cout.flush();
  std::_Exit(18);
}

/// Commits domains in a loop and never returns. The parent terminates this
/// process with an operating-system level force kill while it is mid-commit.
int mode_commit_loop(const Options& options) {
  const power_capacity::Result<std::unique_ptr<CapacityEngine>> engine = open_engine(options, false);
  if (!engine.ok()) {
    write_ready(options, "error=" + engine.status().to_string() + "\n");
    return 1;
  }
  std::uint64_t index = options.start;
  const std::uint64_t limit = options.count == 0 ? 1000000 : options.count;
  std::string last;
  for (std::uint64_t step = 0; step < limit; ++step, ++index) {
    const power_capacity::Result<std::shared_ptr<const power_capacity::CapacityState>> state =
        engine.value()->state();
    if (!state.ok()) {
      write_ready(options, "error=" + state.status().to_string() + "\n");
      return 1;
    }
    MutationContext context;
    context.as_of = Tick(options.tick);
    context.expected_generation = state.value()->generation();
    const std::string id = "probe.domain." + std::to_string(index);
    const power_capacity::Result<power_capacity::CommitResult> committed = engine.value()->put_domain(
        make_domain(id, index, options.nominal_watts), PresenceExpectation::MustNotExist, context);
    if (!committed.ok()) {
      write_ready(options, "error=" + committed.status().to_string() + "\n");
      return 1;
    }
    last = "generation=" + std::to_string(committed.value().generation.value()) + "\n";
    if (step == 0) {
      write_ready(options, last);
    }
  }
  write_ready(options, last);
  return 0;
}

int mode_read(const Options& options) {
  const power_capacity::Result<std::shared_ptr<const power_capacity::CapacityState>> state =
      power_capacity::CapacityStore::read_file(options.store,
                                               power_capacity::StoreReadOptions{});
  if (!state.ok()) {
    const Status status = write_text(options.out, "error=" + state.status().to_string() + "\n");
    (void)status;
    return 1;
  }
  std::string text;
  text += "store_id=" + state.value()->store_identity().to_hex() + "\n";
  text += "generation=" + std::to_string(state.value()->generation().value()) + "\n";
  text += "domains=" + std::to_string(state.value()->domain_count()) + "\n";
  text += "loads=" + std::to_string(state.value()->load_count()) + "\n";
  text += "groups=" + std::to_string(state.value()->group_count()) + "\n";
  text += "updated_at=" + std::to_string(state.value()->updated_at().value()) + "\n";
  for (const auto& entry : state.value()->domains()) {
    text += "domain=" + entry.first.value() + "\n";
  }
  const Status status = write_text(options.out, text);
  return status.ok() ? 0 : 1;
}

/// Writes residue at the staging path a crashed commit would leave behind, then
/// dies without publishing anything.
int mode_residue_and_die(const Options& options) {
  std::filesystem::path staging = options.store;
  staging += ".stg-" + std::to_string(probe_pid()) + "-probe000-1";
  if (!options.source.empty()) {
    std::error_code error;
    std::filesystem::copy_file(options.source, staging,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      write_ready(options, "error=" + error.message() + "\n");
      return 1;
    }
    write_ready(options, "bytes=" + std::to_string(std::filesystem::file_size(staging)) + "\n");
    std::_Exit(19);
  }
  std::ofstream stream(staging, std::ios::binary | std::ios::trunc);
  const std::string filler(4096, 'R');
  std::uint64_t written = 0;
  while (written < options.bytes) {
    const std::uint64_t chunk = std::min<std::uint64_t>(4096, options.bytes - written);
    stream.write(filler.data(), static_cast<std::streamsize>(chunk));
    written += chunk;
  }
  stream.flush();
  if (!stream) {
    write_ready(options, "error=probe could not write staging residue\n");
    return 1;
  }
  write_ready(options, "bytes=" + std::to_string(written) + "\n");
  std::_Exit(20);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) {
    std::cerr << "usage: pc_probe <mode> --store <path> [options]\n";
    return 2;
  }
  if (options.mode == "hold-and-die") {
    return mode_hold_and_die(options);
  }
  if (options.mode == "hold-forever") {
    return mode_hold_forever(options);
  }
  if (options.mode == "commit-and-die") {
    return mode_commit_and_die(options);
  }
  if (options.mode == "commit-loop") {
    return mode_commit_loop(options);
  }
  if (options.mode == "read") {
    return mode_read(options);
  }
  if (options.mode == "residue-and-die") {
    return mode_residue_and_die(options);
  }
  std::cerr << "unknown probe mode: " << options.mode << "\n";
  return 2;
}
