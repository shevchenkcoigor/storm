#!/usr/bin/env python3
"""Tests for scripts/pre_commit_guards.py (issue #688).

The guards used to be six copy-shaped blocks in commit.sh. Now one table
(GUARDS) and one runner. `triggered_steps` is the pure decision — which commands
a staged-file list triggers — and `run_guards` executes them against fakes, so
neither needs a toolchain. Stdlib unittest only, no syntax newer than Python 3.7.

    python3 scripts/tests/test_pre_commit_guards.py
"""
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = REPO_ROOT / "scripts" / "pre_commit_guards.py"

sys.path.insert(0, str(HELPER.parent))
import pre_commit_guards as pcg  # noqa: E402


def names(guards):
    return [g.name for g in guards]


class TriggerTest(unittest.TestCase):
    def test_no_staged_files_trigger_nothing(self):
        self.assertEqual(pcg.triggered_guards([]), [])

    def test_unrelated_files_trigger_nothing(self):
        self.assertEqual(pcg.triggered_guards(["docs/README.md", "src/orm/x.cpp"]), [])

    def test_agent_file_triggers_frontmatter_only(self):
        hit = pcg.triggered_guards([".claude/agents/reviewer.md"])
        self.assertEqual(names(hit), ["agent frontmatter"])

    def test_checker_staged_runs_self_test_before_checker(self):
        guard = pcg.triggered_guards(["scripts/check_agent_frontmatter.py"])[0]
        commands = [s.command[-1] for s in pcg.triggered_steps(guard, ["scripts/check_agent_frontmatter.py"])]
        self.assertEqual([pathlib.Path(c).name for c in commands],
                         ["test_check_agent_frontmatter.py", "check_agent_frontmatter.py"])

    def test_agent_file_alone_skips_the_self_test(self):
        guard = pcg.triggered_guards([".claude/agents/a.md"])[0]
        steps = pcg.triggered_steps(guard, [".claude/agents/a.md"])
        self.assertEqual([pathlib.Path(s.command[-1]).name for s in steps], ["check_agent_frontmatter.py"])

    def test_cppm_triggers_backend_guard_checker_only(self):
        guard = pcg.triggered_guards(["src/orm/queryset.cppm"])[0]
        steps = pcg.triggered_steps(guard, ["src/orm/queryset.cppm"])
        self.assertEqual([pathlib.Path(s.command[-1]).name for s in steps], ["check-orm-backend-imports.sh"])

    def test_backend_checker_staged_also_runs_its_self_test(self):
        staged = ["scripts/check-orm-backend-imports.sh"]
        steps = pcg.triggered_steps(pcg.triggered_guards(staged)[0], staged)
        self.assertEqual([pathlib.Path(s.command[-1]).name for s in steps],
                         ["check-orm-backend-imports.sh", "test_check_orm_backend_imports.sh"])

    def test_commit_sh_triggers_the_script_self_tests(self):
        self.assertEqual(
            set(names(pcg.triggered_guards(["commit.sh"]))),
            {"self-heal configure", "PostgreSQL socket default", "line-coverage parser",
             "uncovered-lines helper", "pre-commit guards", "smart-skip table",
             "clang-tidy prerequisites"})

    def test_every_guard_target_exists(self):
        for guard in pcg.GUARDS:
            for step in guard.steps:
                self.assertTrue(pathlib.Path(step.command[-1]).exists(), step.command)

    def test_every_guard_is_reachable_by_its_own_files(self):
        # A trigger regex that matches none of the files it guards would be dead.
        for guard in pcg.GUARDS:
            for step in guard.steps:
                self.assertTrue(
                    pcg.re.match(guard.trigger, str(pathlib.Path(step.command[-1]).relative_to(pcg.ROOT))),
                    "%s: %s not covered by trigger" % (guard.name, step.command[-1]))


class RunTest(unittest.TestCase):
    def guard(self, *commands, log=False):
        steps = tuple(pcg.Step(None, c, "step %d failed" % i) for i, c in enumerate(commands))
        return pcg.Guard("demo", r"^f$", steps, "demo ok", log)

    def run_guard(self, guard, log_stream=None):
        out = io.StringIO()
        code = pcg.run_guards([guard], ["f"], out, log_stream, "demo.log")
        return code, out.getvalue()

    def test_passing_guard_prints_ok_and_returns_zero(self):
        code, out = self.run_guard(self.guard(["true"]))
        self.assertEqual(code, 0)
        self.assertIn("✓ demo ok", out)

    def test_failing_step_blocks_with_its_message_and_stops(self):
        code, out = self.run_guard(self.guard(["false"], ["true"]))
        self.assertEqual(code, 1)
        self.assertIn("COMMIT BLOCKED — step 0 failed", out)
        self.assertNotIn("demo ok", out)

    def test_log_stream_receives_output_and_block_names_the_log(self):
        with tempfile.TemporaryFile("w+") as log:
            code, out = self.run_guard(
                self.guard(["sh", "-c", "echo boom; exit 3"], log=True), log)
            log.seek(0)
            logged = log.read()
        self.assertEqual(code, 1)
        self.assertIn("boom", logged)
        self.assertIn("full output: demo.log", out)

    def test_every_step_of_a_logged_guard_appends_to_the_same_stream(self):
        with tempfile.TemporaryFile("w+") as log:
            self.run_guard(self.guard(["sh", "-c", "echo one"], ["sh", "-c", "echo two"], log=True), log)
            log.seek(0)
            logged = log.read()
        self.assertEqual(logged.split(), ["one", "two"])

    def test_untriggered_guard_runs_nothing(self):
        out = io.StringIO()
        self.assertEqual(pcg.run_guards([self.guard(["false"])], ["other"], out), 0)
        self.assertEqual(out.getvalue(), "")

    def test_missing_executable_blocks_rather_than_passing(self):
        code, _ = self.run_guard(self.guard(["/nonexistent/guard"]))
        self.assertEqual(code, 1)


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(HELPER)] + list(args), input="",
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)

    def test_empty_stdin_succeeds_silently(self):
        result = self.run_cli()
        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_log_fd_must_be_an_integer(self):
        self.assertEqual(self.run_cli("--log-fd", "x").returncode, 2)

    def test_closed_log_fd_does_not_crash_the_runner(self):
        # fd 99 is not open: the runner falls back to the terminal, it does not die.
        self.assertEqual(self.run_cli("--log-fd", "99", "--log-file", "x.log").returncode, 0)


if __name__ == "__main__":
    unittest.main()
