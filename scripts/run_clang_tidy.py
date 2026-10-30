#!/usr/bin/env python3
"""Run clang-tidy. Three modes - pick one (or none, default is --diff) (issue #692).

Port of the shell runner of the same name (retired in #692); same CLI, exit codes and messages.

  --diff   (default)  Diff-mode - only warnings on lines touched by the staged
                      commit (git diff --cached) block. Pre-existing warnings on
                      other lines are out of scope. This is the pre-commit default.
  --full              Full-file scan, staged files only. Block on any warning in
                      any staged file (including pre-existing drift). Used to be
                      the default before Issue #262.
  --all               Full-file scan, ALL C++ files. Used by the scheduled weekly CI
                      sweep to detect accumulated drift.

Prerequisites:
  - Release build with compile_commands.json: cmake --preset ninja-release
  - .clang-tidy file in project root (contains all check configurations)

Options:
  --fix   Apply suggested fixes automatically (use with caution)
  -j N    Number of parallel jobs (default: min(cores, MemAvailable GB / 2))

Paths are relative to the repo root, so run it from there. Unrecognised arguments
are ignored, as the shell script did. Exit: 0 = clean, 1 = findings or a run that
cannot be trusted, 2 = bad usage.

The skip lists (files clang-tidy cannot parse, vendored files) live in
scripts/clang_tidy_skiplist.py.

Stdlib only, no syntax newer than Python 3.9.
"""
import concurrent.futures
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import threading
from typing import NamedTuple, Optional

import clang_tidy_skiplist as skiplist

RULE = "━" * 40

CLANG_TIDY = "../clang-p2996/build/bin/clang-tidy"
CLANG_TIDY_DIFF = "../clang-p2996/clang-tools-extra/clang-tidy/tool/clang-tidy-diff.py"
BUILD_DIR = "build/release"
COMPILE_COMMANDS = BUILD_DIR + "/compile_commands.json"
CLANG_TIDY_CONFIG = ".clang-tidy"

# Timeout 240s (issue #326): some TUs run a heavy consteval pass under `import std;`
# - e.g. each tests/yaml/test_unified_yaml_*.cpp #embeds and consteval-parses its
# slice of the unified test-case corpus. 60s was too short even before #561 split
# that corpus four ways.
TIDY_TIMEOUT_SECONDS = 240

CPP_FILE = re.compile(r"\.(cpp|cppm|h|hpp)$")
SCAN_ROOTS = ("src", "tests", "benchmarks")
# clang-tidy's own meta-messages; real warnings and errors are kept.
NOISE = re.compile(r"^[0-9]+ warnings? (generated|and)|^Suppressed [0-9]+|^Use -header-filter|^Use -system-headers")
CRASH_MARKER = "PLEASE submit a bug report"
ISSUE_LINE = re.compile(r": (warning|error):")
MAX_ISSUES_SHOWN = 50

# Per-file outcomes. "known" = skipped on purpose, "ok" = checked, "crashed" = the
# result cannot be trusted.
KNOWN, OK, CRASHED = "known", "ok", "crashed"

_print_lock = threading.Lock()


class UsageError(Exception):
    pass


class Options(NamedTuple):
    fix: bool
    mode: str      # diff | full | all
    jobs: int


class FileResult(NamedTuple):
    status: str
    lines: list
    message: str = ""
    error: tuple = ()


def say(text: str = "", err: bool = False) -> None:
    with _print_lock:
        print(text, file=sys.stderr if err else sys.stdout, flush=True)


def read_meminfo(path: str = "/proc/meminfo") -> Optional[str]:
    # Absent off Linux (e.g. macOS): the caller then sizes by core count alone.
    try:
        return pathlib.Path(path).read_text()
    except OSError:
        return None


def default_jobs(nproc: int, meminfo: Optional[str]) -> int:
    """min(nproc, MemAvailable_GB / 2), floor 1 (issue #326).

    Since the `import std;` migration every clang-tidy process loads the full std
    module BMI (~1.5-2 GB resident), so `-j $(nproc)` on a many-core / modest-RAM
    box OOM-kills the run. ~2 GB/job is the headroom budget. #555 tracks replacing
    the volatile MemAvailable input; carried over unchanged by this port.
    """
    match = re.search(r"^MemAvailable:\s+(\d+)", meminfo or "", re.MULTILINE)
    if not match:
        return nproc
    return min(nproc, max(int(match.group(1)) // 1024 // 1024 // 2, 1))


def parse_args(argv: list, jobs: int) -> Options:
    fix, mode = False, "diff"
    args = iter(argv)
    for arg in args:
        if arg == "--fix":
            fix = True
        elif arg in ("--diff", "--full", "--all"):
            mode = arg[2:]
        elif arg == "-j":
            value = next(args, "")
            if not value.isdigit() or int(value) < 1:
                raise UsageError("-j expects a positive integer, got: %r" % value)
            jobs = int(value)
    return Options(fix, mode, jobs)


def check_prerequisites(mode: str) -> int:
    if not pathlib.Path(COMPILE_COMMANDS).is_file():
        say("❌ Error: compile_commands.json not found at %s" % COMPILE_COMMANDS, err=True)
        say("   Run: cmake --preset ninja-release", err=True)
        return 1
    if not (pathlib.Path(CLANG_TIDY).is_file() and os.access(CLANG_TIDY, os.X_OK)):
        say("❌ Error: clang-tidy not found at %s" % CLANG_TIDY, err=True)
        return 1
    if mode == "diff" and not pathlib.Path(CLANG_TIDY_DIFF).is_file():
        say("❌ Error: clang-tidy-diff.py not found at %s" % CLANG_TIDY_DIFF, err=True)
        say("   Required for --diff mode. Re-run with --full or --all, or fix the path.", err=True)
        return 1
    if not pathlib.Path(CLANG_TIDY_CONFIG).is_file():
        say("❌ Error: .clang-tidy config not found at %s" % CLANG_TIDY_CONFIG, err=True)
        return 1
    return 0


def strip_pch(argv: list) -> list:
    out, i = [], 0
    while i < len(argv):
        # CMake emits: -Xclang -include-pch -Xclang <path> -Xclang -include -Xclang <hdr>
        if argv[i] == "-Xclang" and i + 1 < len(argv) and argv[i + 1] in ("-include-pch", "-include"):
            i += 4
            continue
        if argv[i] == "-Winvalid-pch":
            i += 1
            continue
        out.append(argv[i])
        i += 1
    return out


def strip_pch_database(source: pathlib.Path, target: pathlib.Path) -> None:
    database = json.loads(pathlib.Path(source).read_text())
    for entry in database:
        if "command" in entry:
            entry["command"] = " ".join(shlex.quote(a) for a in strip_pch(shlex.split(entry["command"])))
        elif "arguments" in entry:
            entry["arguments"] = strip_pch(entry["arguments"])
    with open(str(target), "w") as handle:
        json.dump(database, handle)


def database_dir(workdir: str) -> str:
    """The directory clang-tidy reads compile_commands.json from.

    PCH is stripped before clang-tidy sees the commands. tests/CMakeLists.txt
    precompiles <gtest/gtest.h> for storm_tests, which puts
    `-Xclang -include-pch ...` into ~103 compile_commands.json entries. clang-tidy
    SIGSEGVs on such a TU once any check is enabled (clang-p2996 bug, same signature
    as the benchmarks/schema.cppm entry in scripts/clang_tidy_skiplist.py), and the
    crash prints no ": error:" or ": warning:" line - so the --diff counters would
    read it as a clean pass.

    DEFENCE IN DEPTH, not a live fix: the PCH lands only on tests/**, and
    tests/.clang-tidy disables every check (`Checks: '-*'`, issue #590), so
    clang-tidy exits before building an AST and never crashes today - verified, and
    verified that no src/ entry carries the flags (74 files, 0 with PCH).
    Re-enabling checks for tests/** would resurrect the crash silently; stripping
    here means that change stays safe.

    Replaying the TU without the PCH is semantically identical - the PCH holds only
    <gtest/gtest.h>, which the TU includes textually anyway - so this restores
    exactly the pre-PCH parse rather than skipping anything.
    """
    try:
        has_pch = "include-pch" in pathlib.Path(COMPILE_COMMANDS).read_text()
    except OSError:
        has_pch = False
    if not has_pch:
        return BUILD_DIR
    strip_pch_database(pathlib.Path(COMPILE_COMMANDS), pathlib.Path(workdir) / "compile_commands.json")
    say("   PCH flags stripped from a temp copy of compile_commands.json")
    return workdir


def print_banner(opts: Options) -> None:
    say("🔍 Running clang-tidy using .clang-tidy configuration...")
    say("   Config file: %s" % CLANG_TIDY_CONFIG)
    say("   Build directory: %s" % BUILD_DIR)
    say("   Parallel jobs: %d" % opts.jobs)
    say({"diff": "   Scope: staged lines only (git diff --cached, lines added/changed)",
         "full": "   Scope: staged files (whole-file scan)",
         "all": "   Scope: ALL C++ source files (sweep)"}[opts.mode])
    say("   Mode: AUTO-FIX enabled" if opts.fix else "   Mode: Check only (use --fix to apply fixes)")
    say()


def git_output(*args: str) -> str:
    proc = subprocess.run(["git"] + list(args), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return proc.stdout.decode("utf-8", errors="replace")


def restage_fixes() -> None:
    subprocess.run(["git", "add", "-u"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def count_lines(lines: list, needle: str) -> int:
    return sum(1 for line in lines if needle in line)


# ─── --diff mode ────────────────────────────────────────────────────────────
# Pipe `git diff -U0 --cached` through clang-tidy-diff.py - it only emits
# diagnostics on lines the staged commit touches. Pre-existing warnings on other
# lines are out of scope. See Issue #262 for rationale.
def tee_clang_tidy_diff(opts: Options, db_path: str, staged_diff: pathlib.Path) -> list:
    # -p1 strips the a/ b/ prefix that `git diff` prepends to paths. -iregex limits
    # the scan to C++ sources we want clang-tidy on.
    #
    # Deliberately NOT passing -config-file: forcing the root .clang-tidy overrides
    # clang-tidy's directory-hierarchy config lookup, which defeats per-directory
    # overrides like tests/.clang-tidy (it sets InheritParentConfig: true and
    # disables readability-function-cognitive-complexity - "large test bodies are
    # intentional coverage"). With the root config forced, that disable was ignored
    # and pre-existing complex TEST bodies fired the moment an unrelated import-std
    # edit touched a line inside them (issue #326).
    command = [sys.executable, CLANG_TIDY_DIFF, "-clang-tidy-binary", CLANG_TIDY, "-p1", "-path", db_path,
               "-iregex", r".*\.(cpp|cppm|h|hpp)", "-j", str(opts.jobs), "-timeout", str(TIDY_TIMEOUT_SECONDS),
               "-quiet"] + (["-fix"] if opts.fix else [])
    with open(str(staged_diff), "rb") as stdin:
        return tee_command(command, stdin)[0]  # exit code unreliable across versions


def tee_command(command: list, stdin=None) -> tuple:
    """Run command, echoing its merged stdout/stderr live; return (lines, exit code)."""
    captured = []
    proc = subprocess.Popen(command, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for raw in proc.stdout:
        line = raw.decode("utf-8", errors="replace")
        sys.stdout.write(line)
        sys.stdout.flush()
        captured.append(line.rstrip("\n"))
    return captured, proc.wait()


def tee_fragment_parents(opts: Options, db_path: str, fragment_filters: dict) -> list:
    """Lint staged #include-fragment lines (#671) through each fragment's parent TU.

    filter_skiplist_from_diff drops fragment sections (a fragment is not a TU), so
    without this their changed lines would never be checked at commit time.

    -header-filter is passed explicitly: the config's HeaderFilterRegex uses a
    (?!...) lookahead, which clang-tidy's POSIX regex engine does not support, so
    on its own it lets no diagnostic through from any #included file.
    """
    captured = []
    for parent, filters in sorted(fragment_filters.items()):
        header_filter = ".*/(%s)$" % "|".join(re.escape(entry["name"]) for entry in filters)
        command = [CLANG_TIDY, "-p", db_path, "-quiet", "-header-filter=" + header_filter,
                   "-line-filter=" + json.dumps(filters)]
        lines, code = tee_command(command + (["-fix"] if opts.fix else []) + [parent])
        if code and not any(": error:" in line or ": warning:" in line for line in lines):
            # A direct clang-tidy exit code is reliable (unlike clang-tidy-diff.py's):
            # a failure that printed no diagnostic must not pass as clean.
            lines.append("%s: error: clang-tidy exited %d with no diagnostic" % (parent, code))
            say(lines[-1])
        captured += lines
    return captured


def run_diff(opts: Options, db_path: str, workdir: str) -> int:
    # Diff sections for known-unparseable / always-skip files are dropped BEFORE
    # clang-tidy-diff.py sees them (source-level skip, keyed by file identity).
    staged = git_output("diff", "-U0", "--cached", "--src-prefix=a/", "--dst-prefix=b/", "--no-ext-diff")
    staged_diff = pathlib.Path(workdir) / "staged.diff"
    staged_diff.write_text("".join(skiplist.filter_skiplist_from_diff(staged.splitlines(keepends=True))))
    output = tee_clang_tidy_diff(opts, db_path, staged_diff)
    fragment_filters = skiplist.fragment_line_filters(staged.splitlines(keepends=True))
    fragment_output = tee_fragment_parents(opts, db_path, fragment_filters)

    if opts.fix:
        restage_fixes()  # so the re-check sees them

    # clang-tidy-diff.py prints "No relevant changes found." when nothing in the
    # diff matches the regex. That's a clean pass.
    if not fragment_filters and any("No relevant changes found" in line for line in output):
        say()
        say("✅ clang-tidy --diff: no staged C++ lines to check")
        return 0

    # Count actual diagnostics - clang-tidy-diff.py's exit code is unreliable
    # across versions (it may return 0 even with warnings). Both counts are
    # unfiltered, and the same way: toolchain noise never reaches the output
    # because the skip-listed sections were dropped above. A post-hoc message-text
    # regex used to filter errors only - asymmetric with warnings (#550 gap 1) and,
    # being unanchored, it once matched a genuine `unknown type name 'concept'`
    # error against a real Storm file (src/orm/fields.cppm) (#550 gap 2). A file
    # not on the reviewed skip list is never silently excluded again.
    #
    # Errors are labelled "in staged files", not "on staged lines" like warnings:
    # clang-tidy's -line-filter only suppresses warnings outside the diffed hunks -
    # a hard parse failure is reported regardless of which line triggered it, since
    # the rest of the TU couldn't be checked at all.
    output = output + fragment_output
    warnings = count_lines(output, ": warning:")
    errors = count_lines(output, ": error:")

    # A crashed clang-tidy prints neither ": error:" nor ": warning:", so the two
    # counters above cannot see it and the run would report success having checked
    # nothing. Files KNOWN to crash never get this far, so any crash here is
    # unexpected and the result is not trustworthy (#550).
    crashes = count_lines(output, CRASH_MARKER)
    if crashes:
        say()
        say(RULE)
        say("❌ clang-tidy --diff: crashed on %d translation unit(s)" % crashes)
        say("   Diagnostics are incomplete — treating as failure rather than clean.")
        say(RULE)
        return 1

    say()
    say(RULE)
    say("📊 --diff summary:")
    say("   Warnings on staged lines: %d" % warnings)
    say("   Errors in staged files:   %d" % errors)
    say(RULE)

    if warnings or errors:
        if opts.fix:
            # Auto-fix pass complete - re-run check-only to confirm clean.
            say()
            say("🔄 Re-checking after auto-fix...")
            return main(["--diff"])
        say("❌ clang-tidy --diff: new warnings/errors on staged lines")
        return 1

    say("✅ clang-tidy --diff: staged lines are clean")
    return 0


# ─── --full and --all modes: file-based scan ───────────────────────────────
def list_files(mode: str) -> list:
    if mode == "all":
        found = [os.path.join(root, name)
                 for top in SCAN_ROOTS if os.path.isdir(top)
                 for root, _dirs, names in os.walk(top)
                 for name in names if CPP_FILE.search(name)]
    else:
        found = [name for name in git_output("diff", "--cached", "--name-only").splitlines()
                 if CPP_FILE.search(name)]
    return sorted(name for name in found if "third_party" not in name)


def lint_file(path: str, db_path: str, fix: bool, clang_tidy: str = CLANG_TIDY,
              timeout: int = TIDY_TIMEOUT_SECONDS, env: Optional[dict] = None) -> FileResult:
    """Run clang-tidy on one file and classify the outcome.

    clang-tidy reads .clang-tidy from the project root (it searches parent
    directories), so no config is passed.
    """
    # Vendored / upstream files we must never touch (e.g. generator.cppm).
    if skiplist.is_always_skip_file(path):
        return FileResult(KNOWN, [], "  ⏭  %s (always-skip — vendored upstream)" % path)

    # Output is captured and failures ignored (`|| true` in the shell script): a
    # crash or a timeout is judged from what was printed. A TU that exceeds the
    # timeout therefore reads as "ok" - the silent gap #556 tracks, carried over.
    command = [clang_tidy, "-p", db_path] + (["-fix"] if fix else []) + [path]
    try:
        raw = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
                             env=env).stdout
    except subprocess.TimeoutExpired as expired:
        raw = expired.stdout or b""
    except OSError as error:
        # Cannot even be started (after the prerequisite check passed): the file was
        # not checked, so it must not read as clean.
        return FileResult(CRASHED, [], "", ("  ❌ %s (cannot run clang-tidy: %s)" % (path, error.strerror or error),))
    lines = [line for line in raw.decode("utf-8", errors="replace").splitlines() if not NOISE.search(line)]

    # Failure modes:
    # 1. "Found compiler error" = clang-tidy couldn't parse the file (C++26 modules)
    # 2. "PLEASE submit a bug report" = clang-tidy crashed AFTER processing
    #    (warnings may exist)
    has_compile_error = count_lines(lines, "Found compiler error") > 0
    has_crash = count_lines(lines, CRASH_MARKER) > 0
    has_warnings = count_lines(lines, ": warning:") > 0

    if has_compile_error:
        return classify_compile_error(path, lines, has_warnings)
    if has_crash:
        return classify_crash(path, lines, has_warnings)
    return FileResult(OK, lines, "  ✓ %s" % path)


def classify_compile_error(path: str, lines: list, has_warnings: bool) -> FileResult:
    if skiplist.is_known_unparseable(path):
        # Known C++26 module file with compile errors - skip entirely. Warnings from
        # partial parsing are unreliable (false positives).
        return FileResult(KNOWN, [], "  ✓ %s (C++26 modules - skipped)" % path)
    if has_warnings:
        # Non-module file with compile errors but real warnings - keep the warnings.
        kept = [l for l in lines if ": error:" not in l and "Found compiler error" not in l]
        return FileResult(OK, kept, "  ❌ %s (with C++26 header errors)" % path)
    # Parse failure on a file we expect to be parseable (e.g. src/*.cppm since the
    # 2026-05-11 clang-p2996 rebuild). Surface it loudly - it almost always means
    # the build state is stale or the toolchain regressed. Silent skipping here was
    # the root cause of Issue #262. The original error stays in the output so the
    # summary can print it.
    return FileResult(CRASHED, lines, "", (
        "  ❌ %s (PARSE FAILURE — toolchain or build state is broken)" % path,
        "     clang-tidy could not parse this file. Re-run cmake --preset",
        "     ninja-release and rebuild before retrying. See Issue #262."))


def classify_crash(path: str, lines: list, has_warnings: bool) -> FileResult:
    if has_warnings:
        # Crashed after processing - the warnings are still real; drop the backtrace.
        cut = next(i for i, line in enumerate(lines) if CRASH_MARKER in line)
        return FileResult(OK, lines[:cut], "  ❌ %s (with crash after analysis)" % path)
    if skiplist.is_known_unparseable(path):
        return FileResult(KNOWN, [], "  ✓ %s (C++26 modules - skipped)" % path)
    return FileResult(CRASHED, [], "  ⚠ %s (clang-tidy crashed - UNEXPECTED)" % path)


def lint_and_report(path: str, db_path: str, fix: bool) -> FileResult:
    result = lint_file(path, db_path, fix)
    if result.message:
        say(result.message)
    for line in result.error:
        say(line, err=True)
    return result


def unique_matching(results: list, needle: str) -> list:
    return sorted({line for result in results for line in result.lines if needle in line})


def run_files(opts: Options, db_path: str) -> int:
    files = list_files(opts.mode)
    if not files:
        say("✅ No C++ files found — clang-tidy skipped" if opts.mode == "all"
            else "✅ No staged C++ files — clang-tidy skipped")
        return 0

    say("📁 Found %d source files to check" % len(files))
    say()
    say("Running clang-tidy on %d files with %d parallel jobs..." % (len(files), opts.jobs))
    say()
    with concurrent.futures.ThreadPoolExecutor(max_workers=opts.jobs) as pool:
        results = list(pool.map(lambda path: lint_and_report(path, db_path, opts.fix), files))
    say()
    say(RULE)
    return summarize(opts, files, results)


def summarize(opts: Options, files: list, results: list) -> int:
    # Deduplicated across files first: without it each file that saw the same
    # header warning inflated the count (50 real warnings reported as 665).
    warnings = len(unique_matching(results, ": warning:"))
    errors = len(unique_matching(results, ": error:"))
    statuses = [result.status for result in results]
    known, crashes, passed = statuses.count(KNOWN), statuses.count(CRASHED), statuses.count(OK)

    if errors or warnings:
        say()
        say("📋 Issues found:")
        issues = sorted({line for result in results for line in result.lines if ISSUE_LINE.search(line)})
        for line in issues[:MAX_ISSUES_SHOWN]:
            say(line)
        say()

    say(RULE)
    say("📊 Summary:")
    say("   Files checked: %d" % len(files))
    say("   Files passed: %d" % passed)
    say("   C++26 skipped: %d (expected - modules/reflection not supported)" % known)
    if crashes:
        say("   Unexpected crashes: %d ⚠️" % crashes)
    say("   Warnings: %d" % warnings)
    say("   Errors: %d" % errors)
    say(RULE)

    # Exit logic: errors always fail; unexpected crashes fail (not in the known
    # list); warnings BLOCK commits (must fix or acknowledge); known C++26 skips
    # are OK.
    if errors:
        say("❌ clang-tidy found errors")
        return 1
    if crashes:
        say("❌ clang-tidy could not parse %d file(s) — toolchain or build state may be stale" % crashes)
        say("   Try: rm -rf build/release && cmake --preset ninja-release && cmake --build --preset ninja-release")
        say("   See: https://github.com/spiritEcosse/storm/issues/262")
        return 1
    if warnings:
        if opts.fix:
            # Fixes were applied - re-stage and re-check. The shell script re-ran
            # itself with no arguments, which is the default --diff mode; kept so
            # the port stays behaviour-preserving.
            restage_fixes()
            say()
            say("🔄 Re-checking after auto-fix...")
            return main([])
        say("❌ clang-tidy found %d warning(s) - fix before committing" % warnings)
        say("   Run with --fix to auto-fix some issues, or update .clang-tidy to exclude checks")
        return 1
    say("✅ clang-tidy passed with no issues")
    return 0


def cpu_count() -> int:
    # nproc honours CPU affinity (cpusets, containers); os.cpu_count() does not.
    if hasattr(os, "sched_getaffinity"):
        return len(os.sched_getaffinity(0)) or 1
    return os.cpu_count() or 1


def main(argv: list) -> int:
    try:
        opts = parse_args(argv, default_jobs(cpu_count(), read_meminfo()))
    except UsageError as error:
        say("run_clang_tidy: %s" % error, err=True)
        return 2
    failed = check_prerequisites(opts.mode)
    if failed:
        return failed
    with tempfile.TemporaryDirectory() as workdir:
        db_path = database_dir(workdir)
        print_banner(opts)
        if opts.mode == "diff":
            return run_diff(opts, db_path, workdir)
        return run_files(opts, db_path)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
