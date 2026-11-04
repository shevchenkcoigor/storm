#!/usr/bin/env python3
"""Tests for cmake/libcxx.cmake symlink and host-layout behaviour (issue #326 phase 1,
ported from bash in #767).

Strategy: build a fake LIBCXX_ROOT layout in a temp dir, include the REAL
cmake/libcxx.cmake from a minimal harness CMakeLists, and assert the symlink (or its
absence) and the resolved paths per scenario. Stdlib unittest, no syntax newer than
Python 3.7.

Prerequisites: cmake at or above the project's own cmake_minimum_required (read from
CMakeLists.txt below) and ninja. Both are checked and reported up front (issue #645)
rather than assumed: the generated harness declares that same floor and configures with
-G Ninja, so either one missing aborts every configure before cmake/libcxx.cmake is ever
included, and the scenarios then fail for a reason none of them is about. Locally an
unusable tool SKIPS the whole file; under CI it FAILS, because CI takes its toolchain from
the pinned storm-ci image, so a missing tool there is a broken image and a skip would
report green for a test that never ran.

    python3 scripts/ci/tests/test_libcxx_modules_symlink.py
"""
import os
import pathlib
import re
import shutil
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "lib"))
import testing  # noqa: E402

LIBCXX_CMAKE = REPO_ROOT / "cmake" / "libcxx.cmake"

# Emitted by cmake/libcxx.cmake before any of the logic under test, so its presence in a
# scenario's log proves the include was actually REACHED - the distinction between
# "asserted on" and "trivially true because cmake never got there".
LIBCXX_REACHED_MARKER = "Using custom libcxx from:"

# How much of a failed configure's log to inline into the failure message.
CMAKE_LOG_TAIL_LINES = 20

HARNESS = """cmake_minimum_required(VERSION %(minimum)s)
project(libcxx_symlink_harness NONE)
set(LIBCXX_ROOT "%(libcxx_root)s")
%(extra_setup)s
# Includes the SAME production file CMakeLists.txt and cmake/libcxx.cmake use - not a
# re-implementation - so these scenarios exercise the real host-layout resolution logic,
# including its EXISTS validation.
include("%(libcxx_cmake)s")
message(STATUS "STORM_TEST_LIB_DIR=${_storm_libcxx_lib_dir}")
message(STATUS "STORM_TEST_BUILD_INCLUDE_DIR=${LIBCXX_BUILD_INCLUDE_DIR}")
message(STATUS "STORM_TEST_MODULES_JSON=${_storm_libcxx_modules_json}")
"""


def harness_cmake_minimum():
    """The floor the generated harness declares, READ from the project's own
    CMakeLists.txt rather than restated here, so the harness keeps mirroring the floor it
    exists to mirror after a bump, and the probe guards the same number it asks for."""
    text = (REPO_ROOT / "CMakeLists.txt").read_text()
    match = re.search(r"^[ \t]*cmake_minimum_required\(VERSION ([0-9][0-9.]*)\)", text, re.MULTILINE)
    if not match:
        raise AssertionError("no cmake_minimum_required(VERSION ...) in %s" % (REPO_ROOT / "CMakeLists.txt"))
    return match.group(1)


def version_key(version):
    """'3.28.3' -> (3, 28). Major/minor only, which is all the floor comparison needs."""
    parts = version.split(".")
    return int(parts[0]), int(parts[1]) if len(parts) > 1 else 0


def detected_cmake_version():
    """The running cmake's version, or None when there is no cmake on PATH or its
    --version output does not parse. The caller tells those two apart (they call for
    different fixes) rather than reporting both as absent."""
    try:
        first_line = testing.run(["cmake", "--version"], merge_stderr=True).stdout.splitlines()[0]
    except (OSError, IndexError):
        return None
    match = re.search(r"([0-9]+\.[0-9]+(\.[0-9]+)?)", first_line)
    return match.group(1) if match else None


def unusable_tool_reason(minimum):
    """Why the harness cannot configure, or "" when it can. Prints the tools about to run -
    one line of version would have collapsed #645 to a five-second diagnosis."""
    cmake_path = shutil.which("cmake")
    version = detected_cmake_version()
    reasons = []
    if not cmake_path:
        print("cmake: not found on PATH; harness requires >= %s" % minimum, file=sys.stderr)
        reasons.append("no cmake on PATH")
    elif not version:
        first_line = testing.run(["cmake", "--version"], merge_stderr=True).stdout.splitlines()[:1]
        print('cmake: unparseable version from %s: "%s"; harness requires >= %s'
              % (cmake_path, first_line[0] if first_line else "", minimum), file=sys.stderr)
        reasons.append("the cmake at %s reports no version this script can read" % cmake_path)
    else:
        print("cmake: %s (%s); harness requires >= %s" % (version, cmake_path, minimum), file=sys.stderr)
        if version_key(version) < version_key(minimum):
            reasons.append("cmake %s is older than the %s this harness requires" % (version, minimum))
    # The harness pins -G Ninja, so a missing ninja fails every configure in exactly the
    # opaque way an old cmake does - #645's shape reached through the other prerequisite.
    if not shutil.which("ninja"):
        print("ninja: not found on PATH; the harness configures with -G Ninja", file=sys.stderr)
        reasons.append("no ninja on PATH")
    return "; ".join(reasons)


def make_fake_libcxx_root(root, layout):
    """layout: "both" (a superset safe for the symlink-behaviour scenarios, which do not
    care which host branch resolves) | "triple" (ONLY the Linux/amd64 per-triple layout -
    no flat build/lib dir) | "flat" (ONLY the macOS-style flat layout - no triple
    subdirectory). The narrower layouts let a host-branch scenario prove resolution picked
    the RIGHT one: if the logic resolved the other layout instead, the validating
    cmake/clang_p2996_host.cmake would FATAL_ERROR on a path that was never created here,
    rather than the test passing by accident on an overlapping fixture."""
    (root / "build" / "modules" / "c++" / "v1").mkdir(parents=True)
    (root / "build" / "include" / "c++" / "v1").mkdir(parents=True)
    (root / "build" / "modules" / "c++" / "v1" / "std.cppm").touch()
    (root / "build" / "modules" / "c++" / "v1" / "std.compat.cppm").touch()
    if layout in ("triple", "both"):
        (root / "build" / "include" / "x86_64-unknown-linux-gnu" / "c++" / "v1").mkdir(parents=True)
        (root / "build" / "lib" / "x86_64-unknown-linux-gnu").mkdir(parents=True)
    if layout == "flat":
        (root / "build" / "lib").mkdir(parents=True, exist_ok=True)


class Configured(object):
    """One finished harness configure: its exit code, log and the paths a scenario asserts on."""

    def __init__(self, returncode, log, libcxx):
        self.returncode = returncode
        self.log = log
        self.libcxx = libcxx
        self.link_path = libcxx / "build" / "share" / "libc++" / "v1"
        self.link_target = libcxx / "build" / "modules" / "c++" / "v1"

    def test_var(self, name):
        """The last `STORM_TEST_<name>=` value the harness printed, or None when it never
        printed one - so an expected "" (include dir unset) cannot pass for a missing line."""
        values = re.findall(r"STORM_TEST_%s=(.*)" % name, self.log)
        return values[-1] if values else None

    def failure_context(self, message):
        """Inline the log tail into a failure message: the temp dir is deleted by the time
        anyone reads it, so merely NAMING the log would point at a path that no longer
        exists (issue #645)."""
        tail = "\n".join("    | " + line for line in self.log.splitlines()[-CMAKE_LOG_TAIL_LINES:])
        return "%s; last %d lines of cmake.log:\n%s" % (message, CMAKE_LOG_TAIL_LINES, tail)


class LibcxxModulesSymlinkTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.minimum = harness_cmake_minimum()
        reason = unusable_tool_reason(cls.minimum)
        if not reason:
            return
        message = "%s; every configure would fail before anything here is exercised." % reason
        if os.environ.get("CI"):
            raise AssertionError(message + " CI takes its toolchain from the pinned storm-ci "
                                           "image, so this is a broken image.")
        message += " Upgrade the tool."
        print("SKIP: " + message, file=sys.stderr)  # unittest hides a skip reason by default
        raise unittest.SkipTest(message)

    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.tmp = pathlib.Path(os.path.realpath(scratch.name))

    def configure(self, layout="both", extra_setup="", prepare=None):
        """Build the fake root, let `prepare(link_path, link_target)` stage the scenario's
        pre-existing state, then configure the harness with the real cmake/libcxx.cmake.

        extra_setup is cmake text forcing a host other than this machine's real one, so both
        the Linux/amd64 and Darwin/arm64 branches, and the error path, are exercisable
        wherever this runs."""
        libcxx = self.tmp / "libcxx"
        make_fake_libcxx_root(libcxx, layout)
        configured = Configured(0, "", libcxx)
        if prepare:
            prepare(configured.link_path, configured.link_target)
        workdir = self.tmp / "harness"
        workdir.mkdir()
        (workdir / "CMakeLists.txt").write_text(HARNESS % {
            "minimum": self.minimum, "libcxx_root": libcxx, "extra_setup": extra_setup,
            "libcxx_cmake": LIBCXX_CMAKE})
        # -G Ninja explicitly: storm always builds with Ninja (CMakePresets.json), and unlike
        # a Mac (Xcode Command Line Tools ships `make`) the storm-ci container only installs
        # ninja - CMake's platform-default generator has no build program there at all.
        result = testing.run(["cmake", "-G", "Ninja", "-B", "build", "."], cwd=str(workdir), merge_stderr=True)
        return Configured(result.returncode, result.stdout, libcxx)

    def assert_configured(self, configured):
        self.assertEqual(configured.returncode, 0, configured.failure_context("cmake configure failed"))

    # --- Scenarios ---

    def test_creates_symlink_when_missing(self):
        configured = self.configure()
        self.assert_configured(configured)
        link = configured.link_path
        self.assertTrue(link.is_symlink(), "expected symlink at %s, but it does not exist" % link)
        self.assertEqual(pathlib.Path(os.readlink(str(link))), configured.link_target)
        self.assertTrue((link / "std.cppm").is_file(), "symlink does not resolve std.cppm")

    def test_idempotent_when_symlink_exists(self):
        def identity(path):
            stat = os.lstat(str(path))  # the link itself, not what it points at
            return stat.st_ino, stat.st_mtime_ns

        saved = {}

        def remember(link_path, link_target):
            link_path.parent.mkdir(parents=True)
            os.symlink(str(link_target), str(link_path))
            saved["identity"] = identity(link_path)

        configured = self.configure(prepare=remember)
        self.assert_configured(configured)
        self.assertEqual(identity(configured.link_path), saved["identity"],
                         "existing correct symlink was modified (inode or mtime changed)")

    def test_refuses_when_share_is_real_dir(self):
        def real_directory(link_path, _link_target):
            link_path.mkdir(parents=True)
            (link_path / "keep.txt").touch()

        # No returncode check, by design: cmake may or may not FATAL_ERROR here, and what MUST
        # hold either way is filesystem state. But that makes both assertions below trivially
        # true for a configure that never reached cmake/libcxx.cmake - which is exactly how
        # #645's run reported "1 passed" while exercising nothing. Require proof first.
        configured = self.configure(prepare=real_directory)
        self.assertIn(LIBCXX_REACHED_MARKER, configured.log,
                      configured.failure_context("cmake never reached cmake/libcxx.cmake"))
        self.assertNotIn("Created libc++ modules symlink", configured.log,
                         "cmake/libcxx.cmake tried to link over a real directory")
        self.assertFalse(configured.link_path.is_symlink(), "real directory was replaced with a symlink")
        self.assertTrue((configured.link_path / "keep.txt").is_file(),
                        "existing content under share/libc++/v1 was destroyed")

    # Host-branch scenarios force CMAKE_HOST_SYSTEM_NAME/_storm_host_arch via extra_setup
    # rather than relying on the runner's real host, so both branches (and the error path)
    # are exercised regardless of whether this runs on macOS/arm64 or Linux/amd64.

    def assert_resolved_paths(self, configured, lib_dir, include_dir, modules_json):
        """cmake must have succeeded, and _storm_libcxx_lib_dir / LIBCXX_BUILD_INCLUDE_DIR /
        _storm_libcxx_modules_json must resolve to exactly the expected paths (include dir ""
        means "unset, no duplicate -I"). Each scenario's fake root contains ONLY its own
        layout - if resolution picked the wrong branch, cmake/clang_p2996_host.cmake's own
        EXISTS validation would FATAL_ERROR on a path this fixture never created."""
        self.assert_configured(configured)
        self.assertEqual(configured.test_var("LIB_DIR"), str(lib_dir), "lib dir")
        self.assertEqual(configured.test_var("BUILD_INCLUDE_DIR"), str(include_dir), "include dir")
        self.assertEqual(configured.test_var("MODULES_JSON"), str(modules_json), "modules.json")

    def test_resolves_linux_amd64_triple_paths(self):
        configured = self.configure(
            layout="triple",
            extra_setup='set(CMAKE_HOST_SYSTEM_NAME "Linux")\nset(_storm_host_arch "x86_64")')
        build = configured.libcxx / "build"
        self.assert_resolved_paths(
            configured,
            build / "lib" / "x86_64-unknown-linux-gnu",
            build / "include" / "x86_64-unknown-linux-gnu" / "c++" / "v1",
            build / "lib" / "x86_64-unknown-linux-gnu" / "libc++.modules.json")

    def test_resolves_darwin_arm64_flat_paths(self):
        configured = self.configure(
            layout="flat",
            extra_setup='set(CMAKE_HOST_SYSTEM_NAME "Darwin")\nset(_storm_host_arch "arm64")')
        build = configured.libcxx / "build"
        self.assert_resolved_paths(configured, build / "lib", "", build / "lib" / "libc++.modules.json")

    def test_rejects_unsupported_host(self):
        configured = self.configure(
            extra_setup='set(CMAKE_HOST_SYSTEM_NAME "Windows")\nset(_storm_host_arch "x86_64")')
        self.assertNotEqual(configured.returncode, 0,
                            "cmake configure succeeded for an unsupported host; expected FATAL_ERROR")
        self.assertIn("Unsupported host for clang-p2996", configured.log,
                      configured.failure_context("expected a clear 'Unsupported host' error"))


if __name__ == "__main__":
    unittest.main()
