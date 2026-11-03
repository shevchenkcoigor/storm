#!/bin/bash
# Tests for .claude/hooks/session-start-docker.sh (issues #651, #628).
#
# The hook has two jobs and the ORDER between them is the thing under test.
# `git config core.hooksPath .githooks` — the only thing that makes
# .githooks/pre-commit (and therefore commit.sh: format, clang-tidy, tests,
# the 100% coverage gate) run at all — must happen UNCONDITIONALLY, before
# every early exit the Docker provisioning half takes. #651: the wiring used to
# live only in CMakeLists.txt, i.e. behind a successful cmake configure, which
# is precisely what a session with no ../clang-p2996 cannot do; such a session
# committed with no checks and no warning. Moving an early exit above the
# wiring reintroduces exactly that, so each exit path gets its own scenario.
#
# Strategy: build a throwaway project dir (a real `git init` repo plus a stub
# scripts/dev-container.sh, so no container is ever provisioned), run the real
# hook against it with a controlled environment, and assert on core.hooksPath,
# on stdout (Claude Code's protocol channel — must stay clean), and on whether
# the stub was called. Each scenario is a function named scenario_<tag>; the
# dispatcher below owns the temp-dir boilerplate.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
HOOK="$REPO_ROOT/.claude/hooks/session-start-docker.sh"

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
# pre-commit, a copy of the hook under test at its real relative location (so
# the CLAUDE_PROJECT_DIR-less fallback resolves here), and a dev-container.sh
# stub that records its arguments instead of provisioning anything.
make_project() {
    PROJ="$TMP/proj"
    mkdir -p "$PROJ/.claude/hooks" "$PROJ/.githooks" "$PROJ/scripts"
    cp "$HOOK" "$PROJ/.claude/hooks/session-start-docker.sh"
    chmod +x "$PROJ/.claude/hooks/session-start-docker.sh"

    printf '#!/bin/bash\nexec ./commit.sh\n' > "$PROJ/.githooks/pre-commit"
    chmod +x "$PROJ/.githooks/pre-commit"

    cat > "$PROJ/scripts/dev-container.sh" <<'STUB'
#!/bin/bash
echo "$@" >> "$STUB_CALLS"
STUB
    chmod +x "$PROJ/scripts/dev-container.sh"

    git -C "$PROJ" init --quiet
    return 0
}

# A PATH containing only what the hook needs, so `command -v docker` misses.
# git and docker live in the same directory on most images, hence symlinking
# the wanted binaries in rather than pruning entries out of $PATH.
make_dockerless_path() {
    mkdir -p "$TMP/bin"
    ln -s "$(command -v bash)" "$TMP/bin/bash"
    ln -s "$(command -v git)" "$TMP/bin/git"
    echo "$TMP/bin"
    return 0
}

# A PATH whose `docker` is a stub, so the Docker branch is reachable on a
# machine (or CI runner) that has no real docker installed.
make_stub_docker_path() {
    mkdir -p "$TMP/bin"
    printf '#!/bin/bash\nexit 0\n' > "$TMP/bin/docker"
    chmod +x "$TMP/bin/docker"
    echo "$TMP/bin:$PATH"
    return 0
}

# Run the hook with a controlled environment. Args are extra `env` assignments;
# CLAUDE_PROJECT_DIR defaults to the fixture project, cwd to a directory that
# is NOT the project (a hook must not depend on the caller's cwd).
run_hook() {
    local script="${HOOK_SCRIPT:-$HOOK}"
    local out="$TMP/stdout" err="$TMP/stderr"
    STUB_CALLS="$TMP/dev-container-calls"

    ( cd "${HOOK_CWD:-$TMP}" && \
      env STUB_CALLS="$STUB_CALLS" \
          CLAUDE_PROJECT_DIR="$PROJ" \
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

assert_stub_not_called() {
    local what="$1"
    if [[ ! -s "$TMP/dev-container-calls" ]]; then
        pass "$what"
    else
        fail "$what — dev-container.sh was called with: $(cat "$TMP/dev-container-calls")"
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

# Every early exit below must be reached only AFTER the wiring. One scenario
# per exit, so a reordering that re-breaks #651 fails here rather than in a
# future session's silently ungated commit.

scenario_wires_hooks_when_not_remote() {
    make_project
    run_hook CLAUDE_CODE_REMOTE=
    assert_rc_zero "exit 0 in a non-remote session"
    assert_hooks_path_wired "core.hooksPath wired in a non-remote session"
    assert_stub_not_called "no container provisioned in a non-remote session"
    assert_stdout_empty "stdout stays clean (no async JSON) when not remote"
    return 0
}

scenario_wires_hooks_when_toolchain_present() {
    make_project
    mkdir -p "$TMP/clang-p2996"   # sibling of $PROJ — the "has toolchain" tell
    run_hook CLAUDE_CODE_REMOTE=true
    assert_rc_zero "exit 0 when ../clang-p2996 exists"
    assert_hooks_path_wired "core.hooksPath wired when ../clang-p2996 exists"
    assert_stub_not_called "no container provisioned when the toolchain is native"
    return 0
}

scenario_wires_hooks_when_docker_missing() {
    make_project
    local bare_path
    bare_path="$(make_dockerless_path)"
    run_hook CLAUDE_CODE_REMOTE=true PATH="$bare_path"
    assert_rc_zero "exit 0 when docker is unavailable"
    assert_hooks_path_wired "core.hooksPath wired when docker is unavailable"
    assert_stub_not_called "no container provisioned without docker"
    return 0
}

# The one path that does provision: wiring must still have happened, and the
# async JSON must still be the only thing on stdout.
scenario_wires_hooks_on_provisioning_path() {
    make_project
    local docker_path
    docker_path="$(make_stub_docker_path)"
    run_hook CLAUDE_CODE_REMOTE=true PATH="$docker_path"
    assert_rc_zero "exit 0 on the provisioning path"
    assert_hooks_path_wired "core.hooksPath wired on the provisioning path"
    # Exact, not a substring: stdout is parsed as the hook's protocol
    # response, so one stray line on THIS path breaks it while an unrelated
    # scenario's assert_stdout_empty stays green.
    if [[ "$OUT" == '{"async": true, "asyncTimeout": 600000}' ]]; then
        pass "stdout on the provisioning path is exactly the async JSON"
    else
        fail "stdout is not exactly the async JSON: [$OUT]"
    fi
    if grep -qx "up" "$TMP/dev-container-calls" 2>/dev/null; then
        pass "dev-container.sh invoked with 'up'"
    else
        fail "dev-container.sh not invoked with 'up' (got: $(cat "$TMP/dev-container-calls" 2>/dev/null))"
    fi
    return 0
}

scenario_idempotent_across_runs() {
    make_project
    run_hook CLAUDE_CODE_REMOTE=
    run_hook CLAUDE_CODE_REMOTE=
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
    run_hook CLAUDE_CODE_REMOTE=
    assert_hooks_path_wired "a pre-existing wrong core.hooksPath is corrected"
    return 0
}

# CLAUDE_PROJECT_DIR is what Claude Code passes; the fallback matters for a
# manual run. Both must target the repo, not the caller's cwd.
scenario_falls_back_to_own_location() {
    make_project
    HOOK_SCRIPT="$PROJ/.claude/hooks/session-start-docker.sh" \
        run_hook CLAUDE_CODE_REMOTE= CLAUDE_PROJECT_DIR=
    assert_rc_zero "exit 0 without CLAUDE_PROJECT_DIR"
    assert_hooks_path_wired "core.hooksPath wired via the script-location fallback"
    return 0
}

scenario_ignores_caller_cwd() {
    make_project
    mkdir -p "$TMP/elsewhere"
    git -C "$TMP/elsewhere" init --quiet
    HOOK_CWD="$TMP/elsewhere" run_hook CLAUDE_CODE_REMOTE=
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
# warning goes there as well as to stderr on every path whose stdout is free.
scenario_warns_when_not_a_git_repo() {
    PROJ="$TMP/not-a-repo"
    mkdir -p "$PROJ/scripts"
    run_hook CLAUDE_CODE_REMOTE=
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
    mkdir -p "$PROJ/scripts"
    run_hook CLAUDE_CODE_REMOTE=
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

# The warning must never break the provisioning path's stdout contract: there
# the protocol JSON is the only thing allowed, so stderr is the only channel.
scenario_warning_never_breaks_the_async_json() {
    make_project
    rm -rf "$PROJ/.git"          # force the wiring to fail
    local docker_path
    docker_path="$(make_stub_docker_path)"
    run_hook CLAUDE_CODE_REMOTE=true PATH="$docker_path"
    if [[ "$OUT" == '{"async": true, "asyncTimeout": 600000}' ]]; then
        pass "stdout stays exactly the async JSON when the wiring failed"
    else
        fail "warning leaked into the protocol channel: [$OUT]"
    fi
    if [[ "$ERR" == *"commit.sh"* ]]; then
        pass "the warning still goes to stderr on the provisioning path"
    else
        fail "wiring failure went unreported entirely: [$ERR]"
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
    if grep -q '\.claude/hooks/session-start-docker\.sh' "$settings"; then
        pass "the declared command names this hook script"
    else
        fail "no SessionStart command names .claude/hooks/session-start-docker.sh"
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
    run_hook CLAUDE_CODE_REMOTE=

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
    wires_hooks_when_not_remote
    wires_hooks_when_toolchain_present
    wires_hooks_when_docker_missing
    wires_hooks_on_provisioning_path
    idempotent_across_runs
    corrects_a_stale_hooks_path
    falls_back_to_own_location
    ignores_caller_cwd
    warns_when_not_a_git_repo
    refuses_to_wire_an_ancestor_repo
    warning_never_breaks_the_async_json
    wired_repo_actually_runs_the_hook
    settings_json_declares_the_hook
    cmake_writer_agrees_on_the_value
    real_repo_hook_is_executable
)

echo "Testing .claude/hooks/session-start-docker.sh"
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
