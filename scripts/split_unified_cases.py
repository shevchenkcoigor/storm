#!/usr/bin/env python3
"""Split the unified YAML test corpus into one JSON file per category.

Usage:
    python3 split_unified_cases.py input.yaml output_dir

Writes <output_dir>/unified_cases_<category>.json for each category. There is no
combined output: every includer of test_parser.hpp selects a slice via
STORM_UNIFIED_CASES_FILE, which since #582 has no default (#error without it).

Issue #561: the 247-case corpus compiled as a single TU took 53 s -- 89% of it
the per-case template instantiation, linear at ~190 ms/case. Splitting the
corpus across TUs lets those instantiations compile in parallel.

The categories group the runners listed in YAML_RUNNERS (tests/test_unified_yaml_body.h),
so each TU instantiates only the runners its own cases need, and a failing suite
name points at a coherent area. A query_type absent from the tables below is a
corpus/runner mismatch and is a hard error -- silently bucketing it would hide a
case that no runner handles. (The C++ side rejects it too: KnownYamlQueryType.)

CATEGORIES below is mirrored by SPLIT_CATEGORIES in tests/CMakeLists.txt and by
the set of tests/yaml/test_unified_yaml_*.cpp TUs. Changing one means changing
all three: adding a category here alone writes a JSON nothing embeds, which
drops those cases from the suite with a green build. One more TU names `crud`:
tests/yaml/test_spec_limits_test_parser.cpp borrows that slice to satisfy
test_parser.hpp's #embed guard (#843); renaming `crud` means updating it too.
"""

from __future__ import annotations

import json
import pathlib
import sys

import yaml  # PyYAML required

# query_type -> category, grouping the YAML_RUNNERS rows by runner:
#   InsertRunner/UpdateRunner/EraseRunner -> crud
#   SelectRunner                          -> select
#   AggregateRunner (chain, group_<func>) / DistinctRunner -> grouped
#   AggregateRunner (scalar aggregates)   -> aggregate
WRITE_OPS = {"insert_one", "insert_batch", "update_batch", "erase_all", "erase_batch"}
SELECT_OPS = {"select", "first", "one"}
GROUPED_OPS = {
    "chain",
    "distinct",
    "group_count",
    "group_sum",
    "group_avg",
    "group_min",
    "group_max",
}
AGGREGATE_OPS = {"count", "count_field", "count_distinct", "sum", "avg", "min", "max"}

CATEGORIES = ("select", "grouped", "aggregate", "crud")

# Mirrors EXPECTED_SQL_CAPACITY in tests/test_parser.hpp (#818). The C++ parser
# cannot report an overflow: it stops copying at capacity and desyncs from the
# JSON, so the length is enforced here, at the one place that sees the YAML.
EXPECTED_SQL_CAPACITY = 160


# Categories whose runner compares the pinned SQL: AggregateRunner (#818) and SelectRunner on
# `select` (#844). On any other category the keys would be silently ignored and the case would
# pass without ever comparing SQL.
EXPECTED_SQL_CATEGORIES = {"aggregate", "select"}

# One text for both backends, or one per dialect (#844): NULLS FIRST/LAST and LIMIT ALL vs
# LIMIT -1 differ, so a single string cannot describe both.
SHARED_SQL_KEY = "expected_sql"
DIALECT_SQL_KEYS = ("expected_sql_sqlite", "expected_sql_pg")
SQL_KEYS = (SHARED_SQL_KEY,) + DIALECT_SQL_KEYS


def validate_sql_text(name: str, key: str, sql: object) -> None:
    """Reject a pinned SQL text the C++ parser would mangle or silently truncate."""
    if not isinstance(sql, str) or not sql:
        raise SystemExit(f"Error: case {name!r}: {key} must be a non-empty string")
    # The C++ parser only unescapes \" and \\; json.dumps would turn anything else
    # (non-ASCII, newline, tab) into a multi-character escape it cannot undo.
    if not (sql.isascii() and sql.isprintable()):
        raise SystemExit(f"Error: case {name!r}: {key} must be printable ASCII")
    # test_parser.hpp's skip_braced ignores string boundaries, so a brace or bracket
    # inside any string value shifts its nesting depth and miscounts the objects.
    if any(ch in sql for ch in "{}[]"):
        raise SystemExit(f"Error: case {name!r}: {key} must not contain any of {{ }} [ ]")
    if len(sql) >= EXPECTED_SQL_CAPACITY:
        raise SystemExit(
            f"Error: case {name!r}: {key} is {len(sql)} chars, capacity is "
            f"{EXPECTED_SQL_CAPACITY - 1}. Raise EXPECTED_SQL_CAPACITY here and in "
            f"tests/test_parser.hpp together."
        )


def validate_expected_sql(case: dict, category: str) -> None:
    """Reject pinned SQL that is unsupported, ill-formed, or that the C++ parser would
    mangle or silently truncate."""
    present = [key for key in SQL_KEYS if key in case]
    if not present:
        return
    name = case.get("name", "<unnamed>")
    if category not in EXPECTED_SQL_CATEGORIES:
        raise SystemExit(
            f"Error: case {name!r} ({category}): {', '.join(present)} is not supported by this "
            f"category's runner yet, so it would be silently ignored. Supported: "
            f"{', '.join(sorted(EXPECTED_SQL_CATEGORIES))}."
        )
    # `first` / `one` share the select category, but SelectRunner pins SQL on `select` only.
    if category == "select" and case.get("query_type") != "select":
        raise SystemExit(
            f"Error: case {name!r}: {', '.join(present)} is supported on query_type select only, "
            f"not {case.get('query_type')!r}: that runner would ignore it and the case would "
            f"pass without comparing SQL."
        )
    has_dialect = [key for key in DIALECT_SQL_KEYS if key in case]
    if SHARED_SQL_KEY in case and has_dialect:
        raise SystemExit(
            f"Error: case {name!r}: {SHARED_SQL_KEY} and {', '.join(has_dialect)} together -- "
            f"set {SHARED_SQL_KEY}, or both {' and '.join(DIALECT_SQL_KEYS)}, never both forms."
        )
    missing = [key for key in DIALECT_SQL_KEYS if has_dialect and key not in case]
    if missing:
        raise SystemExit(
            f"Error: case {name!r}: {', '.join(has_dialect)} without {', '.join(missing)} -- "
            f"a dialect left out would run with no SQL check at all."
        )
    for key in present:
        validate_sql_text(name, key, case[key])


def categorize(case: dict, index: int) -> str:
    """Map one case to its category (the runner group in YAML_RUNNERS)."""
    if not isinstance(case, dict):
        raise SystemExit(
            f"Error: corpus entry {index} is {type(case).__name__}, not a mapping -- "
            f"most likely a YAML indentation slip."
        )
    qt = case.get("query_type")
    if qt in WRITE_OPS:
        return "crud"
    if qt in SELECT_OPS:
        return "select"
    if qt in GROUPED_OPS:
        return "grouped"
    if qt in AGGREGATE_OPS:
        return "aggregate"
    raise SystemExit(
        f"Error: case {case.get('name', '<unnamed>')!r} has unknown query_type "
        f"{qt!r}. Add it to the tables in {pathlib.Path(__file__).name} and and "
        f"add a row to YAML_RUNNERS in tests/test_unified_yaml_body.h unless a prefix row "
        f"(group_) already covers it."
    )


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: split_unified_cases.py input.yaml output_dir", file=sys.stderr)
        sys.exit(1)

    # Resolve both paths up front and require them to already exist: this script
    # only ever rewrites files CMake generates, so a mistyped argument should fail
    # here rather than read or write somewhere unexpected.
    input_path = pathlib.Path(sys.argv[1]).resolve()
    output_dir = pathlib.Path(sys.argv[2]).resolve()

    if not input_path.is_file():
        raise SystemExit(f"Error: corpus {input_path} is not an existing file")
    if not output_dir.is_dir():
        raise SystemExit(f"Error: output directory {output_dir} does not exist")

    with input_path.open(encoding="utf-8") as fh:
        cases = yaml.safe_load(fh) or []

    if not isinstance(cases, list):
        print("Error: expected a top-level list in the YAML corpus", file=sys.stderr)
        sys.exit(1)

    buckets: dict[str, list] = {name: [] for name in CATEGORIES}
    for index, case in enumerate(cases):
        category = categorize(case, index)
        validate_expected_sql(case, category)
        buckets[category].append(case)

    # A truncated, empty or all-comments YAML otherwise yields four empty JSONs,
    # four suites registering zero tests, and a green ctest that verifies nothing.
    # Every category maps to runners that have cases today, so an empty
    # bucket means the corpus or the tables above are wrong, not that the corpus
    # legitimately shrank.
    empty = [name for name in CATEGORIES if not buckets[name]]
    if empty:
        raise SystemExit(
            f"Error: no cases for {', '.join(empty)} in {input_path}. An empty category "
            f"would register a suite with zero tests and still pass. Check the corpus, "
            f"or drop the category from CATEGORIES here, from the foreach() in "
            f"tests/CMakeLists.txt, and delete its test TU."
        )

    for name in CATEGORIES:
        # The basename comes from the fixed CATEGORIES tuple, never from input, but
        # resolve and confirm containment anyway so the write target is provably
        # inside the directory that was passed in.
        out = (output_dir / f"unified_cases_{name}.json").resolve()
        if out.parent != output_dir:
            raise SystemExit(f"Error: refusing to write outside {output_dir}: {out}")
        out.write_text(json.dumps(buckets[name], separators=(",", ":")) + "\n", encoding="utf-8")

    total = sum(len(b) for b in buckets.values())
    parts = ", ".join(f"{name}={len(buckets[name])}" for name in CATEGORIES)
    print(f"Split {total} cases -> {parts}")


if __name__ == "__main__":
    main()
