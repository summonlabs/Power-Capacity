// The command line tool, exercised as a real process against a real store.
//
// The tool is a thin layer over the library, so this suite does not re-test the
// capacity model. It proves that argument parsing, the store lifecycle, the
// authority binding, and the exit-code contract all work end to end, and that the
// tool refuses what the library refuses.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "power_capacity/store.hpp"
#include "support/facility.hpp"
#include "support/proc.hpp"
#include "support/test_harness.hpp"

using pc_test::TempDir;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

std::vector<std::string> base_arguments(const std::filesystem::path& store, int& counter,
                                        std::filesystem::path& output) {
  output = store.parent_path() / ("cli-out-" + std::to_string(++counter) + ".txt");
  return {"--store", store.string()};
}

/// Runs the tool and returns its combined output.
std::string run(const std::filesystem::path& store, const std::vector<std::string>& arguments,
                int expected_exit, const char* label) {
  static int counter = 0;
  std::filesystem::path output;
  std::vector<std::string> full = base_arguments(store, counter, output);
  full.insert(full.end(), arguments.begin(), arguments.end());
  int exit_code = 0;
  std::string error;
  const bool started =
      pc_test::run_captured(pc_test::cli_path(), full, output, exit_code, error);
  PC_CHECK_MSG(started, error);
  std::string text;
  if (started) {
    const auto values = pc_test::read_key_values(output);
    for (const auto& entry : values) {
      text += entry.first + "=" + entry.second + "\n";
    }
    // The tool also prints free-form usage text; keep it for diagnostics.
    for (const auto& entry : pc_test::read_lines(output)) {
      text += entry + "\n";
    }
  }
  PC_CHECK_MSG(exit_code == expected_exit,
               std::string(label) + ": exit " + std::to_string(exit_code) + " output:\n" + text);
  return text;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

PC_TEST(cli, version_and_help_succeed) {
  TempDir directory;
  const std::filesystem::path store = directory.file("unused.pcstore");
  const std::string version = run(store, {"version"}, kExitOk, "version");
  PC_CHECK(contains(version, "version=1.0.0"));
  const std::string help = run(store, {"help"}, kExitOk, "help");
  PC_CHECK(contains(help, "Usage: power-capacity"));
}

PC_TEST(cli, an_unknown_command_is_a_usage_error) {
  TempDir directory;
  const std::filesystem::path store = directory.file("unused.pcstore");
  const std::string text = run(store, {"frobnicate"}, kExitUsage, "unknown command");
  PC_CHECK(contains(text, "error=unsupported"));
}

PC_TEST(cli, a_mutating_command_without_a_store_is_a_usage_error) {
  TempDir directory;
  const std::filesystem::path store = directory.file("unused.pcstore");
  const std::vector<std::string> full = {"put-domain", "--id", "pdu.a"};
  int exit_code = 0;
  std::string error;
  const bool started = pc_test::run_captured(pc_test::cli_path(), full,
                                             directory.file("usage.txt"), exit_code, error);
  PC_CHECK_MSG(started, error);
  PC_CHECK_EQ(exit_code, kExitUsage);
}

PC_TEST(cli, a_full_lifecycle_succeeds_and_is_machine_readable) {
  TempDir directory;
  const std::filesystem::path store = directory.file("cli.pcstore");

  const std::string initialized = run(store, {"init", "--tick", "0"}, kExitOk, "init");
  PC_CHECK(contains(initialized, "ok=true"));
  PC_CHECK(contains(initialized, "created=true"));
  PC_CHECK(contains(initialized, "generation=1"));

  const std::string domain = run(store,
                                 {"put-domain", "--id", "pdu.a", "--kind", "pdu", "--nominal-w",
                                  "100000", "--usable-w", "100000", "--derate-bp", "8000",
                                  "--reserve-mode", "ratio", "--reserve-bp", "2500",
                                  "--evidence-id", "ev.1", "--evidence-source", "source.el",
                                  "--observed-tick", "0", "--valid-until-tick", "1000",
                                  "--as-of-tick", "0", "--expected-generation", "1"},
                                 kExitOk, "put-domain");
  PC_CHECK(contains(domain, "generation=2"));

  const std::string assessment = run(store,
                                     {"assess", "--domain", "pdu.a", "--as-of-tick", "10",
                                      "--read-only"},
                                     kExitOk, "assess");
  // 100000 * 0.8 = 80000, reserve 25% = 20000, allocatable 60000 W.
  PC_CHECK(contains(assessment, "allocatable_w=60000.000"));
  PC_CHECK(contains(assessment, "known=true"));

  const std::string evaluation = run(store,
                                     {"evaluate", "--id", "cand.1", "--domain", "pdu.a",
                                      "--load-w", "50000", "--as-of-tick", "10", "--read-only"},
                                     kExitOk, "evaluate");
  PC_CHECK(contains(evaluation, "verdict=admissible"));

  const std::string too_big = run(store,
                                  {"evaluate", "--id", "cand.2", "--domain", "pdu.a",
                                   "--load-w", "70000", "--as-of-tick", "10", "--read-only"},
                                  kExitOk, "evaluate refused");
  PC_CHECK(contains(too_big, "verdict=refused"));
  PC_CHECK(contains(too_big, "reason=insufficient_headroom"));

  const std::string inspected = run(store, {"inspect"}, kExitOk, "inspect");
  PC_CHECK(contains(inspected, "generation=2"));
  PC_CHECK(contains(inspected, "domains=1"));
  PC_CHECK(contains(inspected, "writable=false"));

  const std::string verified = run(store, {"verify"}, kExitOk, "verify");
  PC_CHECK(contains(verified, "integrity_verified=true"));
}

PC_TEST(cli, json_output_is_one_object) {
  TempDir directory;
  const std::filesystem::path store = directory.file("cli-json.pcstore");
  run(store, {"init", "--tick", "0"}, kExitOk, "init");
  run(store,
      {"put-domain", "--id", "pdu.a", "--nominal-w", "1000", "--usable-w", "1000",
       "--evidence-id", "ev.1", "--evidence-source", "source.el", "--observed-tick", "0",
       "--valid-until-tick", "1000", "--as-of-tick", "0", "--expected-generation", "1"},
      kExitOk, "put-domain");
  const std::string text =
      run(store, {"assess", "--domain", "pdu.a", "--as-of-tick", "10", "--read-only", "--json"},
          kExitOk, "assess --json");
  PC_CHECK(contains(text, "\"ok\": true"));
  PC_CHECK(contains(text, "\"domain\": \"pdu.a\""));
  PC_CHECK(contains(text, "\"allocatable_w\": 1000.000"));
}

PC_TEST(cli, stale_authority_and_stale_generation_are_reported_as_typed_errors) {
  TempDir directory;
  const std::filesystem::path store = directory.file("cli-stale.pcstore");
  run(store, {"init", "--tick", "0"}, kExitOk, "init");
  const std::string stale = run(store,
                                {"put-domain", "--id", "pdu.a", "--nominal-w", "1000",
                                 "--usable-w", "1000", "--as-of-tick", "0",
                                 "--expected-generation", "9"},
                                kExitFailure, "stale generation");
  PC_CHECK(contains(stale, "error=stale_generation"));

  const std::string authority = run(store,
                                    {"put-domain", "--id", "pdu.a", "--nominal-w", "1000",
                                     "--usable-w", "1000", "--as-of-tick", "0", "--epoch", "7",
                                     "--expected-generation", "1"},
                                    kExitFailure, "stale authority");
  PC_CHECK(contains(authority, "error=stale_authority"));

  const std::string evidence = run(store,
                                   {"put-domain", "--id", "pdu.a", "--nominal-w", "1000",
                                    "--usable-w", "1000", "--as-of-tick", "0",
                                    "--expected-generation", "1", "--evidence-id", "ev.1",
                                    "--evidence-source", "source.el", "--observed-tick", "100",
                                    "--valid-until-tick", "50"},
                                   kExitFailure, "inverted evidence window");
  PC_CHECK(contains(evidence, "error=invalid_argument"));
}

PC_TEST(cli, a_read_only_command_cannot_be_tricked_into_writing) {
  TempDir directory;
  const std::filesystem::path store = directory.file("cli-readonly.pcstore");
  run(store, {"init", "--tick", "0"}, kExitOk, "init");
  const std::uint64_t before =
      PC_REQUIRE_OK(power_capacity::CapacityStore::read_file(store, power_capacity::StoreReadOptions{}))
          ->generation()
          .value();
  // assess never mutates, whatever else it is told to do.
  run(store, {"assess", "--domain", "pdu.a", "--as-of-tick", "0", "--read-only"}, kExitFailure,
      "assess unknown domain");
  const std::uint64_t after =
      PC_REQUIRE_OK(power_capacity::CapacityStore::read_file(store, power_capacity::StoreReadOptions{}))
          ->generation()
          .value();
  PC_CHECK_EQ(before, after);
}

PC_TEST(cli, a_store_held_by_another_process_is_reported_as_a_lock_conflict) {
  TempDir directory;
  const std::filesystem::path store = directory.file("cli-locked.pcstore");
  run(store, {"init", "--tick", "0"}, kExitOk, "init");

  power_capacity::StoreOpenOptions options;
  options.path = store;
  options.access = power_capacity::StoreAccess::ReadWrite;
  const auto held = power_capacity::CapacityStore::open(options);
  PC_CHECK(held.ok());
  if (!held.ok()) {
    return;
  }
  const std::string text = run(store,
                               {"put-domain", "--id", "pdu.a", "--nominal-w", "1000",
                                "--usable-w", "1000", "--as-of-tick", "0",
                                "--expected-generation", "1"},
                               kExitFailure, "locked store");
  PC_CHECK(contains(text, "error=lock_conflict"));
  PC_CHECK(held.value()->close().ok());
}
