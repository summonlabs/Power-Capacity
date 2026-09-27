#include "test_harness.hpp"

#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <ostream>
#include <string>

namespace power_capacity {

std::ostream& operator<<(std::ostream& stream, StatusCode value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ReasonCode value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, EvidenceState value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, OperationalState value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, StateCause value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, DomainKind value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, LoadClass value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, RedundancyClass value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, ReservePolicy::Mode value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, CandidateVerdict value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, RevalidationVerdict value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, EngineLifecycle value) {
  return stream << to_string(value);
}
std::ostream& operator<<(std::ostream& stream, Power value) {
  return stream << value.to_watts_string() << " W";
}
std::ostream& operator<<(std::ostream& stream, Ratio value) {
  return stream << value.to_percent_string() << " %";
}
std::ostream& operator<<(std::ostream& stream, const Status& value) {
  return stream << value.to_string();
}
std::ostream& operator<<(std::ostream& stream, const DomainId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const LoadId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const GroupId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const SourceId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const EvidenceId& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, const AuthorityRef& value) {
  return stream << value.value();
}
std::ostream& operator<<(std::ostream& stream, Generation value) {
  return stream << "generation(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, Epoch value) {
  return stream << "epoch(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, Incarnation value) {
  return stream << "incarnation(" << value.value() << ")";
}
std::ostream& operator<<(std::ostream& stream, Tick value) {
  return stream << "tick(" << value.value() << ")";
}

}  // namespace power_capacity

namespace pc_test {
namespace {

struct RunState {
  std::size_t checks_failed = 0;
  std::vector<std::string> failures;
  std::string current;
  bool filter_matched = false;
};

RunState& state() {
  static RunState instance;
  return instance;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> instance;
  return instance;
}

Registrar::Registrar(const char* suite, const char* name, Body body) {
  registry().push_back(TestCase{suite, name, std::move(body)});
}

void report_failure(const char* file, int line, const std::string& expression,
                    const std::string& detail) {
  RunState& run = state();
  ++run.checks_failed;
  std::string message = std::string(file) + ":" + std::to_string(line) + ": " + expression;
  if (!detail.empty()) {
    message += " [" + detail + "]";
  }
  run.failures.push_back(message);
}

std::string display(const std::string& value) { return "\"" + value + "\""; }
std::string display(const char* value) { return std::string("\"") + value + "\""; }
std::string display(bool value) { return value ? "true" : "false"; }
std::string display(std::nullptr_t) { return "nullptr"; }

int run_all(int argc, char** argv) {
  std::string filter;
  if (argc > 1) {
    filter = argv[1];
  }
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : registry()) {
    const std::string full_name = test.suite + "." + test.name;
    if (!filter.empty() && full_name.find(filter) == std::string::npos) {
      continue;
    }
    const std::size_t before = state().checks_failed;
    const std::size_t failures_before = state().failures.size();
    state().current = full_name;
    try {
      test.body();
    } catch (const std::exception& error) {
      report_failure("<test body>", 0, "threw std::exception", error.what());
    } catch (...) {
      report_failure("<test body>", 0, "threw a non-standard exception", "");
    }
    const bool ok = state().checks_failed == before;
    if (ok) {
      ++passed;
      std::cout << "[  PASS  ] " << full_name << "\n";
    } else {
      ++failed;
      std::cout << "[  FAIL  ] " << full_name << "\n";
      for (std::size_t index = failures_before; index < state().failures.size(); ++index) {
        std::cout << "           " << state().failures[index] << "\n";
      }
    }
    std::cout.flush();
  }
  std::cout << "\n" << passed << " passed, " << failed << " failed\n";
  std::cout.flush();
  return failed == 0 ? 0 : 1;
}

}  // namespace pc_test

int main(int argc, char** argv) { return pc_test::run_all(argc, argv); }
