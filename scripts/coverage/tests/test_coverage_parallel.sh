#!/bin/bash
# Tests for scripts/coverage/coverage-run-batched.sh parallelism (issue #268).
#
# Strategy: stub a fake $BUILD_DIR with a fake test binary that:
#   - responds to --gtest_list_tests with a known suite list
#   - on each suite run, sleeps briefly and records its PID + start/end time
# Then we can compare wall-clock and overlap between serial and parallel modes.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/coverage/coverage-run-batched.sh"

PASS=0
FAIL=0
FAILED_TESTS=()

fail() {
    local msg="$1" tag="$2"
    echo "  FAIL: $msg"
    FAIL=$((FAIL+1))
    FAILED_TESTS+=("$tag")
    return 0
}

pass() {
    local msg="$1"
    echo "  PASS: $msg"
    PASS=$((PASS+1))
    return 0
}

# Build a fake build dir with a fake gtest binary.
make_fake_build_dir() {
    local dir="$1" suite_count="${2:-8}" sleep_ms="${3:-200}"
    mkdir -p "$dir/tests"
    local bin="$dir/tests/storm_tests"

    # The fake binary lists N suites and on filter-run, sleeps and logs.
    cat > "$bin" <<EOF
#!/bin/bash
LOG_DIR="\${FAKE_LOG_DIR:-$dir}"
mkdir -p "\$LOG_DIR"
if [[ "\$1" == "--gtest_list_tests" ]]; then
    for i in \$(seq 1 $suite_count); do
        echo "FakeSuite\${i}."
        echo "  TestA"
    done
    exit 0
fi
# Parse --gtest_filter=Name*
filter=""
for arg in "\$@"; do
    [[ "\$arg" == --gtest_filter=* ]] && filter="\${arg#--gtest_filter=}"
done
start=\$(date +%s%N)
# Sleep $sleep_ms ms
sleep $(awk "BEGIN { print $sleep_ms / 1000 }")
end=\$(date +%s%N)
# Write a non-empty profraw file (the real script removes empties)
if [[ -n "\${LLVM_PROFILE_FILE:-}" ]]; then
    echo "fake-profraw-data" > "\$LLVM_PROFILE_FILE"
fi
echo "\$filter \$\$ \$start \$end" >> "\$LOG_DIR/runs.log"
exit 0
EOF
    chmod +x "$bin"
    return 0
}

# Returns 0 if the recorded runs in $1/runs.log show overlap (parallel).
has_overlap() {
    local log="$1"
    # Sort by start time, then check whether any run's end > next run's start
    awk '{print $3, $4}' "$log" | sort -n | awk '
        NR==1 { prev_end=$2; next }
        { if ($1 < prev_end) { found=1 } ; if ($2 > prev_end) prev_end=$2 }
        END { exit !found }
    '
    return $?
}

# Returns the wall-clock duration (ns) covering all runs in $1/runs.log.
wall_ns() {
    local log="$1"
    awk '{print $3; print $4}' "$log" | sort -n | awk 'NR==1{min=$1} {max=$1} END{print max-min}'
    return 0
}

echo "=== test_coverage_parallel.sh ==="

# ----- Test 1: default behavior is parallel on multi-core machines -----
T1=$(mktemp -d)
make_fake_build_dir "$T1" 8 300
FAKE_LOG_DIR="$T1" bash "$SCRIPT" "$T1" > "$T1/stdout.log" 2>&1
if [[ -f "$T1/runs.log" ]] && [[ $(wc -l < "$T1/runs.log") -eq 8 ]]; then
    if has_overlap "$T1/runs.log"; then
        pass "Test 1: default mode runs batches in parallel"
    else
        fail "Test 1: default mode should run batches in parallel (no overlap detected in $T1/runs.log)" "default-parallel"
    fi
else
    fail "Test 1: expected 8 runs.log entries, got $(wc -l < "$T1/runs.log" 2>/dev/null || echo 0)" "default-parallel"
fi
rm -rf "$T1"

# ----- Test 2: COVERAGE_JOBS=1 forces strictly serial -----
T2=$(mktemp -d)
make_fake_build_dir "$T2" 6 150
FAKE_LOG_DIR="$T2" COVERAGE_JOBS=1 bash "$SCRIPT" "$T2" > "$T2/stdout.log" 2>&1
if [[ -f "$T2/runs.log" ]] && [[ $(wc -l < "$T2/runs.log") -eq 6 ]]; then
    if has_overlap "$T2/runs.log"; then
        fail "Test 2: COVERAGE_JOBS=1 should run serially (overlap detected)" "serial-opt-out"
    else
        pass "Test 2: COVERAGE_JOBS=1 runs batches strictly serially"
    fi
else
    fail "Test 2: expected 6 runs.log entries, got $(wc -l < "$T2/runs.log" 2>/dev/null || echo 0)" "serial-opt-out"
fi
rm -rf "$T2"

# ----- Test 3: parallel mode is meaningfully faster than serial -----
T3a=$(mktemp -d); T3b=$(mktemp -d)
make_fake_build_dir "$T3a" 8 250
make_fake_build_dir "$T3b" 8 250
FAKE_LOG_DIR="$T3a" COVERAGE_JOBS=1 bash "$SCRIPT" "$T3a" > /dev/null 2>&1
FAKE_LOG_DIR="$T3b" COVERAGE_JOBS=8 bash "$SCRIPT" "$T3b" > /dev/null 2>&1
if [[ -f "$T3a/runs.log" ]] && [[ -f "$T3b/runs.log" ]]; then
    serial_ns=$(wall_ns "$T3a/runs.log")
    parallel_ns=$(wall_ns "$T3b/runs.log")
    # Expect parallel to be at least 2x faster than serial on 8 suites x 250ms with 8 jobs
    if (( parallel_ns * 2 < serial_ns )); then
        pass "Test 3: parallel (${parallel_ns}ns) is >2x faster than serial (${serial_ns}ns)"
    else
        fail "Test 3: parallel (${parallel_ns}ns) not >2x faster than serial (${serial_ns}ns)" "parallel-faster"
    fi
else
    fail "Test 3: missing runs.log" "parallel-faster"
fi
rm -rf "$T3a" "$T3b"

# ----- Test 4: profraw files still produced under parallel run -----
T4=$(mktemp -d)
make_fake_build_dir "$T4" 5 100
FAKE_LOG_DIR="$T4" bash "$SCRIPT" "$T4" > /dev/null 2>&1
profraw_count=$(ls "$T4"/batch_*.profraw 2>/dev/null | wc -l)
if [[ "$profraw_count" -eq 5 ]]; then
    pass "Test 4: all 5 batch_*.profraw files produced in parallel mode"
else
    fail "Test 4: expected 5 batch_*.profraw, got $profraw_count" "profraw-output"
fi
rm -rf "$T4"

# ----- Test 5: explicit COVERAGE_JOBS value is honored (cap concurrency) -----
T5=$(mktemp -d)
make_fake_build_dir "$T5" 12 200
FAKE_LOG_DIR="$T5" COVERAGE_JOBS=3 bash "$SCRIPT" "$T5" > /dev/null 2>&1
if [[ -f "$T5/runs.log" ]]; then
    # Count maximum simultaneous batches using a sweep-line over starts/ends.
    max_concurrent=$(awk '{print $3" S"; print $4" E"}' "$T5/runs.log" | sort -n -k1,1 | \
        awk '{if ($2=="S") {c++; if (c>m) m=c} else c--} END{print m}')
    if [[ "$max_concurrent" -le 3 ]] && [[ "$max_concurrent" -ge 2 ]]; then
        pass "Test 5: COVERAGE_JOBS=3 caps concurrency at 3 (observed max=$max_concurrent)"
    else
        fail "Test 5: COVERAGE_JOBS=3 expected max concurrency 2..3, got $max_concurrent" "concurrency-cap"
    fi
else
    fail "Test 5: missing runs.log" "concurrency-cap"
fi
rm -rf "$T5"

# ----- Test 6: script still aborts when test binary missing -----
T6=$(mktemp -d)
mkdir -p "$T6/tests"  # no binary
if bash "$SCRIPT" "$T6" > "$T6/stdout.log" 2>&1; then
    fail "Test 6: script should fail when test binary missing" "missing-binary"
else
    if grep -q "Test binary not found" "$T6/stdout.log"; then
        pass "Test 6: missing binary error preserved"
    else
        fail "Test 6: wrong error message" "missing-binary"
    fi
fi
rm -rf "$T6"

echo ""
echo "=== Result: $PASS passed, $FAIL failed ==="
if [[ "$FAIL" -gt 0 ]]; then
    echo "Failed tests: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
