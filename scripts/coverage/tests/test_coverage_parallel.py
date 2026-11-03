#!/usr/bin/env python3
"""Tests for scripts/coverage/coverage-run-batched.sh parallelism (issue #268).

Strategy: stub a fake build dir with a fake test binary that
  - responds to --gtest_list_tests with a known suite list,
  - on each suite run, sleeps briefly and records its filter, PID and start/end time.
Then the wall-clock and overlap between serial and parallel modes can be compared. The script
under test stays the REAL bash script; only the gtest binary is faked. The default job count is
`nproc --ignore=2`, so the "parallel by default" test assumes a host with an `nproc` (GNU
coreutils; Homebrew's on macOS) and at least 3 cores, as the bash test it replaces did.
Stdlib unittest, no syntax newer than Python 3.7.

    python3 scripts/coverage/tests/test_coverage_parallel.py
"""
import os
import pathlib
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "lib"))
import testing  # noqa: E402

SCRIPT = testing.REPO_ROOT / "scripts" / "coverage" / "coverage-run-batched.sh"

# The fake gtest binary. Lists N suites; on a filtered run sleeps, writes a non-empty profraw (the
# real script removes empty ones) and appends "<filter> <pid> <start_ns> <end_ns>" to runs.log in
# one O_APPEND write so concurrent runs cannot interleave a line.
FAKE_BINARY = """#!{python}
import os, sys, time
if sys.argv[1:2] == ["--gtest_list_tests"]:
    for index in range(1, {suites} + 1):
        print("FakeSuite%d." % index)
        print("  TestA")
    sys.exit(0)
selected = ""
for arg in sys.argv[1:]:
    if arg.startswith("--gtest_filter="):
        selected = arg.split("=", 1)[1]
start = time.time_ns()
time.sleep({seconds})
end = time.time_ns()
profile = os.environ.get("LLVM_PROFILE_FILE")
if profile:
    with open(profile, "w") as handle:
        handle.write("fake-profraw-data\\n")
fd = os.open("{log}", os.O_WRONLY | os.O_APPEND | os.O_CREAT)
os.write(fd, ("%s %d %d %d\\n" % (selected, os.getpid(), start, end)).encode())
os.close(fd)
"""


def has_overlap(runs):
    """True if any run started before an earlier-starting run had finished."""
    previous_end = None
    for start, end in sorted(runs):
        if previous_end is not None and start < previous_end:
            return True
        previous_end = end if previous_end is None else max(previous_end, end)
    return False


def wall_ns(runs):
    """The span from the first start to the last end."""
    return max(end for _, end in runs) - min(start for start, _ in runs)


def max_concurrent(runs):
    """Peak number of simultaneously running batches (sweep line; an end sorts before a start)."""
    events = sorted([(start, 1) for start, _ in runs] + [(end, -1) for _, end in runs],
                    key=lambda event: (event[0], event[1]))
    running = peak = 0
    for _, step in events:
        running += step
        peak = max(peak, running)
    return peak


class CoverageParallelTest(unittest.TestCase):
    def setUp(self):
        self.dirs = []
        self.addCleanup(self.cleanup_dirs)

    def cleanup_dirs(self):
        for directory in self.dirs:
            directory.cleanup()

    def fake_build_dir(self, suites, seconds):
        directory = tempfile.TemporaryDirectory()
        self.dirs.append(directory)
        build = pathlib.Path(directory.name)
        (build / "tests").mkdir()
        binary = build / "tests" / "storm_tests"
        binary.write_text(FAKE_BINARY.format(python=sys.executable, suites=suites, seconds=seconds,
                                             log=build / "runs.log"), encoding="utf-8")
        binary.chmod(binary.stat().st_mode | stat.S_IXUSR)
        return build

    def run_script(self, build, jobs=None):
        env = {key: value for key, value in os.environ.items() if key != "COVERAGE_JOBS"}
        if jobs is not None:
            env["COVERAGE_JOBS"] = str(jobs)
        return testing.run(["bash", SCRIPT, build], env=env, merge_stderr=True)

    def runs(self, build, expected):
        """The (start_ns, end_ns) of every batch the fake binary recorded."""
        log = build / "runs.log"
        self.assertTrue(log.exists(), "the fake binary recorded no runs")
        rows = [line.split() for line in log.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(rows), expected)
        return [(int(row[2]), int(row[3])) for row in rows]

    def test_default_mode_runs_batches_in_parallel(self):
        build = self.fake_build_dir(suites=8, seconds=0.3)
        self.run_script(build)
        self.assertTrue(has_overlap(self.runs(build, 8)), "no overlap between batches")

    def test_a_single_job_runs_batches_strictly_serially(self):
        build = self.fake_build_dir(suites=6, seconds=0.15)
        self.run_script(build, jobs=1)
        self.assertFalse(has_overlap(self.runs(build, 6)), "COVERAGE_JOBS=1 overlapped batches")

    def test_parallel_mode_is_meaningfully_faster_than_serial(self):
        serial_build = self.fake_build_dir(suites=8, seconds=0.25)
        parallel_build = self.fake_build_dir(suites=8, seconds=0.25)
        self.run_script(serial_build, jobs=1)
        self.run_script(parallel_build, jobs=8)
        serial = wall_ns(self.runs(serial_build, 8))
        parallel = wall_ns(self.runs(parallel_build, 8))
        # 8 suites x 250 ms with 8 jobs: expect at least a 2x win.
        self.assertLess(parallel * 2, serial, "parallel %d ns vs serial %d ns" % (parallel, serial))

    def test_every_batch_still_produces_its_profraw(self):
        build = self.fake_build_dir(suites=5, seconds=0.1)
        self.run_script(build)
        self.assertEqual(len(list(build.glob("batch_*.profraw"))), 5)

    def test_an_explicit_job_count_caps_concurrency(self):
        build = self.fake_build_dir(suites=12, seconds=0.2)
        self.run_script(build, jobs=3)
        peak = max_concurrent(self.runs(build, 12))
        self.assertTrue(2 <= peak <= 3, "COVERAGE_JOBS=3 expected peak concurrency 2..3, got %d" % peak)

    def test_a_missing_test_binary_aborts_with_its_error(self):
        directory = tempfile.TemporaryDirectory()
        self.dirs.append(directory)
        build = pathlib.Path(directory.name)
        (build / "tests").mkdir()
        result = self.run_script(build)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Test binary not found", result.stdout)


class IntervalHelpersTest(unittest.TestCase):
    """The overlap arithmetic the tests above rest on, checked on hand-made intervals."""

    def test_overlap_is_detected_only_when_intervals_cross(self):
        self.assertTrue(has_overlap([(0, 10), (5, 15)]))
        self.assertTrue(has_overlap([(0, 100), (10, 20), (30, 40)]))
        self.assertFalse(has_overlap([(0, 10), (10, 20), (25, 30)]))

    def test_peak_concurrency_counts_simultaneous_runs(self):
        self.assertEqual(max_concurrent([(0, 10), (0, 10), (0, 10)]), 3)
        self.assertEqual(max_concurrent([(0, 10), (10, 20)]), 1)
        self.assertEqual(max_concurrent([(0, 10), (5, 15), (12, 20)]), 2)

    def test_wall_time_spans_first_start_to_last_end(self):
        self.assertEqual(wall_ns([(5, 10), (0, 7), (8, 20)]), 20)


if __name__ == "__main__":
    unittest.main()
