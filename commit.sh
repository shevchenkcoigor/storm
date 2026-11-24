#!/bin/bash
# Pre-commit checks: format -> test (coverage, benchmarks and clang-tidy are CI-only: #793, #910)
# Runs automatically via git pre-commit hook, or manually: ./commit.sh
#
# All checks are mandatory. No skip flags available.
#
# Smart skips (automatic, based on staged files; classified by
# scripts/ci/detect_changes.py, shared with .github/workflows/ci.yml):
#   No C++ or cmake files         → skip format, tests
#   cmake-only changes            → skip clang-format; run tests + cmake-format
#   C++ but no src/tests/cmake    → skip tests
#   Nothing staged                → git hook: skip everything; manual run: run everything
#
# clang-tidy is not a pre-commit step (#910): it runs on the PR's changed lines in
# ci.yml's clang-tidy job (run_clang_tidy.py --diff --base) and on the whole tree in the
# weekly sweep (clang-tidy-sweep.yml). Locally: ./scripts/tidy/run_clang_tidy.py.
#
# SonarCloud quality gate runs on `git push` via .githooks/pre-push.

# --- Colors & formatting ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
DIM='\033[2m'
RESET='\033[0m'

# --- State tracking ---
declare -a STEP_NAMES=()
declare -a STEP_RESULTS=()
declare -a STEP_TIMES=()
declare -a STEP_LOGFILES=()
FAILED=false
TOTAL_START=$SECONDS

# Fixed log files — one per step, stable paths for easy inspection after failure.
# STORM_COMMIT_LOG_DIR relocates them; only the scripts/*/tests/ self-tests set it, so a self-test
# never overwrites the logs of a real run.
LOG_DIR="${STORM_COMMIT_LOG_DIR:-/tmp}"
mkdir -p "$LOG_DIR"
LOG_FORMAT=$LOG_DIR/storm_format.log
LOG_CMAKE_FORMAT=$LOG_DIR/storm_cmake_format.log
LOG_TESTS=$LOG_DIR/storm_tests.log
LOG_SELF_HEAL=$LOG_DIR/storm_self_heal_test.log

print_header() {
    local step_num=$1 total=$2 name=$3
    echo ""
    echo -e "${BLUE}${BOLD}[$step_num/$total]${RESET} ${BOLD}$name${RESET}"
    echo -e "${DIM}$(printf '%.0s─' {1..60})${RESET}"
    return 0
}

_step_begin() {
    local name="$1"
    local step_num=${#STEP_NAMES[@]}
    step_num=$((step_num + 1))
    STEP_NAMES+=("$name")
    print_header "$step_num" "$TOTAL_STEPS" "$name"
}

_step_finish() {
    local name="$1" exit_code="$2" elapsed="$3" logfile="$4"
    STEP_TIMES+=("${elapsed}s")
    STEP_LOGFILES+=("$logfile")
    if [[ $exit_code -eq 0 ]]; then
        STEP_RESULTS+=("pass")
        echo -e "${GREEN}✓ $name passed${RESET} ${DIM}(${elapsed}s)${RESET}"
        return 0
    else
        STEP_RESULTS+=("FAIL")
        FAILED=true
        echo -e "${RED}✗ $name FAILED${RESET} ${DIM}(${elapsed}s)${RESET}"
        echo -e "${DIM}  → full output: $logfile${RESET}"
        return 1
    fi
}

run_step() {
    local name="$1" logfile="$2"
    shift 2
    _step_begin "$name"
    local start=$SECONDS
    "$@" > "$logfile" 2>&1
    local exit_code=$?
    _step_finish "$name" "$exit_code" "$(( SECONDS - start ))" "$logfile"
}

# Show full output live (for long-running steps like tests)
run_step_live() {
    local name="$1" logfile="$2"
    shift 2
    _step_begin "$name"
    local start=$SECONDS
    "$@" 2>&1 | tee "$logfile"
    local exit_code=${PIPESTATUS[0]}
    _step_finish "$name" "$exit_code" "$(( SECONDS - start ))" "$logfile"
}

# ─── Self-heal configure (issues #489, #674) ────────────────────────────────
# Configures <preset> only when <sentinel> is absent, so the warm path (an
# already-configured tree) never runs cmake. Call it from INSIDE a step's
# command: CMake's output then lands in that step's log, and a failed configure
# fails that step with its log path printed. Run outside a step with its output
# discarded and its status ignored (the pre-#674 shape), a failed configure
# resurfaced one step later as `ninja: error: loading 'build.ninja'`, with the
# CMake error logged nowhere.
configure_if_missing() {
    local preset="$1" sentinel="$2"
    [[ -f "$sentinel" ]] && return 0
    echo "Configuring $preset ($sentinel is absent)..."
    if ! cmake --preset "$preset"; then
        echo "cmake --preset $preset FAILED — the CMake error is above; nothing was built." >&2
        return 1
    fi
    return 0
}

# configure_if_missing, then `cmake --build --preset <preset> [args...]`.
configure_and_build() {
    local preset="$1" sentinel="$2"
    shift 2
    configure_if_missing "$preset" "$sentinel" || return 1
    cmake --build --preset "$preset" "$@"
}

print_summary() {
    local total_elapsed=$(( SECONDS - TOTAL_START ))
    echo ""
    echo -e "${BOLD}$(printf '%.0s═' {1..60})${RESET}"

    if [[ "$FAILED" == true ]]; then
        echo -e "${RED}${BOLD} COMMIT BLOCKED — pre-commit checks failed${RESET}"
    else
        echo -e "${GREEN}${BOLD} All checks passed!${RESET}"
    fi

    echo -e "${BOLD}$(printf '%.0s═' {1..60})${RESET}"
    echo ""

    # Summary table
    for i in "${!STEP_NAMES[@]}"; do
        local icon
        if [[ "${STEP_RESULTS[$i]}" == "pass" ]]; then
            icon="${GREEN}✓${RESET}"
        else
            icon="${RED}✗${RESET}"
        fi
        printf "  %b  %-35s %s\n" "$icon" "${STEP_NAMES[$i]}" "${DIM}${STEP_TIMES[$i]}${RESET}"
    done

    echo ""
    echo -e "  ${DIM}Total: ${total_elapsed}s${RESET}"

    # Show failure log paths
    if [[ "$FAILED" == true ]]; then
        echo -e "${RED}${BOLD}Failed step logs:${RESET}"
        for i in "${!STEP_NAMES[@]}"; do
            if [[ "${STEP_RESULTS[$i]}" == "FAIL" ]]; then
                echo -e "  ${RED}✗${RESET} ${STEP_NAMES[$i]}: ${DIM}${STEP_LOGFILES[$i]}${RESET}"
            fi
        done
        echo ""
        echo -e "${YELLOW}${BOLD}Tip:${RESET} Fix the issue above and run ${BOLD}git commit${RESET} again."
    fi

    echo ""
    return 0
}

# --- Smart skip: detect staged file changes ---
# Classification (HAS_SRC_CHANGES etc.) lives in scripts/ci/detect_changes.py, shared
# with .github/workflows/ci.yml so both places skip on the same rules; the
# decision table (which RUN_* steps a classification needs) is
# scripts/commit/smart_skip.py (#688). With nothing staged there is nothing to classify.
# Under the git hook (STORM_PRE_COMMIT_HOOK=1, set by .githooks/pre-commit) an
# empty index is a commit that changes nothing, so it needs no checks (#706).
# A manual `./commit.sh` with nothing staged keeps the full suite. smart_skip.py
# decides: the hook passes --empty-index-skips, a manual run does not. "Empty" is relative to HEAD, so a
# reword-only `git commit --amend` also skips everything — do not lean on an
# amend to re-validate a commit.
SCRIPTS_DIR="$(dirname "${BASH_SOURCE[0]}")/scripts"
STAGED_FILES=$(git diff --cached --name-only 2>/dev/null || true)
# FAIL CLOSED: a helper that prints nothing would leave every RUN_* unset, so
# TOTAL_STEPS==0 and the commit would pass with no checks. Take the plan with its
# exit status, and require each switch to be exactly true/false before using it.
RUN_SWITCHES=(RUN_FORMAT RUN_CMAKE_FORMAT RUN_TESTS)
unset "${RUN_SWITCHES[@]}" SKIP_NOTE
if [[ -n "$STAGED_FILES" ]]; then
    PLAN=$(set -o pipefail
           printf '%s\n' "$STAGED_FILES" | "$SCRIPTS_DIR/ci/detect_changes.py" | "$SCRIPTS_DIR/commit/smart_skip.py") || PLAN=""
else
    EMPTY_INDEX_ARGS=()
    [[ "${STORM_PRE_COMMIT_HOOK:-}" == 1 ]] && EMPTY_INDEX_ARGS=(--empty-index-skips)
    PLAN=$("$SCRIPTS_DIR/commit/smart_skip.py" ${EMPTY_INDEX_ARGS[@]+"${EMPTY_INDEX_ARGS[@]}"} < /dev/null) || PLAN=""
fi
eval "$PLAN"
for switch in "${RUN_SWITCHES[@]}"; do
    if [[ "${!switch}" != true && "${!switch}" != false ]]; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — could not decide which checks to run (smart-skip failed)${RESET}"
        exit 1
    fi
done
[[ -n "$SKIP_NOTE" ]] && echo -e "${DIM}ℹ  ${SKIP_NOTE}${RESET}"

# --- Staged-file guards (issues #543, #550, #651, #674, #680, #688) ---
# The table of guards (trigger regex -> self-test/checker -> BLOCK message) lives
# in scripts/commit/pre_commit_guards.py. They run BEFORE the TOTAL_STEPS==0 early exit
# below on purpose: a commit touching only a guard's files skips every other
# step, so gating them on the step count would miss their own failure case.
if [[ -n "$STAGED_FILES" ]]; then
    printf '%s\n' "$STAGED_FILES" \
        | "$(dirname "${BASH_SOURCE[0]}")/scripts/commit/pre_commit_guards.py" \
            --log-file "$LOG_SELF_HEAL" 3> "$LOG_SELF_HEAL" || exit 1
fi

# --- Count total steps ---
TOTAL_STEPS=0
[[ "$RUN_FORMAT" == true ]] && ((TOTAL_STEPS++))
[[ "$RUN_CMAKE_FORMAT" == true ]] && ((TOTAL_STEPS++))
[[ "$RUN_TESTS" == true ]] && ((TOTAL_STEPS++))

if [[ $TOTAL_STEPS -eq 0 ]]; then
    echo -e "${GREEN}✓ No checks needed for this commit.${RESET}"
    exit 0
fi

echo -e "${BOLD}Running $TOTAL_STEPS pre-commit checks...${RESET}"

# --- Steps 1-2: clang-format, cmake-format ---
# Both build a target of build/debug, so the first of them to run configures it
# if absent (configure_and_build, #674). Its full output — stderr included —
# lands in that step's log, so on a fresh tree with both formatters the
# configure WARNINGs land in clang-format's log. A missing cmake-format does not
# depend on that: cmake/format.cmake then defines a stub `cmake-format` target
# that fails naming the tool, so the reason is in cmake-format's own log however
# the tree was configured (#643, #690).
if [[ "$RUN_FORMAT" == true ]]; then
    run_step "clang-format" "$LOG_FORMAT" \
        configure_and_build ninja-debug build/debug/build.ninja --target format || true
fi

if [[ "$RUN_CMAKE_FORMAT" == true ]]; then
    run_step "cmake-format" "$LOG_CMAKE_FORMAT" \
        configure_and_build ninja-debug build/debug/build.ninja --target cmake-format || true
fi

# --- Re-stage files modified by format ---
# RUN_CMAKE_FORMAT belongs here too (#643): cmake-format rewrites files in
# place exactly like clang-format, and the cmake-only path above sets
# RUN_FORMAT=false while leaving RUN_CMAKE_FORMAT=true. Omitting it
# meant a cmake-only commit recorded the UNFORMATTED file and left the rewrite
# unstaged in the working tree — with a green hook summary. Unreachable from a
# container until now only because the step could not run there at all.
if [[ "$RUN_FORMAT" == true || "$RUN_CMAKE_FORMAT" == true ]]; then
    git add -u
fi

# --- Step 3: tests ---
# Run the gtest binary directly instead of `ctest --preset ninja-debug`:
# ctest registers each of the ~3000 individual gtest cases as its own CTest test
# (gtest_discover_tests DISCOVERY_MODE PRE_TEST), so it re-launches the whole
# storm_tests process per test case. Measured 2026-08-27: ~260s via ctest vs
# ~33s running the binary once (7.8x) — see
# docs/internals/testing/TESTING.md#local-test-suite-speed. Self-heal first:
# configure build/debug if absent, then build the FULL default debug target set
# so storm_tests exists — otherwise a fresh/stale worktree would be missing it
# (#489). Both are no-ops on the warm path (config guarded
# by a file check; the ninja build short-circuits when up to date).
if [[ "$RUN_TESTS" == true ]]; then
    build_debug_then_test() {
        configure_and_build ninja-debug build/debug/build.ninja || return 1

        # Soft default (not an override): an already-exported STORM_PG_CONNSTR
        # wins, as in scripts/coverage/coverage-run-batched.sh — a hard override here would
        # silently point tests at a different DB than the one coverage-run-batched.sh and
        # CI use. They share the DEFAULT (per-OS socket directory, from
        # scripts/coverage/coverage_gate.py) but not the empty-value rule: this treats
        # a set-but-empty value as unset, the coverage script passes it through.
        # CMakePresets.json's testPresets still carry the Linux path (`ctest
        # --preset` only).
        if [[ -z "${STORM_PG_CONNSTR:-}" ]]; then
            STORM_PG_CONNSTR="$("$(dirname "${BASH_SOURCE[0]}")/scripts/coverage/coverage_gate.py" pg-connstr "$(uname -s)")" || return 1
        fi
        export STORM_PG_CONNSTR

        # PG unreachable means every PG-backed test GTEST_SKIPs (gtest still
        # exits 0), so the gate would otherwise report PASS having silently run
        # only the SQLite half. Warn loudly rather than fail: CLAUDE.md
        # documents PG-skip-if-not-running as accepted behavior, so this
        # matches scripts/coverage/coverage-run-batched.sh's WARN (not fail) policy.
        if command -v pg_isready > /dev/null 2>&1 \
           && ! pg_isready -d "$STORM_PG_CONNSTR" > /dev/null 2>&1; then
            echo -e "${RED}  WARN: PostgreSQL unreachable at [$STORM_PG_CONNSTR] — PG tests will SKIP${RESET}"
        fi

        # Run all three regardless of earlier failures (matches ctest's
        # run-everything-then-report behavior) so a storm_tests failure
        # doesn't hide an unrelated mock-test failure from this run's log.
        # Bounded with `timeout`: ctest applied a 1500s default per-test
        # TIMEOUT, which a bare invocation has no equivalent of — without this
        # a hung/deadlocked test would hang the pre-commit gate indefinitely.
        # `timeout` isn't stock on macOS (BSD userland has neither `timeout`
        # nor `gtimeout` — it's only present via `brew install coreutils`),
        # so degrade gracefully to unbounded rather than fail outright.
        local timeout_bin=""
        if command -v timeout > /dev/null 2>&1; then
            timeout_bin="timeout"
        elif command -v gtimeout > /dev/null 2>&1; then
            timeout_bin="gtimeout"
        fi
        run_with_timeout() {
            local secs="$1"
            shift
            if [[ -n "$timeout_bin" ]]; then
                "$timeout_bin" "$secs" "$@"
            else
                "$@"
            fi
            return $?
        }
        local overall=0
        run_with_timeout 900 ./build/debug/tests/storm_tests --gtest_brief=1 || overall=1
        return $overall
    }
    run_step_live "tests (SQLite + PostgreSQL)" "$LOG_TESTS" build_debug_then_test || true
fi

# --- Final summary ---
print_summary

if [[ "$FAILED" == true ]]; then
    exit 1
fi
