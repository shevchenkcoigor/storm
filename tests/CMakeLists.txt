project(storm_tests LANGUAGES CXX)

# ============================================================================
# YAML → JSON conversion for YAML-driven test cases.
# test_cases/unified_cases.yaml is the single source of truth; it is converted
# to JSON at configure time so the JSON can be embedded via C++26 #embed in
# test_parser.hpp.
# ============================================================================

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(TEST_CASES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/test_cases")

# Issue #561: the unified corpus is split by category, one JSON per test TU, so
# its ~190 ms/case of template instantiation compiles in parallel (53 s -> 24 s
# wall). The split files are generated from the YAML, never hand-maintained.
#
# Issue #582: a second rule here also emitted the whole corpus as one
# unified_cases.json, to feed test_parser.hpp's #ifndef default. Every includer
# selects a slice, so nothing embedded it -- the rule generated a 53 KB file on
# every build for a branch no TU took. It is gone along with the default, and so
# is scripts/yaml_to_json_tests.py, which this rule was the only caller of.
set(SPLIT_CASES_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/split_unified_cases.py")
set(UNIFIED_CASES_YAML "${TEST_CASES_DIR}/unified_cases.yaml")

# Single source of truth for the category split; mirrored by CATEGORIES in
# scripts/split_unified_cases.py and by the tests/yaml/test_unified_yaml_*.cpp
# TUs.
set(SPLIT_CATEGORIES select grouped aggregate crud)

set(SPLIT_JSON_FILES "")
foreach(CATEGORY IN LISTS SPLIT_CATEGORIES)
  list(APPEND SPLIT_JSON_FILES
       "${TEST_CASES_DIR}/unified_cases_${CATEGORY}.json")
endforeach()

add_custom_command(
  OUTPUT ${SPLIT_JSON_FILES}
  COMMAND ${Python3_EXECUTABLE} "${SPLIT_CASES_SCRIPT}" "${UNIFIED_CASES_YAML}"
          "${TEST_CASES_DIR}"
  DEPENDS "${UNIFIED_CASES_YAML}" "${SPLIT_CASES_SCRIPT}"
  COMMENT "Splitting unified_cases.yaml into per-category JSON"
  VERBATIM)

add_custom_target(generate_test_json ALL DEPENDS ${SPLIT_JSON_FILES})

# These are generated into the SOURCE tree (#embed resolves relative to the
# including file, so they cannot live under the build dir). Register them for
# `clean` so a renamed category does not leave a stale JSON behind that keeps
# satisfying an #embed — which would pass on a warm tree and fail on a cold one.
#
# The glob is what makes that true. Listing ${SPLIT_JSON_FILES} alone registers
# only the CURRENT names, so the one file a rename orphans — the only file the
# hazard is about — was precisely the one `clean` could not reach. #582 produced
# the first real instance: the combined unified_cases.json it stopped generating
# still sits in every tree built before it.
file(GLOB STALE_CASES_JSON "${TEST_CASES_DIR}/unified_cases*.json")
list(APPEND STALE_CASES_JSON ${SPLIT_JSON_FILES})
list(REMOVE_DUPLICATES STALE_CASES_JSON)

set_property(
  TARGET generate_test_json
  APPEND
  PROPERTY ADDITIONAL_CLEAN_FILES ${STALE_CASES_JSON})

# Auto-discover all test source files (excluding mock_sqlite/mock_libpq
# directories)
file(
  GLOB
  TEST_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/crud/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/query/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/db/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/errors/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/yaml/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/bench_dashboard/*.cpp")

add_executable(${PROJECT_NAME} ${TEST_SOURCES})

# The YAML corpus reaches these TUs through `#embed` (tests/test_parser.hpp),
# and `#embed` is invisible to the dependency scanner: clang does not list the
# embedded file in the depfile, so ninja records no edge from the object to the
# JSON (`ninja -t deps` showed none). Editing unified_cases.yaml therefore
# regenerated the JSON but left the objects stale — a green build still running
# the previous corpus. OBJECT_DEPENDS states the edge explicitly.
#
# set_property(SOURCE ... APPEND) rather than set_source_files_properties(): the
# latter silently does nothing when the path matches no target source, so a
# renamed category would quietly restore the stale-object bug instead of
# failing.
foreach(CATEGORY IN LISTS SPLIT_CATEGORIES)
  set_property(
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/yaml/test_unified_yaml_${CATEGORY}.cpp"
    APPEND
    PROPERTY OBJECT_DEPENDS "${TEST_CASES_DIR}/unified_cases_${CATEGORY}.json")
endforeach()

target_sources(
  ${PROJECT_NAME}
  PRIVATE FILE_SET
          cxx_modules
          TYPE
          CXX_MODULES
          BASE_DIRS
          ${CMAKE_SOURCE_DIR}/benchmarks
          ${CMAKE_CURRENT_SOURCE_DIR}/schema
          FILES
          ${CMAKE_SOURCE_DIR}/benchmarks/schema.cppm
          ${CMAKE_SOURCE_DIR}/benchmarks/parser.cppm
          ${CMAKE_SOURCE_DIR}/benchmarks/dashboard/wire.cppm
          ${CMAKE_SOURCE_DIR}/benchmarks/dashboard/tui.cppm
          # #694 regression fixture: statements instantiated inside a BMI, used
          # from schema/test_reflection_cache_bmi.cpp (the storm_bench shape).
          ${CMAKE_CURRENT_SOURCE_DIR}/schema/test_bmi_fk_module.cppm)

# ── Precompiled header ────────────────────────────────────────────────────────
# Measured floor per test TU (-ftime-trace on a TU whose only body is
# EXPECT_TRUE(true)): 2.2 s of <gtest/gtest.h>, 2.1 s of `import storm;`, 1.4 s
# of test_db_helpers.h + shared/models.h (the pre-#634 umbrella), 0.2 s of
# `import std;` — and Backend 0.03 s, i.e. the floor is entirely frontend.
# Precompiling gtest removes its share: measured -1.9 to -2.4 s per TU across
# four files (min of 3 runs each, ccache disabled), ~-236 s over the ~110 test
# TUs.
#
# UNITY_BUILD would amortise the whole floor rather than one slice, but CMake
# silently disables it whenever CXX_SCAN_FOR_MODULES is ON (verified: SCAN=OFF
# -> Unity edges appear, SCAN=ON -> none), and scanning cannot be turned off
# here because these TUs `import storm;` (verified: a TU with `import std;` and
# CXX_SCAN_FOR_MODULES OFF fails with "module 'std' not found"). PCH is the one
# mechanism of the two that works alongside module scanning.
#
# NOTE: scripts/run_clang_tidy.sh strips the resulting `-Xclang -include-pch`
# flags from the compile database before replaying commands. clang-tidy SIGSEGVs
# on them once any check is enabled, silently and with no diagnostic. That is
# unreachable today (tests/.clang-tidy sets `Checks: '-*'`, #590, and no src/
# entry gets the PCH), so the strip is defence in depth: it keeps re-enabling
# checks for tests/** from silently un-linting the tree. See COMPILER_ISSUES
# #13.
target_precompile_headers(${PROJECT_NAME} PRIVATE <gtest/gtest.h>)

# The module interface units in this target's FILE_SET must not receive the PCH.
# CMake would otherwise force gtest's entire textual libc++ surface in ahead of
# their `module;` declaration and bake that state into the BMIs every consuming
# TU imports. They include no gtest, so there is nothing to gain either.
set_property(
  SOURCE ${CMAKE_SOURCE_DIR}/benchmarks/schema.cppm
         ${CMAKE_SOURCE_DIR}/benchmarks/parser.cppm
         ${CMAKE_SOURCE_DIR}/benchmarks/dashboard/wire.cppm
         ${CMAKE_SOURCE_DIR}/benchmarks/dashboard/tui.cppm
         ${CMAKE_CURRENT_SOURCE_DIR}/schema/test_bmi_fk_module.cppm
  PROPERTY SKIP_PRECOMPILE_HEADERS ON)

# The #694 cross-BMI fixture and its importer are compiled without coverage
# instrumentation. Instantiating SchemaStatement inside a module BMI (and
# reusing it from an importer) makes clang emit zero-count coverage records for
# the lambdas inside consteval folds (schema.cppm append_create_table_sql,
# field_names.cppm for_each_field_name_impl). Those lambdas never run at
# runtime, so the records only inflate the denominator: 9166/9203 -> 9166/9166
# lines with the same covered count. Every src/ line these two TUs execute is
# covered by the other tests.
if(ENABLE_COVERAGE)
  set_property(
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/schema/test_bmi_fk_module.cppm
           ${CMAKE_CURRENT_SOURCE_DIR}/schema/test_reflection_cache_bmi.cpp
    APPEND
    PROPERTY COMPILE_OPTIONS -fno-profile-instr-generate -fno-coverage-mapping)
endif()

# Two TUs opt out. Reaching libc++ through the PCH, completing an entity that
# lazily pulls in <__fwd/string.h> (std::filesystem::path, storm::UUID, ...)
# makes std::__1::string visible next to the `std` module's std::string, and
# EVERY LATER mention of a string type in that TU becomes ambiguous:
# test_bindable_concept.cpp:40: error: reference to 'string' is ambiguous It is
# order-dependent — naming the string type first compiles clean — so this is not
# confined to these two files or to compile-time-only TUs: any TU that names
# such a trigger before a string type can hit it. A textual <string> does NOT
# help; textually including the trigger's own header does. Excluding is
# preferred over sprinkling includes, which would cut against the `import std;`
# convention (#326). The failure is a hard compile error, never silent.
set_property(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/schema/test_bindable_concept.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/schema/test_dialect_concepts.cpp"
  PROPERTY SKIP_PRECOMPILE_HEADERS ON)

# The bench_dashboard TUs import the module units excluded just above. With the
# consumer carrying the PCH and the module interface not, libc++'s textual
# std::basic_string and the module's copy fail to reconcile: string:1172: error:
# 'std::basic_string<char>::basic_string' from module '' is not present in
# definition of 'std::basic_string<char>' provided earlier Release-only, because
# the dashboard modules are not built in every preset. Opt the consumers out
# too, so both sides of that import agree.
file(GLOB STORM_PCH_SKIP_DASHBOARD
     "${CMAKE_CURRENT_SOURCE_DIR}/bench_dashboard/*.cpp")
set_property(SOURCE ${STORM_PCH_SKIP_DASHBOARD} PROPERTY SKIP_PRECOMPILE_HEADERS
                                                         ON)

# Third symptom of the same PCH/module split: ordinary code, no static_assert
# and no module import, just std::string used normally --
# test_sqlite_tuning.cpp:29: error: cannot initialize object parameter of type
# 'const std::__1::basic_string<char>' with an expression of type 'std::string'
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/db/test_sqlite_tuning.cpp"
             PROPERTY SKIP_PRECOMPILE_HEADERS ON)

include(GoogleTest)

target_link_libraries(${PROJECT_NAME} PRIVATE storm gtest gtest_main gmock
                                              gmock_main pthread)

link_sqlite(${PROJECT_NAME})
link_postgresql(${PROJECT_NAME})

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE ${GTEST_INCLUDE_DIRS} ${GMOCK_INCLUDE_DIRS}
          "${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_SOURCE_DIR}"
          "${CMAKE_SOURCE_DIR}/benchmarks/dashboard")

# Use gtest_discover_tests instead of gtest_add_tests DISCOVERY_MODE PRE_TEST:
# defer test listing to ctest time (not build time) so MSAN_OPTIONS from
# CMakePresets.json applies when the binary runs. Without this, cmake --build
# runs the binary with no MSAN env → abort.
#
# DISCOVERY_TIMEOUT raised from CMake's 5s default: with ~3000 registered
# TYPED_TESTs, --gtest_list_tests takes low single-digit seconds even under
# normal load, but on a busy shared/loaded machine that's enough to blow past 5s
# and fail the whole ctest run with "Process terminated due to timeout" before
# any test actually runs.
gtest_discover_tests(${PROJECT_NAME} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT
                     120)

# Add Clang-specific flags (modules and reflection) to tests (same as Storm
# library)
apply_clang_flags(${PROJECT_NAME})
# cmake-format: off
# No -fconstexpr-steps here, deliberately. The consteval JSON parser for the
# YAML-driven test corpus used to need -fconstexpr-steps=4194304, sized when one
# TU parsed all 247 cases. #561 split the corpus across four tests/yaml TUs, each
# #embedding only its own category slice, and the flag was never re-measured.
#
# #582 measured it, per slice, by bisecting the flag on each TU's real compile
# command down to where clang reports "constexpr evaluation hit maximum step
# limit":
#
#     grouped    295864   <- ceiling, and NOT the slice with the most cases
#     select     213964
#     aggregate  164824
#     crud        82924
#
# grouped leads because cost tracks JSON bytes parsed (20.9 KB vs select's
# 14.3 KB), not case count (69 vs 75). Clang's DEFAULT step limit is 1048576
# (verified: with no flag and with -fconstexpr-steps=1048576 the same probe
# breaks at an identical loop count; 4194304 moves it 4x), i.e. ~3.5x the worst
# slice -- so the flag bought nothing and setting it to 1048576 would only have
# been the default spelled out.
#
# The flag was target-wide, but only these four slices were measured: the other
# ~40 TUs in storm_tests (reflection-heavy consteval in crud/, query/, schema/)
# never had a measured budget, and are covered by the default on the evidence of
# a clean full rebuild. So a step-limit failure elsewhere in this target is a new
# data point, not a regression against a known budget -- measure that TU the same
# way before reaching for the flag again.
#
# The fences matter: cmakelang runs at its defaults (no .cmake-format config in
# the repo) and reflows comment markup, which is what flattened this table into
# two unreadable lines when #582 first landed.
# cmake-format: on

# Treat warnings as errors on Storm-owned test code (issue #317). Third-party
# gtest/gmock headers are silenced via SYSTEM includes in cmake/tests.cmake.
target_compile_options(${PROJECT_NAME} PRIVATE -Werror)
# Note: Custom libc++ linking now handled by global add_link_options() in main
# CMakeLists.txt

add_dependencies(${PROJECT_NAME} generate_test_json)

add_subdirectory(mock_sqlite)
add_subdirectory(mock_libpq)
