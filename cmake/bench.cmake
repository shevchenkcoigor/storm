if(ENABLE_BENCH)
  message(STATUS "Benchmarking is enabled")

  cpmaddpackage(
    NAME
    benchmark
    GITHUB_REPOSITORY
    google/benchmark
    GIT_TAG
    v1.9.5
    GIT_SHALLOW
    FALSE
    VERSION
    1.9.5
    OPTIONS
    "BENCHMARK_ENABLE_TESTING OFF"
    "BENCHMARK_ENABLE_INSTALL OFF"
    "BENCHMARK_ENABLE_WERROR OFF")

  add_subdirectory(benchmarks)
else()
  message(STATUS "Benchmarking is disabled")
endif()
