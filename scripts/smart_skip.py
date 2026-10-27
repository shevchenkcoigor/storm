#!/usr/bin/env python3
"""Decide which pre-commit steps a staged-file set needs (issue #688).

Reads scripts/detect-changes.sh's `HAS_*_CHANGES=true|false` lines on stdin —
the classification stays in that one script, shared with .github/workflows/ci.yml
so both skip on the same rules — and prints shell assignments for commit.sh to
`eval`: the RUN_* switches plus SKIP_NOTE, the one line explaining a skip ("" when
nothing was skipped).

    printf '%s\\n' "$staged" | scripts/detect-changes.sh | scripts/smart_skip.py

Empty stdin (no staged files), or input missing any of the five flags or holding
a value other than exactly true/false, keeps every gate on and the bench build
off: an unclassifiable commit gets every check, never fewer.

Rules (first match wins for the note):
  no C++ and no cmake          -> skip format, tidy, tests, coverage
  cmake but no C++             -> skip clang-format, clang-tidy
  C++ but no src/tests/cmake   -> skip tests, coverage
  no cmake                     -> skip cmake-format
  bench AND C++                -> also build release for the benchmarks

Stdlib only, no syntax newer than Python 3.7.
"""
import shlex
import sys

RUN_KEYS = ("RUN_FORMAT", "RUN_CMAKE_FORMAT", "RUN_TIDY", "RUN_TESTS", "RUN_COVERAGE", "RUN_BENCH_RELEASE")


def decide(flags):
    """flags: {HAS_*_CHANGES: bool}, or None when nothing was classified.

    Returns ({RUN_*: bool}, note).
    """
    plan = {key: key != "RUN_BENCH_RELEASE" for key in RUN_KEYS}
    if flags is None:
        return plan, ""

    cpp, cmake = flags["HAS_CPP_CHANGES"], flags["HAS_CMAKE_CHANGES"]
    note = ""
    if not cpp and not cmake:
        note = "No C++ or cmake files in commit — skipping format, tidy, tests, coverage"
        plan.update(RUN_FORMAT=False, RUN_CMAKE_FORMAT=False, RUN_TIDY=False,
                    RUN_TESTS=False, RUN_COVERAGE=False)
    elif not cpp:
        note = "cmake-only changes — skipping clang-format, clang-tidy"
        plan.update(RUN_FORMAT=False, RUN_TIDY=False)
    elif not (flags["HAS_SRC_CHANGES"] or flags["HAS_TEST_CHANGES"] or cmake):
        note = "No src/, tests/, or cmake changes — skipping tests, coverage"
        plan.update(RUN_TESTS=False, RUN_COVERAGE=False)

    if not cmake:
        plan["RUN_CMAKE_FORMAT"] = False
    plan["RUN_BENCH_RELEASE"] = flags["HAS_BENCH_CHANGES"] and cpp
    return plan, note


FLAG_KEYS = ("HAS_SRC_CHANGES", "HAS_TEST_CHANGES", "HAS_CPP_CHANGES", "HAS_CMAKE_CHANGES", "HAS_BENCH_CHANGES")


def parse_flags(text):
    """The five flags, or None unless ALL are present and exactly true/false.

    None keeps every gate on. A missing or malformed flag must never read as "no
    change": that would skip checks, the one direction this must not fail in.
    """
    parsed = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep and key in FLAG_KEYS and value.strip() in ("true", "false"):
            parsed[key] = value.strip() == "true"
    return parsed if all(key in parsed for key in FLAG_KEYS) else None


def main():
    plan, note = decide(parse_flags(sys.stdin.read()))
    for key in RUN_KEYS:
        print("%s=%s" % (key, "true" if plan[key] else "false"))
    print("SKIP_NOTE=%s" % shlex.quote(note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
