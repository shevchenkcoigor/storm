#!/usr/bin/env python3
"""The 100% line-coverage gate and the PostgreSQL default it depends on (issue #765).

One module for what commit.sh's coverage step, CI's `coverage` job and
coverage-run-batched.sh each used to do their own way (two sourced bash helpers plus an
inline awk block), so the local and CI gates cannot drift.

    coverage_gate.py check < summary.txt     compare the line row to 100.0; on failure
                                             list the uncovered lines. Exit 0 / 1.
    coverage_gate.py line-coverage < summary the bare percentage ("100.0"), or nothing
    coverage_gate.py pg-connstr [OS]         the default STORM_PG_CONNSTR for OS (default:
                                             this host); exit 1 naming an unsupported OS

`check` reads the `lcov --summary` text the coverage target prints, from stdin, and the
filtered lcov file under the current directory (see uncovered_lines.py). A non-empty
NO_COLOR switches the ANSI colours off. Under GitHub Actions a failure also carries a
`::error::` annotation.

Parsing anchors on the LINE row ("lines.......: 100.0% (8613 of 8613 lines)"), not the
functions/branches rows, and takes the last one.

Needs only Python 3.7+; macOS system python3 runs it.
"""
import os
import platform
import sys

REQUIRED_LINE_COVERAGE = "100.0"
LINES_ROW = "lines......."
DIGITS_AND_DOT = set("0123456789.")

# The socket DIRECTORY libpq connects through. A Linux PostgreSQL package (and the CI
# service container) uses /var/run/postgresql; a Homebrew PostgreSQL on macOS listens in
# /tmp. An explicit per-OS table, not a probe of which socket exists: it documents intent,
# and an unsupported host fails naming itself instead of silently skipping every PG test.
SOCKET_DIR_BY_OS = {"Linux": "/var/run/postgresql", "Darwin": "/tmp"}  # NOSONAR S5443: Homebrew's PostgreSQL socket directory, not a file we create
CONNSTR_TEMPLATE = "host=%s dbname=storm_db user=storm_db"


def parse_line_coverage(summary_text):
    """The percentage of the last `lines.......` row without its `%`, or None."""
    rows = [line for line in summary_text.splitlines() if LINES_ROW in line]
    if not rows:
        return None
    for token in rows[-1].split():
        number = token[:-1]
        if token.endswith("%") and number and set(number) <= DIGITS_AND_DOT:
            return number
    return None


def default_pg_connstr(os_name=None):
    """The default STORM_PG_CONNSTR for `os_name` (default: this host's `uname -s`)."""
    os_name = os_name or platform.system()
    if os_name not in SOCKET_DIR_BY_OS:
        raise ValueError("no default PostgreSQL socket directory for OS [%s]; "
                         "export STORM_PG_CONNSTR explicitly" % os_name)
    return CONNSTR_TEMPLATE % SOCKET_DIR_BY_OS[os_name]


def fail(message, annotation=None):
    """Print the failure line (plus a GitHub Actions annotation) and return exit 1."""
    if os.environ.get("GITHUB_ACTIONS") and annotation:
        print("::error::" + annotation)
    print(message)
    return 1


def check(summary_text):
    """The gate. Returns the exit code; everything it says goes to stdout."""
    percent = parse_line_coverage(summary_text)
    if percent is None:
        return fail("Could not parse line coverage from output.",
                    "Could not parse line coverage from the lcov summary.")
    if percent == REQUIRED_LINE_COVERAGE:
        print("Line coverage: %s%%" % percent)
        return 0
    import uncovered_lines  # only a miss needs it; pg-connstr must not depend on it
    colored = not os.environ.get("NO_COLOR")
    colors = uncovered_lines.COLORS if colored else dict.fromkeys(uncovered_lines.COLORS, "")
    red, bold, reset = colors["RED"], colors["BOLD"], colors["RESET"]
    fail("%s%sLine coverage: %s%% (required: %s%%)%s"
         % (red, bold, percent, REQUIRED_LINE_COVERAGE, reset),
         "Line coverage %s%% is below the required %s%%." % (percent, REQUIRED_LINE_COVERAGE))
    uncovered_lines.report(colored)
    return 1


def main(argv):
    command = argv[1] if len(argv) > 1 else ""
    if command == "check" and len(argv) == 2:
        return check(sys.stdin.read())
    if command == "line-coverage" and len(argv) == 2:
        percent = parse_line_coverage(sys.stdin.read())
        if percent is not None:
            print(percent)
        return 0
    if command == "pg-connstr" and len(argv) <= 3:
        try:
            print(default_pg_connstr(argv[2] if len(argv) == 3 else None))
        except ValueError as error:
            print("ERROR: %s" % error, file=sys.stderr)
            return 1
        return 0
    print("usage: %s check | line-coverage | pg-connstr [OS]   (check and line-coverage read "
          "the lcov summary on stdin)" % argv[0], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
