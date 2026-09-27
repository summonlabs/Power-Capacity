# Configures, builds, and runs the out-of-tree downstream consumer against an
# installed Power Capacity package. Invoked by CTest when
# POWER_CAPACITY_DOWNSTREAM_PREFIX is set, and by the release verification steps.
#
# A nested CMake project needs a usable C++ toolchain in this process's
# environment. On Windows with MSVC that environment is produced by
# vcvars64.bat, so this script locates it through vswhere (never through a
# hard-coded path) and runs the nested commands through it. When no toolchain can
# be reached the check fails with the exact reason rather than reporting a pass it
# did not earn.

foreach(required PC_SOURCE_DIR PC_BINARY_DIR PC_PREFIX)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "RunDownstreamCheck.cmake requires -D${required}=...")
  endif()
endforeach()

set(pc_vcvars "")
if(WIN32 AND DEFINED PC_VCVARS AND NOT PC_VCVARS STREQUAL "")
  set(pc_vcvars "${PC_VCVARS}")
elseif(WIN32)
  set(pc_program_files_x86 "$ENV{ProgramFiles\(x86\)}")
  set(pc_vswhere "${pc_program_files_x86}/Microsoft Visual Studio/Installer/vswhere.exe")
  if(EXISTS "${pc_vswhere}")
    execute_process(COMMAND "${pc_vswhere}" -latest -products * -requires
                            Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                            -property installationPath
                    OUTPUT_VARIABLE pc_vs_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(NOT pc_vs_path STREQUAL "")
      set(pc_vcvars "${pc_vs_path}/VC/Auxiliary/Build/vcvars64.bat")
    endif()
  endif()
endif()

# Runs one nested command, entering the MSVC developer environment first when one
# was found. The environment is entered through a generated batch file rather than
# through `cmd /c "call ... && ..."`, because nesting quotes inside an outer quoted
# command line is not reliable.
set(pc_run_serial 0)
function(pc_run description)
  math(EXPR pc_run_serial "${pc_run_serial} + 1")
  if(WIN32 AND NOT pc_vcvars STREQUAL "" AND EXISTS "${pc_vcvars}")
    set(pc_batch "${PC_BINARY_DIR}/pc-run-${pc_run_serial}.bat")
    set(pc_script "@echo off\r\ncall \"${pc_vcvars}\" >nul 2>&1\r\n")
    foreach(argument IN LISTS ARGN)
      set(pc_script "${pc_script}\"${argument}\" ")
    endforeach()
    set(pc_script "${pc_script}\r\nexit /b %ERRORLEVEL%\r\n")
    file(WRITE "${pc_batch}" "${pc_script}")
    execute_process(COMMAND cmd /c "${pc_batch}"
      RESULT_VARIABLE pc_result
      OUTPUT_VARIABLE pc_output
      ERROR_VARIABLE pc_output)
  else()
    execute_process(COMMAND ${ARGN}
      RESULT_VARIABLE pc_result
      OUTPUT_VARIABLE pc_output
      ERROR_VARIABLE pc_output)
  endif()

  if(NOT pc_result EQUAL 0)
    if(pc_output MATCHES "No CMAKE_CXX_COMPILER could be found" OR
       pc_output MATCHES "CMAKE_CXX_COMPILER not set")
      message(FATAL_ERROR
              "${description} could not run because no C++ compiler is reachable from this "
              "environment. Run the suite from a developer environment (for MSVC, after "
              "vcvars64.bat) to execute the installed-package check for real.\n${pc_output}")
    endif()
    if(pc_output MATCHES "Could not find a package configuration file provided by \"PowerCapacity\"")
      message(FATAL_ERROR
              "${description} failed because no Power Capacity package was found under "
              "'${PC_PREFIX}'. Install the project into that prefix first, for example "
              "'cmake --install <build-dir> --prefix ${PC_PREFIX}'.\n${pc_output}")
    endif()
    message(FATAL_ERROR "${description} failed with exit ${pc_result}:\n${pc_output}")
  endif()
  set(pc_last_output "${pc_output}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${PC_BINARY_DIR}")
file(MAKE_DIRECTORY "${PC_BINARY_DIR}")

set(configure_command "${CMAKE_COMMAND}"
  -S "${PC_SOURCE_DIR}/downstream/consumer"
  -B "${PC_BINARY_DIR}"
  "-DCMAKE_PREFIX_PATH=${PC_PREFIX}")
if(DEFINED PC_GENERATOR AND NOT PC_GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${PC_GENERATOR}")
endif()
if(DEFINED PC_CONFIG AND NOT PC_CONFIG STREQUAL "")
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${PC_CONFIG}")
endif()

pc_run("downstream configure" ${configure_command})
pc_run("downstream build" "${CMAKE_COMMAND}" --build "${PC_BINARY_DIR}")
pc_run("downstream run" "${CMAKE_COMMAND}" --build "${PC_BINARY_DIR}" --target run_consumer)

message(STATUS "downstream consumer output:\n${pc_last_output}")
