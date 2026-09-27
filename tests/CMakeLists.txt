# Test suite.
#
# No test is given a timeout. CTest timeouts, timeout wrappers, and watchdog
# processes are prohibited in this repository: a test that does not finish is a
# defect to diagnose, not to bound.

# The probe is a real program that the multiprocess tests start as an independent
# operating-system process.
add_executable(pc_probe support/pc_probe.cpp)
target_link_libraries(pc_probe PRIVATE power_capacity)
power_capacity_apply_strict_options(pc_probe)

add_library(pc_test_support STATIC
  support/test_harness.cpp
  support/facility.cpp
  support/proc.cpp)
target_include_directories(pc_test_support PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(pc_test_support PUBLIC power_capacity)
target_compile_definitions(pc_test_support PUBLIC
  "PC_PROBE_EXECUTABLE=\"$<TARGET_FILE:pc_probe>\"")
add_dependencies(pc_test_support pc_probe)
power_capacity_apply_strict_options(pc_test_support)

if(TARGET power_capacity_cli)
  target_compile_definitions(pc_test_support PUBLIC
    "PC_CLI_EXECUTABLE=\"$<TARGET_FILE:power_capacity_cli>\"")
  add_dependencies(pc_test_support power_capacity_cli)
endif()

function(power_capacity_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE pc_test_support)
  # Adversarial artifacts are produced with the same encoder and checksum the
  # library uses, so the tests need the internal headers.
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  power_capacity_apply_strict_options(${name})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

power_capacity_add_test(pc_test_units test_units.cpp)
power_capacity_add_test(pc_test_model test_model.cpp)
power_capacity_add_test(pc_test_redundancy test_redundancy.cpp)
power_capacity_add_test(pc_test_rollup test_rollup.cpp)
power_capacity_add_test(pc_test_evidence test_evidence.cpp)
power_capacity_add_test(pc_test_persistence test_persistence.cpp)
power_capacity_add_test(pc_test_recovery test_recovery.cpp)
power_capacity_add_test(pc_test_concurrency test_concurrency.cpp)
power_capacity_add_test(pc_test_property test_property.cpp)
power_capacity_add_test(pc_test_adversarial test_adversarial.cpp)

if(TARGET power_capacity_cli)
  power_capacity_add_test(pc_test_cli test_cli.cpp)
endif()

# The downstream consumer is configured, built, and run out of tree against an
# installed package. It is enabled by pointing POWER_CAPACITY_DOWNSTREAM_PREFIX at
# an install prefix produced by this project.
if(POWER_CAPACITY_DOWNSTREAM_PREFIX)
  add_test(NAME pc_test_downstream_install
    COMMAND "${CMAKE_COMMAND}"
      -DPC_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DPC_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/downstream
      -DPC_PREFIX=${POWER_CAPACITY_DOWNSTREAM_PREFIX}
      -DPC_GENERATOR=${CMAKE_GENERATOR}
      -DPC_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DPC_CONFIG=$<CONFIG>
      -P "${CMAKE_SOURCE_DIR}/cmake/RunDownstreamCheck.cmake")
  # The check configures a nested CMake project, which needs a usable C++
  # toolchain. The script locates the MSVC developer environment itself; when none
  # can be reached the check fails with the exact reason rather than passing.
  set_tests_properties(pc_test_downstream_install PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
