#!/usr/bin/env python3
"""Run commit.sh's staged-file guards (issue #688).

Each guard is "if a staged file matches TRIGGER, run these checks and BLOCK the
commit when one fails". There used to be one copy-pasted bash block per guard;
they are now rows of GUARDS below and share one runner. Stdlib only, no syntax
newer than Python 3.7, so macOS system python3 runs it.

    printf '%s\\n' "$staged" | scripts/pre_commit_guards.py [--log-file NAME]  3> logfile
    (commit.sh opens fd 3 on the log file; NAME is only shown to the user)
    scripts/pre_commit_guards.py --all     (CI: every guard, every step, stdin ignored)

Exit: 0 = every triggered guard passed (or none triggered), 1 = a guard blocked.

WHY CI RUNS --all (#708). The hook is local and bypassable — an unwired clone
(#651), a web edit, a merge — and it only runs the guards a commit's files
trigger, so a broken shared helper can pass the commit that broke it. CI is what
checks the merged tree, so it runs the whole table from one job. A new guard is
therefore covered in CI by adding its row here, with no workflow edit.

WHY THIS RUNS BEFORE commit.sh's TOTAL_STEPS==0 EARLY EXIT. A commit that
touches only a guard's own files has no C++ or cmake, so every other step is
skipped and TOTAL_STEPS would be 0 — exactly the commit that can break the
guarded property. Gating the guards on the step count would make each one miss
its own failure case. commit.sh therefore calls this before counting steps.

Each guard also fires when commit.sh itself, or a file its self-test copies into
a throwaway repo, is staged, because changing those can break the self-test.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
from typing import NamedTuple, Optional, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parent.parent
# The descriptor commit.sh opens on the self-heal log (`3> logfile`). A constant, not
# an option: no value from the command line ever selects a file or descriptor.
LOG_FD = 3

RED = "\033[0;31m"
BOLD = "\033[1m"
DIM = "\033[2m"
RESET = "\033[0m"


class Step(NamedTuple):
    trigger: Optional[str]  # None = runs whenever the guard is triggered
    command: Sequence[str]
    blocked: str            # text after "COMMIT BLOCKED — " when it fails


class Guard(NamedTuple):
    name: str
    trigger: str            # regex matched against each staged path
    steps: Tuple[Step, ...]
    ok: str                 # text after "✓ " when every step passed
    # Step output goes to the LOG_FD stream, not the terminal. All logged guards share
    # that one stream (commit.sh opens a single fd), so a second one needs its own fd.
    log: bool = False


def script(relative):
    return [str(ROOT / relative)]


# --- agent frontmatter (#543) ---
# A multi-line `description:` makes Claude Code drop the agent SILENTLY — the bug
# that left storm-sql-reviewer and storm-buildsystem-reviewer undispatchable from
# the day they merged. The self-test runs first so a broken checker is not
# reported as broken agents.
AGENT_FRONTMATTER = Guard(
    "agent frontmatter",
    r"^(\.claude/agents/.*\.md|scripts/(tests/test_)?check_agent_frontmatter\.py)$",
    (
        Step(r"^scripts/(tests/test_)?check_agent_frontmatter\.py$",
             script("scripts/tests/test_check_agent_frontmatter.py"),
             "agent frontmatter self-test failed"),
        Step(None, script("scripts/check_agent_frontmatter.py"), "invalid agent frontmatter"),
    ),
    "agent frontmatter valid",
)

# --- clang-tidy skip-list (#550) ---
# scripts/lib/clang_tidy_skiplist.sh is the one list both run_clang_tidy.sh --diff
# and the weekly --all sweep consult for files clang-tidy cannot parse standalone.
TIDY_SKIPLIST = Guard(
    "clang-tidy skip-list",
    r"^scripts/(lib/clang_tidy_skiplist\.sh|run_clang_tidy\.sh|tests/test_run_clang_tidy_skiplist\.sh)$",
    (Step(None, script("scripts/tests/test_run_clang_tidy_skiplist.sh"),
          "clang-tidy skip-list self-test failed"),),
    "clang-tidy skip-list self-test passed",
)

# --- SessionStart hook wiring (#651) ---
# .claude/hooks/session-start-docker.sh sets core.hooksPath, the only reason
# commit.sh runs at all in a clone that never configures a build. Its wiring must
# stay above the hook's Docker bail-outs, or it goes missing in exactly the
# sessions those bail-outs return from.
SESSION_START_HOOK = Guard(
    "SessionStart hook wiring",
    r"^(\.claude/(hooks/session-start-docker\.sh|settings\.json)|scripts/tests/test_session_start_hook\.sh)$",
    (Step(None, script("scripts/tests/test_session_start_hook.sh"),
          "SessionStart hook wiring self-test failed"),),
    "SessionStart hook wiring self-test passed",
)

# --- self-heal configure (#674) ---
# The configure_if_missing calls must fail their step with the CMake error in its
# log; swallowed, a failed configure resurfaced as a misleading ninja error. The
# self-test runs commit.sh against stub cmake/ninja in a throwaway repo, so every
# file it copies in is a trigger too.
SELF_HEAL = Guard(
    "self-heal configure",
    r"^(commit\.sh|scripts/(detect-changes|pg-default-connstr)\.sh|scripts/pre_commit_guards\.py"
    r"|scripts/(uncovered_lines|smart_skip|tidy_prereqs)\.py|scripts/lib/(clang_tidy_skiplist|line_coverage)\.sh"
    r"|scripts/tests/test_commit_self_heal_configure\.sh)$",
    (Step(None, script("scripts/tests/test_commit_self_heal_configure.sh"),
          "self-heal configure self-test failed"),),
    "self-heal configure self-test passed",
    log=True,
)

# --- cmake-format stub target (#690) ---
# cmake/format.cmake's stub `cmake-format` target is what makes the cmake-format step
# name a missing tool in its own log. Needs plain cmake only.
CMAKE_FORMAT_TARGET = Guard(
    "cmake-format target",
    r"^(cmake/format\.cmake|scripts/tests/test_cmake_format_target\.sh)$",
    (Step(None, script("scripts/tests/test_cmake_format_target.sh"),
          "cmake-format target self-test failed"),),
    "cmake-format target self-test passed",
)

# --- PyYAML configure check (#703) ---
# cmake/pyyaml.cmake turns a Python without PyYAML into a configure-time error instead of a
# mid-build `No module named 'yaml'`. Needs plain cmake only.
PYYAML_CHECK = Guard(
    "PyYAML configure check",
    r"^(cmake/pyyaml\.cmake|scripts/tests/test_pyyaml_check\.py)$",
    (Step(None, script("scripts/tests/test_pyyaml_check.py"),
          "PyYAML configure check self-test failed"),),
    "PyYAML configure check self-test passed",
)

# --- CPM broken-checkout configure check (#705) ---
# cmake/cpm-cache.cmake turns a CPM cache entry left empty by an interrupted clone into a
# configure-time error naming the directory to delete. Needs plain cmake only.
CPM_CACHE_CHECK = Guard(
    "CPM cache configure check",
    r"^(cmake/cpm-cache\.cmake|scripts/tests/test_cpm_cache_check\.py)$",
    (Step(None, script("scripts/tests/test_cpm_cache_check.py"),
          "CPM cache configure check self-test failed"),),
    "CPM cache configure check self-test passed",
)

# --- CPM cold-cache fetch (#709) ---
# A CPM/FetchContent pin without a final GIT_SHALLOW FALSE gets CPM's depth-1 clone,
# which cannot reach a non-branch-tip tag: a fresh worktree fails to configure while
# every warm cache stays green. Pure bash + perl + git; broad trigger, runs in ms.
CPM_COLD_CACHE = Guard(
    "CPM cold-cache fetch",
    r"^(cmake/.*\.cmake|(.+/)?CMakeLists\.txt|scripts/tests/test_cpm_cold_cache\.sh)$",
    (Step(None, script("scripts/tests/test_cpm_cold_cache.sh"),
          "CPM cold-cache fetch policy self-test failed"),),
    "CPM cold-cache fetch policy self-test passed",
)

# --- per-OS PostgreSQL socket default ---
# A wrong branch in scripts/pg-default-connstr.sh silently skips every PG test and
# leaves the coverage gate stuck at ~99.8%.
PG_SOCKET_DEFAULT = Guard(
    "PostgreSQL socket default",
    r"^(commit\.sh|scripts/(pg-default-connstr|coverage-run-batched|dev-container)\.sh"
    r"|scripts/tests/test_pg_default_connstr\.sh)$",
    (Step(None, script("scripts/tests/test_pg_default_connstr.sh"),
          "per-OS PostgreSQL socket default self-test failed"),),
    "per-OS PostgreSQL socket default self-test passed",
)

# --- line-coverage parser (#680) ---
# scripts/lib/line_coverage.sh reads the percentage the coverage gate compares to
# 100.0. A GNU-only grep flag there breaks the gate on macOS BSD grep while CI's
# GNU grep stays green. The self-test also scans commit.sh and the scripts it runs
# or sources for bash-4-only constructs (#689), so staging any of them triggers it.
LINE_COVERAGE = Guard(
    "line-coverage parser",
    r"^(commit\.sh|scripts/lib/[^/]+\.sh"
    r"|scripts/(detect-changes|pg-default-connstr|run_clang_tidy|check-orm-backend-imports|coverage-run-batched)\.sh"
    r"|scripts/tests/test_line_coverage\.sh)$",
    (Step(None, script("scripts/tests/test_line_coverage.sh"),
          "line-coverage parser self-test failed"),),
    "line-coverage parser self-test passed",
)

# --- uncovered-lines helper (#688) ---
UNCOVERED_LINES = Guard(
    "uncovered-lines helper",
    r"^(commit\.sh|scripts/(tests/test_)?uncovered_lines\.py)$",
    (Step(None, script("scripts/tests/test_uncovered_lines.py"),
          "uncovered-lines helper self-test failed"),),
    "uncovered-lines helper self-test passed",
)

# --- smart-skip table (#688) ---
# scripts/smart_skip.py decides which steps run for a staged-file set; a wrong
# row skips a gate silently, and the commit still looks green.
SMART_SKIP = Guard(
    "smart-skip table",
    r"^(commit\.sh|scripts/(detect-changes|smart_skip)\.(sh|py)|scripts/tests/test_(smart_skip|detect_changes)\.(py|sh))$",
    (
        Step(None, script("scripts/tests/test_smart_skip.py"), "smart-skip table self-test failed"),
        Step(r"^scripts/(detect-changes\.sh|tests/test_detect_changes\.sh)$",
             script("scripts/tests/test_detect_changes.sh"), "change-detection self-test failed"),
    ),
    "smart-skip table self-test passed",
)

# --- clang-tidy prerequisites verdict (#557, #688) ---
# scripts/tidy_prereqs.py fails CLOSED: a wrong `complete` skips the full release
# build and clang-tidy --fix runs on a partial parse with false warnings.
TIDY_PREREQS = Guard(
    "clang-tidy prerequisites",
    r"^(commit\.sh|scripts/(tests/test_)?tidy_prereqs\.py)$",
    (Step(None, script("scripts/tests/test_tidy_prereqs.py"), "clang-tidy prerequisites self-test failed"),),
    "clang-tidy prerequisites self-test passed",
)

# --- ORM backend-neutrality ---
# src/orm/** and the generic db facades must name no concrete backend; the SQLite
# default lives only in src/storm_defaults.cppm. A stray `import storm_db_sqlite;`
# compiles cleanly, so nothing else catches it.
ORM_BACKEND_IMPORTS = Guard(
    "ORM backend neutrality",
    r"^(src/.*\.cppm|scripts/check-orm-backend-imports\.sh|scripts/tests/test_check_orm_backend_imports\.sh)$",
    (
        Step(r"^scripts/(check-orm-backend-imports|tests/test_check_orm_backend_imports)\.sh$",
             script("scripts/tests/test_check_orm_backend_imports.sh"),
             "ORM backend-neutrality checker self-test failed"),
        Step(None, script("scripts/check-orm-backend-imports.sh"),
             "backend-neutral ORM module names a concrete backend"),
    ),
    "ORM backend-neutrality guard passed",
)

# --- this runner (#688) ---
PRE_COMMIT_GUARDS = Guard(
    "pre-commit guards",
    r"^(commit\.sh|scripts/(tests/test_)?pre_commit_guards\.py)$",
    (Step(None, script("scripts/tests/test_pre_commit_guards.py"),
          "pre-commit guards self-test failed"),),
    "pre-commit guards self-test passed",
)

# --- storm-ci digest pins (#530, #708) ---
# Every workflow running in the storm-ci image pins it by digest; drift silently
# runs two workflows on two clang-p2996 builds (#498). Was ci.yml's inline
# `digest-guard` job, so a pin edit was only caught after the push.
CI_DIGESTS = Guard(
    "storm-ci digest pins",
    r"^(\.github/workflows/[^/]+\.ya?ml|scripts/(tests/test_)?check_ci_digests\.py)$",
    (
        Step(r"^scripts/(tests/test_)?check_ci_digests\.py$",
             script("scripts/tests/test_check_ci_digests.py"), "storm-ci digest checker self-test failed"),
        Step(None, script("scripts/check_ci_digests.py"), "storm-ci digest pins disagree"),
    ),
    "storm-ci digest pins agree",
)

# --- script self-tests that ran nowhere before #708 ---
# Each pins a tool whose bugs are invisible in its output: a serialised coverage
# batch (#268), or a plausible-but-wrong compile-time number in COMPILE_TIME.md.
COVERAGE_PARALLEL = Guard(
    "coverage batch parallelism",
    r"^scripts/(coverage-run-batched\.sh|pg-default-connstr\.sh|tests/test_coverage_parallel\.sh)$",
    (Step(None, script("scripts/tests/test_coverage_parallel.sh"),
          "coverage batch parallelism self-test failed"),),
    "coverage batch parallelism self-test passed",
)

NINJALOG_STATS = Guard(
    "ninjalog stats",
    r"^scripts/(ninjalog_stats\.py|tests/test_ninjalog_stats\.sh)$",
    (Step(None, script("scripts/tests/test_ninjalog_stats.sh"), "ninjalog stats self-test failed"),),
    "ninjalog stats self-test passed",
)

TIME_TRACE = Guard(
    "time-trace capture",
    r"^scripts/(time_trace\.py|tests/test_time_trace\.sh)$",
    (Step(None, script("scripts/tests/test_time_trace.sh"), "time-trace capture self-test failed"),),
    "time-trace capture self-test passed",
)

# Its self-test also parses every tests/**/*.cpp, but staging one does not trigger
# it: that would run it on nearly every test commit. CI's --all is the backstop.
TU_ABLATION = Guard(
    "TU ablation transforms",
    r"^scripts/(tu_ablation\.py|tests/test_tu_ablation\.sh)$",
    (Step(None, script("scripts/tests/test_tu_ablation.sh"), "TU ablation transforms self-test failed"),),
    "TU ablation transforms self-test passed",
)

# --- SonarCloud gate (#711) ---
# ci.yml's sonar-gate job; `CI gate` requires it, so a bug that reads a pending
# or stale Sonar run as green lets a red quality gate merge.
SONAR_GATE = Guard(
    "SonarCloud gate",
    r"^scripts/(tests/test_)?sonar_gate\.py$",
    (Step(None, script("scripts/tests/test_sonar_gate.py"), "SonarCloud gate self-test failed"),),
    "SonarCloud gate self-test passed",
)

GUARDS = (
    AGENT_FRONTMATTER, TIDY_SKIPLIST, SESSION_START_HOOK, SELF_HEAL, CMAKE_FORMAT_TARGET, PYYAML_CHECK,
    CPM_CACHE_CHECK, CPM_COLD_CACHE, PG_SOCKET_DEFAULT,
    LINE_COVERAGE, UNCOVERED_LINES, SMART_SKIP, TIDY_PREREQS, ORM_BACKEND_IMPORTS, PRE_COMMIT_GUARDS,
    CI_DIGESTS, COVERAGE_PARALLEL, NINJALOG_STATS, TIME_TRACE, TU_ABLATION, SONAR_GATE,
)

# scripts/tests/ self-tests deliberately run by no guard, with where they DO run.
# Every other self-test must be a step above (the table test enforces it), since a
# self-test outside the table is one CI never runs.
NOT_GUARDED = {
    "test_libcxx_modules_symlink.sh":
        "needs the storm-ci image's clang-p2996 layout and CMake 3.30+; "
        "ci.yml's clang-p2996-host-layout job runs it",
}

COMMIT_BLOCKED = "COMMIT BLOCKED"
GUARD_FAILED = "GUARD FAILED"


def any_match(pattern, paths):
    return any(re.search(pattern, path) for path in paths)


def triggered_guards(staged):
    return [guard for guard in GUARDS if any_match(guard.trigger, staged)]


def triggered_steps(guard, staged):
    return [step for step in guard.steps if step.trigger is None or any_match(step.trigger, staged)]


def run_step(step, log_stream):
    """True when the step's command exits 0. An unrunnable command is a failure."""
    try:
        if log_stream is None:
            return subprocess.call(list(step.command)) == 0
        log_stream.flush()
        return subprocess.call(list(step.command), stdout=log_stream, stderr=subprocess.STDOUT) == 0
    except OSError:
        return False


def block(out, verdict, message, log_label):
    out.write("\n%s%s %s — %s%s\n" % (RED, BOLD, verdict, message, RESET))
    if log_label:
        out.write("%s  → full output: %s%s\n" % (DIM, log_label, RESET))
    out.flush()


def run_guard(guard, steps, out, verdict, stream=None, log_label=""):
    """True when every step passed. A failed step ends the guard: its self-test
    runs before its checker, and a broken checker says nothing about the tree."""
    for step in steps:
        out.flush()
        if not run_step(step, stream):
            block(out, verdict, step.blocked, log_label if stream is not None else "")
            return False
    out.write("%s✓ %s%s\n" % (DIM, guard.ok, RESET))
    out.flush()
    return True


def run_guards(guards, staged, out, log_stream=None, log_label=""):
    """Run every triggered guard in order; stop at the first failure. 0 or 1.

    log_stream receives the output of guards flagged `log` (all their steps append
    to it); log_label is only the text shown to the user, never opened.
    """
    for guard in guards:
        if not any_match(guard.trigger, staged):
            continue
        stream = log_stream if guard.log else None
        if not run_guard(guard, triggered_steps(guard, staged), out, COMMIT_BLOCKED, stream, log_label):
            return 1
    return 0


def run_all_guards(guards, out):
    """--all: every guard and every step, whatever is staged. 0 or 1.

    Unlike run_guards it does not stop at the first failed guard, so one CI run
    reports every failure. Output goes to the terminal: there is no log fd in CI.
    """
    failed = [guard.name for guard in guards if not run_guard(guard, guard.steps, out, GUARD_FAILED)]
    if not failed:
        return 0
    out.write("\n%s%s%d guard(s) failed: %s%s\n" % (RED, BOLD, len(failed), ", ".join(failed), RESET))
    return 1


def open_log_fd():
    """A writable stream on LOG_FD, or None if commit.sh did not open it."""
    try:
        return os.fdopen(LOG_FD, "w", closefd=False)
    except (OSError, ValueError):
        return None


def main(argv):
    parser = argparse.ArgumentParser(description="Run commit.sh's staged-file guards.")
    parser.add_argument("--log-file", default="",
                        help="enable guard logging to fd %d; the name is shown in the BLOCK message, never opened" % LOG_FD)
    parser.add_argument("--all", action="store_true",
                        help="run every guard and step regardless of staged files; stdin is not read (CI)")
    args = parser.parse_args(argv[1:])
    if args.all:
        return run_all_guards(GUARDS, sys.stdout)
    staged = [line for line in sys.stdin.read().split("\n") if line]
    return run_guards(GUARDS, staged, sys.stdout, open_log_fd() if args.log_file else None, args.log_file)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
