#!/usr/bin/env python3
"""Tests for scripts/lib/testing.py's environment_without_git_hook_variables() (issue #774).

The git-driving self-tests run `git` in a throwaway repo. When commit.sh runs them from the
pre-commit hook, git has exported GIT_INDEX_FILE (and sometimes GIT_DIR); left in the
environment, those make every `git` call act on the REAL repository. Run by hand
`GIT_*` is unset, so those tests cannot notice a helper that stopped scrubbing - this one
sets the variables itself. Stdlib unittest, no syntax newer than Python 3.7.

    python3 scripts/lib/tests/test_testing.py
"""
import os
import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import testing  # noqa: E402

HOOK_VARIABLES = ("GIT_DIR", "GIT_INDEX_FILE", "GIT_WORK_TREE")


class EnvironmentWithoutGitHookVariablesTest(unittest.TestCase):
    def scrubbed(self, extra=None, **kwargs):
        env = {name: "/real/repo" for name in HOOK_VARIABLES}
        env.update({"STORM_PRE_COMMIT_HOOK": "1", "UNRELATED": "kept"})
        env.update(extra or {})
        with mock.patch.dict(os.environ, env):
            return testing.environment_without_git_hook_variables(**kwargs)

    def test_drops_the_variables_git_lists_as_local(self):
        listed = testing.run(["git", "rev-parse", "--local-env-vars"]).stdout.split()
        self.assertTrue(set(HOOK_VARIABLES) <= set(listed), "git no longer lists the variables this test sets")
        result = self.scrubbed()
        self.assertEqual([name for name in HOOK_VARIABLES if name in result], [])

    def test_keeps_everything_else(self):
        result = self.scrubbed()
        self.assertEqual(result["UNRELATED"], "kept")
        self.assertEqual(result["STORM_PRE_COMMIT_HOOK"], "1", "only also_drop names are removed beyond git's own")

    def test_also_drop_removes_extra_names(self):
        result = self.scrubbed(also_drop=("STORM_PRE_COMMIT_HOOK",))
        self.assertNotIn("STORM_PRE_COMMIT_HOOK", result)
        self.assertEqual(result["UNRELATED"], "kept")

    def test_returns_a_copy_and_leaves_os_environ_alone(self):
        with mock.patch.dict(os.environ, {"GIT_INDEX_FILE": "/real/repo/index"}):
            result = testing.environment_without_git_hook_variables()
            self.assertEqual(os.environ["GIT_INDEX_FILE"], "/real/repo/index")
        self.assertIsNot(result, os.environ)

    def test_rejects_a_bare_string_for_also_drop(self):
        with self.assertRaises(TypeError):
            testing.environment_without_git_hook_variables(also_drop="STORM_PRE_COMMIT_HOOK")

    def test_raises_instead_of_failing_open_when_git_cannot_list(self):
        failed = mock.Mock(returncode=129, stdout="", stderr="usage: git rev-parse")
        with mock.patch.object(testing, "run", return_value=failed):
            with self.assertRaises(RuntimeError):
                testing.environment_without_git_hook_variables()


if __name__ == "__main__":
    unittest.main()
