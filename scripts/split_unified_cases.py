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
drops those cases from the suite with a green build.
"""

from __future__ import annotations

import json
import pathlib
import sys

import yaml  # PyYAML required

# query_type -> category, grouping the YAML_RUNNERS rows by runner:
#   InsertRunner/UpdateRunner/EraseRunner -> crud
#   SelectRunner                          -> select
#   ChainAggRunner/DistinctRunner/GroupByRunner -> grouped
#   AggregateRunner                       -> aggregate
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


# Categories whose runner compares expected_sql. Only AggregateRunner reads it
# today; on any other category the key would be silently ignored and the case
# would pass without ever comparing SQL.
EXPECTED_SQL_CATEGORIES = {"aggregate"}


def validate_expected_sql(case: dict, category: str) -> None:
    """Reject an expected_sql that is unsupported, or that the C++ parser would
    mangle or silently truncate."""
    sql = case.get("expected_sql")
    if sql is None:
        return
    name = case.get("name", "<unnamed>")
    if category not in EXPECTED_SQL_CATEGORIES:
        raise SystemExit(
            f"Error: case {name!r} ({category}): expected_sql is not supported by this "
            f"category's runner yet, so it would be silently ignored. Supported: "
            f"{', '.join(sorted(EXPECTED_SQL_CATEGORIES))}."
        )
    if not isinstance(sql, str) or not sql:
        raise SystemExit(f"Error: case {name!r}: expected_sql must be a non-empty string")
    # The C++ parser only unescapes \" and \\; json.dumps would turn anything else
    # (non-ASCII, newline, tab) into a multi-character escape it cannot undo.
    if not (sql.isascii() and sql.isprintable()):
        raise SystemExit(f"Error: case {name!r}: expected_sql must be printable ASCII")
    # test_parser.hpp's skip_braced ignores string boundaries, so a brace or bracket
    # inside any string value shifts its nesting depth and miscounts the objects.
    if any(ch in sql for ch in "{}[]"):
        raise SystemExit(f"Error: case {name!r}: expected_sql must not contain any of {{ }} [ ]")
    if len(sql) >= EXPECTED_SQL_CAPACITY:
        raise SystemExit(
            f"Error: case {name!r}: expected_sql is {len(sql)} chars, capacity is "
            f"{EXPECTED_SQL_CAPACITY - 1}. Raise EXPECTED_SQL_CAPACITY here and in "
            f"tests/test_parser.hpp together."
        )


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
