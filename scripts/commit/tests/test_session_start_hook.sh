#!/bin/bash
# Tests for .claude/hooks/session-start.sh (issue #651).
#
# `git config core.hooksPath .githooks` — the only thing that makes
# .githooks/pre-commit (and therefore commit.sh: format, clang-tidy, tests,
# the 100% coverage gate) run at all — must happen on every session start.
# #651: the wiring used to live only in CMakeLists.txt, i.e. behind a
# successful cmake configure, which is precisely what a session with no
# ../clang-p2996 cannot do; such a session committed with no checks and no
# warning.
#
# Strategy: build a throwaway project dir (a real `git init` repo), run the real
# hook against it with a controlled environment, and assert on core.hooksPath
# and on stdout/stderr. Each scenario is a function named scenario_<tag>; the
# dispatcher below owns the temp-dir boilerplate.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
HOOK="$REPO_ROOT/.claude/hooks/session-start.sh"

# commit.sh runs this test from the pre-commit hook, where git exports
# GIT_INDEX_FILE (and, for some invocations, GIT_DIR). Left set, every `git`
# call below would act on the REAL repository instead of the throwaway one — the
# wiring scenarios would then read the real repo's core.hooksPath.
while IFS= read -r git_var; do
    unset "$git_var"
done < <(git rev-parse --local-env-vars)

PASS=0
FAIL=0
FAILED_TESTS=()

# Per-scenario state, populated by the dispatcher / run_hook.
TMP=""
PROJ=""
CURRENT_TAG=""
OUT=""
ERR=""
RC=0

fail() {
    local msg="$1"
    echo "  FAIL: $msg"
    FAIL=$((FAIL+1))
    # One entry per scenario, not per failing assertion — scenarios assert
    # several things and a repeated tag in the summary reads as several
    # scenarios failing.
    if [[ ${#FAILED_TESTS[@]} -eq 0 || "${FAILED_TESTS[-1]}" != "$CURRENT_TAG" ]]; then
        FAILED_TESTS+=("$CURRENT_TAG")
    fi
    return 0
}

pass() {
    local msg="$1"
    echo "  PASS: $msg"
    PASS=$((PASS+1))
    return 0
}

# --- Fixtures ---

# A throwaway project dir shaped like the repo: a git repo, a .githooks/
# pre-commit, and a copy of the hook under test at its real relative location
# (so the CLAUDE_PROJECT_DIR-less fallback resolves here).
make_project() {
    PROJ="$TMP/proj"
    mkdir -p "$PROJ/.claude/hooks" "$PROJ/.githooks"
    cp "$HOOK" "$PROJ/.claude/hooks/session-start.sh"
    chmod +x "$PROJ/.claude/hooks/session-start.sh"

    printf '#!/bin/bash\nexec ./commit.sh\n' > "$PROJ/.githooks/pre-commit"
    chmod +x "$PROJ/.githooks/pre-commit"

    git -C "$PROJ" init --quiet
    return 0
}

# Run the hook with a controlled environment. Args are extra `env` assignments;
# CLAUDE_PROJECT_DIR defaults to the fixture project, cwd to a directory that
# is NOT the project (a hook must not depend on the caller's cwd).
run_hook() {
    local script="${HOOK_SCRIPT:-$HOOK}"
    local out="$TMP/stdout" err="$TMP/stderr"

    ( cd "${HOOK_CWD:-$TMP}" && \
      env CLAUDE_PROJECT_DIR="$PROJ" \
          "$@" \
          bash "$script" ) > "$out" 2> "$err"
    RC=$?
    OUT="$(cat "$out")"
    ERR="$(cat "$err")"
    return 0
}

# --- Assertion helpers ---

assert_hooks_path_wired() {
    local what="$1" actual
    actual="$(git -C "$PROJ" config --get core.hooksPath || true)"
    if [[ "$actual" == ".githooks" ]]; then
        pass "$what"
    else
        fail "$what — core.hooksPath is '[$actual]', expected '.githooks'. stderr: $ERR"
    fi
    return 0
}

assert_stdout_empty() {
    local what="$1"
    if [[ -z "$OUT" ]]; then
        pass "$what"
    else
        fail "$what — stdout was not empty: [$OUT]"
    fi
    return 0
}

assert_rc_zero() {
    local what="$1"
    if [[ $RC -eq 0 ]]; then
        pass "$what"
    else
        fail "$what — expected exit 0, got $RC. stderr: $ERR"
    fi
    return 0
}

# --- Scenarios ---

scenario_wires_hooks() {
    make_project
    run_hook
    assert_rc_zero "exit 0"
    assert_hooks_path_wired "core.hooksPath wired"
    assert_stdout_empty "stdout stays clean when the wiring succeeds"
    return 0
}

scenario_idempotent_across_runs() {
    make_project
    run_hook
    run_hook
    assert_hooks_path_wired "core.hooksPath still correct after a second run"
    local count
    count="$(git -C "$PROJ" config --get-all core.hooksPath | wc -l)"
    if [[ "$count" -eq 1 ]]; then
        pass "no duplicate core.hooksPath entries accumulate"
    else
        fail "core.hooksPath has $count values, expected 1"
    fi
    return 0
}

scenario_corrects_a_stale_hooks_path() {
    make_project
    git -C "$PROJ" config core.hooksPath .git/hooks
    run_hook
    assert_hooks_path_wired "a pre-existing wrong core.hooksPath is corrected"
    return 0
}

# CLAUDE_PROJECT_DIR is what Claude Code passes; the fallback matters for a
# manual run. Both must target the repo, not the caller's cwd.
scenario_falls_back_to_own_location() {
    make_project
    HOOK_SCRIPT="$PROJ/.claude/hooks/session-start.sh" \
        run_hook CLAUDE_PROJECT_DIR=
    assert_rc_zero "exit 0 without CLAUDE_PROJECT_DIR"
    assert_hooks_path_wired "core.hooksPath wired via the script-location fallback"
    return 0
}

scenario_ignores_caller_cwd() {
    make_project
    mkdir -p "$TMP/elsewhere"
    git -C "$TMP/elsewhere" init --quiet
    HOOK_CWD="$TMP/elsewhere" run_hook
    assert_hooks_path_wired "the project repo is wired regardless of cwd"
    local stray
    stray="$(git -C "$TMP/elsewhere" config --get core.hooksPath || true)"
    if [[ -z "$stray" ]]; then
        pass "an unrelated repo at the caller's cwd is left alone"
    else
        fail "wired the wrong repo: cwd repo has core.hooksPath '$stray'"
    fi
    return 0
}

# A wiring failure must be LOUD (#651's whole point is that a missing gate
# looked identical to a passing one) but must not fail the session start. For a
# SessionStart hook at exit 0, stdout is what becomes session context, so the
# warning goes there as well as to stderr.
scenario_warns_when_not_a_git_repo() {
    PROJ="$TMP/not-a-repo"
    run_hook
    assert_rc_zero "exit 0 even when the wiring fails"
    if [[ "$ERR" == *"core.hooksPath"* && "$ERR" == *"commit.sh"* ]]; then
        pass "failure warns on stderr, naming core.hooksPath and commit.sh"
    else
        fail "no actionable warning on stderr: [$ERR]"
    fi
    if [[ "$OUT" == *"core.hooksPath"* && "$OUT" == *"commit.sh"* ]]; then
        pass "the warning also reaches stdout, i.e. the session context"
    else
        fail "warning missing from stdout, where a session would see it: [$OUT]"
    fi
    return 0
}

# `git -C <dir> config` walks UP to an enclosing repository, so a project dir
# that is not a repo root must NOT be allowed to re-point an unrelated
# ancestor's hooks — and must say so rather than reporting nothing.
scenario_refuses_to_wire_an_ancestor_repo() {
    local outer="$TMP/outer"
    mkdir -p "$outer"
    git -C "$outer" init --quiet
    PROJ="$outer/nested/project"
    run_hook
    assert_rc_zero "exit 0 when the project dir is not a repo root"
    local ancestor
    ancestor="$(git -C "$outer" config --get core.hooksPath || true)"
    if [[ -z "$ancestor" ]]; then
        pass "an enclosing repo's core.hooksPath is left untouched"
    else
        fail "wired the ancestor repo instead: core.hooksPath '$ancestor'"
    fi
    if [[ "$OUT" == *"not a git repository root"* ]]; then
        pass "the mismatch is reported, not silently skipped"
    else
        fail "no report of the non-root project dir: [$OUT]"
    fi
    return 0
}

# #651's other half: the hook is inert unless .claude/settings.json declares it.
# Deleting that block again must fail HERE — the CI job is named for the wiring,
# and a job that cannot see the declaration would report green with the hook
# never running, which is the exact regression this issue is about.
scenario_settings_json_declares_the_hook() {
    local settings="$REPO_ROOT/.claude/settings.json"
    if [[ ! -f "$settings" ]]; then
        fail ".claude/settings.json is missing"
        return 0
    fi
    if grep -q '"SessionStart"' "$settings"; then
        pass ".claude/settings.json declares a SessionStart hook"
    else
        fail "no SessionStart entry in .claude/settings.json — the hook never runs"
    fi
    if grep -q '\.claude/hooks/session-start\.sh' "$settings"; then
        pass "the declared command names this hook script"
    else
        fail "no SessionStart command names .claude/hooks/session-start.sh"
    fi
    if python3 -c "import json,sys; json.load(open('$settings'))" 2>/dev/null; then
        pass ".claude/settings.json is valid JSON"
    elif ! command -v python3 > /dev/null; then
        pass "JSON validity unchecked (no python3) — grep assertions still hold"
    else
        fail ".claude/settings.json is not valid JSON — Claude Code ignores it"
    fi
    return 0
}

# The value `.githooks` has two independent writers: this hook and
# CMakeLists.txt. They must not drift — a relocated hooks dir would leave every
# `cmake --preset` silently re-pointing core.hooksPath at the stale one.
scenario_cmake_writer_agrees_on_the_value() {
    if grep -q 'config core.hooksPath .githooks' "$REPO_ROOT/CMakeLists.txt"; then
        pass "CMakeLists.txt sets the same core.hooksPath value as the hook"
    else
        fail "CMakeLists.txt no longer sets core.hooksPath to .githooks — the two writers disagree"
    fi
    if grep -q 'config core.hooksPath .githooks' "$HOOK"; then
        pass "the hook itself still sets .githooks"
    else
        fail "the hook no longer sets core.hooksPath to .githooks"
    fi
    return 0
}

# What the wiring is FOR: git must actually run .githooks/pre-commit after it,
# and a hook that fails must abort the commit. Guards the relative-path
# resolution of core.hooksPath, which is what makes one value work across
# worktrees.
scenario_wired_repo_actually_runs_the_hook() {
    make_project
    printf '#!/bin/bash\ntouch "$PWD/hook-ran"\nexit 1\n' > "$PROJ/.githooks/pre-commit"
    chmod +x "$PROJ/.githooks/pre-commit"
    run_hook

    git -C "$PROJ" config user.email storm-test@example.com
    git -C "$PROJ" config user.name "Storm Test"
    git -C "$PROJ" config commit.gpgsign false
    echo 'int main() { return 0; }' > "$PROJ/main.cpp"
    git -C "$PROJ" add main.cpp
    if git -C "$PROJ" commit -m "staged C++ change" >/dev/null 2>&1; then
        fail "commit succeeded despite a failing pre-commit hook"
    else
        pass "a failing pre-commit hook aborts the commit"
    fi
    if [[ -f "$PROJ/hook-ran" ]]; then
        pass ".githooks/pre-commit actually ran (relative hooksPath resolves)"
    else
        fail ".githooks/pre-commit never ran after wiring"
    fi
    return 0
}

# The wiring is worthless if it points at something git cannot execute.
scenario_real_repo_hook_is_executable() {
    if [[ -x "$REPO_ROOT/.githooks/pre-commit" ]]; then
        pass "the repo's .githooks/pre-commit is executable"
    else
        fail "the repo's .githooks/pre-commit is missing or not executable"
    fi
    if [[ -x "$REPO_ROOT/commit.sh" ]]; then
        pass "the repo's commit.sh is executable"
    else
        fail "the repo's commit.sh is missing or not executable"
    fi
    return 0
}

SCENARIOS=(
    wires_hooks
    idempotent_across_runs
    corrects_a_stale_hooks_path
    falls_back_to_own_location
    ignores_caller_cwd
    warns_when_not_a_git_repo
    refuses_to_wire_an_ancestor_repo
    wired_repo_actually_runs_the_hook
    settings_json_declares_the_hook
    cmake_writer_agrees_on_the_value
    real_repo_hook_is_executable
)

echo "Testing .claude/hooks/session-start.sh"
echo "================================================"

if [[ ! -f "$HOOK" ]]; then
    echo "FATAL: $HOOK is missing"
    exit 1
fi

for tag in "${SCENARIOS[@]}"; do
    CURRENT_TAG="$tag"
    echo ""
    echo "Scenario: $tag"
    TMP="$(mktemp -d)"
    # bash keeps a `VAR=val func` assignment in effect after the function
    # returns, so clear the per-scenario overrides here rather than in a
    # subshell (which would also swallow the PASS/FAIL counters).
    unset HOOK_SCRIPT HOOK_CWD
    # A typo'd SCENARIOS entry (no matching scenario_<tag> function) must fail
    # loudly, not silently drop the scenario from the run — a dispatch that
    # never happened must never read as a pass.
    if ! declare -F "scenario_$tag" > /dev/null; then
        fail "scenario_$tag is not defined — check SCENARIOS for a typo"
        rm -rf "$TMP"
        continue
    fi
    "scenario_$tag"
    rm -rf "$TMP"
done

echo ""
echo "================================================"
echo "Passed: $PASS, Failed: $FAIL"

if [[ $FAIL -gt 0 ]]; then
    echo "Failed scenarios: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
