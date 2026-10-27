#!/usr/bin/env python3
"""Tests for scripts/smart_skip.py (issue #688).

The smart-skip table decides which pre-commit steps a staged-file set needs. It
consumes scripts/detect-changes.sh's `HAS_*=true|false` output, so these tests
state the commit.sh matrix as (flags -> RUN_* steps) and also run the real
detect-changes.sh over file lists so the two cannot drift. Stdlib unittest only,
no syntax newer than Python 3.7.

    python3 scripts/tests/test_smart_skip.py
"""
import pathlib
import shlex
import subprocess
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = REPO_ROOT / "scripts" / "smart_skip.py"
DETECT = REPO_ROOT / "scripts" / "detect-changes.sh"

sys.path.insert(0, str(HELPER.parent))
from smart_skip import decide  # noqa: E402

ALL = ("RUN_FORMAT", "RUN_CMAKE_FORMAT", "RUN_TIDY", "RUN_TESTS", "RUN_COVERAGE", "RUN_BENCH_RELEASE")


def flags(**on):
    keys = ("SRC", "TEST", "CPP", "CMAKE", "BENCH")
    return {"HAS_%s_CHANGES" % k: k in on for k in keys}


def ran(plan):
    return {name for name in ALL if plan[name]}


class DecideTest(unittest.TestCase):
    def test_cpp_in_src_runs_everything_but_cmake_format_and_bench(self):
        plan, note = decide(flags(SRC=True, CPP=True))
        self.assertEqual(ran(plan), {"RUN_FORMAT", "RUN_TIDY", "RUN_TESTS", "RUN_COVERAGE"})
        self.assertEqual(note, "")

    def test_cpp_and_cmake_runs_everything(self):
        plan, _ = decide(flags(SRC=True, CPP=True, CMAKE=True))
        self.assertEqual(ran(plan), {"RUN_FORMAT", "RUN_CMAKE_FORMAT", "RUN_TIDY", "RUN_TESTS", "RUN_COVERAGE"})

    def test_neither_cpp_nor_cmake_skips_all(self):
        plan, note = decide(flags())
        self.assertEqual(ran(plan), set())
        self.assertIn("No C++ or cmake files in commit", note)

    def test_cmake_only_skips_clang_format_and_tidy(self):
        plan, note = decide(flags(CMAKE=True))
        self.assertEqual(ran(plan), {"RUN_CMAKE_FORMAT", "RUN_TESTS", "RUN_COVERAGE"})
        self.assertIn("cmake-only changes", note)

    def test_cpp_outside_src_tests_cmake_skips_tests_and_coverage(self):
        plan, note = decide(flags(CPP=True))
        self.assertEqual(ran(plan), {"RUN_FORMAT", "RUN_TIDY"})
        self.assertIn("No src/, tests/, or cmake changes", note)

    def test_tests_dir_cpp_runs_tests(self):
        plan, _ = decide(flags(TEST=True, CPP=True))
        self.assertTrue(plan["RUN_TESTS"] and plan["RUN_COVERAGE"])

    def test_bench_release_needs_bench_and_cpp(self):
        self.assertTrue(decide(flags(BENCH=True, CPP=True))[0]["RUN_BENCH_RELEASE"])
        self.assertFalse(decide(flags(BENCH=True))[0]["RUN_BENCH_RELEASE"])
        self.assertFalse(decide(flags(CPP=True))[0]["RUN_BENCH_RELEASE"])

    def test_no_flags_at_all_runs_every_gate_but_not_bench(self):
        # Empty staged set (e.g. `./commit.sh` with nothing staged): the old
        # script left every RUN_* at its default true and bench off.
        plan, note = decide(None)
        self.assertEqual(ran(plan), {"RUN_FORMAT", "RUN_CMAKE_FORMAT", "RUN_TIDY", "RUN_TESTS", "RUN_COVERAGE"})
        self.assertEqual(note, "")


class MatrixTest(unittest.TestCase):
    """Staged-file matrix from the issue, through the real detect-changes.sh."""

    def steps(self, files):
        detected = subprocess.run([str(DETECT)], input="\n".join(files) + "\n",
                                  stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
        out = subprocess.run([sys.executable, str(HELPER)], input=detected,
                             stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
        return {line.split("=")[0] for line in out.splitlines() if line.endswith("=true")}

    def test_cpp_only(self):
        self.assertEqual(self.steps(["src/orm/x.cppm"]), {"RUN_FORMAT", "RUN_TIDY", "RUN_TESTS", "RUN_COVERAGE"})

    def test_cmake_only(self):
        self.assertEqual(self.steps(["cmake/db.cmake"]), {"RUN_CMAKE_FORMAT", "RUN_TESTS", "RUN_COVERAGE"})

    def test_agent_only_and_docs_only(self):
        self.assertEqual(self.steps([".claude/agents/a.md"]), set())
        self.assertEqual(self.steps(["docs/README.md"]), set())

    def test_bench_only_cpp(self):
        self.assertEqual(self.steps(["benchmarks/b.cpp"]), {"RUN_FORMAT", "RUN_TIDY", "RUN_BENCH_RELEASE"})


NO_CHANGES = ("HAS_SRC_CHANGES=false\nHAS_TEST_CHANGES=false\nHAS_CPP_CHANGES=false\n"
              "HAS_CMAKE_CHANGES=false\nHAS_BENCH_CHANGES=false\n")


class CliTest(unittest.TestCase):
    def run_cli(self, text):
        return subprocess.run([sys.executable, str(HELPER)], input=text, stdout=subprocess.PIPE,
                              universal_newlines=True, check=True).stdout

    def test_emits_shell_assignments_and_a_note_variable(self):
        out = self.run_cli(NO_CHANGES)
        self.assertIn("RUN_FORMAT=false", out)
        self.assertIn("SKIP_NOTE=", out)

    def test_note_is_shell_quoted(self):
        out = self.run_cli(NO_CHANGES)
        line = next(l for l in out.splitlines() if l.startswith("SKIP_NOTE="))
        name, _, note = shlex.split(line)[0].partition("=")
        self.assertEqual(name, "SKIP_NOTE")
        self.assertIn("No C++ or cmake files", note)

    def test_partial_flags_keep_every_gate_on(self):
        out = self.run_cli("HAS_SRC_CHANGES=true\n")
        self.assertIn("RUN_TESTS=true", out)
        self.assertIn("RUN_FORMAT=true", out)
        self.assertIn("SKIP_NOTE=''", out)

    def test_malformed_flag_value_keeps_every_gate_on(self):
        text = "".join("%s=%s\n" % (k, "True" if k == "HAS_CPP_CHANGES" else "false") for k in (
            "HAS_SRC_CHANGES", "HAS_TEST_CHANGES", "HAS_CPP_CHANGES", "HAS_CMAKE_CHANGES", "HAS_BENCH_CHANGES"))
        self.assertIn("RUN_FORMAT=true", self.run_cli(text))

    def test_empty_stdin_keeps_defaults(self):
        out = self.run_cli("")
        self.assertIn("RUN_TESTS=true", out)
        self.assertIn("RUN_BENCH_RELEASE=false", out)


if __name__ == "__main__":
    unittest.main()
