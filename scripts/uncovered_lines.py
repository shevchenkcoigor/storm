#!/usr/bin/env python3
"""List the uncovered lines of an lcov file as compact ranges (issue #688).

commit.sh's coverage step prints this when line coverage is below 100%, so the
author sees which lines to cover without opening the HTML report. It used to be
`show_uncovered_lines` + `format_line_ranges` in bash; as a plain function the
parsing is unit-tested (scripts/tests/test_uncovered_lines.py) instead of being
exercised only by a failing coverage run.

    scripts/uncovered_lines.py      (run from the repo root; takes no arguments)

Reads build/debug/coverage/coverage-filtered.lcov under the current directory and
prints nothing when it does not exist. Source paths under the current directory are
shown relative to it. Exit: 0, or 2 on a stray argument — this is a diagnostic, the
gate is the caller's. No path comes from the command line. A non-empty NO_COLOR
environment variable switches the ANSI colours off.

Needs only Python 3.7+ (f-strings); no newer str/subprocess APIs.
"""
import os
import sys

COLORS = {"RED": "\033[0;31m", "YELLOW": "\033[0;33m", "BOLD": "\033[1m", "DIM": "\033[2m", "RESET": "\033[0m"}

# The one file commit.sh's coverage target writes, relative to the repo root. It is
# a constant, not an argument: nothing from the command line names a path.
LCOV_RELATIVE_PATH = os.path.join("build", "debug", "coverage", "coverage-filtered.lcov")
RULE_WIDTH = 60
HTML_REPORT_HINT = (
    "cmake --build --preset ninja-debug-coverage --target coverage-html",
    "open build/debug/coverage/html-filtered/index.html",
)


def format_ranges(line_numbers):
    """Compress ascending line numbers to "5, 10-15, 20"."""
    ranges = []
    for line in line_numbers:
        if ranges and line == ranges[-1][1] + 1:
            ranges[-1][1] = line
        else:
            ranges.append([line, line])
    return ", ".join(str(lo) if lo == hi else f"{lo}-{hi}" for lo, hi in ranges)


def uncovered_by_file(lcov_text):
    """Return [(source_file, [uncovered line numbers])], files without gaps omitted."""
    result = []
    current_file = None
    uncovered = []

    def flush():
        if uncovered:
            result.append((current_file, list(uncovered)))

    for raw in lcov_text.split("\n"):
        line = raw[:-1] if raw.endswith("\r") else raw
        if line.startswith("SF:"):
            flush()
            current_file = line[len("SF:"):]
            uncovered = []
        elif line.startswith("DA:"):
            line_number, _, rest = line[len("DA:"):].partition(",")
            if rest.split(",")[0] == "0":
                uncovered.append(int(line_number))
    flush()
    return result


def relative_to(path, prefix):
    return path[len(prefix):] if path.startswith(prefix) else path


def render(lcov_text, base_dir, color=True):
    """The full report text, ready to print."""
    c = COLORS if color else {name: "" for name in COLORS}
    out = ["", f"{c['BOLD']}Uncovered lines:{c['RESET']}", f"{c['DIM']}{'─' * RULE_WIDTH}{c['RESET']}"]
    prefix = base_dir.rstrip("/") + "/"
    files = uncovered_by_file(lcov_text)
    for path, lines in files:
        out.append(f"  {c['YELLOW']}{relative_to(path, prefix)}{c['RESET']}")
        out.append(f"    Lines: {c['RED']}{format_ranges(lines)}{c['RESET']}")
    if not files:
        out.append(f"  {c['DIM']}(could not parse uncovered lines from lcov){c['RESET']}")
    out += ["", f"{c['DIM']}For detailed HTML report:{c['RESET']}"]
    out += [f"{c['DIM']}  {command}{c['RESET']}" for command in HTML_REPORT_HINT]
    return "\n".join(out)


def working_dir():
    """The logical working directory, as bash's $PWD (what the old script stripped).

    getcwd() is the physical path, which differs under a symlink (macOS /tmp is
    /private/tmp), while lcov's SF: lines hold whichever path the build used. $PWD
    is honoured only when it really names the current directory.
    """
    logical = os.environ.get("PWD", "")
    try:
        if logical and os.path.samefile(logical, "."):
            return logical
    except OSError:
        pass
    return os.getcwd()


def main(argv):
    if len(argv) != 1:
        print(f"usage: {argv[0]}  (run from the repo root; takes no arguments)", file=sys.stderr)
        return 2
    base_dir = working_dir()
    lcov_path = os.path.join(base_dir, LCOV_RELATIVE_PATH)
    if not os.path.isfile(lcov_path):
        return 0
    with open(lcov_path, encoding="utf-8", errors="replace") as lcov:
        print(render(lcov.read(), base_dir, color=not os.environ.get("NO_COLOR")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
