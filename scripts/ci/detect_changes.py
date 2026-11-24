#!/usr/bin/env python3
"""Classify a list of changed file paths into HAS_*_CHANGES flags (issue #765).

Used to skip work a commit/PR cannot affect: one place shared by commit.sh (staged files,
pre-commit) and .github/workflows/ci.yml (diff against the PR base / previous push), so the
two never drift apart. It was detect-changes.sh; the output is unchanged.

Usage: printf '%s\n' "${files[@]}" | scripts/ci/detect_changes.py
       (or: git diff --name-only ... | scripts/ci/detect_changes.py)

Reads file paths on stdin (one per line), prints `KEY=true|false` lines on stdout - eval
that output to get the flags as shell variables:
    eval "$(git diff --cached --name-only | ./scripts/ci/detect_changes.py)"

Needs only Python 3.7+; macOS system python3 runs it.
"""
import re
import sys

CPP_FILE = re.compile(r"\.(cpp|cppm|h|hpp)$")

# (flag, test on one path). Order is the output order.
RULES = (
    ("HAS_SRC_CHANGES", lambda path: path.startswith("src/")),
    ("HAS_TEST_CHANGES", lambda path: path.startswith("tests/")),
    ("HAS_CPP_CHANGES", CPP_FILE.search),
    ("HAS_CMAKE_CHANGES", re.compile(r"(CMakeLists\.txt|\.cmake)$").search),
    ("HAS_BENCH_CHANGES", lambda path: path.startswith("benchmarks/")),
    # Whether the PR clang-tidy job has anything to do (#913): it lints changed C++ lines only, so a
    # change without C++ gives it nothing. The tidy plumbing itself (the runner and its tests, and
    # ci.yml, which holds the job) counts too, so an edit to it is verified by the job it configures.
    # A .clang-tidy config change does not: the weekly sweep validates it.
    ("HAS_TIDY_CHANGES", lambda path: bool(CPP_FILE.search(path)) or path.startswith("scripts/tidy/")
     or path == ".github/workflows/ci.yml"),
)


def classify(paths):
    """{flag: bool} for the given paths; blank lines are ignored."""
    paths = [path for path in paths if path]
    return {flag: any(rule(path) for path in paths) for flag, rule in RULES}


def main():
    flags = classify(line.rstrip("\n") for line in sys.stdin)
    for flag, value in flags.items():
        print("%s=%s" % (flag, "true" if value else "false"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
