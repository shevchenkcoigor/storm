project(storm_benchmarks LANGUAGES CXX)

message(STATUS "Configuring benchmarks")

# ============================================================================
# YAML to JSON conversion for benchmark tests
# ============================================================================
# benchmark_tests.yaml is the human-friendly source of truth
# benchmark_tests.json is auto-generated for C++ #embed parsing
set(YAML_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_tests.yaml")
set(JSON_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_tests.json")
set(CONVERTER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/yaml_to_json.py")

find_package(
  Python3
  COMPONENTS Interpreter
  REQUIRED)
storm_require_pyyaml()

add_custom_command(
  OUTPUT "${JSON_OUTPUT}"
  COMMAND ${Python3_EXECUTABLE} "${CONVERTER_SCRIPT}" "${YAML_SOURCE}"
          "${JSON_OUTPUT}"
  DEPENDS "${YAML_SOURCE}" "${CONVERTER_SCRIPT}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Converting benchmark_tests.yaml to JSON..."
  VERBATIM)

add_custom_target(generate_benchmark_json DEPENDS "${JSON_OUTPUT}")

# ============================================================================
# storm_bench — Google Benchmark entry point (Issue #235)
# ============================================================================
# Phase 1 keeps the benchmark-local .cppm module FILE_SET intact so the
# parser/registry/sizes/base/raw/schema modules continue to build (Phase 2+ will
# consume them from new fixtures). main.cpp itself imports nothing yet.
file(GLOB STORM_BENCH_MODULE_UNITS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm")

add_executable(
  storm_bench
  "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/register.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dashboard/reporter.cpp")
target_sources(storm_bench PRIVATE FILE_SET cxx_modules TYPE CXX_MODULES FILES
                                   ${STORM_BENCH_MODULE_UNITS})

# register.cpp pulls the benchmark corpus in through `#embed`
# (benchmarks/benchmark_tests.hpp), which the dependency scanner cannot see:
# clang omits embedded files from the depfile, so ninja recorded no edge from
# the object to the JSON and editing benchmark_tests.yaml regenerated the JSON
# while leaving register.cpp.o stale — feeding a stale corpus to the regression
# gate. Same fix, and same rationale for set_property over
# set_source_files_properties, as the tests/yaml TUs in tests/CMakeLists.txt.
set_property(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/register.cpp"
  APPEND
  PROPERTY OBJECT_DEPENDS "${JSON_OUTPUT}")
# import std; migration (issue #326): the bench .cppm module units do `import
# std;` directly, so this target must consume the std named module too.
set_target_properties(storm_bench PROPERTIES CXX_MODULE_STD ON)
# Bypass benchmark::benchmark's INTERFACE_* properties (we link the static
# archive directly) so the bench-only flags stay confined to main.cpp via
# per-source properties below.
#
# Why the split — main.cpp owns <benchmark/benchmark.h>, register.cpp owns
# `import storm_*`. Putting both in one TU diverges its PCM-cache hash from
# libstorm's and produces two _Builtin_stddef.pcm copies under the cache,
# tripping a "module defined in both" fatal at any `import storm` site
# (project_pcm_hash_divergence). The split keeps main.cpp out of the import
# graph entirely (it sees only the run-loop trampoline vector via
# bench_register.h, a POD-only header) and register.cpp without the bench
# include path so its hash matches libstorm's.
target_link_libraries(storm_bench PRIVATE storm pthread
                                          $<TARGET_FILE:benchmark::benchmark>)
add_dependencies(storm_bench benchmark::benchmark)
link_sqlite(storm_bench)
link_postgresql(storm_bench)
apply_clang_flags(storm_bench)
# Treat warnings as errors on Storm-owned benchmark code (issue #317). The
# Google Benchmark headers reach only main.cpp/reporter.cpp and arrive via
# -isystem (see _benchmark_includes below), so their warnings are not promoted.
target_compile_options(storm_bench PRIVATE -Werror)
target_include_directories(
  storm_bench PRIVATE "${CMAKE_SOURCE_DIR}/src"
                      "${CMAKE_SOURCE_DIR}/third_party")

get_target_property(_benchmark_includes benchmark::benchmark
                    INTERFACE_INCLUDE_DIRECTORIES)
set_property(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/dashboard/reporter.cpp"
  APPEND
  PROPERTY INCLUDE_DIRECTORIES "${_benchmark_includes}")
set_property(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/dashboard/reporter.cpp"
  APPEND
  PROPERTY COMPILE_DEFINITIONS "BENCHMARK_STATIC_DEFINE")

add_dependencies(storm_bench generate_benchmark_json)

message(STATUS "Benchmark executable: storm_bench")

# ============================================================================
# storm_anchors — raw SQLite spot-check binary (Issue #235 — Phase 4)
# ============================================================================
# Sparse, intentional raw-SQLite anchors used at release time to re-verify the
# "96–108% of raw SQLite" efficiency claim. Lives in its own binary so anchor
# results are never confused with per-Storm-benchmark runs and so its stream
# stays out of `storm_bench` output.
#
# Anchors do NOT import storm and do NOT depend on any benchmark .cppm module.
# Single TU = anchors_raw.cpp, owns BENCHMARK_MAIN itself.
#
# Kept as a plain .cpp (not .cppm) — converting to a module unit segfaults
# clang-p2996 inside ASTWriter::GenerateNameLookupTable when serializing the BMI
# for the BENCHMARK(...) macro's static-init globals. When STORM_BENCH_SOCKET is
# set, anchors_raw.cpp streams its raw subset over the dashboard wire via the
# shared streaming reporter (Issue #74). Both TUs are plain .cpp and never
# import storm, so linking reporter.cpp here is safe from PCM-hash divergence.
add_executable(
  storm_anchors "${CMAKE_CURRENT_SOURCE_DIR}/anchors_raw.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/dashboard/reporter.cpp")
target_link_libraries(storm_anchors PRIVATE pthread
                                            $<TARGET_FILE:benchmark::benchmark>)
add_dependencies(storm_anchors benchmark::benchmark)
link_sqlite(storm_anchors)
apply_clang_flags(storm_anchors)
# Treat warnings as errors on Storm-owned benchmark code (issue #317).
target_compile_options(storm_anchors PRIVATE -Werror)
# anchors_raw.cpp does #include "dashboard/reporter.h" (path relative to
# benchmarks/), so the benchmarks/ dir must be on its include path. third_party
# carries plf_hive (anchors_raw.cpp materializes SELECT rows into a plf::hive to
# mirror Storm's result container — fairness rule, #68).
target_include_directories(
  storm_anchors PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                        "${CMAKE_SOURCE_DIR}/third_party")

# anchors_raw.cpp needs the benchmark include dirs + BENCHMARK_STATIC_DEFINE.
# dashboard/reporter.cpp's source-scoped properties are already set by the
# storm_bench block above (source-file properties are directory-scoped and apply
# to every target using that file), so it is not repeated here.
set_property(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/anchors_raw.cpp"
  APPEND
  PROPERTY INCLUDE_DIRECTORIES "${_benchmark_includes}")
set_property(
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/anchors_raw.cpp"
  APPEND
  PROPERTY COMPILE_DEFINITIONS "BENCHMARK_STATIC_DEFINE")

message(STATUS "Benchmark executable: storm_anchors")

# ============================================================================
# storm_bench_dashboard — Issue #247
# ============================================================================
# Phase 1 skeleton lives in its own subdirectory so the dashboard's reflection
# schema (bench_dashboard namespace) and Atlas migration targets stay isolated
# from the gbench-side files in benchmarks/.
add_subdirectory(dashboard)
