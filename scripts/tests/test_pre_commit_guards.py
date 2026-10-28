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
from unittest import mock

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
        self.assertEqual([pathlib.Path(s.command[-1]).name for s in steps], ["check_orm_backend_imports.py"])

    def test_backend_checker_staged_runs_its_self_test_first(self):
        staged = ["scripts/check_orm_backend_imports.py"]
        guard = next(g for g in pcg.triggered_guards(staged) if g.name == "ORM backend neutrality")
        steps = pcg.triggered_steps(guard, staged)
        self.assertEqual([pathlib.Path(s.command[-1]).name for s in steps],
                         ["test_check_orm_backend_imports.py", "check_orm_backend_imports.py"])

    def test_commit_sh_triggers_the_script_self_tests(self):
        self.assertEqual(
            set(names(pcg.triggered_guards(["commit.sh"]))),
            {"self-heal configure", "PostgreSQL socket default", "line-coverage parser",
             "uncovered-lines helper", "pre-commit guards", "smart-skip table",
             "clang-tidy prerequisites"})

    def test_cmake_format_files_trigger_their_self_test(self):
        self.assertEqual(names(pcg.triggered_guards(["scripts/tests/test_cmake_format_target.sh"])),
                         ["cmake-format target"])
        # Any .cmake file also triggers the CPM cold-cache scan (#709).
        self.assertEqual(set(names(pcg.triggered_guards(["cmake/format.cmake"]))),
                         {"cmake-format target", "CPM cold-cache fetch"})

    def test_cmake_files_trigger_the_cpm_cold_cache_scan(self):
        for staged in ("cmake/uuid.cmake", "python/CMakeLists.txt", "tools/CMakeLists.txt",
                       "scripts/tests/test_cpm_cold_cache.sh"):
            self.assertIn("CPM cold-cache fetch", names(pcg.triggered_guards([staged])), staged)

    def test_pyyaml_check_files_trigger_their_self_test(self):
        self.assertEqual(names(pcg.triggered_guards(["scripts/tests/test_pyyaml_check.py"])),
                         ["PyYAML configure check"])
        # Any .cmake file also triggers the CPM cold-cache scan (#709).
        self.assertEqual(set(names(pcg.triggered_guards(["cmake/pyyaml.cmake"]))),
                         {"PyYAML configure check", "CPM cold-cache fetch"})

    def test_bash_scripts_commit_sh_runs_trigger_the_bash_3_2_scan(self):
        # test_line_coverage.sh scans these for bash-4-only constructs (#689).
        for staged in ("scripts/lib/line_coverage.sh", "scripts/detect-changes.sh", "scripts/coverage-run-batched.sh"):
            self.assertIn("line-coverage parser", names(pcg.triggered_guards([staged])), staged)

    def test_clang_tidy_runner_guard_triggers_on_every_file_it_runs(self):
        # The runner tests drive the real script through a fake toolchain, so the
        # helper that builds it is as much a trigger as the script itself (#692).
        for staged in ("scripts/run_clang_tidy.py", "scripts/clang_tidy_skiplist.py",
                       "scripts/tests/test_clang_tidy_skiplist.py", "scripts/tests/test_run_clang_tidy.py",
                       "scripts/tests/test_run_clang_tidy_full.py", "scripts/tests/tidy_toolchain.py"):
            self.assertIn("clang-tidy runner", names(pcg.triggered_guards([staged])), staged)

    def test_every_guard_target_exists(self):
        for guard in pcg.GUARDS:
            for step in guard.steps:
                self.assertTrue(pathlib.Path(step.command[-1]).exists(), step.command)

    def test_every_script_self_test_is_run_by_a_guard(self):
        # CI runs the table with --all, so a self-test no guard names runs nowhere —
        # four of them did before #708. New ones must join the table.
        run = {pathlib.Path(step.command[-1]).name for guard in pcg.GUARDS for step in guard.steps}
        tests = {p.name for p in (pcg.ROOT / "scripts" / "tests").glob("test_*") if p.is_file()}
        self.assertEqual(sorted(tests - run - set(pcg.NOT_GUARDED)), [])

    def test_not_guarded_entries_exist_and_are_really_unguarded(self):
        run = {pathlib.Path(step.command[-1]).name for guard in pcg.GUARDS for step in guard.steps}
        for name in pcg.NOT_GUARDED:
            self.assertTrue((pcg.ROOT / "scripts" / "tests" / name).is_file(), name)
            self.assertNotIn(name, run)

    def test_workflow_edit_triggers_the_digest_check(self):
        for staged in (".github/workflows/ci.yml", ".github/workflows/bench-ratio-spike.yml"):
            self.assertEqual(names(pcg.triggered_guards([staged])), ["storm-ci digest pins"], staged)

    def test_orphaned_self_tests_trigger_on_their_scripts(self):
        for staged, guard in (("scripts/ninjalog_stats.py", "ninjalog stats"),
                              ("scripts/time_trace.py", "time-trace capture"),
                              ("scripts/tu_ablation.py", "TU ablation transforms"),
                              ("scripts/coverage-run-batched.sh", "coverage batch parallelism")):
            self.assertIn(guard, names(pcg.triggered_guards([staged])), staged)

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


class RunAllTest(unittest.TestCase):
    """--all (#708): what CI runs, so every guard regardless of staged files."""

    def guard(self, name, *steps):
        return pcg.Guard(name, r"^never-staged$", tuple(steps), name + " ok")

    def step(self, command, trigger=r"^never-staged$"):
        return pcg.Step(trigger, command, "%s failed" % command[-1])

    def run_all(self, *guards):
        out = io.StringIO()
        return pcg.run_all_guards(guards, out), out.getvalue()

    def test_untriggered_guards_and_step_triggers_are_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = pathlib.Path(tmp) / "ran"
            code, out = self.run_all(self.guard("g", self.step(["true"]),
                                                self.step(["sh", "-c", "touch '%s'" % marker])))
            self.assertTrue(marker.exists())
        self.assertEqual(code, 0)
        self.assertIn("✓ g ok", out)

    def test_continues_past_a_failing_guard_and_returns_one(self):
        code, out = self.run_all(self.guard("bad", self.step(["false"])),
                                 self.guard("good", self.step(["true"])))
        self.assertEqual(code, 1)
        self.assertIn("✓ good ok", out)

    def test_a_failing_step_stops_the_rest_of_its_own_guard(self):
        # Self-tests run before their checker: a broken checker must not be
        # reported as a broken tree.
        code, out = self.run_all(self.guard("g", self.step(["false"]), self.step(["/nonexistent/second"])))
        self.assertEqual(code, 1)
        self.assertNotIn("second failed", out)

    def test_failure_is_not_worded_as_a_blocked_commit(self):
        _, out = self.run_all(self.guard("bad", self.step(["false"])))
        self.assertIn("GUARD FAILED — false failed", out)
        self.assertNotIn("COMMIT BLOCKED", out)

    def test_summary_names_every_failed_guard(self):
        _, out = self.run_all(self.guard("one", self.step(["false"])), self.guard("two", self.step(["false"])))
        self.assertIn("2 guard(s) failed: one, two", out)


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(HELPER)] + list(args), input="",
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)

    def test_empty_stdin_succeeds_silently(self):
        result = self.run_cli()
        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_log_fd_is_not_a_command_line_option(self):
        # The descriptor is the constant LOG_FD: nothing from argv picks an fd.
        self.assertEqual(self.run_cli("--log-fd", "3").returncode, 2)

    def test_closed_log_fd_does_not_crash_the_runner(self):
        # fd 3 is not open in the child: the runner falls back to the terminal.
        self.assertEqual(self.run_cli("--log-file", "x.log").returncode, 0)

    def test_all_flag_runs_the_whole_table_without_reading_stdin(self):
        # In-process: the real table includes this very test, so a subprocess
        # `--all` here would recurse.
        with mock.patch.object(pcg, "run_all_guards", return_value=0) as run_all, \
                mock.patch.object(pcg.sys, "stdin") as stdin:
            self.assertEqual(pcg.main(["pre_commit_guards.py", "--all"]), 0)
        run_all.assert_called_once_with(pcg.GUARDS, pcg.sys.stdout)
        stdin.read.assert_not_called()

    def test_all_flag_propagates_failure(self):
        with mock.patch.object(pcg, "run_all_guards", return_value=1):
            self.assertEqual(pcg.main(["pre_commit_guards.py", "--all"]), 1)


class OpenLogFdTest(unittest.TestCase):
    """In-process on purpose: feeding the real guard table `commit.sh` would run this very test."""

    def test_returns_a_stream_on_an_open_descriptor(self):
        with tempfile.TemporaryFile("w+") as log, mock.patch.object(pcg, "LOG_FD", log.fileno()):
            stream = pcg.open_log_fd()
            stream.write("hello")
            stream.flush()
            log.seek(0)
            self.assertEqual(log.read(), "hello")

    def test_returns_none_when_the_descriptor_is_not_open(self):
        with mock.patch.object(pcg, "LOG_FD", 987):
            self.assertIsNone(pcg.open_log_fd())

    def test_negative_descriptor_is_none_not_a_crash(self):
        with mock.patch.object(pcg, "LOG_FD", -1):
            self.assertIsNone(pcg.open_log_fd())


if __name__ == "__main__":
    unittest.main()
