"""Shared fake-repo scaffolding for the tests of scripts/ci/check_*.py checkers.

A subclass sets CHECKER, builds its tree in setUp() after calling super().setUp(), and
inherits write/append/run_checker/assert_fails. Not a test module itself.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


class FakeRepoBase(unittest.TestCase):
    CHECKER = pathlib.Path()

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = pathlib.Path(tmp.name)

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def append(self, relative, line):
        with open(str(self.root / relative), "a") as handle:
            handle.write(line + "\n")

    def run_checker(self, root=None, env=None):
        environment = dict(os.environ)
        environment.pop("GITHUB_ACTIONS", None)
        environment.update(env or {})
        return subprocess.run([sys.executable, str(self.CHECKER), str(root if root is not None else self.root)],
                              capture_output=True, text=True, env=environment)

    def assert_fails(self, text, proc=None):
        proc = proc or self.run_checker()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(text, proc.stderr)
        return proc
