#!/usr/bin/env python3
"""Tests for scripts/check_ci_digests.py (issue #708).

The checker replaced ci.yml's inline-bash `digest-guard` job: every workflow that
runs in the storm-ci image pins it by digest, and the pins must agree or two
workflows silently run on two different clang-p2996 builds (#498). It now runs
from the pre-commit guard table too, so a workflow edit that drifts a pin is
caught before the commit, not after the push.

`pins_in` and `problems` are exercised on fixture workflow directories; the CLI
is run as a subprocess for exit codes. Stdlib unittest only, no syntax newer than
Python 3.7.

    python3 scripts/tests/test_check_ci_digests.py
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts" / "check_ci_digests.py"

sys.path.insert(0, str(CHECKER.parent))
import check_ci_digests as ccd  # noqa: E402

DIGEST_A = "a" * 64
DIGEST_B = "b" * 64


def workflow(*digests):
    jobs = "".join(
        "  job%d:\n    container:\n      image: ghcr.io/spiritecosse/storm-ci@sha256:%s\n" % (i, d)
        for i, d in enumerate(digests))
    return "on: push\njobs:\n" + jobs


class FixtureDir:
    """A throwaway workflows directory holding {filename: text}."""

    def __init__(self, files):
        self.files = files

    def __enter__(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        for name, text in self.files.items():
            (root / name).write_text(text, encoding="utf-8")
        return root

    def __exit__(self, *exc):
        self.tmp.cleanup()


class ProblemsTest(unittest.TestCase):
    def test_agreeing_pins_across_files_pass(self):
        with FixtureDir({"ci.yml": workflow(DIGEST_A, DIGEST_A), "sweep.yml": workflow(DIGEST_A)}) as d:
            self.assertEqual(ccd.problems(ccd.pins_in(d)), [])

    def test_drift_between_files_is_reported_with_each_location(self):
        with FixtureDir({"ci.yml": workflow(DIGEST_A), "sweep.yml": workflow(DIGEST_B)}) as d:
            [problem] = ccd.problems(ccd.pins_in(d))
        self.assertIn("drift", problem)
        self.assertIn("ci.yml:5", problem)
        self.assertIn("sweep.yml:5", problem)

    def test_drift_within_one_file_is_reported(self):
        with FixtureDir({"ci.yml": workflow(DIGEST_A, DIGEST_B)}) as d:
            self.assertEqual(len(ccd.problems(ccd.pins_in(d))), 1)

    def test_fewer_than_two_pins_fails_rather_than_passing_vacuously(self):
        # A renamed image reference would leave nothing to compare.
        for files in ({"ci.yml": workflow(DIGEST_A)}, {"ci.yml": "on: push\n"}):
            with FixtureDir(files) as d:
                [problem] = ccd.problems(ccd.pins_in(d))
            self.assertIn("at least 2", problem)

    def test_yaml_extension_and_non_workflow_files(self):
        with FixtureDir({"a.yaml": workflow(DIGEST_A), "b.yml": workflow(DIGEST_A),
                         "notes.txt": workflow(DIGEST_B)}) as d:
            self.assertEqual(len(ccd.pins_in(d)), 2)

    def test_short_or_tag_references_are_not_pins(self):
        text = workflow(DIGEST_A, DIGEST_A) + "# storm-ci@sha256:abc\n# storm-ci:latest\n"
        with FixtureDir({"ci.yml": text}) as d:
            self.assertEqual(len(ccd.pins_in(d)), 2)

    def test_real_workflows_agree(self):
        self.assertEqual(ccd.problems(ccd.pins_in(ccd.DEFAULT_WORKFLOWS_DIR)), [])


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(CHECKER)] + list(args),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)

    def test_agreeing_pins_exit_zero(self):
        with FixtureDir({"ci.yml": workflow(DIGEST_A, DIGEST_A)}) as d:
            result = self.run_cli(str(d))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 storm-ci pins agree", result.stdout)

    def test_drift_exits_one_with_an_actions_annotation(self):
        with FixtureDir({"ci.yml": workflow(DIGEST_A, DIGEST_B)}) as d:
            result = self.run_cli(str(d))
        self.assertEqual(result.returncode, 1)
        self.assertIn("::error::", result.stderr)

    def test_missing_directory_exits_one(self):
        self.assertEqual(self.run_cli("/nonexistent/workflows").returncode, 1)


if __name__ == "__main__":
    unittest.main()
