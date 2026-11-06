# Coverage targets — must be included after test targets are defined (after
# tests.cmake).
#
# Combines data from two test binaries: - storm_tests:             main tests
# (real SQLite/PostgreSQL, incl. the SQLite error paths driven by the fake
# sqlite3 API policy, #784) - storm_orm_pg_mock_tests: PG mock tests via
# LD_PRELOAD (PG error paths)
if(ENABLE_COVERAGE AND ENABLE_TESTS)

  # The raw-profile inputs, shared by coverage-merge (profdata merge) and
  # coverage-clean (rm). Kept as a space-joined string so it drops straight into
  # the bash -c command lines below.
  set(COVERAGE_PROFRAW
      "${CMAKE_BINARY_DIR}/batch_*.profraw ${CMAKE_BINARY_DIR}/pg_mock.profraw")

  # The coverage run needs a REAL PostgreSQL: three constexpr
  # transaction-nesting methods on the PG Connection (postgresql_connection.cppm
  # in_transaction / enter_transaction / leave_transaction) plus part of
  # pool.cppm are only instantiated by a live connection — tests/mock_libpq does
  # not reach them. Without a server those lines are uncovered and the gate
  # fails at ~99.8-99.9%.
  #
  # This used to hardcode the socket path, which silently OVERRODE any caller
  # value — so the gate passed only on a machine that happened to run PG at
  # /var/run/postgresql, and failed for every contributor and for CI. That is
  # the environment-dependence #528 exists to remove. Now the hardcoded value is
  # a DEFAULT: an already-set STORM_PG_CONNSTR wins, which is how the CI
  # coverage job points these tests at its postgres service container. Local
  # behavior is unchanged when the variable is unset.
  #
  # The default is applied inside coverage-run-batched.sh, NOT here: a
  # `$ENV{...}` read in this file would be evaluated at CONFIGURE time and baked
  # into build.ninja, so a later STORM_PG_CONNSTR change would silently not take
  # effect until the preset was reconfigured. Resolving it in the script keeps
  # it a build-time decision.
  add_custom_target(
    coverage-run-main
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/coverage/coverage-run-batched.sh
            ${CMAKE_BINARY_DIR}
    DEPENDS storm_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running main tests with coverage instrumentation (batched)"
    VERBATIM)

  add_custom_target(
    coverage-run-pg-mock
    COMMAND
      ${CMAKE_COMMAND} -E env
      LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/pg_mock.profraw
      LD_PRELOAD=$<TARGET_FILE:mock_libpq>
      $<TARGET_FILE:storm_orm_pg_mock_tests>
    DEPENDS storm_orm_pg_mock_tests mock_libpq
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running PG mock tests with coverage instrumentation (LD_PRELOAD)"
    VERBATIM)

  add_custom_target(
    coverage-run
    DEPENDS coverage-run-main coverage-run-pg-mock
    COMMENT "All coverage tests completed")

  add_custom_target(
    coverage-merge
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_OUTPUT_DIR}
    COMMAND
      bash -c
      "${LLVM_PROFDATA} merge -sparse ${COVERAGE_PROFRAW} -o ${COVERAGE_OUTPUT_DIR}/coverage.profdata"
    DEPENDS coverage-run
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Merging coverage data from batched main tests and PG mock tests"
    VERBATIM)

  # Single source of truth for excluded path fragments. These are filtered out
  # at the llvm-cov export stage (coverage-lcov) — it scans the storm_tests and
  # the PG mock binary, so vendored/build paths and the mock shared object show
  # up there and must be dropped. By the time the lcov stage runs, the exported
  # tracefile already contains src/ paths only, so it needs no path globs (the
  # previous "*/third_party/*" etc. were dead — lcov reported them as unused).
  # The one src/ file that still needs removing is the vendored generator,
  # handled separately below.
  set(COVERAGE_EXCLUDE_PATHS third_party googletest build mock_libpq)
  list(JOIN COVERAGE_EXCLUDE_PATHS "|" COVERAGE_EXCLUDE_REGEX)

  add_custom_target(
    coverage-lcov
    COMMAND
      ${LLVM_COV} export $<TARGET_FILE:storm_tests>
      -object=$<TARGET_FILE:storm_orm_pg_mock_tests>
      -instr-profile=${COVERAGE_OUTPUT_DIR}/coverage.profdata -format=lcov
      -ignore-filename-regex="${COVERAGE_EXCLUDE_REGEX}" ${CMAKE_SOURCE_DIR}/src
      > ${COVERAGE_OUTPUT_DIR}/coverage.lcov
    DEPENDS coverage-merge
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT
      "Exporting coverage data in LCOV format to ${COVERAGE_OUTPUT_DIR}/coverage.lcov"
    VERBATIM)

  set(LCOV_C_EXTENSIONS "c,h,i,C,H,I,icc,cpp,cc,cxx,hh,hpp,hxx,cppm")
  # Branch coverage + treat .cppm as C-family — shared by coverage and
  # coverage-html.
  set(LCOV_RC --rc branch_coverage=1 --rc
              c_file_extensions=${LCOV_C_EXTENSIONS})
  find_program(LCOV_TOOL lcov)
  find_program(GENHTML_TOOL genhtml)

  # Both tools ship in the same lcov package, so they share one install hint.
  foreach(tool LCOV_TOOL GENHTML_TOOL)
    if(NOT ${tool})
      message(
        FATAL_ERROR
          "${tool} not found but required for coverage builds.\n"
          "  Manjaro/Arch: sudo pacman -S lcov\n"
          "  Ubuntu/Debian: sudo apt install lcov")
    endif()
    message(STATUS "${tool} found: ${${tool}}")
  endforeach()

  add_custom_target(
    coverage
    COMMAND
      ${LCOV_TOOL} ${LCOV_RC} --ignore-errors
      unused,deprecated,unsupported,inconsistent,range --filter
      region,branch_region --remove ${COVERAGE_OUTPUT_DIR}/coverage.lcov
      "*/src/orm/generator.cppm" --output-file
      ${COVERAGE_OUTPUT_DIR}/coverage-filtered.lcov
    COMMAND
      ${LCOV_TOOL} --rc branch_coverage=1 --ignore-errors
      deprecated,inconsistent --summary
      ${COVERAGE_OUTPUT_DIR}/coverage-filtered.lcov
    DEPENDS coverage-lcov
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Filtering coverage with LCOV_EXCL markers"
    VERBATIM)

  # NOTE: the `coverage` CI job (.github/workflows/ci.yml) invokes genhtml
  # directly with these same flags instead of building this target, because this
  # target DEPENDS on `coverage` and would re-run the whole instrumented test
  # suite a second time just to render a report. Keep the flag list below in
  # sync with that step if it changes.
  add_custom_target(
    coverage-html
    COMMAND
      ${GENHTML_TOOL} ${LCOV_RC} --ignore-errors deprecated,range,inconsistent
      --legend --title "Storm ORM Coverage (filtered)" --output-directory
      ${COVERAGE_OUTPUT_DIR}/html-filtered
      ${COVERAGE_OUTPUT_DIR}/coverage-filtered.lcov
    DEPENDS coverage
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT
      "Generating filtered HTML coverage report in ${COVERAGE_OUTPUT_DIR}/html-filtered"
    VERBATIM)

  add_custom_target(
    coverage-clean
    COMMAND bash -c "rm -rf ${COVERAGE_OUTPUT_DIR} ${COVERAGE_PROFRAW}"
    COMMENT "Cleaning coverage data"
    VERBATIM)

  message(STATUS "Coverage targets available:")
  message(STATUS "  coverage       - Run tests + show filtered text summary")
  message(STATUS "  coverage-html  - Run tests + generate filtered HTML report")
  message(STATUS "  coverage-clean - Clean all coverage data")
endif()
