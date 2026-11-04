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
    coverage_gate.py uncovered               the uncovered files and line ranges of the
                                             filtered lcov file, or nothing if it is absent

`check` reads the `lcov --summary` text the coverage target prints, from stdin, and the
filtered lcov file under the current directory (build/debug/coverage/coverage-filtered.lcov,
a constant: no path comes from the command line). Source paths under the current directory
are shown relative to it. A non-empty NO_COLOR switches the ANSI colours off. Under GitHub
Actions a failure also carries a `::error::` annotation.

`uncovered` is the same report on its own (run from the repo root; takes no arguments). It
replaced the former uncovered_lines.py (#776), formerly `show_uncovered_lines` +
`format_line_ranges` in commit.sh's bash (#688); the parsing is unit-tested rather than
exercised only by a failing coverage run.

Parsing anchors on the LINE row ("lines.......: 100.0% (8613 of 8613 lines)"), not the
functions/branches rows, and takes the last one.

Needs only Python 3.7+ (f-strings); macOS system python3 runs it.
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

COLORS = {"RED": "\033[0;31m", "YELLOW": "\033[0;33m", "BOLD": "\033[1m", "DIM": "\033[2m", "RESET": "\033[0m"}

# The one file commit.sh's coverage target writes, relative to the repo root. It is
# a constant, not an argument: nothing from the command line names a path.
LCOV_RELATIVE_PATH = os.path.join("build", "debug", "coverage", "coverage-filtered.lcov")
RULE_WIDTH = 60
HTML_REPORT_HINT = (
    "cmake --build --preset ninja-debug-coverage --target coverage-html",
    "open build/debug/coverage/html-filtered/index.html",
)


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
    c = COLORS if color else dict.fromkeys(COLORS, "")
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


def report(color):
    """Print the report for the lcov file under the working directory; nothing if absent."""
    base_dir = working_dir()
    lcov_path = os.path.join(base_dir, LCOV_RELATIVE_PATH)
    if not os.path.isfile(lcov_path):
        return
    with open(lcov_path, encoding="utf-8", errors="replace") as lcov:
        print(render(lcov.read(), base_dir, color=color))


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
    colored = not os.environ.get("NO_COLOR")
    colors = COLORS if colored else dict.fromkeys(COLORS, "")
    red, bold, reset = colors["RED"], colors["BOLD"], colors["RESET"]
    fail("%s%sLine coverage: %s%% (required: %s%%)%s"
         % (red, bold, percent, REQUIRED_LINE_COVERAGE, reset),
         "Line coverage %s%% is below the required %s%%." % (percent, REQUIRED_LINE_COVERAGE))
    report(colored)
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
    if command == "uncovered" and len(argv) == 2:
        report(color=not os.environ.get("NO_COLOR"))
        return 0
    print("usage: %s check | line-coverage | pg-connstr [OS] | uncovered   (check and "
          "line-coverage read the lcov summary on stdin)" % argv[0], file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
