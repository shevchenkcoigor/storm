#!/usr/bin/env python3
"""Tests for scripts/ci/check_cpm_vendored.py (issue #851).

cmake/CPM_<version>.cmake is CPM.cmake vendored byte for byte, so the bootstrap needs no network.
cmake/cpm.cmake records the version (CPM_DOWNLOAD_VERSION) and the file's SHA256 (CPM_SHA256);
the checker fails when there is not exactly one vendored file, when its name disagrees with the
recorded version, or when its bytes disagree with the recorded hash. Run against throwaway trees
built to break each rule, plus the real repository. Stdlib unittest, no syntax newer than
Python 3.7.

    python3 scripts/ci/tests/test_check_cpm_vendored.py
"""
import hashlib
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve()
REPO_ROOT = HERE.parents[3]
CHECKER = REPO_ROOT / "scripts" / "ci" / "check_cpm_vendored.py"

CONTENT = b"# vendored CPM.cmake\nmessage(STATUS CPM)\n"
VERSION = "1.2.3"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def cpm_cmake(version=VERSION, digest=None):
    digest = sha256(CONTENT) if digest is None else digest
    return "set(CPM_DOWNLOAD_VERSION %s)\nset(CPM_SHA256 %s)\ninclude(x)\n" % (version, digest)


def build_tree(root, vendored=(("CPM_%s.cmake" % VERSION, CONTENT),), cpm_text=None):
    (root / "cmake").mkdir()
    for name, data in vendored:
        (root / "cmake" / name).write_bytes(data)
    if cpm_text is not None:
        (root / "cmake" / "cpm.cmake").write_text(cpm_text)


def run_checker(root):
    proc = subprocess.run([sys.executable, str(CHECKER), "--root", str(root)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    return proc.returncode, proc.stdout


class CheckCpmVendoredTest(unittest.TestCase):
    def check(self, **tree):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tree.setdefault("cpm_text", cpm_cmake())
            build_tree(root, **tree)
            return run_checker(root)

    def test_consistent_tree_passes(self):
        code, out = self.check()
        self.assertEqual(code, 0, out)

    def test_no_vendored_file_fails(self):
        code, out = self.check(vendored=())
        self.assertEqual(code, 1)
        self.assertIn("exactly one", out)

    def test_two_vendored_files_fail(self):
        code, out = self.check(vendored=(("CPM_1.2.3.cmake", CONTENT), ("CPM_1.2.4.cmake", CONTENT)))
        self.assertEqual(code, 1)
        self.assertIn("exactly one", out)

    def test_name_version_mismatch_fails(self):
        code, out = self.check(cpm_text=cpm_cmake(version="1.2.4"))
        self.assertEqual(code, 1)
        self.assertIn("CPM_DOWNLOAD_VERSION", out)

    def test_hash_mismatch_fails(self):
        code, out = self.check(cpm_text=cpm_cmake(digest="0" * 64))
        self.assertEqual(code, 1)
        self.assertIn("SHA256", out)

    def test_modified_vendored_bytes_fail(self):
        code, out = self.check(vendored=(("CPM_1.2.3.cmake", CONTENT + b"# edited\n"),))
        self.assertEqual(code, 1)
        self.assertIn("SHA256", out)

    def test_missing_version_variable_fails(self):
        code, out = self.check(cpm_text="set(CPM_SHA256 %s)\n" % sha256(CONTENT))
        self.assertEqual(code, 1)
        self.assertIn("CPM_DOWNLOAD_VERSION", out)

    def test_missing_hash_variable_fails(self):
        code, out = self.check(cpm_text="set(CPM_DOWNLOAD_VERSION %s)\n" % VERSION)
        self.assertEqual(code, 1)
        self.assertIn("CPM_SHA256", out)

    def test_missing_cpm_cmake_fails(self):
        code, out = self.check(cpm_text=None)
        self.assertEqual(code, 1)
        self.assertIn("cpm.cmake", out)

    def test_uppercase_hash_is_accepted(self):
        code, out = self.check(cpm_text=cpm_cmake(digest=sha256(CONTENT).upper()))
        self.assertEqual(code, 0, out)


class RealRepositoryTest(unittest.TestCase):
    def test_the_repository_is_consistent(self):
        code, out = run_checker(REPO_ROOT)
        self.assertEqual(code, 0, out)


class StaleCacheTest(unittest.TestCase):
    """A build tree configured before #851 caches CPM_DIRECTORY=<build>/cmake (CACHE INTERNAL).
    The vendored file returns early when that differs from its own directory, defining nothing,
    so every reconfigure of an existing tree died with 'Unknown CMake command CPMAddPackage'."""

    HARNESS = (
        'cmake_minimum_required(VERSION 3.16)\n'
        'project(cpm_stale_harness LANGUAGES NONE)\n'
        'include("%s")\n'
        'if(NOT COMMAND CPMAddPackage)\n'
        '  message(FATAL_ERROR "CPMAddPackage is not defined")\n'
        'endif()\n')

    def configure(self, *extra):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "CMakeLists.txt").write_text(self.HARNESS % (REPO_ROOT / "cmake" / "cpm.cmake"))
            proc = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build")] + list(extra),
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
            return proc.returncode, proc.stdout

    def test_fresh_tree_defines_cpm(self):
        code, out = self.configure()
        self.assertEqual(code, 0, out)

    def test_tree_with_a_stale_cpm_directory_still_defines_cpm(self):
        code, out = self.configure("-DCPM_DIRECTORY=/old/build/cmake")
        self.assertEqual(code, 0, out)


if __name__ == "__main__":
    unittest.main()
