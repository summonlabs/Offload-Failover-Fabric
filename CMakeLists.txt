cmake_minimum_required(VERSION 3.20)

project(OffloadFailoverFabric
  VERSION 1.0.0
  DESCRIPTION "Governed failover runtime for offloaded network services"
  HOMEPAGE_URL "https://github.com/summonlabs/Offload-Failover-Fabric"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(OFF_BUILD_TOOLS "Build the off inspection CLI and the offd service daemon" ON)
option(OFF_BUILD_BENCHMARKS "Build the benchmark executables" ON)
option(OFF_BUILD_DOWNSTREAM_EXAMPLE "Configure the packaged downstream consumer" OFF)
option(OFF_ENABLE_ASAN "Build with AddressSanitizer when the toolchain provides a runtime" OFF)
option(OFF_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(OFF_ENABLE_IPO "Enable interprocedural optimisation for Release builds" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# First-party warning profile. Applied PRIVATE so nothing leaks downstream.
# ---------------------------------------------------------------------------

add_library(off_warnings INTERFACE)
if(MSVC)
  target_compile_options(off_warnings INTERFACE
    /W4
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /utf-8
    /EHsc)
  if(OFF_WARNINGS_AS_ERRORS)
    target_compile_options(off_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(off_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(off_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(OFF_WARNINGS_AS_ERRORS)
    target_compile_options(off_warnings INTERFACE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Sanitizer support. Reported honestly: if the toolchain has no runtime the
# option stays off and the configure step says so.
# ---------------------------------------------------------------------------

set(OFF_ASAN_AVAILABLE OFF)
if(OFF_ENABLE_ASAN)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS_BACKUP "${CMAKE_REQUIRED_FLAGS}")
  if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "/fsanitize=address /Zi")
  else()
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
  endif()
  check_cxx_source_compiles("
    #include <vector>
    int main() { std::vector<int> v; v.push_back(1); return v[0] - 1; }
  " OFF_ASAN_LINKS)
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_BACKUP}")
  if(OFF_ASAN_LINKS)
    set(OFF_ASAN_AVAILABLE ON)
  else()
    message(STATUS "OFF_ENABLE_ASAN was requested but this toolchain has no AddressSanitizer runtime; "
                   "the build continues WITHOUT sanitizer coverage.")
  endif()
endif()

# ---------------------------------------------------------------------------
# The runtime library.
# ---------------------------------------------------------------------------

set(OFF_PUBLIC_HEADERS
  include/off/canonical.hpp
  include/off/digest.hpp
  include/off/explain.hpp
  include/off/fabric.hpp
  include/off/ids.hpp
  include/off/model.hpp
  include/off/off.hpp
  include/off/plan.hpp
  include/off/protocol.hpp
  include/off/reason.hpp
  include/off/store.hpp
  include/off/strong.hpp
  include/off/version.hpp)

set(OFF_SOURCES
  src/canonical.cpp
  src/digest.cpp
  src/fabric.cpp
  src/model.cpp
  src/protocol.cpp
  src/reason.cpp
  src/store.cpp
  src/strong.cpp
  src/version.cpp)

add_library(offload_failover_fabric ${OFF_SOURCES} ${OFF_PUBLIC_HEADERS})
add_library(OffloadFailoverFabric::offload_failover_fabric ALIAS offload_failover_fabric)

target_include_directories(offload_failover_fabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# BUILD_INTERFACE keeps the warning profile out of the installed export, so the
# settings cannot leak into a downstream project.
target_link_libraries(offload_failover_fabric PRIVATE $<BUILD_INTERFACE:off_warnings>)
target_compile_features(offload_failover_fabric PUBLIC cxx_std_20)
set_target_properties(offload_failover_fabric PROPERTIES
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  DEFINE_SYMBOL offload_failover_fabric_EXPORTS)

if(WIN32)
  target_link_libraries(offload_failover_fabric PUBLIC ws2_32)
endif()

if(OFF_ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT OFF_IPO_OK OUTPUT OFF_IPO_MESSAGE)
  if(OFF_IPO_OK)
    set_property(TARGET offload_failover_fabric PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
  else()
    message(STATUS "Interprocedural optimisation unavailable: ${OFF_IPO_MESSAGE}")
  endif()
endif()

if(OFF_ASAN_AVAILABLE)
  if(MSVC)
    target_compile_options(offload_failover_fabric PUBLIC /fsanitize=address /Zi)
  else()
    target_compile_options(offload_failover_fabric PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(offload_failover_fabric PUBLIC -fsanitize=address)
  endif()
  target_compile_definitions(offload_failover_fabric PUBLIC OFF_SANITIZED=1)
endif()

# ---------------------------------------------------------------------------
# Tools.
# ---------------------------------------------------------------------------

if(OFF_BUILD_TOOLS)
  add_executable(off apps/off_main.cpp)
  target_link_libraries(off PRIVATE offload_failover_fabric off_warnings)
  set_target_properties(off PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

  add_executable(offd apps/offd_main.cpp)
  target_link_libraries(offd PRIVATE offload_failover_fabric off_warnings)
  set_target_properties(offd PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------

if(BUILD_TESTING)
  add_executable(off_tests
    tests/harness.cpp
    tests/test_main.cpp
    tests/test_strong.cpp
    tests/test_canonical.cpp
    tests/test_model.cpp
    tests/test_store.cpp
    tests/test_qualify.cpp
    tests/test_fence.cpp
    tests/test_planner.cpp
    tests/test_stateful.cpp
    tests/test_effects.cpp
    tests/test_failback.cpp
    tests/test_restart.cpp
    tests/test_concurrency.cpp
    tests/test_adversarial.cpp
    tests/test_property.cpp)
  target_include_directories(off_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(off_tests PRIVATE offload_failover_fabric off_warnings)
  set_target_properties(off_tests PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)

  add_test(NAME off_tests COMMAND off_tests)
  set_tests_properties(off_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 0)

  if(OFF_BUILD_TOOLS)
    add_executable(off_process_tests tests/harness.cpp tests/test_process.cpp)
    target_include_directories(off_process_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(off_process_tests PRIVATE offload_failover_fabric off_warnings)
    set_target_properties(off_process_tests PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    add_dependencies(off_process_tests offd off)
    add_test(NAME off_process_tests
      COMMAND off_process_tests "$<TARGET_FILE:offd>" "$<TARGET_FILE:off>")
    set_tests_properties(off_process_tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      TIMEOUT 0)
  endif()

  if(OFF_BUILD_DOWNSTREAM_EXAMPLE)
    add_subdirectory(examples/downstream)
  endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks.
# ---------------------------------------------------------------------------

if(OFF_BUILD_BENCHMARKS)
  add_executable(off_bench bench/bench_off.cpp)
  target_link_libraries(off_bench PRIVATE offload_failover_fabric off_warnings)
  set_target_properties(off_bench PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
endif()

# ---------------------------------------------------------------------------
# Install and export.
# ---------------------------------------------------------------------------

install(TARGETS offload_failover_fabric
  EXPORT OffloadFailoverFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(OFF_BUILD_TOOLS)
  install(TARGETS off offd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT OffloadFailoverFabricTargets
  FILE OffloadFailoverFabricTargets.cmake
  NAMESPACE OffloadFailoverFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OffloadFailoverFabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/OffloadFailoverFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/OffloadFailoverFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OffloadFailoverFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/OffloadFailoverFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/OffloadFailoverFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/OffloadFailoverFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OffloadFailoverFabric)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/OffloadFailoverFabric)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/OffloadFailoverFabric)
