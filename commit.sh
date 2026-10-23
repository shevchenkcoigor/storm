#!/bin/bash
# Pre-commit checks: format -> tidy --fix -> test -> coverage
# Runs automatically via git pre-commit hook, or manually: ./commit.sh
#
# All checks are mandatory. No skip flags available.
#
# Smart skips (automatic, based on staged files; classified by
# scripts/detect-changes.sh, shared with .github/workflows/ci.yml):
#   No C++ or cmake files         → skip format, tidy, tests, coverage
#   cmake-only changes            → skip clang-format, clang-tidy; run tests + coverage + cmake-format
#   C++ but no src/tests/cmake    → skip tests, coverage
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
# STORM_COMMIT_LOG_DIR relocates them; only scripts/tests/ sets it, so a self-test
# never overwrites the logs of a real run.
LOG_DIR="${STORM_COMMIT_LOG_DIR:-/tmp}"
mkdir -p "$LOG_DIR"
LOG_FORMAT=$LOG_DIR/storm_format.log
LOG_CMAKE_FORMAT=$LOG_DIR/storm_cmake_format.log
LOG_TIDY=$LOG_DIR/storm_tidy.log
LOG_TIDY_BMI=$LOG_DIR/storm_tidy_bmi.log
LOG_RELEASE=$LOG_DIR/storm_release.log
LOG_TESTS=$LOG_DIR/storm_tests.log
LOG_COVERAGE=$LOG_DIR/storm_coverage.log

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

# Show full output live (for long-running steps like tests/coverage)
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
# set against 172s for the three things it actually needs:
#
#   a) the storm library BMIs, for `import storm;`
#   b) the BMIs of the module targets CMake SYNTHESIZES for module consumers
#      (CMakeFiles/*@synth_N.dir/*.bmi). These are discovered through dyndep,
#      so they are not reachable as static ninja targets until the .dd files
#      exist — building `storm` alone leaves them absent and clang-tidy then
#      fails with `module 'std' not found`, which is #489's first failure mode.
#   c) the mock test binaries, for #489's second failure mode.
#
# FAIL SAFE, per #557: every uncertain path falls back to the full build. A
# wrong skip is expensive to attribute — a missing BMI does not make clang-tidy
# error out, it degrades it to a PARTIAL PARSE that emits false warnings (4
# instead of 1 on src/orm/utilities.cppm, verified), and the hook runs
# clang-tidy with --fix.
# Answers "does clang-tidy have everything it needs to parse the staged files?"
# Prints exactly `complete` when it does. EVERY other outcome — `incomplete`, a
# crash, a missing python3, a staged file with no compile_commands entry — is
# read as incomplete by the caller, so an unanticipated failure rebuilds rather
# than silently skips. That direction is the whole point (#557 DoD): a missing
# BMI does not make clang-tidy fail, it degrades it to a partial parse that
# emits FALSE warnings, and the hook runs clang-tidy with --fix.
tidy_prereqs_verdict() {
    local build_dir="$1"
    shift
    [[ $# -eq 0 ]] && { echo "incomplete"; return; }
    python3 - "$build_dir" "$@" 2>/dev/null <<'PYPREREQ' || echo "incomplete"
import json, pathlib, re, shlex, sys

def main():
    build_dir = pathlib.Path(sys.argv[1])
    staged = {pathlib.Path(f).resolve() for f in sys.argv[2:]}
    entries = json.loads((build_dir / "compile_commands.json").read_text())

    def argv_of(entry):
        if "arguments" in entry:
            return entry["arguments"]
        return shlex.split(entry.get("command", ""))

    # Only the TUs clang-tidy will actually replay matter. Every other target's
    # modmap legitimately references BMIs this targeted build does not produce,
    # so checking the whole tree would always answer "incomplete".
    seen = set()
    for entry in entries:
        path = pathlib.Path(entry["file"]).resolve()
        if path not in staged:
            continue
        seen.add(path)
        root = pathlib.Path(entry.get("directory", build_dir))
        for arg in argv_of(entry):
            if not arg.startswith("@") or not arg.endswith(".modmap"):
                continue
            modmap = root / arg[1:]
            if not modmap.exists():
                return False
            for ref in re.findall(r'-fmodule-file="?(?:[^=\s"]+=)?([^"\s]+)',
                                  modmap.read_text()):
                if not (root / ref).exists() and not pathlib.Path(ref).exists():
                    return False

    # A staged input with no entry at all was never verified. clang-tidy infers
    # a command for it from a neighbouring entry (this is how headers are
    # linted — the database holds no header entries), so "no entry" means
    # "unknown", not "fine".
    return staged == seen

print("complete" if main() else "incomplete")
PYPREREQ
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

    # mapfile is bash >= 4.0 and ninja is the preset's generator. Without
    # either, every enumeration below would come back empty and we would skip
    # the BMIs silently — the one direction this function must never fail in.
    if ! command -v ninja > /dev/null 2>&1 || ! type -t mapfile > /dev/null 2>&1; then
        echo "ninja or mapfile unavailable — building the full release target set."
        cmake --build --preset ninja-release
        return
    fi

    # storm: the library BMIs. The two mock binaries: #489's failure mode 2, and
    # their modmaps, which tests/mock_*/ TUs need — those files get staged often.
    # Named explicitly, so renaming either (tests/mock_sqlite/CMakeLists.txt,
    # tests/mock_libpq/CMakeLists.txt) fails this step CLOSED and blocks the
    # commit, rather than silently under-building.
    cmake --build --preset ninja-release \
        --target storm storm_mock_tests storm_pq_mock_tests || return 1

    # The BMIs of the module targets CMake synthesizes for module CONSUMERS
    # (CMakeFiles/*@synth_N.dir/*.bmi). `--target storm` does not build them —
    # they belong to the consumer, not the library — and without them clang-tidy
    # reports `module 'std' not found` and degrades to a partial parse (#489
    # mode 1). ninja knows these edges at configure time (build.ninja binds
    # `dyndep = …/CXX.dd` statically and builds the .dd as an order-only input),
    # so they are enumerable on a cold tree.
    local -a synth_bmis
    mapfile -t synth_bmis < <(ninja -C "$build_dir" -t targets all 2>/dev/null \
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
    local -a dd_targets
    mapfile -t dd_targets < <(ninja -C "$build_dir" -t targets all 2>/dev/null \
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
    # The extension list matches run_clang_tidy.sh's own selector (cpp|cppm|h|
    # hpp) in both modes this hook uses; headers are linted through a
    # neighbouring TU's command, so they must be checked too.
    local -a tidy_inputs
    mapfile -t tidy_inputs < <(grep -E '\.(cpp|cppm|h|hpp)$' <<< "$STAGED_FILES" || true)

    # Files run_clang_tidy.sh refuses to parse are not evidence of anything —
    # dropping them keeps a staged benchmarks/schema.cppm from forcing a full
    # build for a file clang-tidy never opens. Same source of truth both scripts
    # consult (#550); if it is unavailable, keep every input, which can only
    # over-trigger the fallback.
    local -a probe_inputs=()
    local skiplist f
    skiplist="$(dirname "${BASH_SOURCE[0]}")/scripts/lib/clang_tidy_skiplist.sh"
    if [[ -r "$skiplist" ]]; then
        # shellcheck source=scripts/lib/clang_tidy_skiplist.sh
        source "$skiplist"
    fi
    for f in "${tidy_inputs[@]}"; do
        if declare -F is_known_unparseable > /dev/null \
           && { is_known_unparseable "$f" || is_always_skip_file "$f"; }; then
            continue
        fi
        probe_inputs+=("$f")
    done

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
RUN_FORMAT=true
RUN_CMAKE_FORMAT=true
RUN_TIDY=true
RUN_TESTS=true
RUN_COVERAGE=true

STAGED_FILES=$(git diff --cached --name-only 2>/dev/null || true)
if [[ -n "$STAGED_FILES" ]]; then
    # Classification (HAS_SRC_CHANGES etc.) lives in scripts/detect-changes.sh,
    # shared with .github/workflows/ci.yml so both places skip on the same rules.
    eval "$(printf '%s\n' "$STAGED_FILES" | "$(dirname "${BASH_SOURCE[0]}")/scripts/detect-changes.sh")"

    if [[ "$HAS_CPP_CHANGES" == false && "$HAS_CMAKE_CHANGES" == false ]]; then
        echo -e "${DIM}ℹ  No C++ or cmake files in commit — skipping format, tidy, tests, coverage${RESET}"
        RUN_FORMAT=false RUN_CMAKE_FORMAT=false RUN_TIDY=false
        RUN_TESTS=false RUN_COVERAGE=false
    elif [[ "$HAS_CPP_CHANGES" == false ]]; then
        echo -e "${DIM}ℹ  cmake-only changes — skipping clang-format, clang-tidy${RESET}"
        RUN_FORMAT=false RUN_TIDY=false
    elif [[ "$HAS_SRC_CHANGES" == false && "$HAS_TEST_CHANGES" == false && "$HAS_CMAKE_CHANGES" == false ]]; then
        echo -e "${DIM}ℹ  No src/, tests/, or cmake changes — skipping tests, coverage${RESET}"
        RUN_TESTS=false RUN_COVERAGE=false
    fi

    if [[ "$HAS_CMAKE_CHANGES" == false ]]; then
        RUN_CMAKE_FORMAT=false
    fi
fi

RUN_BENCH_RELEASE=false
if [[ "$HAS_BENCH_CHANGES" == true && "$HAS_CPP_CHANGES" == true ]]; then
    RUN_BENCH_RELEASE=true
fi

# --- Agent frontmatter guard (issue #543) ---
# A multi-line `description:` makes Claude Code drop the agent SILENTLY — the
# bug that left storm-sql-reviewer and storm-buildsystem-reviewer
# undispatchable from the day they merged, and CLAUDE.md rule #13 partly
# unenforceable. Cheap pure-bash check, so it runs whenever an agent file is
# staged.
#
# Deliberately placed BEFORE the TOTAL_STEPS==0 early exit below: a commit
# touching only .claude/agents/*.md has no C++ or cmake, so it skips every
# other step — exactly the commit that can introduce this bug. Gating it on
# the step count would make the guard miss its own failure case.
if [[ -n "$STAGED_FILES" ]] \
   && grep -q '^\.claude/agents/.*\.md$' <<< "$STAGED_FILES"; then
    if ! "$(dirname "${BASH_SOURCE[0]}")/scripts/check-agent-frontmatter.sh"; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — invalid agent frontmatter${RESET}"
        exit 1
    fi
    echo -e "${DIM}✓ agent frontmatter valid${RESET}"
fi

# --- clang-tidy skip-list self-test (issue #550) ---
# scripts/lib/clang_tidy_skiplist.sh is the single source of truth both
# run_clang_tidy.sh --diff and the weekly --all sweep consult for which files
# clang-tidy cannot parse standalone. Cheap pure-bash check.
#
# Deliberately placed BEFORE the TOTAL_STEPS==0 early exit below, same
# reasoning as the agent-frontmatter guard above: a commit touching only
# scripts/lib/clang_tidy_skiplist.sh or scripts/run_clang_tidy.sh has no
# src/tests/cmake changes, so RUN_TIDY (and every other step) is false and
# TOTAL_STEPS would otherwise be 0 — exactly the commit that most needs this
# self-test.
if [[ -n "$STAGED_FILES" ]] \
   && grep -qE '^scripts/(lib/clang_tidy_skiplist\.sh|run_clang_tidy\.sh)$' <<< "$STAGED_FILES"; then
    if ! "$(dirname "${BASH_SOURCE[0]}")/scripts/tests/test_run_clang_tidy_skiplist.sh"; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — clang-tidy skip-list self-test failed${RESET}"
        exit 1
    fi
    echo -e "${DIM}✓ clang-tidy skip-list self-test passed${RESET}"
fi

# --- SessionStart hook wiring self-test (issue #651) ---
# .claude/hooks/session-start-docker.sh sets core.hooksPath, which is the only
# reason THIS script runs at all in a clone that never configures a build (the
# other writer, CMakeLists.txt, needs a working ../clang-p2996). Its wiring must
# stay above the hook's three Docker bail-outs, or it goes missing in exactly
# the sessions those bail-outs return from. Cheap pure-bash check.
#
# Deliberately placed BEFORE the TOTAL_STEPS==0 early exit below, same
# reasoning as the two guards above: a commit touching only the hook,
# .claude/settings.json (which declares it) or this self-test has no
# src/tests/cmake changes, so TOTAL_STEPS would otherwise be 0 — exactly the
# commit that can unwire the gate.
if [[ -n "$STAGED_FILES" ]] \
   && grep -qE '^(\.claude/(hooks/session-start-docker\.sh|settings\.json)|scripts/tests/test_session_start_hook\.sh)$' <<< "$STAGED_FILES"; then
    if ! "$(dirname "${BASH_SOURCE[0]}")/scripts/tests/test_session_start_hook.sh"; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — SessionStart hook wiring self-test failed${RESET}"
        exit 1
    fi
    echo -e "${DIM}✓ SessionStart hook wiring self-test passed${RESET}"
fi

# --- Self-heal configure self-test (issue #674) ---
# The configure_if_missing calls above must fail their step with the CMake
# error in its log; swallowed, a failed configure resurfaced as a misleading
# ninja error. The self-test runs this script against stub cmake/ninja in a
# throwaway repo — no toolchain needed.
#
# Deliberately placed BEFORE the TOTAL_STEPS==0 early exit below, same
# reasoning as the guards above: a commit touching only commit.sh or this
# self-test has no C++ or cmake, so TOTAL_STEPS would otherwise be 0 — exactly
# the commit that can reintroduce the bug. The scripts the self-test copies into
# its throwaway repo are listed too: changing one can break its scenarios.
if [[ -n "$STAGED_FILES" ]] \
   && grep -qE '^(commit\.sh|scripts/(detect-changes|pg-default-connstr)\.sh|scripts/lib/clang_tidy_skiplist\.sh|scripts/tests/test_commit_self_heal_configure\.sh)$' <<< "$STAGED_FILES"; then
    if ! "$(dirname "${BASH_SOURCE[0]}")/scripts/tests/test_commit_self_heal_configure.sh" > "$LOG_DIR/storm_self_heal_test.log" 2>&1; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — self-heal configure self-test failed${RESET}"
        echo -e "${DIM}  → full output: $LOG_DIR/storm_self_heal_test.log${RESET}"
        exit 1
    fi
    echo -e "${DIM}✓ self-heal configure self-test passed${RESET}"
fi

# --- Per-OS PostgreSQL socket default self-test ---
# scripts/pg-default-connstr.sh picks the default STORM_PG_CONNSTR socket directory
# (Linux /var/run/postgresql, macOS /tmp). A commit touching only it, its two callers
# or dev-container.sh has no src/tests/cmake changes, so TOTAL_STEPS would be 0 and
# nothing would run — yet a wrong branch there silently skips every PG test and
# leaves the coverage gate stuck at ~99.8%. Same reasoning as the guards above, so
# it sits BEFORE the TOTAL_STEPS==0 early exit. Cheap pure-bash check.
if [[ -n "$STAGED_FILES" ]] \
   && grep -qE '^(commit\.sh|scripts/(pg-default-connstr|coverage-run-batched|dev-container)\.sh|scripts/tests/test_pg_default_connstr\.sh)$' <<< "$STAGED_FILES"; then
    if ! "$(dirname "${BASH_SOURCE[0]}")/scripts/tests/test_pg_default_connstr.sh"; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — per-OS PostgreSQL socket default self-test failed${RESET}"
        exit 1
    fi
    echo -e "${DIM}✓ per-OS PostgreSQL socket default self-test passed${RESET}"
fi

# --- ORM backend-neutrality guard ---
# src/orm/** and the generic db facades must name no concrete backend; the
# SQLite default lives only in src/storm_defaults.cppm. A stray
# `import storm_db_sqlite;` compiles cleanly, so nothing else catches it. Cheap
# pure-bash grep, run whenever a module unit or the checker itself is staged.
# Placed BEFORE the TOTAL_STEPS==0 early exit, same reasoning as the guards
# above: a commit touching only the checker or its self-test skips every step.
if [[ -n "$STAGED_FILES" ]] \
   && grep -qE '^(src/.*\.cppm|scripts/check-orm-backend-imports\.sh|scripts/tests/test_check_orm_backend_imports\.sh)$' <<< "$STAGED_FILES"; then
    if ! "$(dirname "${BASH_SOURCE[0]}")/scripts/check-orm-backend-imports.sh"; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — backend-neutral ORM module names a concrete backend${RESET}"
        exit 1
    fi
    if grep -qE '^scripts/(check-orm-backend-imports|tests/test_check_orm_backend_imports)\.sh$' <<< "$STAGED_FILES" \
       && ! "$(dirname "${BASH_SOURCE[0]}")/scripts/tests/test_check_orm_backend_imports.sh"; then
        echo ""
        echo -e "${RED}${BOLD} COMMIT BLOCKED — ORM backend-neutrality checker self-test failed${RESET}"
        exit 1
    fi
    echo -e "${DIM}✓ ORM backend-neutrality guard passed${RESET}"
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
#      the mock test binaries were never compiled. Building `--target storm` alone
#      (the pre-#489 behavior) produced the BMIs but not the mock binaries.
# So this prebuild configures release if needed, then builds what clang-tidy
# needs (BMIs + mock test binaries), self-healing a fresh/stale worktree for
# both the git-hook path and a manual `./commit.sh` run (#489). It builds those
# targets specifically rather than the full default set, which spent ~90% of a
# cold build on release test TUs clang-tidy never opens (#557) — see
# build_tidy_prereqs above for the target list, the measurements, and the
# fallbacks that keep both failure modes below covered.
RELEASE_BUILD_NINJA="build/release/build.ninja"
RUN_TIDY_BMI=false
if [[ "$RUN_TIDY" == true ]]; then
    RUN_TIDY_BMI=true
    ((TOTAL_STEPS++))
fi
[[ "$RUN_BENCH_RELEASE" == true ]] && ((TOTAL_STEPS++))
[[ "$RUN_TESTS" == true ]] && ((TOTAL_STEPS++))
[[ "$RUN_COVERAGE" == true ]] && ((TOTAL_STEPS++))

if [[ $TOTAL_STEPS -eq 0 ]]; then
    echo -e "${GREEN}✓ No checks needed for this commit.${RESET}"
    exit 0
fi

echo -e "${BOLD}Running $TOTAL_STEPS pre-commit checks...${RESET}"

# --- Steps 1-2: clang-format, cmake-format ---
# Both build a target of build/debug, so the first of them to run configures it
# if absent (configure_and_build, #674). Its full output — stderr included —
# lands in that step's log. stderr matters here: it carries cmake's
# message(WARNING)s, among them cmake/format.cmake's "cmake-format not found",
# which names the cause of the `unknown target 'cmake-format'` that step then
# hits (#643). On a cmake-only commit (the container path #643 was about)
# cmake-format is the only one that runs, so the warning is in its own log; when
# both run, it is in clang-format's.
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
    # needs: the module BMIs (for `import std;`/`import storm;`) and the mock test
    # binaries. build_tidy_prereqs builds those targets specifically rather than the full
    # default set (#557), and falls back to the full build whenever it cannot
    # prove that is enough. This is a no-op when release is already up to date
    # (the ninja build short-circuits), so the warm path stays fast. See the
    # RUN_TIDY_BMI comment above for the two failures this prevents.
    if [[ "$RUN_TIDY_BMI" == true ]]; then
        run_step "release prereqs (BMIs + mock binaries, for clang-tidy)" "$LOG_TIDY_BMI" \
            build_tidy_prereqs
    fi

    if [[ -n "$STORM_TIDY_FULL" ]]; then
        run_step "clang-tidy --full --fix" "$LOG_TIDY" ./scripts/run_clang_tidy.sh --full --fix || true
    else
        run_step "clang-tidy --diff --fix" "$LOG_TIDY" ./scripts/run_clang_tidy.sh --diff --fix || true
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

# --- Step 3b: bench release build ---
if [[ "$RUN_BENCH_RELEASE" == true ]]; then
    run_step "release build" "$LOG_RELEASE" \
        configure_and_build ninja-release "$RELEASE_BUILD_NINJA"
fi

# --- Step 4: tests ---
# Run the three gtest binaries directly instead of `ctest --preset ninja-debug`:
# ctest registers each of the ~3000 individual gtest cases as its own CTest test
# (gtest_discover_tests DISCOVERY_MODE PRE_TEST), so it re-launches the whole
# storm_tests process per test case. Measured 2026-08-27: ~260s via ctest vs
# ~33s running the binaries once each (7.8x) — see
# docs/internals/testing/TESTING.md#local-test-suite-speed. Self-heal first:
# configure build/debug if absent, then build the FULL default debug target set
# so every binary (main + both mocks) exists — otherwise a fresh/stale worktree
# would be missing them (#489). Both are no-ops on the warm path (config guarded
# by a file check; the ninja build short-circuits when up to date).
if [[ "$RUN_TESTS" == true ]]; then
    build_debug_then_test() {
        configure_and_build ninja-debug build/debug/build.ninja || return 1

        # Soft default (not an override): an already-exported STORM_PG_CONNSTR
        # wins, as in scripts/coverage-run-batched.sh — a hard override here would
        # silently point tests at a different DB than coverage uses in the same
        # commit.sh run. The two share the DEFAULT (per-OS socket directory, from
        # scripts/pg-default-connstr.sh) but not the empty-value rule: this treats
        # a set-but-empty value as unset, the coverage script passes it through.
        # CMakePresets.json's testPresets still carry the Linux path (`ctest
        # --preset` only).
        if [[ -z "${STORM_PG_CONNSTR:-}" ]]; then
            # shellcheck source=scripts/pg-default-connstr.sh
            source "$(dirname "${BASH_SOURCE[0]}")/scripts/pg-default-connstr.sh"
            STORM_PG_CONNSTR="$(storm_pg_default_connstr)" || return 1
        fi
        export STORM_PG_CONNSTR

        # PG unreachable means every PG-backed test GTEST_SKIPs (gtest still
        # exits 0), so the gate would otherwise report PASS having silently run
        # only the SQLite half. Warn loudly rather than fail: CLAUDE.md
        # documents PG-skip-if-not-running as accepted behavior, so this
        # matches scripts/coverage-run-batched.sh's WARN (not fail) policy.
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
        run_with_timeout 60 ./build/debug/tests/mock_sqlite/storm_mock_tests --gtest_brief=1 || overall=1
        run_with_timeout 60 ./build/debug/tests/mock_libpq/storm_pq_mock_tests --gtest_brief=1 || overall=1
        return $overall
    }
    run_step_live "tests (SQLite + PostgreSQL)" "$LOG_TESTS" build_debug_then_test || true
fi

# --- Step 5: coverage ---
if [[ "$RUN_COVERAGE" == true ]]; then
    # Format line numbers as compact ranges (e.g., "5, 10-15, 20")
    format_line_ranges() {
        local -n _lines=$1
        local result="" range_start="" range_end=""
        for ln in "${_lines[@]}"; do
            if [[ -z "$range_start" ]]; then
                range_start=$ln; range_end=$ln
            elif [[ $ln -eq $((range_end + 1)) ]]; then
                range_end=$ln
            else
                if [[ "$range_start" == "$range_end" ]]; then
                    result+="${result:+, }$range_start"
                else
                    result+="${result:+, }${range_start}-${range_end}"
                fi
                range_start=$ln; range_end=$ln
            fi
        done
        if [[ -n "$range_start" ]]; then
            if [[ "$range_start" == "$range_end" ]]; then
                result+="${result:+, }$range_start"
            else
                result+="${result:+, }${range_start}-${range_end}"
            fi
        fi
        echo "$result"
    }

    # Parse lcov file and display uncovered files + line ranges
    show_uncovered_lines() {
        local lcov_file="build/debug/coverage/coverage-filtered.lcov"
        if [[ ! -f "$lcov_file" ]]; then
            return
        fi

        echo ""
        echo -e "${BOLD}Uncovered lines:${RESET}"
        echo -e "${DIM}$(printf '%.0s─' {1..60})${RESET}"

        local current_file=""
        local uncovered_lines=()
        local has_uncovered=false

        # Flush accumulated uncovered lines for current file
        flush_file() {
            if [[ ${#uncovered_lines[@]} -gt 0 ]]; then
                local rel_path="${current_file#$PWD/}"
                echo -e "  ${YELLOW}${rel_path}${RESET}"
                echo -e "    Lines: ${RED}$(format_line_ranges uncovered_lines)${RESET}"
                has_uncovered=true
            fi
        }

        while IFS= read -r line; do
            case "$line" in
                SF:*)
                    flush_file
                    current_file="${line#SF:}"
                    uncovered_lines=()
                    ;;
                DA:*)
                    local da_data="${line#DA:}"
                    local ln_num="${da_data%%,*}"
                    local exec_count="${da_data#*,}"
                    if [[ "$exec_count" == "0" ]]; then
                        uncovered_lines+=("$ln_num")
                    fi
                    ;;
            esac
        done < "$lcov_file"
        flush_file

        if [[ "$has_uncovered" == false ]]; then
            echo -e "  ${DIM}(could not parse uncovered lines from lcov)${RESET}"
        fi

        echo ""
        echo -e "${DIM}For detailed HTML report:${RESET}"
        echo -e "${DIM}  cmake --build --preset ninja-debug-coverage --target coverage-html${RESET}"
        echo -e "${DIM}  open build/debug/coverage/html-filtered/index.html${RESET}"
    }

    # Run coverage as a compound check: clean + build + parse + threshold
    run_coverage_check() {
        # Self-heal an absent build/debug inside the step (#674), so a failed
        # configure is reported here rather than as a coverage build error.
        configure_if_missing ninja-debug build/debug/build.ninja || return 1

        # Clean stale profraw/profdata to prevent false uncovered lines
        cmake --build --preset ninja-debug-coverage --target coverage-clean > /dev/null 2>&1
        local output
        output=$(cmake --build --preset ninja-debug-coverage --target coverage 2>&1)
        local build_exit=$?

        if [[ $build_exit -ne 0 ]]; then
            echo "$output" | tail -20
            echo ""
            echo "Coverage build/analysis failed."
            return 1
        fi

        # Extract line coverage percentage
        local line_cov
        line_cov=$(echo "$output" | grep "lines\.\.\.\.\.\.\." | tail -1 | grep -oP '[0-9.]+(?=%)')

        if [[ -z "$line_cov" ]]; then
            echo ""
            echo "Could not parse line coverage from output."
            return 1
        fi

        if [[ "$line_cov" != "100.0" ]]; then
            echo -e "${RED}${BOLD}Line coverage: ${line_cov}% (required: 100.0%)${RESET}"
            show_uncovered_lines
            return 1
        fi

        echo "Line coverage: 100.0%"
        return 0
    }

    run_step_live "coverage (100% required)" "$LOG_COVERAGE" run_coverage_check || true
fi

# --- Final summary ---
print_summary

if [[ "$FAILED" == true ]]; then
    exit 1
fi
