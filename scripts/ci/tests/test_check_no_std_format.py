#!/usr/bin/env python3
"""Tests for scripts/ci/check_no_std_format.py.

The checker keeps std::format / std::print out of src/ and the shared test headers,
where one call costs every including TU ~0.4 s of compile time. Strategy: build a
minimal fake repo in a temp dir, run the checker against it, and assert the exit
code and message per scenario; a final scenario runs it against the real repository,
which must pass. Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/ci/tests/test_check_no_std_format.py
"""
import pathlib
import subprocess
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fake_repo import FakeRepoBase  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
CHECKER = REPO_ROOT / "scripts" / "ci" / "check_no_std_format.py"

CLEAN_MESSAGE = "no std::format/std::print"
FORMAT_CALL = 'auto s = std::format("{}", 1);'


class FakeRepoTest(FakeRepoBase):
    CHECKER = CHECKER

    def setUp(self):
        super().setUp()
        self.write("src/orm/where.cppm", "export module storm_orm_where;\n")
        self.write("tests/test_db_helpers.h", "#include <string>\n")
        self.write("tests/query/test_where.cpp", "import storm;\n")
        self.write("shared/models/person.h", "struct Person {};\n")

    def assert_passes(self):
        proc = self.run_checker()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(CLEAN_MESSAGE, proc.stdout)

    def test_clean_tree_passes(self):
        self.assert_passes()

    def test_format_in_a_module_fails_with_file_and_line(self):
        self.append("src/orm/where.cppm", FORMAT_CALL)
        self.assert_fails("src/orm/where.cppm:2:")

    def test_format_in_a_module_fragment_fails(self):
        self.write("src/orm/statements/aggregate_grammar.hpp", FORMAT_CALL + "\n")
        self.assert_fails("src/orm/statements/aggregate_grammar.hpp:1:")

    def test_format_in_a_test_header_fails(self):
        self.append("tests/test_db_helpers.h", FORMAT_CALL)
        self.assert_fails("tests/test_db_helpers.h:2:")

    def test_format_in_a_nested_test_header_fails(self):
        self.write("tests/crud/test_crud_body.h", FORMAT_CALL + "\n")
        self.assert_fails("tests/crud/test_crud_body.h:1:")

    def test_format_in_a_shared_model_header_fails(self):
        self.append("shared/models/person.h", FORMAT_CALL)
        self.assert_fails("shared/models/person.h:2:")

    def test_format_in_a_test_source_file_is_allowed(self):
        self.append("tests/query/test_where.cpp", FORMAT_CALL)
        self.assert_passes()

    def test_every_formatting_entry_point_is_caught(self):
        for call in ('std::vformat("{}", args);', "std::format_to(out, \"{}\", 1);",
                     "std::format_to_n(out, 4, \"{}\", 1);", 'std::print("x");',
                     'std::println(stderr, "x");', 'std::format ("{}", 1);',
                     "std::vformat_to(out, fmt, args);", "std::formatted_size(\"{}\", 1);",
                     "std::vprint_unicode(fmt, args);", "std::vprint_nonunicode(fmt, args);",
                     'std::format<int>("{}", 1);', 'std:: format("{}", 1);', 'std ::print("x");'):
            with self.subTest(call=call):
                self.write("src/orm/where.cppm", "export module storm_orm_where;\n" + call + "\n")
                self.assert_fails("src/orm/where.cppm:2:")

    def test_a_comment_naming_the_function_is_allowed(self):
        self.append("src/orm/where.cppm", "// Plain appends rather than std::format(\"{}\"): see COMPILE_TIME.md")
        self.append("src/orm/where.cppm", "sql += name; // not std::format(...)")
        self.assert_passes()

    def test_block_comments_and_string_literals_fail_safe(self):
        # Only `//` comments are skipped; anything else naming a call is flagged, not passed.
        self.append("src/orm/where.cppm", "/* std::format(x) */")
        self.assert_fails("src/orm/where.cppm:2:")

    def test_a_call_before_a_trailing_comment_is_caught(self):
        self.append("src/orm/where.cppm", FORMAT_CALL + " // why not")
        self.assert_fails("src/orm/where.cppm:2:")

    def test_similar_names_are_not_calls(self):
        self.append("src/orm/where.cppm", "std::formatter<int> f; my_std::format(1); std::format_error e{\"x\"};")
        self.append("src/orm/where.cppm", "#include <format>")
        self.assert_passes()

    def test_every_hit_is_reported_and_counted(self):
        self.append("src/orm/where.cppm", FORMAT_CALL)
        self.append("tests/test_db_helpers.h", 'std::println("x");')
        proc = self.run_checker()
        self.assert_fails("2 std::format/std::print call(s)", proc)
        self.assertIn("src/orm/where.cppm:2:", proc.stderr)
        self.assertIn("tests/test_db_helpers.h:2:", proc.stderr)

    def test_github_actions_annotates_the_line(self):
        self.append("src/orm/where.cppm", FORMAT_CALL)
        proc = self.run_checker(env={"GITHUB_ACTIONS": "true"})
        self.assert_fails("::error file=src/orm/where.cppm,line=2::", proc)

    def test_no_annotation_outside_github_actions(self):
        self.append("src/orm/where.cppm", FORMAT_CALL)
        self.assertNotIn("::error", self.run_checker().stderr)

    def test_a_missing_scanned_directory_fails_instead_of_passing(self):
        for directory in ("src", "tests", "shared"):
            with self.subTest(directory=directory):
                root = self.root / ("without_" + directory)
                for other in ("src", "tests", "shared"):
                    if other != directory:
                        (root / other).mkdir(parents=True)
                self.assert_fails("scanned directory missing: " + directory, self.run_checker(root=root))


class RealRepoTest(unittest.TestCase):
    def test_the_real_repository_passes(self):
        proc = subprocess.run([sys.executable, str(CHECKER)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(CLEAN_MESSAGE, proc.stdout)


if __name__ == "__main__":
    unittest.main()
