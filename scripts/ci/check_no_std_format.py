#!/usr/bin/env python3
"""Keep std::format / std::print out of the library and the shared test headers.

One std::format (or std::print) call that an importing or including TU can reach
makes that TU generate the whole formatting stack - about 470 functions and ~0.4 s
of compile time per TU, measured with -ftime-trace (see
docs/internals/performance/COMPILE_TIME.md). The library's modules and the headers
most tests include are therefore format-free: text is built with plain appends, and
`storm::test::numbered()` / `utilities::zero_padded()` cover the common shapes.

A single new call in a widely included header brings the cost back for every TU
that includes it, and nothing else notices - it compiles and passes. This guard does.

Scanned: every module unit and fragment under src/, every header and test module
interface (.cppm) under tests/, and every header under shared/. A test .cpp may still use
std::format: it pays only for itself.

Deliberately out of scope: benchmarks/, fuzz/ and python/. Only the ninja-release (bench)
and fuzz builds pay for them, not the default test build. benchmarks/ already holds ~75
calls (mostly dashboard/*.hpp and the *.cppm benchmark units), so bringing it under the
guard is its own migration, not a gap in this one.

Only `//` comments are skipped (from `//` to the end of the line), so line-comment prose
may name the functions. Everything else is matched as text and fails safe: a call inside
a block comment, a Doxygen ` * ` line, a raw string or a string literal is flagged too -
reword it. The one false negative is a `//` inside a string literal ahead of a call on
the same line ("http://" + std::format(...)); it does not occur in the tree and is not
worth a C++ lexer. A call split across lines (`std::format`, newline, `(`) and a
namespace alias or `using std::format;` are likewise not caught.

    check_no_std_format.py [repo-root]      (default: this repo)

Exit: 0 = no call found, 1 = at least one found, or the scan itself could not be
trusted (a scanned directory missing, an unreadable file). Under GitHub Actions each
hit is also annotated on the PR diff.

Stdlib only, no syntax newer than Python 3.9.
"""
import os
import pathlib
import re
import sys
from typing import NamedTuple

TOOL = "check-no-std-format"

# directory -> suffixes scanned inside it.
SCANNED = (
    ("src", (".cppm", ".hpp", ".h")),
    ("tests", (".cppm", ".hpp", ".h")),  # .cppm: a test module interface is imported by other TUs
    ("shared", (".hpp", ".h")),
)

# Every entry point that instantiates the formatting stack. `\s*[(<]` also catches explicit
# template arguments (std::format<...>(...)), while std::formatter / std::format_error
# stay out: the name must end at the alternative.
PATTERN = re.compile(
    r"\bstd\s*::\s*"
    r"(format|vformat|format_to|format_to_n|vformat_to|formatted_size|print|println|vprint_unicode|vprint_nonunicode)"
    r"\b\s*[(<]"
)
LINE_COMMENT = re.compile(r"//.*")


class Violation(NamedTuple):
    path: str       # relative to the repo root, forward slashes
    line_number: int
    text: str


class CheckError(Exception):
    """The scan cannot be trusted: fail rather than pass while checking nothing."""


def scanned_sources(root: pathlib.Path) -> list[pathlib.Path]:
    """Every scanned file. A listed directory must exist, or the guard checks nothing."""
    sources = []
    for directory, suffixes in SCANNED:
        path = root / directory
        if not path.is_dir():
            raise CheckError("scanned directory missing: %s — update SCANNED" % directory)
        sources.extend(sorted(p for p in path.rglob("*") if p.is_file() and p.suffix in suffixes))
    return sources


def scan_file(root: pathlib.Path, path: pathlib.Path) -> list[Violation]:
    relative = path.relative_to(root).as_posix()
    try:
        text = path.read_bytes().decode("utf-8", errors="replace")
    except OSError as error:
        raise CheckError("cannot read %s: %s" % (relative, error.strerror or error))
    return [Violation(relative, number, line.strip())
            for number, line in enumerate(text.split("\n"), start=1)
            if PATTERN.search(LINE_COMMENT.sub("", line))]


def find_violations(root: pathlib.Path) -> list[Violation]:
    violations = []
    for path in scanned_sources(root):
        violations.extend(scan_file(root, path))
    return violations


def report(violations: list[Violation]) -> None:
    annotate = bool(os.environ.get("GITHUB_ACTIONS"))
    for hit in violations:
        if annotate:
            print("::error file=%s,line=%d::std::format/std::print in a module or shared test header — "
                  "build the text with plain appends" % (hit.path, hit.line_number), file=sys.stderr)
        print("✗ %s:%d: %s" % (hit.path, hit.line_number, hit.text), file=sys.stderr)
    print("", file=sys.stderr)
    print("%s: %d std::format/std::print call(s) in src/ or a shared test header." % (TOOL, len(violations)),
          file=sys.stderr)
    print("Each one costs every including TU ~0.4 s. Use plain appends, storm::test::numbered() "
          "or utilities::zero_padded().", file=sys.stderr)


def main(argv: list[str]) -> int:
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
    try:
        violations = find_violations(root)
    except CheckError as error:
        print("%s: %s" % (TOOL, error), file=sys.stderr)
        return 1
    if not violations:
        print("✓ no std::format/std::print in src/ or the shared test headers")
        return 0
    report(violations)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
