#!/usr/bin/env python3
"""Keep the generic ORM layer free of any concrete database backend (issue #692).

The ORM templates (BasicQuerySet, Basic*Statement, BasicDefaultConnection,
SchemaStatement, ...) are written against the DatabaseConnection concept only.
The SQLite default that `QuerySet<T>` / `SelectStatement<T>` / ... carry lives in
ONE place - the `storm_defaults` alias module - so no generic module's import
graph reaches the SQLite backend. (This is module-level decoupling; the single
`storm` CMake target still builds every backend.)

This guard fails if a backend-neutral module names a concrete backend again, which
is how the coupling crept in originally: a `= sqlite::Connection` template default
needs `import storm_db_sqlite;` in the generic module. tests/db/test_backend_layering.cpp
is the compile-time twin of this check (it pins that the generic templates have no
default backend at all).

Comments are checked too, deliberately: telling a comment from code reliably across
raw strings and trailing comments is not worth a parser, and prose about a backend
belongs in that backend's module or in storm_defaults.

    check_orm_backend_imports.py [repo-root]      (default: this repo)

Exit: 0 = no backend reference in a neutral module, 1 = at least one found, or the
scan itself could not be trusted (no src/orm, a listed path missing, an unreadable
file). Under GitHub Actions each hit is also annotated on the PR diff.

Stdlib only, no syntax newer than Python 3.9.
"""
import os
import pathlib
import re
import sys
from typing import NamedTuple

TOOL = "check-orm-backend-imports"

# Backend-neutral sources: the whole ORM tree plus the generic db-layer facades that
# sit beside the backends in src/db/.
NEUTRAL_PATHS = (
    "src/orm",
    "src/db/default_connection.cppm",
    "src/db/pool.cppm",
    "src/storm_model.cppm",
)

# A backend module import, a qualified backend name, the backend namespace itself
# (`using namespace storm::db::sqlite;`), or the defaults/umbrella modules that bind a
# backend (`import storm_defaults;` / `import storm;`).
PATTERN = re.compile(
    r"storm_db_(sqlite|postgresql)|(sqlite|postgresql)::|storm::db::(sqlite|postgresql)\b|import storm(_defaults)?;"
)


# Module units, plus the textual fragments a module #includes into its purview (#671):
# a split file that kept half its code in a .hpp must not drop out of the scan.
SCANNED_SUFFIXES = (".cppm", ".hpp", ".h")


class Violation(NamedTuple):
    path: str       # relative to the repo root, forward slashes
    line_number: int
    text: str


class CheckError(Exception):
    """The scan cannot be trusted: fail rather than pass while checking nothing."""


def neutral_sources(root: pathlib.Path) -> list[pathlib.Path]:
    """Every .cppm, and every #include fragment (.hpp/.h, #671), under the neutral paths.

    Every listed path must exist: a renamed or split file would otherwise drop out of
    the scan silently and the guard would pass while checking nothing.
    """
    sources = []
    for relative in NEUTRAL_PATHS:
        path = root / relative
        if not path.exists():
            raise CheckError("listed path missing: %s — update NEUTRAL_PATHS" % relative)
        candidates = path.rglob("*") if path.is_dir() else [path]
        sources.extend(sorted(p for p in candidates if p.suffix in SCANNED_SUFFIXES))
    return sources


def scan_file(root: pathlib.Path, path: pathlib.Path) -> list[Violation]:
    relative = path.relative_to(root).as_posix()
    try:
        text = path.read_bytes().decode("utf-8", errors="replace")
    except OSError as error:
        raise CheckError("cannot read %s: %s" % (relative, error.strerror or error))
    return [Violation(relative, number, line)
            for number, line in enumerate(text.split("\n"), start=1) if PATTERN.search(line)]


def find_violations(root: pathlib.Path) -> list[Violation]:
    if not (root / "src" / "orm").is_dir():
        raise CheckError("no src/orm under: %s" % root)
    violations = []
    for path in neutral_sources(root):
        violations.extend(scan_file(root, path))
    return violations


def report(violations: list[Violation]) -> None:
    annotate = bool(os.environ.get("GITHUB_ACTIONS"))
    for hit in violations:
        # Under GitHub Actions, annotate the offending line in the PR diff.
        if annotate:
            print("::error file=%s,line=%d::backend-neutral module names a concrete backend — "
                  "move the default into storm_defaults" % (hit.path, hit.line_number), file=sys.stderr)
        print("✗ %s:%d: %s" % (hit.path, hit.line_number, hit.text), file=sys.stderr)
    print("", file=sys.stderr)
    print("%s: %d backend reference(s) in backend-neutral modules." % (TOOL, len(violations)), file=sys.stderr)
    print("Generic templates must not default to a backend; add the default alias to src/storm_defaults.cppm.",
          file=sys.stderr)


def main(argv: list[str]) -> int:
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path(__file__).resolve().parent.parent
    try:
        violations = find_violations(root)
    except CheckError as error:
        print("%s: %s" % (TOOL, error), file=sys.stderr)
        return 1
    if not violations:
        print("✓ ORM layer names no concrete database backend")
        return 0
    report(violations)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
