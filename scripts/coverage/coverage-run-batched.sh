#!/bin/bash
# Run coverage tests in batches, one process per gtest suite, each producing
# a .profraw file that llvm-profdata merges later.
#
# Concurrency: batches run in parallel by default (one per suite, each emits an
# independent ${BUILD_DIR}/batch_${name}.profraw). Set COVERAGE_JOBS=N to cap
# concurrency (default: nproc --ignore=2). COVERAGE_JOBS=1 forces strictly serial
# — useful on shared hardware or when debugging a flaky suite. See issue #268.
#
# Historical note: this script was originally a workaround for what looked like
# a clang coverage-instrumentation segfault / hang. Issue #269 traced the hang
# to a Storm bug — a use-after-free of a thread-local `Statement*` cache in
# `AggregateStatement::execute_simple` — which manifested as an infinite loop
# under coverage instrumentation. The fix landed; this batched layout is kept
# because per-suite isolation is still the right shape (and #176 / #268 still
# matter for cross-module template coverage).
#
# IMPORTANT: Suites must NOT be split into individual test runs.
# Per-test profraw files lose cross-module template coverage data because
# the profiling counters for template instantiations across module boundaries
# require enough test volume within a single process to be properly recorded.
# See: https://github.com/spiritEcosse/storm/issues/176
# See: https://github.com/spiritEcosse/storm/issues/269

set -e

BUILD_DIR="${1:-.}"
TEST_BIN="${BUILD_DIR}/tests/storm_tests"

# The coverage gate requires a REAL PostgreSQL. Three constexpr transaction
# methods on the PG Connection (postgresql_connection.cppm in_transaction /
# enter_transaction / leave_transaction) plus part of pool.cppm are instantiated
# only by a live connection — tests/mock_libpq never reaches them — so without a
# server the run lands at ~99.8-99.9% and the 100% gate fails.
#
# Default to the local unix socket, but let an already-exported value win: that
# is how the CI coverage job points these tests at its postgres service
# container. This default previously lived in cmake/coverage-targets.cmake as an
# unconditional override, which meant the gate passed only on a machine that
# happened to run PG at this socket and failed for everyone else (#528).
#
# `${VAR+set}` tests DECLARATION, not emptiness: an explicitly-set value is
# passed through untouched (including an empty one), and only a genuinely unset
# variable gets the default.
#
# Note an empty value is NOT a way to disable PostgreSQL. libpq reads "" as "use
# the PG* environment defaults", so PQconnectdb("") succeeds on a machine with
# PGHOST/PGUSER exported (verified). backend_available() only rejects a NULL
# pointer, so an empty string reaches libpq. To genuinely force SQLite-only,
# clear the libpq defaults too:
#   env -u STORM_PG_CONNSTR -u PGHOST -u PGUSER -u PGDATABASE ...
# — which fails the 100% gate by design, since the PG-only lines go uncovered.
#
# The default's socket directory depends on the host OS (Linux /var/run/postgresql,
# macOS /tmp) and is owned by pg-default-connstr.sh so commit.sh agrees with it.
if [[ -z "${STORM_PG_CONNSTR+set}" ]]; then
    # shellcheck source=pg-default-connstr.sh
    source "$(dirname "${BASH_SOURCE[0]}")/pg-default-connstr.sh"
    # Separate statement: `export x="$(f)"` would mask f's failure under set -e.
    default_connstr="$(storm_pg_default_connstr)"
    export STORM_PG_CONNSTR="$default_connstr"
fi

# Fail-loud diagnostic: without a reachable server the PG suites SKIP silently
# (test output goes to /dev/null below, and a skip is not a batch failure), so
# the only symptom would be ~11 uncovered lines in files the contributor never
# touched. Say so up front instead.
if command -v pg_isready > /dev/null 2>&1 \
   && ! pg_isready -d "$STORM_PG_CONNSTR" > /dev/null 2>&1; then
    echo "  WARN: PostgreSQL unreachable at [$STORM_PG_CONNSTR]" >&2
    echo "        PG tests will SKIP; the 100% line-coverage gate CANNOT be met" >&2
    echo "        without a live server (expect ~99.8-99.9%, functions ~48.7%)." >&2
fi

if [[ ! -x "$TEST_BIN" ]]; then
    echo "ERROR: Test binary not found: $TEST_BIN" >&2
    exit 1
fi

# Concurrency: default to (nproc - 2) leaving headroom for the OS / IDE.
# Users can override (COVERAGE_JOBS=1 for serial, =N for explicit cap).
DEFAULT_JOBS=$(nproc --ignore=2 2>/dev/null || echo 1)
JOBS="${COVERAGE_JOBS:-$DEFAULT_JOBS}"
[[ "$JOBS" -lt 1 ]] && JOBS=1

# Clean old batch profraw files
rm -f "${BUILD_DIR}"/batch_*.profraw

# Discover all test suites
TEST_LIST=$("$TEST_BIN" --gtest_list_tests 2>/dev/null)

# Collect (suite, safe_name) pairs into a temp file for xargs.
WORK_FILE=$(mktemp)
trap 'rm -f "$WORK_FILE"' EXIT

while IFS= read -r line; do
    if [[ "$line" =~ ^[A-Za-z] ]]; then
        suite="${line%% *}"          # strip "# TypeParam = ..." comment
        safe="${suite//\//_}"        # safe filename: replace / with _
        safe="${safe%.}"
        printf '%s\t%s\n' "$suite" "$safe" >> "$WORK_FILE"
    fi
done <<< "$TEST_LIST"

TOTAL=$(wc -l < "$WORK_FILE")
[[ "$TOTAL" -eq 0 ]] && { echo "  No suites discovered"; exit 0; }

# Run one batch (called per worker by xargs). Writes a `.failed` marker file
# next to the profraw so the parent can count failures after the parallel run.
export BUILD_DIR TEST_BIN
run_one() {
    local suite="$1" safe="$2"
    local profraw="${BUILD_DIR}/batch_${safe}.profraw"
    if ! LLVM_PROFILE_FILE="$profraw" "$TEST_BIN" --gtest_filter="${suite}*" > /dev/null 2>&1; then
        echo "  WARN: batch $safe crashed (coverage data may be partial)"
        touch "${BUILD_DIR}/batch_${safe}.failed"
    fi
    # Remove empty profraw files from crashed runs (llvm-profdata merge rejects them)
    if [[ -f "$profraw" && ! -s "$profraw" ]]; then
        rm -f "$profraw"
    fi
}
export -f run_one

# Clear any stale failure markers from prior runs
rm -f "${BUILD_DIR}"/batch_*.failed

# Drive xargs with NUL-separated arg pairs to be safe on weird suite names.
# Each work-file line is "suite\tsafe"; convert to NUL-pairs for xargs -0 -n2.
tr '\t\n' '\0\0' < "$WORK_FILE" | \
    xargs -0 -n2 -P "$JOBS" bash -c 'run_one "$1" "$2"' _ || true

FAILED=$(ls "${BUILD_DIR}"/batch_*.failed 2>/dev/null | wc -l)
rm -f "${BUILD_DIR}"/batch_*.failed

echo "  Coverage batches: $((TOTAL - FAILED))/$TOTAL succeeded (jobs=$JOBS)"
if [[ $FAILED -gt 0 ]]; then
    echo "  ($FAILED batches crashed — partial coverage collected)"
fi
