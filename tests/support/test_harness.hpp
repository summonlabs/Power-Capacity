#pragma once

// Minimal deterministic test harness. No third-party dependency, no timeouts,
// no watchdog logic: a check either passes, fails, or the program does not
// finish, and an unfinished program is a defect to diagnose.

#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

#include "power_capacity/capacity.hpp"
#include "power_capacity/engine.hpp"
#include "power_capacity/ids.hpp"
#include "power_capacity/load.hpp"
#include "power_capacity/power_domain.hpp"
#include "power_capacity/redundancy.hpp"
#include "power_capacity/status.hpp"
#include "power_capacity/units.hpp"

namespace power_capacity {

// Display helpers so that a failing check prints a readable value. These exist
// only for test diagnostics and are not part of the library.
std::ostream& operator<<(std::ostream& stream, StatusCode value);
std::ostream& operator<<(std::ostream& stream, ReasonCode value);
std::ostream& operator<<(std::ostream& stream, EvidenceState value);
std::ostream& operator<<(std::ostream& stream, OperationalState value);
std::ostream& operator<<(std::ostream& stream, StateCause value);
std::ostream& operator<<(std::ostream& stream, DomainKind value);
std::ostream& operator<<(std::ostream& stream, LoadClass value);
std::ostream& operator<<(std::ostream& stream, RedundancyClass value);
std::ostream& operator<<(std::ostream& stream, ReservePolicy::Mode value);
std::ostream& operator<<(std::ostream& stream, CandidateVerdict value);
std::ostream& operator<<(std::ostream& stream, RevalidationVerdict value);
std::ostream& operator<<(std::ostream& stream, EngineLifecycle value);
std::ostream& operator<<(std::ostream& stream, Power value);
std::ostream& operator<<(std::ostream& stream, Ratio value);
std::ostream& operator<<(std::ostream& stream, const Status& value);
std::ostream& operator<<(std::ostream& stream, const DomainId& value);
std::ostream& operator<<(std::ostream& stream, const LoadId& value);
std::ostream& operator<<(std::ostream& stream, const GroupId& value);
std::ostream& operator<<(std::ostream& stream, const SourceId& value);
std::ostream& operator<<(std::ostream& stream, const EvidenceId& value);
std::ostream& operator<<(std::ostream& stream, const AuthorityRef& value);
std::ostream& operator<<(std::ostream& stream, Generation value);
std::ostream& operator<<(std::ostream& stream, Epoch value);
std::ostream& operator<<(std::ostream& stream, Incarnation value);
std::ostream& operator<<(std::ostream& stream, Tick value);

}  // namespace power_capacity

namespace pc_test {

using Body = std::function<void()>;

struct TestCase {
  std::string suite;
  std::string name;
  Body body;
};

std::vector<TestCase>& registry();

class Registrar {
 public:
  Registrar(const char* suite, const char* name, Body body);
};

/// Records a failure for the currently running test.
void report_failure(const char* file, int line, const std::string& expression,
                    const std::string& detail);

/// Renders a value for a failure message. Types without a stream operator are
/// named by their RTTI name rather than silently omitted.
template <typename T>
std::string display(const T& value) {
  if constexpr (requires(std::ostream& stream, const T& item) { stream << item; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return std::string("<value of type ") + typeid(T).name() + ">";
  }
}

std::string display(const std::string& value);
std::string display(const char* value);
std::string display(bool value);
std::string display(std::nullptr_t value);

/// Extracts the status of either a `Status` or a `Result<T>`, so that a check can
/// be written the same way for both. Returns by value: returning a reference
/// would dangle as soon as the temporary `Result<T>` in the caller's full
/// expression is destroyed.
inline ::power_capacity::Status status_of(const ::power_capacity::Status& value) { return value; }

template <typename T>
::power_capacity::Status status_of(const ::power_capacity::Result<T>& value) {
  return value.status();
}

int run_all(int argc, char** argv);

}  // namespace pc_test

#define PC_TEST(suite, name)                                                          \
  static void pc_test_body_##suite##_##name();                                        \
  static const ::pc_test::Registrar pc_test_registrar_##suite##_##name(               \
      #suite, #name, pc_test_body_##suite##_##name);                                  \
  static void pc_test_body_##suite##_##name()

#define PC_CHECK(expression)                                                       \
  do {                                                                             \
    if (!(expression)) {                                                           \
      ::pc_test::report_failure(__FILE__, __LINE__, #expression, "");              \
    }                                                                              \
  } while (false)

#define PC_CHECK_MSG(expression, detail)                                           \
  do {                                                                             \
    if (!(expression)) {                                                           \
      ::pc_test::report_failure(__FILE__, __LINE__, #expression, (detail));        \
    }                                                                              \
  } while (false)

#define PC_CHECK_EQ(left, right)                                                   \
  do {                                                                             \
    const auto& pc_left_value = (left);                                            \
    const auto& pc_right_value = (right);                                          \
    if (!(pc_left_value == pc_right_value)) {                                      \
      ::pc_test::report_failure(__FILE__, __LINE__, #left " == " #right,           \
                                "left=" + ::pc_test::display(pc_left_value) +      \
                                    " right=" + ::pc_test::display(pc_right_value)); \
    }                                                                              \
  } while (false)

#define PC_CHECK_NE(left, right)                                                   \
  do {                                                                             \
    const auto& pc_left_value = (left);                                            \
    const auto& pc_right_value = (right);                                          \
    if (pc_left_value == pc_right_value) {                                         \
      ::pc_test::report_failure(__FILE__, __LINE__, #left " != " #right,           \
                                "both=" + ::pc_test::display(pc_left_value));      \
    }                                                                              \
  } while (false)

/// Requires the result to hold and returns its value.
#define PC_REQUIRE_OK(expression)                                                       \
  ([&]() {                                                                              \
    auto pc_result_value = (expression);                                                \
    if (!pc_result_value.ok()) {                                                        \
      ::pc_test::report_failure(__FILE__, __LINE__, #expression " is ok",               \
                                "status=" + ::pc_test::display(pc_result_value.status())); \
    }                                                                                   \
    return pc_result_value.value();                                                     \
  }())

/// Requires the status to fail with exactly the given code. Accepts either a
/// `Status` or a `Result<T>`.
#define PC_REQUIRE_STATUS(expression, expected_code)                                       \
  do {                                                                                     \
    const ::power_capacity::Status pc_status_value = ::pc_test::status_of(expression);      \
    if (pc_status_value.code() != (expected_code)) {                                        \
      ::pc_test::report_failure(__FILE__, __LINE__, #expression " fails with " #expected_code, \
                                "actual=" + ::pc_test::display(pc_status_value));          \
    }                                                                                      \
  } while (false)
