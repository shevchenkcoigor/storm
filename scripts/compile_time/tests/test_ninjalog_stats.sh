#!/bin/bash
# Tests for scripts/compile_time/ninjalog_stats.py.
#
# That script regenerates the whole-build tables in
# docs/internals/performance/COMPILE_TIME.md, and the two rules it implements
# are exactly the ones that are invisible when wrong: a missed dedup silently
# DOUBLES every module edge (ninja logs one line per output, and a module
# compile emits both a .pcm and a .o from one invocation), and counting the
# module-scan edges silently INFLATES the total with work the document's
# baseline excludes. Either would land in the doc as a plausible-looking
# number, so the scenarios below pin both against hand-built logs whose
# correct answer is known by construction.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
STATS="$REPO_ROOT/scripts/compile_time/ninjalog_stats.py"

# Bucket labels the script prints, named once — each is asserted on several
# times below and SonarCloud (S1192) rejects the repeated literal.
BUCKET_STORM="storm library modules"
BUCKET_TESTS="hand-written test TUs"
BUCKET_YAML="yaml corpus TUs"
BUCKET_MOCK="mock test binaries"

PASS=0
FAIL=0
FAILED_TESTS=()
CURRENT_TAG=""

fail() {
    local msg="$1"
    echo "  FAIL: $msg"
    FAIL=$((FAIL+1))
    FAILED_TESTS+=("$CURRENT_TAG")
    return 0
}

pass() {
    local msg="$1"
    echo "  PASS: $msg"
    PASS=$((PASS+1))
    return 0
}

# Fixtures live INSIDE the repository, not in /tmp: the script under test
# refuses a log outside the tree (see scenario_path_confined_to_repo), so a
# /tmp fixture would be rejected before any of the parsing is exercised.
# build/ is already gitignored, so nothing here can be committed by accident.
mkdir -p "$REPO_ROOT/build"
TMPDIR_TEST="$(mktemp -d "$REPO_ROOT/build/ninjalog-test.XXXXXX")"
trap 'rm -rf "$TMPDIR_TEST"' EXIT

# Writes a ninja log from tab-separated "start end output cmdhash" rows on
# stdin, filling in the mtime column the script ignores.
make_log() {
    local path="$1"
    echo "# ninja log v7" > "$path"
    while IFS=$'\t' read -r start end output cmdhash; do
        [[ -z "$start" ]] && continue
        printf '%s\t%s\t%s\t%s\t%s\n' "$start" "$end" "1788894584000000000" "$output" "$cmdhash" >> "$path"
    done
    return 0
}

# Asserts a field of the script's output line for `bucket` equals `expected`.
# field 2 = files, field 3 = sec.
assert_field() {
    local what="$1" log="$2" bucket="$3" field="$4" expected="$5"
    local actual
    actual="$(python3 "$STATS" "$log" | grep -F "$bucket" | head -1 \
              | awk -v f="$field" '{print $(NF-5+f)}')"
    if [[ "$actual" == "$expected" ]]; then
        pass "$what"
    else
        fail "$what — expected '$expected', got '$actual'"
    fi
    return 0
}

# --- Scenario: one module compile logged twice must be counted once ----------
# Same invocation (identical start/end/cmdhash), two outputs. Without the
# dedup this reports 2 files / 20 s instead of 1 file / 10 s.
scenario_module_edge_deduplicated() {
    local log="$TMPDIR_TEST/dedup.ninja_log"
    make_log "$log" <<'ROWS'
0	10000	CMakeFiles/storm.dir/src/orm/queryset.cppm.o	aaaa1111
0	10000	CMakeFiles/storm.dir/src/orm/queryset.pcm	aaaa1111
ROWS
    assert_field "module .o/.pcm pair counts as one file" "$log" "$BUCKET_STORM" 2 "1"
    assert_field "module .o/.pcm pair counts its time once" "$log" "$BUCKET_STORM" 3 "10"
    return 0
}

# --- Scenario: scan edges are excluded from the compile total ---------------
scenario_scan_edges_excluded() {
    local log="$TMPDIR_TEST/scan.ninja_log"
    make_log "$log" <<'ROWS'
0	4000	tests/CMakeFiles/storm_tests.dir/query/test_where.cpp.o	bbbb2222
0	1000	CMakeFiles/storm.dir/src/orm/queryset.cppm.o.ddi	cccc3333
0	1000	tests/CMakeFiles/storm_tests.dir/query/test_where.cpp.o.modmap	dddd4444
ROWS
    local out; out="$(python3 "$STATS" "$log")"
    if grep -q "TOTAL (object compiles) *1 *4" <<< "$out"; then
        pass "scan edges kept out of the object-compile total"
    else
        fail "scan edges leaked into the total. Output: $out"
    fi
    if grep -q "module scan edges (excluded) *2 *2" <<< "$out"; then
        pass "scan edges reported separately, not dropped"
    else
        fail "scan edges not reported. Output: $out"
    fi
    return 0
}

# --- Scenario: bucketing matches the document's rows ------------------------
scenario_buckets_match_doc_rows() {
    local log="$TMPDIR_TEST/buckets.ninja_log"
    make_log "$log" <<'ROWS'
0	1000	tests/CMakeFiles/storm_tests.dir/query/test_where.cpp.o	1111aaaa
0	2000	tests/CMakeFiles/storm_tests.dir/yaml/test_unified_yaml_select.cpp.o	2222bbbb
0	3000	tests/mock_sqlite/CMakeFiles/storm_mock_tests.dir/mock_sqlite3.cpp.o	3333cccc
0	4000	CMakeFiles/storm.dir/src/orm/queryset.cppm.o	4444dddd
ROWS
    assert_field "a yaml/ TU lands in the corpus bucket, not the test bucket" \
        "$log" "$BUCKET_YAML" 2 "1"
    assert_field "a non-yaml storm_tests TU lands in the hand-written bucket" \
        "$log" "$BUCKET_TESTS" 2 "1"
    assert_field "a mock binary TU lands in the mock bucket" \
        "$log" "$BUCKET_MOCK" 2 "1"
    assert_field "a storm module lands in the library bucket" \
        "$log" "$BUCKET_STORM" 2 "1"
    return 0
}

# --- Scenario: near-misses that a loose substring match would swallow -------
# Both were live bugs caught by classifying a real build log: a bare "mock"
# also matches GoogleMock's own library build, and a bare "@synth" also
# matches the *std* module's synthesized BMI. Either one silently pads a
# Storm row with work that is not Storm's.
scenario_foreign_targets_stay_out_of_storm_rows() {
    local log="$TMPDIR_TEST/foreign.ninja_log"
    make_log "$log" <<'ROWS'
0	1000	_deps/googletest-build/googlemock/CMakeFiles/gmock.dir/src/gmock-all.cc.o	ffff6666
0	1000	CMakeFiles/@cmake_cxx_std@synth_0.dir/abcdef123456.bmi	ffff7777
0	1000	CMakeFiles/@cmake_cxx_std.dir/std.cppm.o	ffff8888
ROWS
    local out; out="$(python3 "$STATS" "$log")"
    if grep -q "$BUCKET_MOCK" <<< "$out"; then
        fail "GoogleMock's own build was counted as a Storm mock binary"
    else
        pass "gmock library build stays out of the mock-binaries row"
    fi
    if grep -q "$BUCKET_STORM" <<< "$out"; then
        fail "the std module's synthesized BMI was counted as a Storm module"
    else
        pass "std module (incl. its @synth BMI) stays out of the storm row"
    fi
    return 0
}

# --- Scenario: a log with no compiles is an error, not a zero-row table -----
# An incremental "nothing to do" build produces exactly this, and reporting
# 0 s as if measured would be worse than refusing.
scenario_empty_log_is_an_error() {
    local log="$TMPDIR_TEST/empty.ninja_log"
    make_log "$log" < /dev/null
    if python3 "$STATS" "$log" > /dev/null 2>&1; then
        fail "empty log exited 0 — a measurement of nothing must not look like a result"
    else
        pass "empty log exits non-zero"
    fi
    return 0
}

# --- Scenario: closing the pipe early is not a crash -----------------------
# `ninjalog_stats.py … | head` is the obvious way to use this, and Python's
# default turns the closed pipe into a BrokenPipeError traceback on stderr.
scenario_survives_a_closed_pipe() {
    local log="$TMPDIR_TEST/pipe.ninja_log"
    make_log "$log" <<'ROWS'
0	1000	CMakeFiles/storm.dir/src/orm/queryset.cppm.o	eeee5555
ROWS
    local err
    err="$(python3 "$STATS" "$log" 2>&1 >/dev/null | head -c 400)"
    if [[ -z "$err" ]]; then
        pass "no stderr on a normal run"
    else
        fail "unexpected stderr: $err"
    fi

    err="$( { python3 "$STATS" "$log" 2>&1 >/dev/null | head -1; } 2>&1 )"
    if [[ "$err" != *BrokenPipeError* ]]; then
        pass "piping into head does not print a traceback"
    else
        fail "BrokenPipeError leaked to stderr when the pipe closed early"
    fi
    return 0
}

# --- Scenario: the log path is confined to the repository ------------------
# The path comes from argv. Escaping the tree is a mistake (wrong copy of the
# script for the worktree being measured), not a use case, so it must fail
# with a readable message rather than opening whatever it was handed.
scenario_path_confined_to_repo() {
    local out rc
    out="$(python3 "$STATS" /etc/hostname 2>&1)"; rc=$?
    if [[ $rc -ne 0 && "$out" == *"outside the repository"* ]]; then
        pass "an absolute path outside the tree is refused"
    else
        fail "absolute escape not refused (rc=$rc): $out"
    fi

    out="$(python3 "$STATS" "$REPO_ROOT/build/../../etc/hostname" 2>&1)"; rc=$?
    if [[ $rc -ne 0 && "$out" == *"outside the repository"* ]]; then
        pass "a traversal through .. is refused"
    else
        fail "traversal not refused (rc=$rc): $out"
    fi
    return 0
}

SCENARIOS=(
    module_edge_deduplicated
    scan_edges_excluded
    buckets_match_doc_rows
    empty_log_is_an_error
    survives_a_closed_pipe
    foreign_targets_stay_out_of_storm_rows
    path_confined_to_repo
)

if [[ ! -x "$STATS" ]]; then
    echo "FATAL: $STATS is missing or not executable"
    exit 1
fi

for tag in "${SCENARIOS[@]}"; do
    CURRENT_TAG="$tag"
    echo ""
    echo "Scenario: $tag"
    "scenario_$tag"
done

echo ""
echo "================================================"
echo "Passed: $PASS, Failed: $FAIL"

if [[ $FAIL -gt 0 ]]; then
    echo "Failed scenarios: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
