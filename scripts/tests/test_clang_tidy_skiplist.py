#!/usr/bin/env python3
"""Tests for scripts/clang_tidy_skiplist.py (issues #550, #692).

Two gaps this guards against, both from #550:

  1. --diff counted warnings unfiltered while errors went through a post-hoc
     message-text regex - an asymmetry that let toolchain parse noise block a
     commit as a "warning" with no way to tell it from a real one.
  2. That regex was unanchored and once matched a genuine `unknown type name
     'concept'` error reported against a real Storm file (REAL_STORM_FILE).

The fix filters by FILE IDENTITY only: a skip-listed file's whole diff section is
dropped before clang-tidy-diff.py runs, so message content can never fool it.
These tests exercise that mechanism directly, with no clang-tidy binary. The two
diagnostics in the "genuine" tests are the exact text of a real
`clang-tidy -p build/release shared/models.h` run - a synthetic fixture has
false-greened this repo's guards before.

Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/tests/test_clang_tidy_skiplist.py
"""
import pathlib
import re
import subprocess
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SKIPLIST = REPO_ROOT / "scripts" / "clang_tidy_skiplist.py"

sys.path.insert(0, str(SKIPLIST.parent))
import clang_tidy_skiplist as skiplist  # noqa: E402

REAL_STORM_FILE = "src/orm/fields.cppm"


def diff_section(path, content):
    """One synthetic `git diff --cached` section naming <path>, one added line."""
    return ("diff --git a/%s b/%s\nindex 1111111..2222222 100644\n--- a/%s\n+++ b/%s\n"
            "@@ -1,1 +1,2 @@\n existing line\n+%s\n" % (path, path, path, path, content))


def run_filter(*sections):
    return "".join(skiplist.filter_skiplist_from_diff("".join(sections).splitlines(keepends=True)))


class KnownUnparseableTest(unittest.TestCase):
    def test_real_storm_file_is_not_known_unparseable(self):
        self.assertFalse(skiplist.is_known_unparseable(REAL_STORM_FILE))

    def test_documented_entries_stay_classified(self):
        for path in ("shared/models.h", "shared/query_builder.hpp", "tests/test_models.h",
                     "benchmarks/schema.cppm", "python/bindings.cpp",
                     "tests/crud/test_composite_pk_crud_body.h"):
            with self.subTest(path=path):
                self.assertTrue(skiplist.is_known_unparseable(path))

    def test_fuzz_files_are_known_unparseable(self):
        # They import storm and have no compile_commands entry (ENABLE_FUZZING is
        # off by default). Unlisted, a staged fuzz/ edit would block forever.
        for path in ("fuzz/fuzz_batch_insert.cpp", "fuzz/fuzz_connection_string.cpp",
                     "fuzz/fuzz_like_pattern.cpp", "fuzz/fuzz_where_int.cpp",
                     "fuzz/fuzz_where_string.cpp", "fuzz/fuzz_models.h"):
            with self.subTest(path=path):
                self.assertTrue(skiplist.is_known_unparseable(path))

    def test_every_entry_names_a_file_that_exists(self):
        # A rename or deletion would otherwise leave a stale entry that silently
        # matches nothing, and the file's new name would be linted standalone.
        for path in sorted(skiplist.KNOWN_UNPARSEABLE):
            with self.subTest(path=path):
                self.assertTrue((REPO_ROOT / path).is_file(), path)

    def test_entries_are_exact_paths_not_wildcards(self):
        # #308: a broad tests/* wildcard once masked parse failures in files that
        # parse fine.
        for path in ("tests/test_something_new.cpp", "benchmarks/main.cpp", "shared/other.h",
                     "xtests/test_models.h", "tests/test_models.hh"):
            with self.subTest(path=path):
                self.assertFalse(skiplist.is_known_unparseable(path))

    def test_color_header_is_linted_directly(self):
        # A bare enum with no storm dependency: the one shared/models/ exception.
        self.assertFalse(skiplist.is_known_unparseable("shared/models/color.h"))


class AlwaysSkipTest(unittest.TestCase):
    def test_generator_matches_in_bare_and_prefixed_forms(self):
        self.assertTrue(skiplist.is_always_skip_file("src/orm/generator.cppm"))
        self.assertTrue(skiplist.is_always_skip_file("vendor/src/orm/generator.cppm"))

    def test_ordinary_file_does_not_match(self):
        self.assertFalse(skiplist.is_always_skip_file(REAL_STORM_FILE))

    def test_lookalike_names_do_not_match(self):
        self.assertFalse(skiplist.is_always_skip_file("src/orm/generator.cppm.bak"))
        self.assertFalse(skiplist.is_always_skip_file("src/orm/my_generator.cppm"))

    def test_generator_is_not_in_the_known_unparseable_list(self):
        # It parses fine; it is skipped because Storm does not own it.
        self.assertFalse(skiplist.is_known_unparseable("src/orm/generator.cppm"))


class FilterDiffTest(unittest.TestCase):
    def test_drops_the_whole_section_of_an_unparseable_file(self):
        output = run_filter(diff_section("shared/models.h", "some content"),
                            diff_section(REAL_STORM_FILE, "export module storm.orm.fields;"))
        self.assertNotIn("shared/models.h", output)
        self.assertNotIn("some content", output)
        self.assertIn(REAL_STORM_FILE, output)

    def test_genuine_toolchain_error_on_a_real_file_survives(self):
        # The #550 gap 2 regression: the old regex swallowed exactly this text.
        real_error = ("/Users/ihor/projects/storm/storm/../clang-p2996/build/include/c++/v1/meta:440:1: "
                      "error: unknown type name 'concept' [clang-diagnostic-error]")
        self.assertIn("unknown type name 'concept'", run_filter(diff_section(REAL_STORM_FILE, real_error)))

    def test_genuine_warning_on_a_real_file_survives(self):
        real_warning = ("/Users/ihor/projects/storm/storm/%s:70:12: warning: enum 'Color' uses a larger base "
                        "type ('int', size: 4 bytes) than necessary for its value set, consider using "
                        "'std::uint8_t' (1 byte) as the base type to reduce its size [performance-enum-size]"
                        % REAL_STORM_FILE)
        self.assertIn("performance-enum-size", run_filter(diff_section(REAL_STORM_FILE, real_warning)))

    def test_drops_the_vendored_always_skip_file(self):
        self.assertNotIn("generator.cppm", run_filter(diff_section("src/orm/generator.cppm", "_T unused;")))

    def test_drops_a_prefixed_always_skip_file(self):
        self.assertNotIn("generator.cppm", run_filter(diff_section("vendor/src/orm/generator.cppm", "x")))

    def test_a_kept_section_after_a_dropped_one_is_kept_whole(self):
        output = run_filter(diff_section("shared/models.h", "dropped"), diff_section(REAL_STORM_FILE, "kept"))
        self.assertEqual(output, diff_section(REAL_STORM_FILE, "kept"))

    def test_a_dropped_section_after_a_kept_one_does_not_leak_into_it(self):
        output = run_filter(diff_section(REAL_STORM_FILE, "kept"), diff_section("shared/models.h", "dropped"))
        self.assertEqual(output, diff_section(REAL_STORM_FILE, "kept"))

    def test_text_before_any_header_is_kept(self):
        self.assertEqual(run_filter("preamble line\n"), "preamble line\n")

    def test_clean_diff_passes_through_unchanged(self):
        text = diff_section(REAL_STORM_FILE, "a") + diff_section("src/orm/utilities.cppm", "b")
        self.assertEqual(run_filter(text), text)

    def test_empty_input_gives_empty_output(self):
        self.assertEqual(run_filter(), "")

    def test_diff_path_is_the_b_side_after_the_last_separator(self):
        # `diff --git a/<p> b/<p>`: the b-side is what follows the LAST " b/".
        self.assertEqual(skiplist.diff_path("diff --git a/x/y.cpp b/x/y.cpp"), "x/y.cpp")
        self.assertEqual(skiplist.diff_path("diff --git a/d b/e b/f.h b/d b/e b/f.h"), "f.h")


class FragmentParentsTest(unittest.TestCase):
    """#671: src/ #include fragments are skip-listed standalone but linted via their parent."""

    def test_every_src_skiplist_entry_is_a_mapped_fragment(self):
        src_entries = {p for p in skiplist.KNOWN_UNPARSEABLE if p.startswith("src/")}
        self.assertTrue(src_entries)
        self.assertEqual(src_entries, set(skiplist.FRAGMENT_PARENTS))

    def test_every_src_header_on_disk_is_a_mapped_fragment(self):
        on_disk = {p.relative_to(REPO_ROOT).as_posix() for ext in ("*.h", "*.hpp")
                   for p in (REPO_ROOT / "src").rglob(ext)}
        self.assertEqual(on_disk, set(skiplist.FRAGMENT_PARENTS))

    def test_each_parent_exists_and_includes_its_fragment(self):
        for fragment, parent in skiplist.FRAGMENT_PARENTS.items():
            with self.subTest(fragment=fragment):
                text = (REPO_ROOT / parent).read_text()
                name = pathlib.PurePosixPath(fragment).name
                self.assertRegex(text, r'(?m)^\s*#include "%s"' % re.escape(name))

    def test_line_filters_map_fragment_hunks_onto_the_parent(self):
        fragment = "src/orm/statements/select_stitch.hpp"
        diff = ("diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n"
                "@@ -10,0 +11,3 @@ ctx\n+a\n+b\n+c\n@@ -40 +44 @@ ctx\n-x\n+y\n@@ -50,2 +53,0 @@\n-d\n-e\n"
                % (fragment, fragment, fragment, fragment))
        diff += diff_section(REAL_STORM_FILE, "unrelated")
        self.assertEqual(skiplist.fragment_line_filters(diff.splitlines(keepends=True)),
                         {"src/orm/statements/select.cppm": [{"name": fragment, "lines": [[11, 13], [44, 44]]}]})

    def test_no_fragment_hunks_means_no_parent_runs(self):
        self.assertEqual(skiplist.fragment_line_filters(diff_section(REAL_STORM_FILE, "x").splitlines(True)), {})


class KeepCommandTest(unittest.TestCase):
    """`--keep` is how commit.sh drops skipped files from its BMI probe."""

    def keep(self, stdin):
        return subprocess.run([sys.executable, str(SKIPLIST), "--keep"], input=stdin,
                              capture_output=True, text=True)

    def test_prints_only_files_clang_tidy_will_open(self):
        proc = self.keep("%s\nshared/models.h\nsrc/orm/generator.cppm\nsrc/orm/utilities.cppm\n" % REAL_STORM_FILE)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.splitlines(), [REAL_STORM_FILE, "src/orm/utilities.cppm"])

    def test_empty_input_prints_nothing(self):
        proc = self.keep("")
        self.assertEqual((proc.returncode, proc.stdout), (0, ""))

    def test_blank_lines_are_ignored(self):
        self.assertEqual(self.keep("\n%s\n\n" % REAL_STORM_FILE).stdout.splitlines(), [REAL_STORM_FILE])

    def test_a_fragment_is_replaced_by_its_parent(self):
        # commit.sh's BMI probe must check the TU clang-tidy actually opens for a fragment.
        proc = self.keep("src/orm/statements/aggregate_grammar.hpp\n")
        self.assertEqual(proc.stdout.splitlines(), ["src/orm/statements/aggregate.cppm"])

    def test_unknown_option_fails(self):
        proc = subprocess.run([sys.executable, str(SKIPLIST), "--nope"], capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
