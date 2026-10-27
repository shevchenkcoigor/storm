#!/usr/bin/env python3
"""Tests for cmake/cpm-cache.cmake's storm_verify_cpm_cache() (issue #705).

An interrupted CPM `git clone` can leave `<cache>/<name>/<hash>/` holding a complete
`.git` but an empty working tree. CPM only checks that the directory exists, so it
reuses the checkout forever and every configure dies with a confusing
`include could not find requested file`. storm_verify_cpm_cache() turns that state
into a configure-time FATAL_ERROR naming the directory to delete.

The REAL cmake/cpm-cache.cmake is included from a minimal harness project (LANGUAGES
NONE, no compiler, no network) against a fabricated cache directory. Stdlib unittest,
no syntax newer than Python 3.7.

    python3 scripts/tests/test_cpm_cache_check.py
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CPM_CACHE_CMAKE = REPO_ROOT / "cmake" / "cpm-cache.cmake"

HARNESS = """cmake_minimum_required(VERSION 3.16)
project(cpm_cache_harness LANGUAGES NONE)
include("{module}")
{pre}
storm_verify_cpm_cache("{cache}")
"""


class VerifyCpmCacheTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake"):
            raise AssertionError("cmake not found on PATH; the harness configures with it")

    def configure(self, build_cache, cache_exists=True, hold_lock_of=None):
        """Configure the harness; build_cache(cache_path) fabricates the cache."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            cache = root / "cache"
            if cache_exists:
                cache.mkdir()
                build_cache(cache)
            pre = ""
            if hold_lock_of:
                lock = cache / hold_lock_of / "cmake.lock"
                pre = 'file(LOCK "%s" RESULT_VARIABLE held)' % lock
            (root / "src").mkdir()
            (root / "src" / "CMakeLists.txt").write_text(
                HARNESS.format(module=CPM_CACHE_CMAKE, cache=cache, pre=pre))
            proc = subprocess.run(
                ["cmake", "-S", str(root / "src"), "-B", str(root / "build")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
            return proc.returncode, proc.stdout, str(cache)

    @staticmethod
    def entry(cache, name, files=("CMakeLists.txt",)):
        path = cache / name / "abc123"
        (path / ".git").mkdir(parents=True)
        for f in files:
            (path / f).write_text("")
        return path

    def test_fails_when_a_checkout_has_only_dot_git(self):
        rc, out, cache = self.configure(lambda c: self.entry(c, "cmake-scripts", files=()))
        self.assertNotEqual(rc, 0)
        self.assertIn(str(pathlib.Path(cache) / "cmake-scripts" / "abc123"), out)

    def test_failure_tells_the_user_what_to_delete(self):
        _, out, _ = self.configure(lambda c: self.entry(c, "cmake-scripts", files=()))
        self.assertIn("rm -rf", out)

    def test_every_broken_checkout_is_named(self):
        def build(c):
            self.entry(c, "cmake-scripts", files=())
            self.entry(c, "stduuid", files=())
        _, out, _ = self.configure(build)
        self.assertIn("cmake-scripts", out)
        self.assertIn("stduuid", out)

    def test_checkout_being_cloned_under_a_held_lock_is_skipped(self):
        rc, out, _ = self.configure(
            lambda c: self.entry(c, "cmake-scripts", files=()), hold_lock_of="cmake-scripts")
        self.assertEqual(rc, 0, out)

    def test_other_broken_checkout_still_fails_when_one_is_locked(self):
        def build(c):
            self.entry(c, "cmake-scripts", files=())
            self.entry(c, "stduuid", files=())
        rc, out, _ = self.configure(build, hold_lock_of="cmake-scripts")
        self.assertNotEqual(rc, 0)
        self.assertIn("stduuid", out)
        self.assertNotIn("rm -rf", out.split("stduuid")[0].split("cmake-scripts")[-1])

    def test_healthy_checkout_passes(self):
        rc, out, _ = self.configure(lambda c: self.entry(c, "cmake-scripts"))
        self.assertEqual(rc, 0, out)

    def test_dot_git_less_directory_passes(self):
        def build(c):
            (c / "other" / "abc123").mkdir(parents=True)
        rc, out, _ = self.configure(build)
        self.assertEqual(rc, 0, out)

    def test_missing_cache_directory_passes(self):
        rc, out, _ = self.configure(lambda c: None, cache_exists=False)
        self.assertEqual(rc, 0, out)

    def test_empty_cache_argument_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "CMakeLists.txt").write_text(
                HARNESS.format(module=CPM_CACHE_CMAKE, cache="", pre=""))
            proc = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        self.assertEqual(proc.returncode, 0, proc.stdout)


if __name__ == "__main__":
    unittest.main()
