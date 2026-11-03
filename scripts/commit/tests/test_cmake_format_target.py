#!/usr/bin/env python3
"""Tests for cmake/format.cmake's `cmake-format` target (issue #690).

With cmake-format absent, upstream cmake-scripts creates no `cmake-format` target, so commit.sh's
cmake-format step used to fail with `unknown target 'cmake-format'` and only a configure-time
WARNING named the cause (#643). That warning lands in whichever step configured the tree —
clang-format's log when both formatters run on a fresh tree (#674) — so the step that failed did
not explain itself. cmake/format.cmake now defines a stub `cmake-format` target that fails at
BUILD time naming the missing tool.

What is asserted:
  - absent, fresh tree: configure + build in one go, the build fails and its output names the
    missing tool (the cmake-only-commit path);
  - absent, configured tree: the configure output is discarded (another step owned it) and the
    build output ALONE still names the missing tool;
  - present: the real tool runs and the build succeeds, with no stub message.

Strategy: include the REAL cmake/format.cmake from a minimal harness project, with cmake-scripts'
formatting.cmake replaced by a stub that mirrors upstream's cmake_format(): it creates
`cmake-format` only when CMAKE_FORMAT_EXE is set. CMAKE_FORMAT_EXE is passed on the command line,
so the host's own cmake-format (present or not) never decides the outcome. No compiler is needed
(project LANGUAGES NONE). The generator is pinned to Ninja — the one commit.sh's own builds use —
because the docker/ci image ships ninja but not make, so the default Unix Makefiles generator would
fail every configure there. Needs cmake and ninja. Stdlib unittest, no syntax newer than Python 3.7.

    python3 scripts/commit/tests/test_cmake_format_target.py
"""
import pathlib
import shutil
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "lib"))
import testing  # noqa: E402

FORMAT_CMAKE = testing.REPO_ROOT / "cmake" / "format.cmake"

# Line the stub target prints; the cmake/format.cmake message starts with it.
MISSING_TOOL_MSG = "cmake-format not found"

FORMATTING_STUB = """function(clang_format TARGET_NAME)
endfunction()
function(cmake_format TARGET_NAME)
  if(CMAKE_FORMAT_EXE)
    add_custom_target(${TARGET_NAME} COMMAND ${CMAKE_FORMAT_EXE} -i ${ARGN})
    add_custom_target(cmake-format)
    add_dependencies(cmake-format ${TARGET_NAME})
  endif()
endfunction()
"""

HARNESS = """cmake_minimum_required(VERSION 3.16)
project(format_harness LANGUAGES NONE)
set(cmake-scripts_SOURCE_DIR "{scripts_dir}")
include("{format_cmake}")
"""


class CmakeFormatTargetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for tool in ("cmake", "ninja"):
            if not shutil.which(tool):
                raise AssertionError("%s not found on PATH; the harness configures with -G Ninja" % tool)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = pathlib.Path(temporary.name)
        scripts_dir = self.tmp / "cmake-scripts"
        scripts_dir.mkdir()
        (scripts_dir / "formatting.cmake").write_text(FORMATTING_STUB, encoding="utf-8")
        (self.tmp / "src").mkdir()
        (self.tmp / "src" / "CMakeLists.txt").write_text(
            HARNESS.format(scripts_dir=scripts_dir, format_cmake=FORMAT_CMAKE), encoding="utf-8")

    def configure(self, format_exe):
        return testing.run(["cmake", "-G", "Ninja", "-S", self.tmp / "src", "-B", self.tmp / "build",
                            "-DCMAKE_FORMAT_EXE=%s" % format_exe], merge_stderr=True)

    def build_target(self):
        return testing.run(["cmake", "--build", self.tmp / "build", "--target", "cmake-format"],
                           merge_stderr=True)

    def write_fake_tool(self):
        """A stand-in cmake-format that records it ran."""
        tool = self.tmp / "cmake-format"
        tool.write_text('#!/bin/sh\ntouch "%s"\n' % (self.tmp / "tool-ran"), encoding="utf-8")
        tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
        return tool

    def test_absent_on_a_fresh_tree_fails_and_names_the_tool(self):
        # The cmake-only commit: cmake-format's own step configures, then builds.
        configured = self.configure("")
        self.assertEqual(configured.returncode, 0, "harness configure failed: " + configured.stdout)
        built = self.build_target()
        self.assertNotEqual(built.returncode, 0, "build succeeded with the tool absent")
        self.assertIn(MISSING_TOOL_MSG, configured.stdout + built.stdout)

    def test_absent_on_a_configured_tree_explains_itself_in_the_build_output_alone(self):
        # Both formatters run: clang-format's step configured the tree, so only the build
        # output reaches cmake-format's log. It must explain itself.
        configured = self.configure("")
        self.assertEqual(configured.returncode, 0, "harness configure failed: " + configured.stdout)
        built = self.build_target()
        self.assertNotEqual(built.returncode, 0, "build succeeded with the tool absent")
        self.assertIn(MISSING_TOOL_MSG, built.stdout, built.stdout[-300:])
        self.assertNotIn("unknown target", built.stdout.lower())
        self.assertNotIn("No rule to make target", built.stdout)

    def test_present_runs_the_real_tool(self):
        tool = self.write_fake_tool()
        configured = self.configure(tool)
        self.assertEqual(configured.returncode, 0, "harness configure failed: " + configured.stdout)
        built = self.build_target()
        self.assertEqual(built.returncode, 0, built.stdout[-300:])
        self.assertTrue((self.tmp / "tool-ran").exists(), "cmake-format did not run")
        self.assertNotIn(MISSING_TOOL_MSG, configured.stdout + built.stdout)


if __name__ == "__main__":
    unittest.main()
