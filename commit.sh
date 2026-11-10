#!/bin/bash
# Pre-commit checks: format -> tidy --fix -> test (coverage and benchmarks are CI-only, #793)
# Runs automatically via git pre-commit hook, or manually: ./commit.sh
#
# All checks are mandatory. No skip flags available.
#
# Smart skips (automatic, based on staged files; classified by
# scripts/ci/detect_changes.py, shared with .github/workflows/ci.yml):
#   No C++ or cmake files         → skip format, tidy, tests
#   cmake-only changes            → skip clang-format, clang-tidy; run tests + cmake-format
#   C++ but no src/tests/cmake    → skip tests
#   Nothing staged                → git hook: skip everything; manual run: run everything
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
LOG_TIDY=$LOG_DIR/storm_tidy.log
LOG_TIDY_BMI=$LOG_DIR/storm_tidy_bmi.log
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

# ─── clang-tidy release prerequisites (issue #557) ──────────────────────────
# clang-tidy replays build/release/compile_commands.json and only PARSES — it
# never links — so building the full default release target set to enable it
# spends ~90% of its time on the ~110 release test TUs of storm_tests that
# clang-tidy never opens. Measured on a cold tree (4 cores): 764s for the full
# set against 172s for what it actually needs:
#
#   a) the storm library BMIs, for `import storm;`
#   b) the BMIs of the module targets CMake SYNTHESIZES for module consumers
#      (CMakeFiles/*@synth_N.dir/*.bmi). These are discovered through dyndep,
#      so they are not reachable as static ninja targets until the .dd files
#      exist — building `storm` alone leaves them absent and clang-tidy then
#      fails with `module 'std' not found`, which is #489's first failure mode.
#   (#489's second failure mode, unbuilt mock test binaries, is gone with the
#   binaries: the SQLite and PG error-path tests run in storm_tests, #784/#787.)
#
# FAIL SAFE, per #557: every uncertain path falls back to the full build. A
# wrong skip is expensive to attribute — a missing BMI does not make clang-tidy
# error out, it degrades it to a PARTIAL PARSE that emits false warnings (4
# instead of 1 on src/orm/utilities.cppm, verified), and the hook runs
# clang-tidy with --fix.
# Answers "does clang-tidy have everything it needs to parse the staged files?"
# via scripts/tidy/tidy_prereqs.py (#688), which prints exactly `complete` when it
# does. EVERY other outcome — `incomplete`, a crash, a missing python3, a staged
# file with no compile_commands entry — is read as incomplete by the caller, so
# an unanticipated failure rebuilds rather than silently skips. That direction is
# the whole point (#557 DoD): a missing BMI does not make clang-tidy fail, it
# degrades it to a partial parse that emits FALSE warnings, and the hook runs
# clang-tidy with --fix.
tidy_prereqs_verdict() {
    local build_dir="$1"
    shift
    [[ $# -eq 0 ]] && { echo "incomplete"; return; }
    "$(dirname "${BASH_SOURCE[0]}")/scripts/tidy/tidy_prereqs.py" "$build_dir" "$@" 2>/dev/null \
        || echo "incomplete"
}

build_tidy_prereqs() {
    local build_dir="build/release"

    # compile_commands.json is the self-heal sentinel because it is what
    # clang-tidy replays (#489). It does not prove build.ninja exists, so check
    # that too: a tree without it is reported as unconfigured here instead of as
    # a ninja error from the build below (#674).
    configure_if_missing ninja-release "$build_dir/compile_commands.json" || return 1
    if [[ ! -f "$build_dir/build.ninja" ]]; then
        echo "$build_dir is not configured: $build_dir/build.ninja is missing."
        echo "Re-run \`cmake --preset ninja-release\` (or delete $build_dir) and commit again."
        return 1
    fi

    # ninja is the preset's generator. Without it, every enumeration below
    # would come back empty and we would skip the BMIs silently — the one
    # direction this function must never fail in. The enumerations use `while
    # read`, not `mapfile`: this script runs under macOS /bin/bash 3.2 (#689).
    local line
    if ! command -v ninja > /dev/null 2>&1; then
        echo "ninja unavailable — building the full release target set."
        cmake --build --preset ninja-release
        return
    fi

    # storm: the library BMIs. (The mock test binaries this also built are gone:
    # their tests run in storm_tests, #784/#787.)
    cmake --build --preset ninja-release --target storm || return 1

    # The BMIs of the module targets CMake synthesizes for module CONSUMERS
    # (CMakeFiles/*@synth_N.dir/*.bmi). `--target storm` does not build them —
    # they belong to the consumer, not the library — and without them clang-tidy
    # reports `module 'std' not found` and degrades to a partial parse (#489
    # mode 1). ninja knows these edges at configure time (build.ninja binds
    # `dyndep = …/CXX.dd` statically and builds the .dd as an order-only input),
    # so they are enumerable on a cold tree.
    local -a synth_bmis=()
    while IFS= read -r line; do synth_bmis+=("$line"); done < <(ninja -C "$build_dir" -t targets all 2>/dev/null \
        | sed 's/:.*//' \
        | grep -xE 'CMakeFiles/[^ ]*@synth_[0-9]+\.dir/[0-9a-f]+\.bmi' | sort -u)
    if [[ ${#synth_bmis[@]} -eq 0 ]]; then
        # This project has ~111 of them; zero means the naming changed, ninja
        # failed, or build.ninja is unusable. Never a legitimate state.
        echo "No synthesized module BMI targets found — building the full release target set."
        cmake --build --preset ninja-release
        return
    fi
    ninja -C "$build_dir" "${synth_bmis[@]}" || return 1

    # Generate every target's dyndep file. That materializes the per-TU modmaps
    # clang-tidy needs to replay a command, for the cost of the module scan
    # alone rather than of compiling those TUs — storm_tests' 109 modmaps take
    # 6 s this way against ~600 s to build the TUs. Without it, staging any
    # tests/** file leaves its modmap absent, the check below cannot prove the
    # tree is sufficient, and the fallback rebuilds everything — which is most
    # commits, since CLAUDE.md rule 9 pairs tests with every change.
    local -a dd_targets=()
    while IFS= read -r line; do dd_targets+=("$line"); done < <(ninja -C "$build_dir" -t targets all 2>/dev/null \
        | sed 's/:.*//' | grep -E '\.dd$' | sort -u)
    if [[ ${#dd_targets[@]} -gt 0 ]]; then
        ninja -C "$build_dir" "${dd_targets[@]}" || return 1
    fi

    # Verify rather than assume: every BMI referenced by the modmaps of the
    # files clang-tidy will parse must exist. This is what makes the enumeration
    # above safe to get wrong. Scoping to staged files is load-bearing — checking
    # every modmap in the tree always answers "incomplete", because the
    # storm_tests and benchmark targets legitimately reference BMIs this
    # targeted build does not produce.
    #
    # The extension list matches run_clang_tidy.py's own selector (cpp|cppm|h|
    # hpp) in both modes this hook uses; headers are linted through a
    # neighbouring TU's command, so they must be checked too.
    local -a tidy_inputs=()
    while IFS= read -r line; do tidy_inputs+=("$line"); done < <(grep -E '\.(cpp|cppm|h|hpp)$' <<< "$STAGED_FILES" || true)

    # Files run_clang_tidy.py refuses to parse are not evidence of anything —
    # dropping them keeps a staged benchmarks/schema.cppm from forcing a full
    # build for a file clang-tidy never opens. Same source of truth both consult
    # (#550, scripts/tidy/clang_tidy_skiplist.py); if it is unavailable, keep every
    # input, which can only over-trigger the fallback.
    local -a probe_inputs=()
    local skiplist f
    skiplist="$(dirname "${BASH_SOURCE[0]}")/scripts/tidy/clang_tidy_skiplist.py"
    if [[ ${#tidy_inputs[@]} -gt 0 ]]; then
        while IFS= read -r f; do probe_inputs+=("$f"); done < <(
            printf '%s\n' "${tidy_inputs[@]}" | python3 "$skiplist" --keep 2>/dev/null \
                || printf '%s\n' "${tidy_inputs[@]}")
    fi

    if [[ $(tidy_prereqs_verdict "$build_dir" "${probe_inputs[@]}") != "complete" ]]; then
        echo "Staged files need module BMIs this targeted build does not produce —"
        echo "falling back to the full release build."
        cmake --build --preset ninja-release || return 1
    fi
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
RUN_SWITCHES=(RUN_FORMAT RUN_CMAKE_FORMAT RUN_TIDY RUN_TESTS)
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
[[ "$RUN_TIDY" == true ]] && ((TOTAL_STEPS++))
# clang-tidy replays build/release/compile_commands.json, so it needs the release
# build both CONFIGURED (compile_commands.json present) and its default targets
# BUILT before it runs (issues #330, #326, #489). Two failure modes it prevents:
#   1. `module file '…std.pcm' not found` — a TU that does `import std;` (or
#      `import storm;`) needs the std/storm module BMIs (.pcm), which clang-tidy
#      will NOT build itself. CMake wires the std module + per-TU `@….modmap` via
#      ninja dyndep, invisible to compile_commands — only a real release build
#      produces them (#326 Finding C).
#   2. `storm_mock_tests_NOT_BUILT` on a later `ctest`/tidy run against release —
#      the mock test binaries were never compiled. Moot since #787 removed the
#      last mock binary (both suites now run in storm_tests).
# So this prebuild configures release if needed, then builds what clang-tidy
# needs (the module BMIs), self-healing a fresh/stale worktree for
# both the git-hook path and a manual `./commit.sh` run (#489). It builds those
# targets specifically rather than the full default set, which spent ~90% of a
# cold build on release test TUs clang-tidy never opens (#557) — see
# build_tidy_prereqs above for the target list, the measurements, and the
# fallbacks that keep both failure modes below covered.
RUN_TIDY_BMI=false
if [[ "$RUN_TIDY" == true ]]; then
    RUN_TIDY_BMI=true
    ((TOTAL_STEPS++))
fi
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

# --- Step 3: clang-tidy ---
# Default to --diff mode (issue #262): only block on warnings touching staged
# lines, so pre-existing drift in unrelated files doesn't block unrelated work.
# Set STORM_TIDY_FULL=1 to force whole-file staged scan (the pre-#262 behavior).
if [[ "$RUN_TIDY" == true ]]; then
    # Self-heal a fresh/stale build/release before clang-tidy (#489): configure
    # if compile_commands.json is absent — inside the step, so a failed configure
    # fails it with the CMake error in its log (#674) — then build what clang-tidy
    # needs: the module BMIs (for `import std;`/`import storm;`).
    # build_tidy_prereqs builds those targets specifically rather than the full
    # default set (#557), and falls back to the full build whenever it cannot
    # prove that is enough. This is a no-op when release is already up to date
    # (the ninja build short-circuits), so the warm path stays fast. See the
    # RUN_TIDY_BMI comment above for the two failures this prevents.
    if [[ "$RUN_TIDY_BMI" == true ]]; then
        run_step "release prereqs (BMIs, for clang-tidy)" "$LOG_TIDY_BMI" \
            build_tidy_prereqs
    fi

    if [[ -n "$STORM_TIDY_FULL" ]]; then
        run_step "clang-tidy --full --fix" "$LOG_TIDY" ./scripts/tidy/run_clang_tidy.py --full --fix || true
    else
        run_step "clang-tidy --diff --fix" "$LOG_TIDY" ./scripts/tidy/run_clang_tidy.py --diff --fix || true
    fi
fi

# --- Re-stage files modified by format/tidy ---
# RUN_CMAKE_FORMAT belongs here too (#643): cmake-format rewrites files in
# place exactly like clang-format, and the cmake-only path above sets
# RUN_FORMAT=RUN_TIDY=false while leaving RUN_CMAKE_FORMAT=true. Omitting it
# meant a cmake-only commit recorded the UNFORMATTED file and left the rewrite
# unstaged in the working tree — with a green hook summary. Unreachable from a
# container until now only because the step could not run there at all.
if [[ "$RUN_FORMAT" == true || "$RUN_CMAKE_FORMAT" == true || "$RUN_TIDY" == true ]]; then
    git add -u
fi

# --- Step 4: tests ---
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
