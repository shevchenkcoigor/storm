#!/usr/bin/env python3
"""Tests for scripts/compile_time/ninjalog_stats.py.

That script regenerates the whole-build tables in docs/internals/performance/COMPILE_TIME.md,
and the two rules it implements are exactly the ones that are invisible when wrong: a missed
dedup silently DOUBLES every module edge (ninja logs one line per output, and a module compile
emits both a .pcm and a .o from one invocation), and counting the module-scan edges silently
INFLATES the total with work the document's baseline excludes. Either would land in the doc as
a plausible-looking number, so the scenarios below pin both against hand-built logs whose
correct answer is known by construction. Stdlib unittest, no syntax newer than Python 3.7.

    python3 scripts/compile_time/tests/test_ninjalog_stats.py
"""
import pathlib
import re
import subprocess
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "lib"))
import testing  # noqa: E402

STATS = testing.REPO_ROOT / "scripts" / "compile_time" / "ninjalog_stats.py"

# Bucket labels the script prints.
BUCKET_STORM = "storm library modules"
BUCKET_TESTS = "hand-written test TUs"
BUCKET_YAML = "yaml corpus TUs"
BUCKET_OTHER = "other (gtest/gmock, std module, tools, deps)"
TOTAL = "TOTAL (object compiles)"
SCANS = "module scan edges (excluded)"

# "<label>   <files> <sec> ..." — the label is padded, so two spaces end it.
ROW = re.compile(r"^(?P<label>\S.*?)\s{2,}(?P<files>\d+)\s+(?P<sec>\d+)\b")

QUEUE_MODULE_OBJ = "CMakeFiles/storm.dir/src/orm/queryset.cppm.o"


def write_log(path, rows):
    """A ninja log v7 from (start_ms, end_ms, output, cmdhash) rows, mtime column filled in."""
    lines = ["# ninja log v7"]
    lines += ["%s\t%s\t1788894584000000000\t%s\t%s" % row for row in rows]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path


def table(output):
    """{label: (files, sec)} for every row of the script's output table."""
    rows = {}
    for line in output.splitlines():
        found = ROW.match(line)
        if found:
            rows[found.group("label")] = (int(found.group("files")), int(found.group("sec")))
    return rows


class NinjalogStatsTest(unittest.TestCase):
    def test_the_script_is_executable(self):
        # commit.sh and the docs invoke it by path, so a lost exec bit breaks them.
        self.assertTrue(STATS.is_file(), "%s is missing" % STATS)
        self.assertTrue(STATS.stat().st_mode & 0o111, "%s is not executable" % STATS)

    def setUp(self):
        scratch = testing.scratch_dir_in_repo("ninjalog-test.")
        self.tmp = scratch.__enter__()
        self.addCleanup(scratch.__exit__, None, None, None)

    def stats(self, rows):
        """Run the script over a log built from `rows`; return the CompletedProcess."""
        log = write_log(self.tmp / "case.ninja_log", rows)
        return testing.run([sys.executable, STATS, log])

    def test_module_compile_logged_twice_is_counted_once(self):
        # Same invocation (identical start/end/cmdhash), two outputs. Without the dedup this
        # reports 2 files / 20 s instead of 1 file / 10 s.
        result = self.stats([(0, 10000, QUEUE_MODULE_OBJ, "aaaa1111"),
                             (0, 10000, "CMakeFiles/storm.dir/src/orm/queryset.pcm", "aaaa1111")])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(table(result.stdout)[BUCKET_STORM], (1, 10))

    def test_scan_edges_are_excluded_from_the_total_but_reported(self):
        result = self.stats([
            (0, 4000, "tests/CMakeFiles/storm_tests.dir/query/test_where.cpp.o", "bbbb2222"),
            (0, 1000, "CMakeFiles/storm.dir/src/orm/queryset.cppm.o.ddi", "cccc3333"),
            (0, 1000, "tests/CMakeFiles/storm_tests.dir/query/test_where.cpp.o.modmap", "dddd4444")])
        rows = table(result.stdout)
        self.assertEqual(rows[TOTAL], (1, 4), result.stdout)
        self.assertEqual(rows[SCANS], (2, 2), result.stdout)

    def test_buckets_match_the_documents_rows(self):
        result = self.stats([
            (0, 1000, "tests/CMakeFiles/storm_tests.dir/query/test_where.cpp.o", "1111aaaa"),
            (0, 2000, "tests/CMakeFiles/storm_tests.dir/yaml/test_unified_yaml_select.cpp.o", "2222bbbb"),
            (0, 4000, QUEUE_MODULE_OBJ, "4444dddd")])
        rows = table(result.stdout)
        # A yaml/ TU is the corpus, not a hand-written test; the other two land as named.
        self.assertEqual({label: rows[label][0] for label in
                          (BUCKET_YAML, BUCKET_TESTS, BUCKET_STORM)},
                         {BUCKET_YAML: 1, BUCKET_TESTS: 1, BUCKET_STORM: 1})

    def test_foreign_targets_stay_out_of_storm_rows(self):
        # Both were live bugs caught by classifying a real build log: a bare "mock" also
        # matches GoogleMock's own library build, and a bare "@synth" also matches the *std*
        # module's synthesized BMI. Either one silently pads a Storm row with work that is not
        # Storm's.
        result = self.stats([
            (0, 1000, "_deps/googletest-build/googlemock/CMakeFiles/gmock.dir/src/gmock-all.cc.o", "ffff6666"),
            (0, 1000, "CMakeFiles/@cmake_cxx_std@synth_0.dir/abcdef123456.bmi", "ffff7777"),
            (0, 1000, "CMakeFiles/@cmake_cxx_std.dir/std.cppm.o", "ffff8888")])
        self.assertEqual(result.returncode, 0, result.stderr)
        # All three were seen and filed under "other", so an empty stdout cannot pass vacuously.
        self.assertEqual(table(result.stdout)[BUCKET_OTHER][0], 3, result.stdout)
        self.assertNotIn(BUCKET_STORM, result.stdout)

    def test_a_log_with_no_compiles_is_an_error(self):
        # An incremental "nothing to do" build produces exactly this, and reporting 0 s as if
        # measured would be worse than refusing.
        result = self.stats([])
        self.assertNotEqual(result.returncode, 0)

    def test_a_normal_run_writes_nothing_to_stderr(self):
        result = self.stats([(0, 1000, QUEUE_MODULE_OBJ, "eeee5555")])
        self.assertEqual(result.stderr, "")

    def test_closing_the_pipe_early_is_not_a_crash(self):
        # `ninjalog_stats.py … | head` is the obvious way to use this, and Python's default
        # turns the closed pipe into a BrokenPipeError traceback on stderr. The reader goes
        # away before the script has printed anything, so its first write hits a closed pipe.
        log = write_log(self.tmp / "pipe.ninja_log", [(0, 1000, QUEUE_MODULE_OBJ, "eeee5555")])
        child = subprocess.Popen([sys.executable, str(STATS), str(log)], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, universal_newlines=True)
        child.stdout.close()
        stderr = child.stderr.read()
        child.stderr.close()
        child.wait()
        self.assertNotIn("BrokenPipeError", stderr)
        self.assertNotIn("Traceback", stderr)

    def test_the_log_path_is_confined_to_the_repository(self):
        # The path comes from argv. Escaping the tree is a mistake (wrong copy of the script
        # for the worktree being measured), not a use case, so it must fail with a readable
        # message rather than opening whatever it was handed.
        escapes = {"an absolute path outside the tree": "/etc/hostname",
                   "a traversal through ..": str(testing.REPO_ROOT / "build" / ".." / ".." / "etc" / "hostname")}
        for description, path in escapes.items():
            with self.subTest(description):
                result = testing.run([sys.executable, STATS, path], merge_stderr=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("outside the repository", result.stdout)


if __name__ == "__main__":
    unittest.main()
