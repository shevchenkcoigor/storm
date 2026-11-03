#!/bin/bash
# Tests for scripts/ci/detect-changes.sh.
#
# This script classifies a changed-file list into HAS_*_CHANGES flags that
# both commit.sh (pre-commit smart skips) and the `changes` job in
# .github/workflows/ci.yml (skipping the test/coverage/clang-p2996-host-layout
# jobs on a PR that doesn't touch src/tests/cmake) rely on to decide whether
# to run anything at all. A silent misclassification here is invisible in a
# diff and would either skip real coverage or force every PR through the
# full matrix — table-driven scenarios below pin the exact flag output for
# each file class.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DETECTOR="$REPO_ROOT/scripts/ci/detect-changes.sh"

# Expected KEY=value strings, named once so scenarios below don't repeat the
# same literal (SonarCloud S1192).
SRC_TRUE="HAS_SRC_CHANGES=true";     SRC_FALSE="HAS_SRC_CHANGES=false"
TEST_TRUE="HAS_TEST_CHANGES=true";   TEST_FALSE="HAS_TEST_CHANGES=false"
CPP_TRUE="HAS_CPP_CHANGES=true";     CPP_FALSE="HAS_CPP_CHANGES=false"
CMAKE_TRUE="HAS_CMAKE_CHANGES=true"; CMAKE_FALSE="HAS_CMAKE_CHANGES=false"
BENCH_TRUE="HAS_BENCH_CHANGES=true"; BENCH_FALSE="HAS_BENCH_CHANGES=false"

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

# Runs the detector over the given files (one arg per file) and asserts the
# resulting flags exactly match the expected `KEY=value` pairs (also one arg
# per pair). Order-independent; every flag must be present in the output.
assert_flags() {
    local what="$1"
    shift
    local -a files=()
    local -a expected=()
    local mode="files"
    for arg in "$@"; do
        if [[ "$arg" == "--expect" ]]; then
            mode="expect"
            continue
        fi
        if [[ "$mode" == "files" ]]; then
            files+=("$arg")
        else
            expected+=("$arg")
        fi
    done

    local out
    out="$(printf '%s\n' "${files[@]}" | "$DETECTOR")"
    local rc=$?

    if [[ $rc -ne 0 ]]; then
        fail "$what — detector exited $rc. Output: $out"
        return 0
    fi

    local ok=true
    for kv in "${expected[@]}"; do
        if ! grep -qxF "$kv" <<< "$out"; then
            ok=false
            break
        fi
    done

    if [[ "$ok" == true ]]; then
        pass "$what"
    else
        fail "$what — expected [${expected[*]}], got: $out"
    fi
    return 0
}

# --- Scenarios ---

scenario_src_file_sets_src_and_cpp() {
    assert_flags "src/foo.cppm sets HAS_SRC_CHANGES + HAS_CPP_CHANGES" \
        "src/foo.cppm" \
        --expect "$SRC_TRUE" "$CPP_TRUE" "$TEST_FALSE" "$CMAKE_FALSE" "$BENCH_FALSE"
    return 0
}

scenario_test_file_sets_test_and_cpp() {
    assert_flags "tests/test_foo.cpp sets HAS_TEST_CHANGES + HAS_CPP_CHANGES" \
        "tests/test_foo.cpp" \
        --expect "$TEST_TRUE" "$CPP_TRUE" "$SRC_FALSE"
    return 0
}

scenario_header_outside_src_still_sets_cpp() {
    # A shared header like include/storm/storm.h isn't under src/ or tests/
    # but is still compiled — HAS_CPP_CHANGES must catch it even though
    # HAS_SRC/TEST_CHANGES don't apply.
    assert_flags "include/storm/storm.h sets HAS_CPP_CHANGES only" \
        "include/storm/storm.h" \
        --expect "$CPP_TRUE" "$SRC_FALSE" "$TEST_FALSE"
    return 0
}

scenario_cmake_file_sets_cmake_only() {
    assert_flags "cmake/foo.cmake sets HAS_CMAKE_CHANGES only" \
        "cmake/foo.cmake" \
        --expect "$CMAKE_TRUE" "$CPP_FALSE" "$SRC_FALSE"
    return 0
}

scenario_cmakelists_sets_cmake() {
    assert_flags "CMakeLists.txt sets HAS_CMAKE_CHANGES" \
        "src/CMakeLists.txt" \
        --expect "$CMAKE_TRUE"
    return 0
}

scenario_bench_file_sets_bench_only() {
    assert_flags "benchmarks/foo.cpp sets HAS_BENCH_CHANGES + HAS_CPP_CHANGES" \
        "benchmarks/foo.cpp" \
        --expect "$BENCH_TRUE" "$CPP_TRUE" "$SRC_FALSE"
    return 0
}

scenario_docs_only_sets_nothing() {
    assert_flags "docs/README.md sets no flags" \
        "docs/README.md" \
        --expect "$SRC_FALSE" "$TEST_FALSE" "$CPP_FALSE" "$CMAKE_FALSE" "$BENCH_FALSE"
    return 0
}

scenario_empty_input_sets_nothing() {
    local out rc
    out="$(printf '' | "$DETECTOR")"
    rc=$?
    if [[ $rc -eq 0 ]] && grep -qxF "$SRC_FALSE" <<< "$out"; then
        pass "empty input exits 0 with every flag false"
    else
        fail "empty input — rc=$rc, output: $out"
    fi
    return 0
}

scenario_mixed_files_ors_flags() {
    assert_flags "docs + src together still sets HAS_SRC_CHANGES" \
        "docs/README.md" "src/foo.cppm" \
        --expect "$SRC_TRUE" "$CPP_TRUE"
    return 0
}

SCENARIOS=(
    src_file_sets_src_and_cpp
    test_file_sets_test_and_cpp
    header_outside_src_still_sets_cpp
    cmake_file_sets_cmake_only
    cmakelists_sets_cmake
    bench_file_sets_bench_only
    docs_only_sets_nothing
    empty_input_sets_nothing
    mixed_files_ors_flags
)

echo "Testing detect-changes.sh"
echo "================================================"

if [[ ! -x "$DETECTOR" ]]; then
    echo "FATAL: $DETECTOR is missing or not executable"
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
