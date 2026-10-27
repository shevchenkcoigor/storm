cmake_minimum_required(VERSION 3.30)

# Gate for CMake's experimental `import std;` support. Must be set BEFORE
# project(). The UUID is CMake-version-specific — see issue #326. Verified for
# CMake 4.4.0. Bump UUID if the minimum-supported CMake changes.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

# Pin the import-std descriptor to clang-p2996's libc++.modules.json before
# project(). Without this, CMake's auto-detection picks up the host GCC's
# /usr/lib64/libstdc++.modules.json and tries to build bits/std.cc against our
# libc++ include paths — which fails. See issue #326.
#
# LIBCXX_ROOT is passed via preset/-D. cmake/clang_p2996_host.cmake resolves the
# on-disk layout (shared with cmake/libcxx.cmake's compile/link flags — a plain
# include() of a variable-setting file is safe before project()). Falls back to
# letting CMake auto-detect when LIBCXX_ROOT is unset (e.g. plain library
# configures, where import-std is not used).
if(DEFINED LIBCXX_ROOT)
  include(cmake/clang_p2996_host.cmake)
  set(CMAKE_CXX_STDLIB_MODULES_JSON "${_storm_libcxx_modules_json}")
endif()

project(storm)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(
    FATAL_ERROR "This project requires Clang. Got: ${CMAKE_CXX_COMPILER_ID}")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "21")
  message(
    FATAL_ERROR "Clang >= 21 required. Got: ${CMAKE_CXX_COMPILER_VERSION}")
endif()

message(STATUS "Using C++26 (standard build)")
message(STATUS "CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")
message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "CMAKE_CXX_COMPILER_VERSION: ${CMAKE_CXX_COMPILER_VERSION}")

# Configure git hooks path (so pre-commit hook works on fresh clones)
execute_process(
  COMMAND git config core.hooksPath .githooks
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  ERROR_QUIET)

add_compile_options(-ferror-limit=1
                    $<$<CONFIG:Debug>:-ftemplate-backtrace-limit=0>)

# No -fmodules-cache-path: that flag configures Clang's implicit-modules cache,
# which only exists with -fmodules. The tree consumes named modules (`import
# std;`, issue #326) built by CMake/ninja into each preset's own binaryDir, so
# presets are isolated already and the flag was dead -- it only made
# clang-scan-deps print "argument unused" once per TU (#714).
add_compile_options(-Wno-unused-command-line-argument)

# CMake 4.4.0 (#521/#522) leaks target-scoped -Werror onto the CMake-synthesized
# std-module target that builds libc++'s own std.cppm (export module std;),
# where Clang's always-on -Wreserved-module-identifier becomes a hard error.
# Suppress it globally, same pattern as -Wno-unused-command-line-argument above.
add_compile_options(-Wno-reserved-module-identifier)

include(cmake/libcxx.cmake)

# ── Core library ──────────────────────────────────────────────────────────────

add_library(${PROJECT_NAME} STATIC)

# import std; migration (issue #326): consume the std named module on this
# target.
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_MODULE_STD ON)

file(GLOB_RECURSE STORM_MODULE_UNITS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cppm")

target_sources(${PROJECT_NAME} PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES
                                      FILES ${STORM_MODULE_UNITS})

target_compile_options(${PROJECT_NAME} PRIVATE -Werror)

apply_clang_flags(${PROJECT_NAME})

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STORM_MODULE_UNITS})

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
         "${CMAKE_CURRENT_SOURCE_DIR}/include"
         "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

include(cmake/db.cmake)

link_sqlite(${PROJECT_NAME})
link_postgresql(${PROJECT_NAME})

include(cmake/cpm.cmake)
include(cmake/cpm-cache.cmake)
# CPM resolves CPM_SOURCE_CACHE (variable or env) into the cache variable; OFF =
# unset.
if(CPM_SOURCE_CACHE)
  storm_verify_cpm_cache("${CPM_SOURCE_CACHE}")
endif()
include(cmake/uuid.cmake)
include(cmake/pyyaml.cmake)
include(cmake/cmake-scripts.cmake)

# ── Feature options ───────────────────────────────────────────────────────────

option(ENABLE_TESTS "Enable testing support" OFF)
option(ENABLE_BENCH "Enable benchmarking with sqlite_orm" OFF)
option(ENABLE_COVERAGE "Enable code coverage (requires LLVM tools)" OFF)
option(ENABLE_TOOLS "Build CLI tools (storm-schema, etc.)" OFF)

include(cmake/sanitizers.cmake)

# coverage flags must be set before test targets are compiled
include(cmake/coverage.cmake)
include(cmake/tests.cmake)
# coverage targets reference test targets — must come after tests.cmake
include(cmake/coverage-targets.cmake)
include(cmake/bench.cmake)
include(cmake/fuzz.cmake)
include(cmake/python.cmake)
include(cmake/format.cmake)
include(cmake/tools.cmake)

# ── Build summary ─────────────────────────────────────────────────────────────

message(STATUS "=== Storm Build Configuration ===")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(
  STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(
  STATUS "SQLite: ${SQLite3_VERSION} (STRICT=${STORM_SQLITE_STRICT_TABLES})")
message(STATUS "Testing: ${ENABLE_TESTS}")
message(STATUS "Benchmarks: ${ENABLE_BENCH}")
message(STATUS "Coverage: ${ENABLE_COVERAGE}")
message(STATUS "Sanitizer: ${USE_SANITIZER}")
message(STATUS "Fuzzing: ${ENABLE_FUZZING}")
message(STATUS "Tools: ${ENABLE_TOOLS}")
message(STATUS "Python bindings: ${ENABLE_PYTHON}")
message(STATUS "===================================")
