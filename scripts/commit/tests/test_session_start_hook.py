#!/usr/bin/env python3
"""Tests for .claude/hooks/session-start.sh (issue #651, ported from bash in #767).

`git config core.hooksPath .githooks` - the only thing that makes .githooks/pre-commit (and
therefore commit.sh: format, clang-tidy, tests) run at all - must
happen on every session start. #651: the wiring used to live only in CMakeLists.txt, i.e.
behind a successful cmake configure, which is precisely what a session with no
../clang-p2996 cannot do; such a session committed with no checks and no warning.

Strategy: build a throwaway project dir (a real `git init` repo), run the real hook against
it with a controlled environment, and assert on core.hooksPath and on stdout/stderr. The
hook itself stays bash; only the test is Python. Stdlib unittest, no syntax newer than
Python 3.7.

    python3 scripts/commit/tests/test_session_start_hook.py
"""
import json
import os
import pathlib
import shutil
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "lib"))
import testing  # noqa: E402

HOOK = REPO_ROOT / ".claude" / "hooks" / "session-start.sh"
SETTINGS = REPO_ROOT / ".claude" / "settings.json"
WIRED = ".githooks"


ENVIRONMENT = testing.environment_without_git_hook_variables()


def git(directory, *args):
    return testing.run(["git", "-C", directory] + list(args), env=ENVIRONMENT, merge_stderr=True)


def hooks_path(directory):
    """core.hooksPath values of `directory`'s own repository, as a list."""
    result = git(directory, "config", "--get-all", "core.hooksPath")
    return result.stdout.split()


class SessionStartHookTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.tmp = pathlib.Path(os.path.realpath(scratch.name))

    # --- Fixtures ---

    def make_project(self):
        """A throwaway project dir shaped like the repo: a git repo, a .githooks/pre-commit,
        and a copy of the hook under test at its real relative location (so the
        CLAUDE_PROJECT_DIR-less fallback resolves here)."""
        project = self.tmp / "proj"
        (project / ".claude" / "hooks").mkdir(parents=True)
        (project / ".githooks").mkdir()
        shutil.copy(str(HOOK), str(project / ".claude" / "hooks" / "session-start.sh"))
        self.write_executable(project / ".githooks" / "pre-commit", "#!/bin/bash\nexec ./commit.sh\n")
        self.assertEqual(git(project, "init", "--quiet").returncode, 0)
        return project

    @staticmethod
    def write_executable(path, text):
        path.write_text(text)
        path.chmod(0o755)

    def run_hook(self, project, script=None, cwd=None, project_dir=None):
        """Run the hook with a controlled environment. CLAUDE_PROJECT_DIR defaults to
        `project`, cwd to a directory that is NOT the project (a hook must not depend on the
        caller's cwd). project_dir="" runs it with the variable empty."""
        env = dict(ENVIRONMENT)
        env["CLAUDE_PROJECT_DIR"] = str(project) if project_dir is None else project_dir
        return testing.run(["bash", script or HOOK], env=env, cwd=str(cwd or self.tmp))

    def assert_wired(self, project, what):
        self.assertEqual(hooks_path(project), [WIRED], what)

    # --- Scenarios ---

    def test_wires_hooks(self):
        project = self.make_project()
        result = self.run_hook(project)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_wired(project, "core.hooksPath wired")
        self.assertEqual(result.stdout, "", "stdout stays clean when the wiring succeeds")

    def test_idempotent_across_runs(self):
        project = self.make_project()
        self.run_hook(project)
        self.run_hook(project)
        # Exactly one value: no duplicate core.hooksPath entries accumulate.
        self.assert_wired(project, "core.hooksPath still correct after a second run")

    def test_corrects_a_stale_hooks_path(self):
        project = self.make_project()
        git(project, "config", "core.hooksPath", ".git/hooks")
        self.run_hook(project)
        self.assert_wired(project, "a pre-existing wrong core.hooksPath is corrected")

    def test_falls_back_to_own_location(self):
        # CLAUDE_PROJECT_DIR is what Claude Code passes; the fallback matters for a manual run.
        project = self.make_project()
        result = self.run_hook(project, script=project / ".claude" / "hooks" / "session-start.sh",
                               project_dir="")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_wired(project, "core.hooksPath wired via the script-location fallback")

    def test_ignores_caller_cwd(self):
        project = self.make_project()
        elsewhere = self.tmp / "elsewhere"
        elsewhere.mkdir()
        git(elsewhere, "init", "--quiet")
        self.run_hook(project, cwd=elsewhere)
        self.assert_wired(project, "the project repo is wired regardless of cwd")
        self.assertEqual(hooks_path(elsewhere), [], "an unrelated repo at the caller's cwd is left alone")

    def test_warns_when_not_a_git_repo(self):
        # A wiring failure must be LOUD (#651's whole point is that a missing gate looked
        # identical to a passing one) but must not fail the session start. For a SessionStart
        # hook at exit 0, stdout is what becomes session context, so the warning goes there
        # as well as to stderr.
        result = self.run_hook(self.tmp / "not-a-repo")
        self.assertEqual(result.returncode, 0, "exit 0 even when the wiring fails")
        for channel, text in (("stderr", result.stderr), ("stdout (the session context)", result.stdout)):
            self.assertIn("core.hooksPath", text, channel)
            self.assertIn("commit.sh", text, channel)

    def test_refuses_to_wire_an_ancestor_repo(self):
        # `git -C <dir> config` walks UP to an enclosing repository, so a project dir that is
        # not a repo root must NOT be allowed to re-point an unrelated ancestor's hooks - and
        # must say so rather than reporting nothing.
        outer = self.tmp / "outer"
        outer.mkdir()
        git(outer, "init", "--quiet")
        nested = outer / "nested" / "project"
        nested.mkdir(parents=True)  # must exist, or the hook stops before the ancestor check
        result = self.run_hook(nested)
        self.assertEqual(result.returncode, 0, "exit 0 when the project dir is not a repo root")
        self.assertEqual(hooks_path(outer), [], "an enclosing repo's core.hooksPath is left untouched")
        self.assertIn("not a git repository root", result.stdout,
                      "the mismatch is reported, not silently skipped")

    def test_wired_repo_actually_runs_the_hook(self):
        # What the wiring is FOR: git must actually run .githooks/pre-commit after it, and a
        # hook that fails must abort the commit. Guards the relative-path resolution of
        # core.hooksPath, which is what makes one value work across worktrees.
        project = self.make_project()
        self.write_executable(project / ".githooks" / "pre-commit",
                              '#!/bin/bash\ntouch "$PWD/hook-ran"\nexit 1\n')
        self.run_hook(project)
        for key, value in (("user.email", "storm-test@example.com"), ("user.name", "Storm Test"),
                           ("commit.gpgsign", "false")):
            git(project, "config", key, value)
        (project / "main.cpp").write_text("int main() { return 0; }\n")
        git(project, "add", "main.cpp")
        commit = git(project, "commit", "-m", "staged C++ change")
        self.assertNotEqual(commit.returncode, 0, "a failing pre-commit hook aborts the commit")
        self.assertTrue((project / "hook-ran").exists(),
                        ".githooks/pre-commit actually ran (relative hooksPath resolves)")

    def test_settings_json_declares_the_hook(self):
        # #651's other half: the hook is inert unless .claude/settings.json declares it.
        # Deleting that block again must fail HERE - the CI job is named for the wiring, and a
        # job that cannot see the declaration would report green with the hook never running.
        self.assertTrue(SETTINGS.is_file(), ".claude/settings.json is missing")
        text = SETTINGS.read_text()
        self.assertIn('"SessionStart"', text, "no SessionStart entry - the hook never runs")
        self.assertIn(".claude/hooks/session-start.sh", text, "no SessionStart command names the hook")
        json.loads(text)  # not valid JSON -> Claude Code ignores the file

    def test_cmake_writer_agrees_on_the_value(self):
        # The value `.githooks` has two independent writers: this hook and CMakeLists.txt.
        # They must not drift - a relocated hooks dir would leave every `cmake --preset`
        # silently re-pointing core.hooksPath at the stale one.
        needle = "config core.hooksPath .githooks"
        self.assertIn(needle, (REPO_ROOT / "CMakeLists.txt").read_text(), "the two writers disagree")
        self.assertIn(needle, HOOK.read_text(), "the hook no longer sets .githooks")

    def test_real_repo_hook_is_executable(self):
        # The wiring is worthless if it points at something git cannot execute.
        for relative in (".githooks/pre-commit", "commit.sh"):
            self.assertTrue(os.access(str(REPO_ROOT / relative), os.X_OK),
                            "%s is missing or not executable" % relative)


if __name__ == "__main__":
    unittest.main()
