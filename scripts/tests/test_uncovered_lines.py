#!/usr/bin/env python3
"""Tests for scripts/uncovered_lines.py (issue #688).

commit.sh's coverage step used to parse the filtered lcov file in bash
(`show_uncovered_lines` + `format_line_ranges`). The parsing is now a plain
function, `uncovered_by_file`, and the range compression is `format_ranges`,
both exercised on fixture text. The CLI is run as a subprocess for what only a
real process shows: the missing-file and no-parse paths. Stdlib unittest only, and no syntax or API newer than Python 3.7.

    python3 scripts/tests/test_uncovered_lines.py
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = REPO_ROOT / "scripts" / "uncovered_lines.py"

sys.path.insert(0, str(HELPER.parent))
from uncovered_lines import LCOV_RELATIVE_PATH, format_ranges, uncovered_by_file  # noqa: E402


class FormatRangesTest(unittest.TestCase):
    def test_empty_input_gives_empty_string(self):
        self.assertEqual(format_ranges([]), "")

    def test_single_line(self):
        self.assertEqual(format_ranges([7]), "7")

    def test_consecutive_lines_collapse_to_a_range(self):
        self.assertEqual(format_ranges([10, 11, 12]), "10-12")

    def test_gaps_split_ranges(self):
        self.assertEqual(format_ranges([5, 10, 11, 12, 20]), "5, 10-12, 20")

    def test_two_adjacent_lines_are_a_range(self):
        self.assertEqual(format_ranges([3, 4]), "3-4")

    def test_trailing_singleton_after_range(self):
        self.assertEqual(format_ranges([1, 2, 9]), "1-2, 9")


class UncoveredByFileTest(unittest.TestCase):
    def test_collects_only_zero_count_lines(self):
        lcov = "SF:/r/a.cpp\nDA:1,3\nDA:2,0\nDA:3,0\nend_of_record\n"
        self.assertEqual(uncovered_by_file(lcov), [("/r/a.cpp", [2, 3])])

    def test_fully_covered_file_is_omitted(self):
        lcov = "SF:/r/a.cpp\nDA:1,1\nend_of_record\nSF:/r/b.cpp\nDA:4,0\nend_of_record\n"
        self.assertEqual(uncovered_by_file(lcov), [("/r/b.cpp", [4])])

    def test_multiple_files_keep_order(self):
        lcov = "SF:b\nDA:2,0\nSF:a\nDA:1,0\n"
        self.assertEqual(uncovered_by_file(lcov), [("b", [2]), ("a", [1])])

    def test_ignores_branch_and_function_records(self):
        lcov = "SF:a\nFN:1,f\nFNDA:0,f\nBRDA:1,0,0,0\nDA:5,0\n"
        self.assertEqual(uncovered_by_file(lcov), [("a", [5])])

    def test_count_with_checksum_field_is_parsed(self):
        # `DA:<line>,<count>[,<checksum>]` — the count is the second field only.
        self.assertEqual(uncovered_by_file("SF:a\nDA:6,0,abc\nDA:7,2,abc\n"), [("a", [6])])

    def test_no_records_gives_empty_list(self):
        self.assertEqual(uncovered_by_file(""), [])

    def test_crlf_input(self):
        self.assertEqual(uncovered_by_file("SF:a\r\nDA:1,0\r\n"), [("a", [1])])


class CliTest(unittest.TestCase):
    """The helper reads one fixed path under its cwd and takes no arguments."""

    def run_cli(self, lcov_text, *args):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            if lcov_text is not None:
                lcov = root / LCOV_RELATIVE_PATH
                lcov.parent.mkdir(parents=True)
                lcov.write_text(lcov_text.replace("/r", str(root)))
            return subprocess.run(
                [sys.executable, str(HELPER)] + list(args), cwd=str(root),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                universal_newlines=True, check=False)

    def test_missing_file_prints_nothing_and_succeeds(self):
        result = self.run_cli(None)
        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_prints_relative_path_and_ranges(self):
        result = self.run_cli("SF:/r/src/a.cpp\nDA:1,0\nDA:2,0\nDA:9,0\n")
        self.assertEqual(result.returncode, 0)
        self.assertIn("src/a.cpp", result.stdout)
        self.assertNotIn("/r/src/a.cpp", result.stdout)
        self.assertIn("1-2, 9", result.stdout)

    def test_unparseable_lcov_prints_fallback_note(self):
        result = self.run_cli("SF:/r/a.cpp\nDA:1,1\n")
        self.assertIn("could not parse uncovered lines from lcov", result.stdout)

    def test_logical_pwd_through_a_symlink_still_gives_relative_paths(self):
        # bash printed paths relative to the LOGICAL $PWD (/tmp/x), while getcwd()
        # is the physical one (/private/tmp/x on macOS). The SF: lines hold whichever
        # the build recorded, so the helper must honour $PWD when it names the cwd.
        with tempfile.TemporaryDirectory() as tmp:
            real = pathlib.Path(tmp).resolve() / "real"
            link = pathlib.Path(tmp).resolve() / "link"
            (real / LCOV_RELATIVE_PATH).parent.mkdir(parents=True)
            (real / LCOV_RELATIVE_PATH).write_text("SF:%s/src/a.cpp\nDA:1,0\n" % link)
            link.symlink_to(real)
            env = dict(os.environ, PWD=str(link))
            result = subprocess.run([sys.executable, str(HELPER)], cwd=str(link), env=env,
                                    stdout=subprocess.PIPE, universal_newlines=True, check=False)
        self.assertIn("src/a.cpp", result.stdout)
        self.assertNotIn(str(link), result.stdout)

    def test_stale_pwd_falls_back_to_the_real_cwd(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            (root / LCOV_RELATIVE_PATH).parent.mkdir(parents=True)
            (root / LCOV_RELATIVE_PATH).write_text("SF:%s/src/a.cpp\nDA:1,0\n" % root)
            env = dict(os.environ, PWD="/definitely/not/here")
            result = subprocess.run([sys.executable, str(HELPER)], cwd=str(root), env=env,
                                    stdout=subprocess.PIPE, universal_newlines=True, check=False)
        self.assertIn("src/a.cpp", result.stdout)
        self.assertNotIn(str(root) + "/", result.stdout)

    def test_no_color_env_gives_plain_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            (root / LCOV_RELATIVE_PATH).parent.mkdir(parents=True)
            (root / LCOV_RELATIVE_PATH).write_text("SF:%s/src/a.cpp\nDA:1,0\n" % root)
            env = dict(os.environ, PWD=str(root), NO_COLOR="1")
            result = subprocess.run([sys.executable, str(HELPER)], cwd=str(root), env=env,
                                    stdout=subprocess.PIPE, universal_newlines=True, check=False)
        self.assertNotIn("\033", result.stdout)
        self.assertIn("  src/a.cpp\n    Lines: 1\n", result.stdout)

    def test_empty_no_color_keeps_the_colors(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            (root / LCOV_RELATIVE_PATH).parent.mkdir(parents=True)
            (root / LCOV_RELATIVE_PATH).write_text("SF:%s/src/a.cpp\nDA:1,0\n" % root)
            env = dict(os.environ, PWD=str(root), NO_COLOR="")
            result = subprocess.run([sys.executable, str(HELPER)], cwd=str(root), env=env,
                                    stdout=subprocess.PIPE, universal_newlines=True, check=False)
        self.assertIn("\033[", result.stdout)

    def test_arguments_are_rejected(self):
        # No path comes from the command line: a stray argument is a usage error.
        result = self.run_cli("SF:/r/a.cpp\nDA:1,0\n", "/etc/passwd")
        self.assertEqual((result.returncode, result.stdout), (2, ""))


if __name__ == "__main__":
    unittest.main()
