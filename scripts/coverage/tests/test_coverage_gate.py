#!/usr/bin/env python3
"""Tests for scripts/coverage/coverage_gate.py (issue #765).

One module owns what used to be two sourced bash helpers (`line_coverage.sh`,
`pg-default-connstr.sh`) plus the gate logic that commit.sh and CI's `coverage` job each
spelled out: read the line percentage from the lcov summary, compare it to 100.0, report
the uncovered lines, and pick the per-OS default PostgreSQL socket.

The parser and the OS table are plain functions, tested on fixture text. The CLI runs as a
subprocess for what only a process shows (exit codes, stdout). The callers are tested by
running their REAL text: the PostgreSQL default blocks of commit.sh and
coverage-run-batched.sh and commit.sh's run_coverage_check(), extracted and run in bash
with `cmake` stubbed, because a deleted call or a hardcoded path is invisible to a unit
test of the module alone. They run under /bin/bash too (macOS: bash 3.2, #689), and
commit.sh plus the scripts it runs are scanned for bash-4-only constructs.

Stdlib unittest only, no syntax newer than Python 3.7.

    python3 scripts/coverage/tests/test_coverage_gate.py
"""
import os
import pathlib
import re
import shutil
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "lib"))
sys.path.insert(0, str(REPO_ROOT / "scripts" / "coverage"))
import testing  # noqa: E402
from coverage_gate import default_pg_connstr, parse_line_coverage  # noqa: E402

GATE = REPO_ROOT / "scripts" / "coverage" / "coverage_gate.py"
COMMIT = REPO_ROOT / "commit.sh"
BATCHED = REPO_ROOT / "scripts" / "coverage" / "coverage-run-batched.sh"

LINUX_DEFAULT = "host=/var/run/postgresql dbname=storm_db user=storm_db"
DARWIN_DEFAULT = "host=/tmp dbname=storm_db user=storm_db"
EXPORTED = "host=ci-pg dbname=x user=y"


def summary(line_percent):
    """A realistic `lcov --summary` (lcov 2.x). functions/branches carry percentages too:
    anchoring on the wrong row would gate against ~81% or ~92%."""
    return "\n".join([
        "[100%] Built target coverage-run",
        "Summary coverage rate:",
        "  source files: 24",
        "  lines.......: %s%% (8613 of 8613 lines)" % line_percent,
        "  functions...: 80.6% (4100 of 5087 functions)",
        "  branches....: 92.1% (812 of 882 branches)",
        "",
    ])


class ParseLineCoverageTest(unittest.TestCase):
    def test_full_coverage(self):
        self.assertEqual(parse_line_coverage(summary("100.0")), "100.0")

    def test_partial_coverage(self):
        self.assertEqual(parse_line_coverage(summary("99.8")), "99.8")

    def test_lines_row_wins_over_functions_and_branches(self):
        self.assertEqual(parse_line_coverage(summary("97.3")), "97.3")

    def test_last_lines_row_wins(self):
        self.assertEqual(parse_line_coverage(summary("42.0") + summary("100.0")), "100.0")

    def test_no_lines_row_gives_none(self):
        text = "error: build failed\n  functions...: 80.6% (1 of 2)\n"
        self.assertIsNone(parse_line_coverage(text))

    def test_empty_input_gives_none(self):
        self.assertIsNone(parse_line_coverage(""))


class DefaultPgConnstrTest(unittest.TestCase):
    def test_linux_uses_the_system_socket_directory(self):
        self.assertEqual(default_pg_connstr("Linux"), LINUX_DEFAULT)

    def test_darwin_uses_tmp_for_homebrew_postgresql(self):
        self.assertEqual(default_pg_connstr("Darwin"), DARWIN_DEFAULT)

    def test_unknown_os_fails_and_names_the_os(self):
        # Falling through to one of the two paths would skip every PG test silently.
        with self.assertRaises(ValueError) as caught:
            default_pg_connstr("FreeBSD")
        self.assertIn("FreeBSD", str(caught.exception))

    def test_no_argument_resolves_from_the_running_host(self):
        expected = {"Darwin": DARWIN_DEFAULT, "Linux": LINUX_DEFAULT}.get(os.uname()[0])
        if expected is None:
            self.skipTest("no default for this host OS")
        self.assertEqual(default_pg_connstr(), expected)


class CliTest(unittest.TestCase):
    def gate(self, *args, **kwargs):
        env = dict(os.environ, NO_COLOR="1")
        env.pop("GITHUB_ACTIONS", None)
        return testing.run([GATE] + list(args), env=env, **kwargs)

    def test_script_is_executable(self):
        self.assertTrue(os.access(str(GATE), os.X_OK), "%s is not executable" % GATE)

    def test_line_coverage_prints_the_number(self):
        result = self.gate("line-coverage", input=summary("99.8"))
        self.assertEqual((result.returncode, result.stdout), (0, "99.8\n"))

    def test_line_coverage_with_no_row_prints_nothing_and_succeeds(self):
        # A no-match must come back empty for the caller's own loud error.
        result = self.gate("line-coverage", input="nothing\n")
        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_pg_connstr_for_a_named_os(self):
        result = self.gate("pg-connstr", "Darwin")
        self.assertEqual((result.returncode, result.stdout), (0, DARWIN_DEFAULT + "\n"))

    def test_pg_connstr_for_an_unknown_os_fails_and_names_it(self):
        result = self.gate("pg-connstr", "FreeBSD")
        self.assertEqual(result.returncode, 1)
        self.assertIn("FreeBSD", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_check_passes_at_100(self):
        result = self.gate("check", input=summary("100.0"))
        self.assertEqual((result.returncode, result.stdout), (0, "Line coverage: 100.0%\n"))

    def test_check_fails_below_100_with_the_real_number(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.gate("check", input=summary("99.8"), cwd=tmp)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Line coverage: 99.8% (required: 100.0%)", result.stdout)

    def test_check_lists_the_uncovered_ranges_per_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = os.path.realpath(tmp)
            lcov_dir = os.path.join(root, "build", "debug", "coverage")
            os.makedirs(lcov_dir)
            records = ["SF:%s/src/a.cpp" % root, "DA:1,4", "DA:3,0", "DA:5,0", "DA:6,0", "DA:7,0",
                       "DA:8,2", "end_of_record", "SF:%s/src/b.cpp" % root, "DA:1,1", "end_of_record"]
            with open(os.path.join(lcov_dir, "coverage-filtered.lcov"), "w") as lcov:
                lcov.write("\n".join(records) + "\n")
            result = self.gate("check", input=summary("99.8"), cwd=root)
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/a.cpp", result.stdout)
        self.assertIn("Lines: 3, 5-7", result.stdout)
        self.assertNotIn("src/b.cpp", result.stdout)

    def test_check_with_an_unparseable_summary_fails_loudly(self):
        result = self.gate("check", input="error: build failed\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Could not parse line coverage", result.stdout)

    def test_check_on_empty_input_fails(self):
        self.assertEqual(self.gate("check", input="").returncode, 1)

    def test_unknown_command_is_a_usage_error(self):
        self.assertEqual(self.gate("bogus").returncode, 2)

    def test_github_actions_failure_carries_an_error_annotation(self):
        env = dict(os.environ, NO_COLOR="1", GITHUB_ACTIONS="true")
        with tempfile.TemporaryDirectory() as tmp:
            result = testing.run([GATE, "check"], env=env, input=summary("99.8"), cwd=tmp)
        self.assertIn("::error::Line coverage 99.8% is below the required 100.0%", result.stdout)


def block_between(path, start_regex, end_regex):
    """Lines of `path` from the first one matching start_regex through the next matching
    end_regex, with BASH_SOURCE[0] pointing at the real file (the hook runs it by path)."""
    lines = path.read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if re.search(start_regex, line))
    end = next(i for i in range(start, len(lines)) if re.search(end_regex, lines[i]))
    return "\n".join(lines[start:end + 1]).replace("${BASH_SOURCE[0]}", str(path))


def bash_shells():
    """The current bash, plus /bin/bash when it differs (macOS: bash 3.2, #689)."""
    shells = [shutil.which("bash")]
    system_bash = pathlib.Path("/bin/bash")
    if system_bash.exists() and system_bash.resolve() != pathlib.Path(shells[0]).resolve():
        shells.append(str(system_bash))
    return shells


class DefaultBlockTest(unittest.TestCase):
    """The STORM_PG_CONNSTR default in commit.sh and coverage-run-batched.sh, run for real.

    Each block runs as a Linux and as a Darwin host by stubbing `uname` with a shell
    function: comparing against THIS machine's default would let a hardcoded path pass on
    the one OS where it happens to be right.
    """

    COVERAGE_START = r'^if \[\[ -z "\$\{STORM_PG_CONNSTR\+set\}" \]\]'
    COMMIT_START = r'^\s*if \[\[ -z "\$\{STORM_PG_CONNSTR:-\}" \]\]; then'
    AS_LINUX = "uname() { echo Linux; }"
    AS_DARWIN = "uname() { echo Darwin; }"

    def run_block(self, shell, script, start, setup):
        block = block_between(script, start, r"^\s*fi$")
        # Wrapped in a function: commit.sh's `return 1` is only legal inside one.
        program = ("set -e; %s; f() {\n%s\n}; f; printf '%%s' \"${STORM_PG_CONNSTR-<unset>}\""
                   % (setup, block))
        return testing.run([shell, "-c", program]).stdout

    def test_coverage_run_batched(self):
        for shell in bash_shells():
            with self.subTest(shell=shell):
                def run(setup):
                    return self.run_block(shell, BATCHED, self.COVERAGE_START, setup)
                self.assertEqual(run(self.AS_LINUX + "; unset STORM_PG_CONNSTR"), LINUX_DEFAULT)
                self.assertEqual(run(self.AS_DARWIN + "; unset STORM_PG_CONNSTR"), DARWIN_DEFAULT)
                self.assertEqual(run("export STORM_PG_CONNSTR='%s'" % EXPORTED), EXPORTED)
                # An empty value is passed through: libpq reads "" as "use the PG* defaults".
                self.assertEqual(run("export STORM_PG_CONNSTR="), "")

    def test_commit_sh(self):
        for shell in bash_shells():
            with self.subTest(shell=shell):
                def run(setup):
                    return self.run_block(shell, COMMIT, self.COMMIT_START, setup)
                self.assertEqual(run(self.AS_LINUX + "; unset STORM_PG_CONNSTR"), LINUX_DEFAULT)
                self.assertEqual(run(self.AS_DARWIN + "; unset STORM_PG_CONNSTR"), DARWIN_DEFAULT)
                self.assertEqual(run("export STORM_PG_CONNSTR='%s'" % EXPORTED), EXPORTED)
                # commit.sh differs deliberately: a set-but-empty value counts as unset.
                self.assertEqual(run(self.AS_DARWIN + "; export STORM_PG_CONNSTR="), DARWIN_DEFAULT)


class CommitCoverageStepTest(unittest.TestCase):
    """commit.sh's run_coverage_check(), the real text, with cmake stubbed to print a summary."""

    def run_check(self, shell, percent):
        block = block_between(COMMIT, r"^    run_coverage_check\(\) \{", r"^    \}$")
        program = "\n".join([
            "RED= BOLD= RESET= DIM=",
            "cmake() { [[ \"$*\" == *'--target coverage' ]] && printf '%s' \"$SUMMARY\"; return 0; }",
            "configure_if_missing() { return 0; }",
            block,
            "run_coverage_check; echo \"rc=$?\"",
        ])
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ, SUMMARY=summary(percent))
            return testing.run([shell, "-c", program], env=env, cwd=tmp, merge_stderr=True).stdout

    def test_full_coverage_passes_the_gate(self):
        for shell in bash_shells():
            with self.subTest(shell=shell):
                out = self.run_check(shell, "100.0")
                self.assertIn("Line coverage: 100.0%", out)
                self.assertTrue(out.rstrip().endswith("rc=0"), out)

    def test_partial_coverage_fails_the_gate_with_the_real_number(self):
        for shell in bash_shells():
            with self.subTest(shell=shell):
                out = self.run_check(shell, "99.8")
                self.assertIn("Line coverage: 99.8% (required: 100.0%)", out)
                self.assertTrue(out.rstrip().endswith("rc=1"), out)


class NoBash4ConstructsTest(unittest.TestCase):
    """commit.sh runs under `#!/bin/bash`, bash 3.2 on macOS (#689): a bash-4 construct
    breaks there while CI's bash 5 accepts it. Comment lines are skipped; the pattern covers
    the common constructs, not every bash-4 feature."""

    BASH4 = re.compile(r"\b(mapfile|readarray|coproc)\b"
                       r"|\b(local|declare|typeset)\s+-[a-zA-Z]*[nAglu]"
                       r"|\$\{[a-zA-Z_][a-zA-Z_0-9]*(\[@\])?(,,?|\^\^?)")
    TARGETS = ("commit.sh", "scripts/coverage/coverage-run-batched.sh")

    def test_pattern_matches_what_it_claims_to(self):
        # A broken regex must not make the scan vacuous.
        samples = ["mapfile -t a < f", "  local -n r=$1", "declare -A m", "x=${v,,}",
                   "declare -l low", "y=${arr[@]^^}"]
        for sample in samples:
            self.assertTrue(self.BASH4.search(sample), sample)
        for fine in ("local -a ok", "x=${v:-,}"):
            self.assertFalse(self.BASH4.search(fine), fine)

    def test_targets_exist_and_use_no_bash4_constructs(self):
        for relative in self.TARGETS:
            path = REPO_ROOT / relative
            self.assertTrue(path.is_file(), "%s is missing" % relative)
            for number, line in enumerate(path.read_text().splitlines(), 1):
                if line.lstrip().startswith("#"):
                    continue
                self.assertFalse(self.BASH4.search(line), "%s:%d: %s" % (relative, number, line))


if __name__ == "__main__":
    unittest.main()
