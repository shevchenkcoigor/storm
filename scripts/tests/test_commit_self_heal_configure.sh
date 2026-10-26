#!/bin/bash
# Tests for commit.sh's self-heal configure (issue #674).
#
# Before a step that needs a build tree, commit.sh configures the tree when it
# is absent (#489): build/release for the clang-tidy prerequisites and the bench
# release build, build/debug for clang-format, cmake-format, tests and coverage.
# Those configures used to run as `cmake --preset … > /dev/null 2>&1` with the
# exit status ignored, so a failed configure surfaced one step later as
# `ninja: error: loading 'build.ninja': No such file or directory` — with the
# real CMake error logged nowhere.
#
# What is asserted, per configure site:
#   - a failed configure fails the step that needed it, and blocks the commit;
#   - the CMake error is in that step's log, and the log path is printed;
#   - the misleading ninja error is not what the log reports.
# And for the warm path: an already-configured tree never runs a configure.
#
# Strategy: run the REAL commit.sh in a throwaway git repo, with `cmake`,
# `ninja` and `pg_isready` stubbed on PATH, so no compiler is needed. The stub
# `cmake --preset P` fails with a recognizable CMake error when P is listed in
# FAIL_PRESETS, and otherwise writes the files a real configure leaves behind.
# The stub `cmake --build` behaves like ninja on an unconfigured tree. Logs go
# to a per-scenario directory via STORM_COMMIT_LOG_DIR. Each scenario is a
# function named scenario_<tag>; the dispatcher owns the temp-dir boilerplate.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# commit.sh runs this test from the pre-commit hook, where git exports
# GIT_INDEX_FILE (and, for some invocations, GIT_DIR). Left set, every `git`
# call below would act on the REAL repository's index instead of the throwaway
# one — staging scenario files into the commit being checked.
while IFS= read -r git_var; do
    unset "$git_var"
done < <(git rev-parse --local-env-vars)

PASS=0
FAIL=0
FAILED_TESTS=()

# Per-scenario state, populated by the dispatcher / run_commit.
TMP=""
REPO=""
CURRENT_TAG=""
OUT=""
RC=0

CMAKE_ERROR="CMake Error at CMakeLists.txt:1 (message): FAKE_CONFIGURE_ERROR"
NINJA_ERROR="ninja: error: loading 'build.ninja': No such file or directory"

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL+1))
    # One entry per scenario, not per failing assertion.
    local n=${#FAILED_TESTS[@]}
    if [[ $n -eq 0 || "${FAILED_TESTS[$((n-1))]}" != "$CURRENT_TAG" ]]; then
        FAILED_TESTS+=("$CURRENT_TAG")
    fi
    return 0
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS+1))
    return 0
}

# --- Fixtures ---

write_stubs() {
    local bin="$TMP/bin"
    mkdir -p "$bin"

    # Maps a configure/build preset to its binaryDir (CMakePresets.json).
    cat > "$bin/cmake" <<'STUB'
#!/bin/bash
echo "cmake $*" >> "$STUB_CALLS"
preset_dir() {
    case "$1" in
        ninja-debug|ninja-debug-coverage) echo build/debug ;;
        ninja-release) echo build/release ;;
        *) echo "stub cmake: unknown preset $1" >&2; exit 2 ;;
    esac
}
if [[ "$1" == "--preset" ]]; then
    if [[ " ${FAIL_PRESETS:-} " == *" $2 "* ]]; then
        echo "-- Configuring incomplete, errors occurred!"
        echo "CMake Error at CMakeLists.txt:1 (message): FAKE_CONFIGURE_ERROR ($2)" >&2
        exit 1
    fi
    dir="$(preset_dir "$2")"
    mkdir -p "$dir"
    touch "$dir/build.ninja" "$dir/compile_commands.json"
    exit 0
fi
if [[ "$1" == "--build" && "$2" == "--preset" ]]; then
    dir="$(preset_dir "$3")"
    if [[ ! -f "$dir/build.ninja" ]]; then
        echo "ninja: error: loading 'build.ninja': No such file or directory" >&2
        exit 1
    fi
    if [[ "$3" == "ninja-debug" && $# -eq 3 ]]; then
        for t in tests/storm_tests tests/mock_sqlite/storm_mock_tests \
                 tests/mock_libpq/storm_pq_mock_tests; do
            mkdir -p "$dir/$(dirname "$t")"
            printf '#!/bin/sh\nexit 0\n' > "$dir/$t"
            chmod +x "$dir/$t"
        done
    fi
    [[ "$*" == *"--target coverage"* && "$*" != *coverage-clean* ]] \
        && echo "  lines.......: 100.0% (1 of 1 lines)"
    exit 0
fi
echo "stub cmake: unexpected invocation: $*" >&2
exit 2
STUB

    # Empty `ninja -t targets` output sends build_tidy_prereqs down its
    # full-build fallback, which the cmake stub handles. A scenario that wants
    # the targeted path writes a target list to $TMP/ninja_targets.
    cat > "$bin/ninja" <<'STUB'
#!/bin/sh
echo "ninja $*" >> "$STUB_CALLS"
case "$*" in
    *"-t targets"*) [ -f "$NINJA_TARGETS" ] && cat "$NINJA_TARGETS" ;;
esac
exit 0
STUB
    printf '#!/bin/sh\nexit 0\n' > "$bin/pg_isready"
    chmod +x "$bin/cmake" "$bin/ninja" "$bin/pg_isready"
    return 0
}

# A throwaway repo holding the real commit.sh and the scripts it runs, committed
# so that only each scenario's own file is staged.
make_repo() {
    REPO="$TMP/repo"
    mkdir -p "$REPO/scripts/lib"
    cp "$REPO_ROOT/commit.sh" "$REPO/"
    cp "$REPO_ROOT/scripts/detect-changes.sh" "$REPO_ROOT/scripts/pg-default-connstr.sh" \
        "$REPO/scripts/"
    cp "$REPO_ROOT/scripts/lib/clang_tidy_skiplist.sh" "$REPO/scripts/lib/"
    cp "$REPO_ROOT/scripts/lib/line_coverage.sh" "$REPO/scripts/lib/"
    printf '#!/bin/sh\nexit 0\n' > "$REPO/scripts/run_clang_tidy.sh"
    chmod +x "$REPO/scripts/run_clang_tidy.sh"
    # A failed fixture commit (e.g. a global signing setup) would leave every
    # file staged, and the scenarios would pass or fail for the wrong reason.
    git -C "$REPO" init -q \
        && git -C "$REPO" add -A \
        && git -C "$REPO" -c user.name=t -c user.email=t@t -c commit.gpgsign=false \
            commit -q -m init \
        || { echo "FATAL: fixture repo setup failed in $REPO"; exit 1; }
}

stage() { # <path>...
    local f
    for f in "$@"; do
        mkdir -p "$REPO/$(dirname "$f")"
        echo "// staged" > "$REPO/$f"
        git -C "$REPO" add "$f" || { echo "FATAL: could not stage $f"; exit 1; }
    done
}

run_commit() { # <FAIL_PRESETS>
    OUT="$(cd "$REPO" && PATH="$TMP/bin:$PATH" STUB_CALLS="$TMP/calls" \
        NINJA_TARGETS="$TMP/ninja_targets" \
        FAIL_PRESETS="$1" STORM_COMMIT_LOG_DIR="$TMP/logs" ./commit.sh 2>&1)"
    RC=$?
    # Strip ANSI colours; a character-class regex has no ${var//} equivalent.
    # shellcheck disable=SC2001
    OUT="$(sed $'s/\x1b\\[[0-9;]*m//g' <<< "$OUT")"
    return 0
}

# --- Assertions ---

expect_blocked() {
    if [[ $RC -ne 0 ]]; then
        pass "commit is blocked (rc=$RC)"
    else
        fail "commit was NOT blocked (rc=0)"
    fi
    return 0
}

# Matches run_step's own verdict line ("✓ <name> passed" / "✗ <name> FAILED"),
# not the summary table, which pads the icon with two spaces.
expect_step() { # <passed|FAILED> <step-name-prefix>
    local verdict="$1" step="$2" icon="✗"
    [[ "$verdict" == passed ]] && icon="✓"
    if grep -F "$icon $step" <<< "$OUT" | grep -qF " $verdict"; then
        pass "step '$step' $verdict"
    else
        fail "step '$step' did not report $verdict"
    fi
    return 0
}

# The step that needed the failed configure carries the CMake error in its own
# log, names that log, and does not blame ninja.
expect_configure_error_in() { # <step-name-prefix> <log basename>
    local step="$1" log="$TMP/logs/$2"
    expect_step FAILED "$step"
    if [[ -f "$log" ]] && grep -qF "$CMAKE_ERROR" "$log"; then
        pass "'$step' log holds the CMake error"
    else
        fail "'$step' log ($log) does not hold the CMake error"
    fi
    if [[ -f "$log" ]] && grep -qF "$NINJA_ERROR" "$log"; then
        fail "'$step' log reports the misleading ninja error"
    else
        pass "'$step' log does not report the misleading ninja error"
    fi
    if grep -qF "full output: $log" <<< "$OUT"; then
        pass "'$step' prints its log path"
    else
        fail "'$step' does not print its log path"
    fi
    return 0
}

# --- Scenarios ---

# Cold build/release, release configure fails: the clang-tidy prerequisites.
scenario_tidy_release_configure_fails() {
    stage src/a.cpp
    run_commit "ninja-release"
    expect_blocked
    expect_configure_error_in "release prereqs" storm_tidy_bmi.log
    return 0
}

# Cold build/release with a bench change: the bench release build.
scenario_bench_release_configure_fails() {
    stage benchmarks/b.cpp
    run_commit "ninja-release"
    expect_blocked
    expect_configure_error_in "release build" storm_release.log
    return 0
}

# Cold build/debug, debug configure fails: clang-format, tests and coverage
# each need it, and each must report the CMake error itself.
scenario_debug_configure_fails() {
    stage src/a.cpp
    run_commit "ninja-debug"
    expect_blocked
    expect_configure_error_in "clang-format" storm_format.log
    expect_configure_error_in "tests (SQLite" storm_tests.log
    expect_configure_error_in "coverage" storm_coverage.log
    return 0
}

# cmake-only commits run cmake-format without clang-format — the #643 path.
scenario_cmake_format_debug_configure_fails() {
    stage cmake/c.cmake
    run_commit "ninja-debug"
    expect_blocked
    expect_configure_error_in "cmake-format" storm_cmake_format.log
    return 0
}

# compile_commands.json present but no build.ninja: the tidy self-heal sentinel
# says "configured", so build_tidy_prereqs must say "not configured" rather
# than hand ninja a missing build.ninja.
scenario_tidy_tree_without_build_ninja() {
    mkdir -p "$REPO/build/release"
    touch "$REPO/build/release/compile_commands.json"
    stage src/a.cpp
    run_commit ""
    expect_blocked
    expect_step FAILED "release prereqs"
    local log="$TMP/logs/storm_tidy_bmi.log"
    if [[ -f "$log" ]] && grep -qF "not configured" "$log" && ! grep -qF "$NINJA_ERROR" "$log"; then
        pass "release prereqs log reports 'not configured', not the ninja error"
    else
        fail "release prereqs log does not report 'not configured' cleanly"
    fi
    return 0
}

# A configured tree whose ninja lists synthesized BMIs takes the TARGETED prereq
# build, under the hook's own `#!/bin/bash` — bash 3.2 on macOS. It used to probe
# for `mapfile` (bash >= 4.0) and, on a Mac, always fall back to building the
# full release target set (#689).
scenario_tidy_targeted_prereqs_under_system_bash() {
    mkdir -p "$REPO/build/release"
    touch "$REPO/build/release/build.ninja"
    # One entry for the staged TU and no modmap, so the prereq verdict is
    # "complete" and no fallback of any kind should run.
    printf '[{"directory": "%s", "file": "%s/src/a.cpp", "arguments": ["clang++", "-c", "src/a.cpp"]}]\n' \
        "$REPO" "$REPO" > "$REPO/build/release/compile_commands.json"
    printf '%s\n' \
        "CMakeFiles/storm_tests@synth_1.dir/0123abcd.bmi: CXX_MODULE_BMI" \
        "CMakeFiles/storm.dir/CXX.dd: CXX_DYNDEP" > "$TMP/ninja_targets"
    stage src/a.cpp
    run_commit ""
    expect_step passed "release prereqs"
    # Every fallback, whatever its message, is this one call.
    if grep -qxF "cmake --build --preset ninja-release" "$TMP/calls"; then
        fail "release prereqs took a full-build fallback: $(cat "$TMP/logs/storm_tidy_bmi.log" 2>/dev/null)"
    else
        pass "release prereqs took no full-build fallback"
    fi
    if grep -qxF "ninja -C build/release CMakeFiles/storm_tests@synth_1.dir/0123abcd.bmi" "$TMP/calls"; then
        pass "the synthesized BMI was built by name"
    else
        fail "the synthesized BMI was not built by name: $(grep '^ninja' "$TMP/calls" | tr '\n' ';')"
    fi
    if grep -qxF "ninja -C build/release CMakeFiles/storm.dir/CXX.dd" "$TMP/calls"; then
        pass "the dyndep files were generated by name"
    else
        fail "the dyndep files were not generated by name"
    fi
    return 0
}

# Warm path: both trees configured. Every configure would fail if it ran, so a
# configure on the warm path shows up as a failed step as well as a call.
scenario_warm_tree_never_configures() {
    mkdir -p "$REPO/build/debug" "$REPO/build/release"
    touch "$REPO/build/debug/build.ninja" \
        "$REPO/build/release/build.ninja" "$REPO/build/release/compile_commands.json"
    stage src/a.cpp benchmarks/b.cpp
    run_commit "ninja-debug ninja-release"
    if grep -q '^cmake --preset' "$TMP/calls"; then
        fail "a configured tree ran a configure: $(grep '^cmake --preset' "$TMP/calls" | tr '\n' ';')"
    else
        pass "a configured tree runs no configure"
    fi
    expect_step passed "clang-format"
    expect_step passed "release prereqs"
    expect_step passed "release build"
    expect_step passed "tests (SQLite"
    expect_step passed "coverage"
    return 0
}

# --- Dispatcher ---

run_scenario() {
    CURRENT_TAG="$1"
    TMP="$(mktemp -d)"
    echo "[$CURRENT_TAG]"
    write_stubs
    make_repo
    : > "$TMP/calls"
    "scenario_$CURRENT_TAG"
    rm -rf "$TMP"
    return 0
}

run_scenario tidy_release_configure_fails
run_scenario bench_release_configure_fails
run_scenario debug_configure_fails
run_scenario cmake_format_debug_configure_fails
run_scenario tidy_tree_without_build_ninja
run_scenario tidy_targeted_prereqs_under_system_bash
run_scenario warm_tree_never_configures

echo ""
echo "$PASS passed, $FAIL failed"
if [[ $FAIL -ne 0 ]]; then
    echo "Failed scenarios: ${FAILED_TESTS[*]}"
    exit 1
fi
