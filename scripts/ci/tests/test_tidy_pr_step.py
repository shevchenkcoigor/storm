#!/usr/bin/env python3
"""Tests for the `clang-tidy` job in .github/workflows/ci.yml (issue #910).

clang-tidy runs on the lines a PR changed, in a job of its own - it is the only per-PR tidy
gate (the pre-commit hook no longer runs it). A quiet edit (dropping `--base`, narrowing the
condition, leaving the job out of `ci-gate`'s `needs:`, putting tidy back into `test`) would
turn it off with CI still green, so the shape of the job is pinned here and its
`Trust the checkout` script is run for real against a stub git.

Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/ci/tests/test_tidy_pr_step.py
"""
import os
import pathlib
import re
import stat
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "ci.yml"

TIDY_STEP = "clang-tidy (changed lines)"
JOB_END = r"(?=\n  [A-Za-z0-9_-]+:\n|\Z)"


def job_block(name):
    match = re.search(r"\n  %s:\n(?P<body>.*?)%s" % (re.escape(name), JOB_END), WORKFLOW.read_text(), re.DOTALL)
    if not match:
        raise AssertionError("job %r is gone from %s" % (name, WORKFLOW))
    return match.group("body")


def step_body(job, name):
    pattern = r"      - name: %s\n(?P<body>(?:        .*\n|\n)+?)(?=\n      - name:|\n  [^ ]|\Z)" % re.escape(name)
    match = re.search(pattern, job_block(job) + "\n")
    if not match:
        raise AssertionError("step %r of job %r is gone" % (name, job))
    return match.group("body")


def run_script(job, name):
    body = step_body(job, name)
    if "run: |\n" in body:
        return "\n".join(line[10:] for line in body.split("run: |\n", 1)[1].splitlines() if line.strip()) + "\n"
    return re.search(r"run: (.*)\n", body).group(1) + "\n"


def step_names(job):
    return re.findall(r"      - name: (.*)\n", job_block(job))


class JobShapeTest(unittest.TestCase):
    def test_it_runs_the_changed_lines_runner_against_the_base(self):
        self.assertEqual(run_script("clang-tidy", TIDY_STEP).strip(),
                         './scripts/tidy/run_clang_tidy.py --diff --base "$BASE_SHA"')

    def test_it_never_edits_the_checkout(self):
        self.assertNotIn("--fix", job_block("clang-tidy"))

    def test_it_is_gated_to_prs_and_the_merge_queue_and_stays_fail_closed(self):
        condition = re.search(r"\n    if: (.*)\n", job_block("clang-tidy")).group(1)
        self.assertIn("github.event_name == 'pull_request'", condition)
        self.assertIn("github.event_name == 'merge_group'", condition)
        self.assertIn("needs.changes.outputs.run_heavy != 'false'", condition)

    def test_the_base_comes_from_the_event_for_both_triggers(self):
        # The env line itself, not a substring of the job: a comment still naming the two
        # expressions must not let `BASE_SHA: ${{ github.sha }}` (an empty diff) through.
        self.assertIn("\n      BASE_SHA: ${{ github.event.pull_request.base.sha || github.event.merge_group.base_sha }}\n",
                      job_block("clang-tidy"))

    def test_the_condition_is_pinned_whole(self):
        condition = re.search(r"\n    if: (.*)\n", job_block("clang-tidy")).group(1)
        self.assertEqual(condition, "${{ !cancelled() && (github.event_name == 'pull_request' || "
                                    "github.event_name == 'merge_group') && needs.changes.outputs.run_heavy != 'false' }}")

    def test_a_tidy_failure_cannot_be_turned_into_a_green_job(self):
        live = "\n".join(line for line in job_block("clang-tidy").splitlines() if not line.lstrip().startswith("#"))
        self.assertNotIn("continue-on-error", live)
        self.assertNotIn("|| true", live)

    def test_the_checkout_has_the_history_the_merge_base_needs(self):
        self.assertRegex(step_body("clang-tidy", "Checkout"), r"fetch-depth: 0")

    def test_the_checkout_is_trusted_then_built_then_linted(self):
        names = step_names("clang-tidy")
        self.assertLess(names.index("Checkout"), names.index("Trust the checkout"))
        self.assertLess(names.index("Trust the checkout"), names.index("Build (release)"))
        self.assertLess(names.index("Configure (release)"), names.index("Build (release)"))
        self.assertEqual(names[-1], TIDY_STEP)

    def test_it_builds_the_release_preset_tidy_reads_the_database_of(self):
        self.assertIn("cmake --preset ninja-release", run_script("clang-tidy", "Configure (release)"))
        self.assertIn("cmake --build --preset ninja-release", run_script("clang-tidy", "Build (release)"))

    def test_it_has_a_timeout(self):
        self.assertRegex(job_block("clang-tidy"), r"\n    timeout-minutes: \d+\n")

    def test_the_workflow_does_not_replace_bash_as_the_shell(self):
        self.assertNotRegex(job_block("clang-tidy"), r"(?m)^\s*shell:")

    def test_ci_gate_waits_for_it(self):
        needs = re.search(r"\n    needs:\n((?:      - .*\n)+)", job_block("ci-gate")).group(1)
        self.assertIn("      - clang-tidy\n", needs)


class TestJobIsPlainTest(unittest.TestCase):
    def test_the_test_matrix_no_longer_runs_tidy(self):
        live = "\n".join(line for line in job_block("test").splitlines() if not line.lstrip().startswith("#"))
        self.assertNotIn("run_clang_tidy", live)
        self.assertFalse([name for name in step_names("test") if "tidy" in name], step_names("test"))

    def test_its_test_step_is_just_ctest(self):
        self.assertTrue(run_script("test", "Test").startswith("ctest "))


class TrustCheckoutScriptTest(unittest.TestCase):
    def test_the_workspace_is_trusted(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        (root / "bin").mkdir()
        log = root / "calls"
        git = root / "bin" / "git"
        git.write_text('#!/bin/sh\necho "git $*" >> "%s"\n' % log)
        git.chmod(git.stat().st_mode | stat.S_IEXEC)
        env = dict(os.environ, PATH="%s:%s" % (root / "bin", os.environ["PATH"]), GITHUB_WORKSPACE="/w/storm")
        proc = subprocess.run(["bash", "-e", "-c", run_script("clang-tidy", "Trust the checkout")],
                              env=env, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(log.read_text().strip(), "git config --global --add safe.directory /w/storm")

    def test_a_workspace_path_with_a_space_stays_one_argument(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        (root / "bin").mkdir()
        log = root / "calls"
        git = root / "bin" / "git"
        git.write_text('#!/bin/sh\necho "$#" >> "%s"\n' % log)
        git.chmod(git.stat().st_mode | stat.S_IEXEC)
        env = dict(os.environ, PATH="%s:%s" % (root / "bin", os.environ["PATH"]), GITHUB_WORKSPACE="/w/my storm")
        subprocess.run(["bash", "-e", "-c", run_script("clang-tidy", "Trust the checkout")], env=env,
                       capture_output=True, text=True, check=True)
        self.assertEqual(log.read_text().strip(), "5")


if __name__ == "__main__":
    unittest.main()
