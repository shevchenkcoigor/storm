#!/usr/bin/env python3
"""Tests for the exit code .github/workflows/clang-tidy-sweep.yml reports (issue #725).

The sweep step pipes the runner through `tee`, so `$?` after the pipeline is tee's
status and the summary said "Exit code: 0" whatever the sweep found. These tests
run the step's real `run:` script under `bash -e` (what GitHub Actions uses when no
`shell:` is given) against a stub runner with a chosen exit status, and read back
what lands in $GITHUB_OUTPUT.

Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/tests/test_sweep_workflow.py
"""
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "clang-tidy-sweep.yml"

STEP = re.compile(r"- name: Run clang-tidy --all \(report-only\)\n(?P<body>(?:        .*\n|\n)+?)(?=\n      - name:|\Z)")


def sweep_script():
    """The `run: |` block of the sweep step, dedented."""
    match = STEP.search(WORKFLOW.read_text())
    if not match:
        raise AssertionError("sweep step not found in %s" % WORKFLOW)
    lines = match.group("body").split("run: |\n", 1)[1].splitlines()
    return "\n".join(line[10:] for line in lines if line.strip()) + "\n"


class SweepExitCodeTest(unittest.TestCase):
    def run_sweep(self, runner_status, runner_output="sweep output\n"):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        (root / "scripts").mkdir()
        stub = root / "scripts" / "run_clang_tidy.py"
        stub.write_text("#!/bin/sh\nprintf '%%s' '%s'\nexit %d\n" % (runner_output, runner_status))
        stub.chmod(0o755)
        output = root / "github_output"
        output.touch()
        script = sweep_script().replace("/tmp/sweep.log", str(root / "sweep.log"))
        env = dict(os.environ, GITHUB_OUTPUT=str(output))
        proc = subprocess.run(["bash", "-e", "-c", script], cwd=str(root), env=env,
                              capture_output=True, text=True)
        return proc, output.read_text(), (root / "sweep.log").read_text()

    def test_a_failing_sweep_is_reported_with_its_own_exit_code(self):
        proc, output, _log = self.run_sweep(3)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(output.strip(), "exit_code=3")

    def test_a_clean_sweep_is_reported_as_zero(self):
        _proc, output, _log = self.run_sweep(0)
        self.assertEqual(output.strip(), "exit_code=0")

    def test_the_step_itself_never_fails(self):
        # report-only: a red sweep must not fail the job, only the reported number
        proc, _output, _log = self.run_sweep(1)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_the_sweep_output_still_reaches_the_log(self):
        _proc, _output, log = self.run_sweep(1, "the summary block\n")
        self.assertIn("the summary block", log)

    def test_the_workflow_does_not_replace_bash_as_the_shell(self):
        # ${PIPESTATUS[0]} is a bash array; under `shell: sh` the echo would fail and
        # exit_code would never be written, while the tests above (which run bash)
        # stayed green.
        text = WORKFLOW.read_text()
        self.assertNotRegex(text, r"(?m)^\s*shell:")
        self.assertNotRegex(text, r"(?m)^\s*defaults:")

    def test_the_script_is_extracted_from_the_real_workflow(self):
        script = sweep_script()
        self.assertIn("./scripts/run_clang_tidy.py --all", script)
        self.assertIn("GITHUB_OUTPUT", script)


if __name__ == "__main__":
    unittest.main()
