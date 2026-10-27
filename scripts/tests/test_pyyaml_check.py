#!/usr/bin/env python3
"""Tests for cmake/pyyaml.cmake's storm_require_pyyaml() (issue #703).

tests/ and benchmarks/ run PyYAML-dependent scripts at BUILD time, but
find_package(Python3) accepts any interpreter, so a fresh tree that picked a Python
without PyYAML configured fine and then failed mid-build with
`ModuleNotFoundError: No module named 'yaml'`. storm_require_pyyaml() makes that a
configure-time FATAL_ERROR naming the interpreter and both fixes.

The REAL cmake/pyyaml.cmake is included from a minimal harness project (LANGUAGES
NONE, no compiler). Python3_EXECUTABLE points at a stub that exits 0 or 1, so the
host's own Python never decides the outcome. Needs plain `cmake` only. Stdlib
unittest, no syntax newer than Python 3.7.

    python3 scripts/tests/test_pyyaml_check.py
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
PYYAML_CMAKE = REPO_ROOT / "cmake" / "pyyaml.cmake"

HARNESS = """cmake_minimum_required(VERSION 3.16)
project(pyyaml_harness LANGUAGES NONE)
include("{module}")
storm_require_pyyaml()
"""


STUB = """#!/bin/sh
exit {code}
"""


class RequirePyyamlTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise AssertionError("cmake not found on PATH; the harness configures with it")

    def configure(self, stub_exit_code):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "src").mkdir()
            (root / "src" / "CMakeLists.txt").write_text(HARNESS.format(module=PYYAML_CMAKE))
            python = root / "python3"
            python.write_text(STUB.format(code=stub_exit_code))
            python.chmod(0o755)
            proc = subprocess.run(
                ["cmake", "-S", str(root / "src"), "-B", str(root / "build"),
                 "-DPython3_EXECUTABLE=%s" % python],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
            return proc.returncode, proc.stdout, str(python)

    def test_configure_fails_when_the_interpreter_cannot_import_yaml(self):
        rc, out, python = self.configure(1)
        self.assertNotEqual(rc, 0)
        self.assertIn(python, out)

    def test_failure_offers_both_fixes(self):
        _, out, _ = self.configure(1)
        self.assertIn("pip install pyyaml", out)
        self.assertIn("-DPython3_EXECUTABLE", out)

    def test_configure_succeeds_when_the_interpreter_can_import_yaml(self):
        rc, out, _ = self.configure(0)
        self.assertEqual(rc, 0, out)

    def test_success_is_silent_about_pyyaml(self):
        _, out, _ = self.configure(0)
        self.assertNotIn("pyyaml", out.lower())


if __name__ == "__main__":
    unittest.main()
