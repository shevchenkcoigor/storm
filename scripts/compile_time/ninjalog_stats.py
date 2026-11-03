#!/usr/bin/env python3
"""Whole-build compile-time breakdown from a ninja build log.

Implements the "Whole-build breakdown" bullet of
docs/internals/performance/COMPILE_TIME.md#method, so the tables in that
document can be regenerated rather than retyped. Both rules it states are
load-bearing:

  * **Deduplicate on (start, end, cmdhash).** One C++ module compile emits both
    a .pcm/.bmi and a .o, and ninja logs a line per OUTPUT — counting the log
    verbatim double-counts every module edge. Deduplicating on the invocation's
    identity collapses them to one.

  * **Object compiles only.** The module-scan edges (.ddi/.dd/.modmap, plus the
    corpus .json) are real build work but are not compiles, and the baseline
    table excludes them. They are reported separately rather than silently
    dropped, so the exclusion stays visible.

Note the log records wall time per edge, so the "sec" column sums to more than
the build's wall clock whenever ninja ran jobs in parallel — that ratio is the
effective parallelism, printed at the end.

Usage:
    scripts/compile_time/ninjalog_stats.py build/debug/.ninja_log

For a total comparable with the document's, measure a build from scratch
(`rm -rf build/debug` first): the log accumulates across runs, so a tree built
incrementally reports only the edges those runs happened to rebuild.
"""

import os
import signal
import sys
from collections import defaultdict

# This is a pipeline tool — `ninjalog_stats.py … | head` closes the pipe early,
# and Python's default is to turn that into a BrokenPipeError traceback on
# stderr, which reads as a crash. Restore the shell default (die quietly on
# SIGPIPE) where the platform has one.
if hasattr(signal, "SIGPIPE"):
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)

SCAN_SUFFIXES = (".ddi", ".dd", ".modmap", ".json")
OBJECT_SUFFIXES = (".o", ".pcm", ".bmi", ".pch")


def bucket(output: str) -> str:
    """Classify an edge by output path, matching COMPILE_TIME.md's rows.

    Anchored on the CMake target directories rather than on loose substrings.
    Two near-misses are why: a bare "mock" also matches GoogleMock's own
    `_deps/googletest-build/googlemock/CMakeFiles/gmock.dir/`, and a bare
    "@synth" also matches `@cmake_cxx_std@synth_0.dir` — the synthesized BMI of
    the *std* module, which would then be counted as a Storm module while
    `@cmake_cxx_std.dir` itself fell through to the dependency bucket. Both
    inflate a Storm row with work that is not Storm's.
    """
    if "/storm_tests.dir/" in output:
        return "yaml corpus TUs" if "/yaml/" in output else "hand-written test TUs"
    if output.startswith(("tests/mock_sqlite/", "tests/mock_libpq/")):
        return "mock test binaries"
    if output.startswith(("CMakeFiles/storm.dir/", "CMakeFiles/storm@synth")):
        return "storm library modules"
    return "other (gtest/gmock, std module, tools, deps)"


# The repository this copy of the script belongs to. Logs are required to live
# under it — see resolve_log_path.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))


def safe_log_path(path: str) -> str:
    """Resolve an argv-supplied log path, confined to this repository.

    realpath collapses symlinks and `..`; the result must then sit inside the
    repository this copy of the script belongs to, and be a regular file.
    Confinement is the point rather than a formality — the tool measures *this*
    tree's builds, so a path that escapes it is a mistake (usually the wrong
    worktree's copy of the script) worth failing on.

    Called inside the open() it guards, not assigned first: that is the shape
    the taint analysis behind S8707 recognises as sanitizing the sink.
    """
    resolved = os.path.realpath(path)
    if resolved != REPO_ROOT and not resolved.startswith(REPO_ROOT + os.sep):
        raise SystemExit(
            f"{path}: outside the repository ({REPO_ROOT}); run the copy of "
            f"this script that lives in the tree you want to measure")
    if not os.path.isfile(resolved):
        raise SystemExit(f"{path}: not a readable file (expected a .ninja_log)")
    return resolved


def main(raw_path: str) -> int:
    objects: list[tuple[float, str]] = []
    scans: list[float] = []
    seen: set[tuple[int, int, str]] = set()
    last_end_ms = 0

    with open(safe_log_path(raw_path), encoding="utf-8") as log:
        for line in log:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 5:
                continue
            start_ms, end_ms, output, cmdhash = (
                int(parts[0]), int(parts[1]), parts[3], parts[4])

            last_end_ms = max(last_end_ms, end_ms)

            key = (start_ms, end_ms, cmdhash)
            if key in seen:
                continue
            seen.add(key)

            seconds = (end_ms - start_ms) / 1000.0
            if output.endswith(SCAN_SUFFIXES):
                scans.append(seconds)
            elif output.endswith(OBJECT_SUFFIXES):
                objects.append((seconds, output))

    if not objects:
        print(f"{path}: no object-compile edges found — wrong log, or a build "
              f"with nothing to do", file=sys.stderr)
        return 1

    aggregate: dict[str, list] = defaultdict(lambda: [0, 0.0])
    for seconds, output in objects:
        row = aggregate[bucket(output)]
        row[0] += 1
        row[1] += seconds

    total = sum(seconds for seconds, _ in objects)
    wall = last_end_ms / 1000.0

    print(f"{'bucket':<32}{'files':>7}{'sec':>9}{'%':>8}{'avg':>8}")
    for name, (count, seconds) in sorted(aggregate.items(), key=lambda kv: -kv[1][1]):
        print(f"{name:<32}{count:>7}{seconds:>9.0f}"
              f"{100 * seconds / total:>7.1f}%{seconds / count:>7.1f}s")
    print(f"{'TOTAL (object compiles)':<32}{len(objects):>7}{total:>9.0f}{100:>7.1f}%")
    print(f"{'module scan edges (excluded)':<32}{len(scans):>7}{sum(scans):>9.0f}")
    print(f"\nwall {wall:.0f} s, so {total / wall:.1f}x effective parallelism")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path to .ninja_log>", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1]))
