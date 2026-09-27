cmake_minimum_required(VERSION 3.21)

project(PowerCapacity
  VERSION 1.0.0
  DESCRIPTION "Electrical capacity authority for the Data Center Control Plane"
  HOMEPAGE_URL "https://github.com/summonlabs/Power-Capacity"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(POWER_CAPACITY_IS_TOP_LEVEL ON)
else()
  set(POWER_CAPACITY_IS_TOP_LEVEL OFF)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(POWER_CAPACITY_IS_TOP_LEVEL)
  option(POWER_CAPACITY_BUILD_TESTS "Build the Power Capacity test suite" ON)
  option(POWER_CAPACITY_BUILD_EXAMPLES "Build the Power Capacity examples" ON)
  option(POWER_CAPACITY_BUILD_BENCHMARKS "Build the Power Capacity benchmarks" ON)
  option(POWER_CAPACITY_BUILD_TOOLS "Build the Power Capacity command line tool" ON)
else()
  option(POWER_CAPACITY_BUILD_TESTS "Build the Power Capacity test suite" OFF)
  option(POWER_CAPACITY_BUILD_EXAMPLES "Build the Power Capacity examples" OFF)
  option(POWER_CAPACITY_BUILD_BENCHMARKS "Build the Power Capacity benchmarks" OFF)
  option(POWER_CAPACITY_BUILD_TOOLS "Build the Power Capacity command line tool" OFF)
endif()

option(POWER_CAPACITY_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)

set(POWER_CAPACITY_DOWNSTREAM_PREFIX "" CACHE PATH
  "Install prefix of a previous Power Capacity installation; enables the downstream consumer test")

# ---------------------------------------------------------------------------
# First-party compile options
# ---------------------------------------------------------------------------
function(power_capacity_apply_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(POWER_CAPACITY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
      # Nothing extra: MSVC deprecation of /MP is handled by the generator.
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wnon-virtual-dtor
      -Wcast-qual
      -Wdouble-promotion)
    if(POWER_CAPACITY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Generated version header
# ---------------------------------------------------------------------------
set(POWER_CAPACITY_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/power_capacity/version.hpp.in"
  "${POWER_CAPACITY_GENERATED_INCLUDE_DIR}/power_capacity/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(power_capacity
  src/status.cpp
  src/ids.cpp
  src/units.cpp
  src/evidence.cpp
  src/power_domain.cpp
  src/load.cpp
  src/redundancy.cpp
  src/capacity.cpp
  src/state.cpp
  src/store.cpp
  src/engine.cpp
  src/detail/codec.cpp
  src/detail/crc32c.cpp
  src/detail/digest.cpp
  src/detail/file_lock.cpp
  src/detail/platform_io.cpp
  src/detail/serialization.cpp)
add_library(PowerCapacity::power_capacity ALIAS power_capacity)

target_include_directories(power_capacity
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${POWER_CAPACITY_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(power_capacity PUBLIC cxx_std_20)
power_capacity_apply_strict_options(power_capacity)

set_target_properties(power_capacity PROPERTIES
  OUTPUT_NAME "power_capacity"
  VERSION "${PROJECT_VERSION}"
  SOVERSION "${PROJECT_VERSION_MAJOR}"
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

if(BUILD_SHARED_LIBS AND WIN32)
  set_target_properties(power_capacity PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# ---------------------------------------------------------------------------
# CLI tool
# ---------------------------------------------------------------------------
if(POWER_CAPACITY_BUILD_TOOLS)
  add_executable(power_capacity_cli tools/pc_cli.cpp tools/cli_json.cpp)
  set_target_properties(power_capacity_cli PROPERTIES OUTPUT_NAME "power-capacity")
  target_link_libraries(power_capacity_cli PRIVATE power_capacity)
  target_include_directories(power_capacity_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools")
  power_capacity_apply_strict_options(power_capacity_cli)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS power_capacity
  EXPORT PowerCapacityTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(POWER_CAPACITY_BUILD_TOOLS)
  install(TARGETS power_capacity_cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/power_capacity"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
install(FILES "${POWER_CAPACITY_GENERATED_INCLUDE_DIR}/power_capacity/version.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/power_capacity")

set(POWER_CAPACITY_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/PowerCapacity")

install(EXPORT PowerCapacityTargets
  NAMESPACE PowerCapacity::
  FILE PowerCapacityTargets.cmake
  DESTINATION "${POWER_CAPACITY_INSTALL_CMAKEDIR}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PowerCapacityConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/PowerCapacityConfig.cmake"
  INSTALL_DESTINATION "${POWER_CAPACITY_INSTALL_CMAKEDIR}")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PowerCapacityConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/PowerCapacityConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/PowerCapacityConfigVersion.cmake"
  DESTINATION "${POWER_CAPACITY_INSTALL_CMAKEDIR}")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/PowerCapacity")

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
if(POWER_CAPACITY_IS_TOP_LEVEL AND (POWER_CAPACITY_BUILD_TESTS OR POWER_CAPACITY_BUILD_EXAMPLES))
  enable_testing()
endif()

if(POWER_CAPACITY_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(POWER_CAPACITY_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(POWER_CAPACITY_BUILD_TESTS)
  add_subdirectory(tests)
endif()
