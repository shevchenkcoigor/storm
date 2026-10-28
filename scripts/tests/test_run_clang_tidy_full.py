#!/usr/bin/env python3
"""Tests for scripts/run_clang_tidy.py: --full/--all modes, job count, one-file lint (issue #692).

Port of the file-scan half of the shell runner. The fake toolchain lives in
tidy_toolchain.py. Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/tests/test_run_clang_tidy_full.py
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from tidy_toolchain import CRASH, ERROR, SCRIPT, WARNING, ToolchainTest  # noqa: E402

sys.path.insert(0, str(SCRIPT.parent))
import run_clang_tidy as rct  # noqa: E402


class FullModeTest(ToolchainTest):
    def test_no_staged_files_is_a_pass(self):
        self.assert_passes(self.run_script("--full"), "✅ No staged C++ files — clang-tidy skipped")
        self.assertEqual(self.calls("tidy_calls"), [])

    def test_non_cpp_files_are_not_checked(self):
        self.stage("docs/NOTES.md")
        self.stage("scripts/tool.py")
        self.assert_passes(self.run_script("--full"), "No staged C++ files")

    def test_every_cpp_extension_is_checked(self):
        for name in ("a.cpp", "a.cppm", "a.h", "a.hpp"):
            self.stage("src/" + name)
        proc = self.run_script("--full")
        self.assertIn("Found 4 source files to check", proc.stdout)
        self.assertEqual(len(self.calls("tidy_calls")), 4)

    def test_third_party_is_excluded(self):
        self.stage("src/third_party/lib.cpp")
        self.stage("src/real.cpp")
        proc = self.run_script("--full")
        self.assertIn("Found 1 source files to check", proc.stdout)

    def test_a_clean_file_passes(self):
        self.stage("src/orm/fields.cppm")
        proc = self.run_script("--full", "-j", "1")
        self.assert_passes(proc, "✅ clang-tidy passed with no issues")
        self.assertIn("  ✓ src/orm/fields.cppm", proc.stdout)
        self.assertIn("Files checked: 1", proc.stdout)
        self.assertIn("Files passed: 1", proc.stdout)

    def test_clang_tidy_gets_the_database_and_the_file(self):
        self.stage("src/orm/fields.cppm")
        self.run_script("--full")
        self.assertEqual(self.calls("tidy_calls"), ["-p build/release src/orm/fields.cppm"])

    def test_fix_is_passed_through(self):
        self.stage("src/orm/fields.cppm")
        self.run_script("--full", "--fix")
        self.assertEqual(self.calls("tidy_calls")[0], "-p build/release -fix src/orm/fields.cppm")

    def test_a_warning_blocks_and_is_listed(self):
        self.stage("src/orm/fields.cppm")
        self.canned_tidy("src/orm/fields.cppm", WARNING + "\n")
        proc = self.run_script("--full")
        self.assert_fails(proc, "❌ clang-tidy found 1 warning(s) - fix before committing")
        self.assertIn("📋 Issues found:", proc.stdout)
        self.assertIn(WARNING, proc.stdout)
        self.assertIn("Run with --fix to auto-fix some issues", proc.stdout)

    def test_an_error_blocks(self):
        self.stage("src/orm/fields.cppm")
        self.canned_tidy("src/orm/fields.cppm", ERROR + "\n")
        self.assert_fails(self.run_script("--full"), "❌ clang-tidy found errors")

    def test_the_same_warning_from_two_files_is_counted_once(self):
        # Without the dedup each parallel worker that saw a header warning inflated
        # the count (50 real warnings once read as 665).
        self.stage("src/a.cpp")
        self.stage("src/b.cpp")
        self.canned_tidy("src/a.cpp", "src/shared.h:1:1: warning: shared [check]\n")
        self.canned_tidy("src/b.cpp", "src/shared.h:1:1: warning: shared [check]\n")
        proc = self.run_script("--full")
        self.assertIn("Warnings: 1", proc.stdout)
        self.assertIn("found 1 warning(s)", proc.stdout)

    def test_generated_and_suppressed_chatter_is_dropped(self):
        self.stage("src/a.cpp")
        self.canned_tidy("src/a.cpp", "12 warnings generated.\n1 warning and 2 errors generated.\n"
                                      "Suppressed 11 warnings (11 in non-user code).\n"
                                      "Use -header-filter=.* to display errors from all non-system headers.\n"
                                      "Use -system-headers to display errors from system headers as well.\n")
        self.assert_passes(self.run_script("--full"), "✅ clang-tidy passed with no issues")

    def test_the_issue_list_is_capped_at_fifty(self):
        self.stage("src/a.cpp")
        self.canned_tidy("src/a.cpp", "".join("src/a.cpp:%d:1: warning: w%03d [c]\n" % (n, n) for n in range(60)))
        proc = self.run_script("--full")
        self.assertIn("Warnings: 60", proc.stdout)
        self.assertEqual(sum(1 for line in proc.stdout.splitlines() if ": warning: w" in line), 50)

    def test_known_unparseable_compile_error_is_skipped(self):
        self.stage("shared/models.h")
        self.canned_tidy("shared/models.h", ERROR + "\nFound compiler error(s).\n")
        proc = self.run_script("--full")
        self.assert_passes(proc, "  ✓ shared/models.h (C++26 modules - skipped)")
        self.assertIn("C++26 skipped: 1", proc.stdout)
        self.assertNotIn("Errors: 1", proc.stdout)

    def test_compile_error_with_real_warnings_keeps_the_warnings(self):
        self.stage("src/a.cpp")
        self.canned_tidy("src/a.cpp", WARNING + "\n" + ERROR + "\nFound compiler error(s).\n")
        proc = self.run_script("--full")
        self.assert_fails(proc, "found 1 warning(s)")
        self.assertIn("  ❌ src/a.cpp (with C++26 header errors)", proc.stdout)
        self.assertIn("Errors: 0", proc.stdout)
        self.assertNotIn("Found compiler error", proc.stdout)

    def test_unexpected_parse_failure_is_loud(self):
        self.stage("src/orm/utilities.cppm")
        self.canned_tidy("src/orm/utilities.cppm", ERROR + "\nFound compiler error(s).\n")
        proc = self.run_script("--full")
        # The original error line is kept for the summary, so it is counted and
        # "found errors" outranks "could not parse" - as in the shell script.
        self.assert_fails(proc, "❌ clang-tidy found errors")
        self.assertIn("❌ src/orm/utilities.cppm (PARSE FAILURE — toolchain or build state is broken)", proc.stderr)
        self.assertIn("See Issue #262.", proc.stderr)
        self.assertIn("Unexpected crashes: 1", proc.stdout)
        self.assertIn(ERROR, proc.stdout)

    def test_crash_after_analysis_keeps_warnings_and_drops_the_backtrace(self):
        self.stage("src/a.cpp")
        self.canned_tidy("src/a.cpp", WARNING + "\n" + CRASH + "\nStack dump:\n0. Program arguments\n")
        proc = self.run_script("--full")
        self.assert_fails(proc, "found 1 warning(s)")
        self.assertIn("  ❌ src/a.cpp (with crash after analysis)", proc.stdout)
        self.assertNotIn("Stack dump", proc.stdout)
        self.assertNotIn("PLEASE submit", proc.stdout)

    def test_known_file_crash_without_warnings_is_skipped(self):
        self.stage("benchmarks/schema.cppm")
        self.canned_tidy("benchmarks/schema.cppm", CRASH + "\n")
        proc = self.run_script("--full")
        self.assert_passes(proc, "  ✓ benchmarks/schema.cppm (C++26 modules - skipped)")
        self.assertIn("C++26 skipped: 1", proc.stdout)

    def test_unexpected_crash_without_warnings_fails(self):
        self.stage("src/a.cpp")
        self.canned_tidy("src/a.cpp", CRASH + "\n")
        proc = self.run_script("--full")
        self.assert_fails(proc, "could not parse 1 file(s)")
        self.assertIn("  ⚠ src/a.cpp (clang-tidy crashed - UNEXPECTED)", proc.stdout)
        self.assertIn("Try: rm -rf build/release && cmake --preset ninja-release", proc.stdout)

    def test_vendored_file_is_never_given_to_clang_tidy(self):
        self.stage("src/orm/generator.cppm")
        proc = self.run_script("--full", "--fix")
        self.assert_passes(proc, "  ⏭  src/orm/generator.cppm (always-skip — vendored upstream)")
        self.assertEqual(self.calls("tidy_calls"), [])
        self.assertIn("C++26 skipped: 1", proc.stdout)

    def test_errors_outrank_crashes_and_warnings(self):
        self.stage("src/a.cpp")
        self.stage("src/b.cpp")
        self.canned_tidy("src/a.cpp", ERROR + "\n")
        self.canned_tidy("src/b.cpp", CRASH + "\n")
        self.assert_fails(self.run_script("--full"), "❌ clang-tidy found errors")

    def test_every_file_is_checked_with_several_workers(self):
        for n in range(12):
            self.stage("src/f%02d.cpp" % n)
        proc = self.run_script("--full", "-j", "4")
        self.assert_passes(proc, "Files passed: 12")
        self.assertEqual(len(self.calls("tidy_calls")), 12)

    def test_fix_with_warnings_restages_and_rechecks_in_default_mode(self):
        # The shell script exec'd itself with NO arguments here, so the re-check
        # runs in the default --diff mode. Kept as is: behaviour-preserving port.
        self.stage("src/a.cpp")
        self.canned_tidy("src/a.cpp", WARNING + "\n")
        proc = self.run_script("--full", "--fix")
        self.assertIn("🔄 Re-checking after auto-fix...", proc.stdout)
        self.assertEqual(len(self.calls("diff_calls")), 1)
        self.assertFalse(self.calls("diff_calls")[0].endswith("-fix"))


class UnrunnableToolTest(ToolchainTest):
    def test_a_clang_tidy_that_cannot_start_is_a_failure_not_a_pass(self):
        self.stage("src/a.cpp")
        (self.root / "clang-p2996" / "build" / "bin" / "clang-tidy").write_text("not an executable format\n")
        proc = self.run_script("--full")
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("cannot run clang-tidy", proc.stderr)


class AllModeTest(ToolchainTest):
    def make(self, relative):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("int x;\n")

    def test_scans_the_tree_without_needing_anything_staged(self):
        self.make("src/a.cpp")
        self.make("tests/b.cppm")
        self.make("benchmarks/c.hpp")
        proc = self.run_script("--all")
        self.assert_passes(proc, "Found 3 source files to check")

    def test_only_source_tests_and_benchmarks_are_scanned(self):
        self.make("src/a.cpp")
        self.make("docs/b.cpp")
        self.make("fuzz/c.cpp")
        self.assertIn("Found 1 source files to check", self.run_script("--all").stdout)

    def test_third_party_and_other_extensions_are_excluded(self):
        self.make("src/a.cpp")
        self.make("src/third_party/b.cpp")
        self.make("src/notes.txt")
        self.assertIn("Found 1 source files to check", self.run_script("--all").stdout)

    def test_empty_tree_is_a_pass(self):
        self.assert_passes(self.run_script("--all"), "✅ No C++ files found — clang-tidy skipped")

    def test_missing_top_level_directories_are_tolerated(self):
        self.make("src/a.cpp")
        self.assert_passes(self.run_script("--all"), "Found 1 source files")


class JobCountTest(unittest.TestCase):
    """Default -j is min(nproc, MemAvailable_GB / 2), floor 1 (#326). Carried over
    unchanged: #555 tracks replacing the volatile MemAvailable input."""

    def meminfo(self, kb):
        return "MemTotal:  99999999 kB\nMemAvailable:   %d kB\nMemFree: 1 kB\n" % kb

    def test_memory_limits_the_count(self):
        self.assertEqual(rct.default_jobs(16, self.meminfo(8 * 1024 * 1024)), 4)

    def test_cores_limit_the_count(self):
        self.assertEqual(rct.default_jobs(4, self.meminfo(64 * 1024 * 1024)), 4)

    def test_floor_is_one(self):
        self.assertEqual(rct.default_jobs(8, self.meminfo(1024)), 1)

    def test_odd_gigabytes_round_down(self):
        self.assertEqual(rct.default_jobs(16, self.meminfo(5 * 1024 * 1024)), 2)

    def test_no_meminfo_falls_back_to_cores(self):
        self.assertEqual(rct.default_jobs(6, None), 6)

    def test_meminfo_without_available_falls_back_to_cores(self):
        self.assertEqual(rct.default_jobs(6, "MemTotal: 1 kB\n"), 6)


class LintFileTest(ToolchainTest):
    def lint(self, relative, timeout=30, fix=False):
        return rct.lint_file(relative, "build/release", fix,
                             clang_tidy=str(self.root / "clang-p2996" / "build" / "bin" / "clang-tidy"),
                             timeout=timeout, env={"FAKE_LOG": str(self.log), "FAKE_TIDY_OUT": str(self.tidy_out)})

    def test_a_timed_out_run_reads_as_ok_carried_over_from_the_shell_script(self):
        # `timeout 240 clang-tidy ... || true`: a TU that exceeds the limit is
        # killed, prints nothing classifiable and counts as passed - the silent
        # gap #556 tracks. Pinned so the port is provably behaviour-preserving;
        # fixing it belongs to #556, and this test is what that change flips.
        tidy = self.root / "clang-p2996" / "build" / "bin" / "clang-tidy"
        tidy.write_text("#!/bin/sh\nexec sleep 30\n")
        result = self.lint("src/slow.cpp", timeout=1)
        self.assertEqual(result.status, "ok")
        self.assertEqual(result.lines, [])

    def test_output_is_split_into_lines(self):
        self.canned_tidy("src/a.cpp", WARNING + "\n")
        self.assertEqual(self.lint("src/a.cpp").lines, [WARNING])



if __name__ == "__main__":
    unittest.main()
