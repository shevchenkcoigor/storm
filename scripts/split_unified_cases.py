#!/usr/bin/env python3
"""Split the YAML test corpus into one JSON file per category.

Usage:
    python3 split_unified_cases.py output_dir input.yaml [input.yaml ...]

The corpus is several authoring files, tests/test_cases/<topic>.yaml (#846), grouped by
meaning for humans: a case spans several topics and a topic spans several runners, so which
file a case lives in is never an error. This script reads them ALL (in the order given --
CMake passes them sorted), buckets every case by query_type (a case on dataset `relations`
goes to the `join` slice whatever its query_type, #869) and writes
<output_dir>/unified_cases_<category>.json for each category. Case names must be unique
across the whole corpus. There is no
combined output: every includer of test_unified_yaml_body.h selects a slice via
STORM_UNIFIED_CASES_FILE, which since #582 has no default (#error without it).

Issue #561: the 247-case corpus compiled as a single TU took 53 s -- 89% of it
the per-case template instantiation, linear at ~190 ms/case. Splitting the
corpus across TUs lets those instantiations compile in parallel.

The categories (but `join`) group the runners listed in YAML_RUNNERS (tests/test_unified_yaml_body.h),
so each TU instantiates only the runners its own cases need, and a failing suite
name points at a coherent area. A query_type absent from the tables below is a
corpus/runner mismatch and is a hard error -- silently bucketing it would hide a
case that no runner handles. (The C++ side rejects it too: KnownYamlQueryType.)

A JSON whose bytes did not change is left untouched (mtime included). ninja re-runs this
script whenever any YAML changes and, because CMake marks custom commands `restat`, then
rebuilds only the TUs whose slice really changed -- editing one case recompiles one slice,
not five.

CATEGORIES below is mirrored by SPLIT_CATEGORIES in tests/CMakeLists.txt and by
the set of tests/yaml/test_unified_yaml_*.cpp TUs. Changing one means changing
all three: adding a category here alone writes a JSON nothing embeds, which
drops those cases from the suite with a green build.
"""

from __future__ import annotations

import datetime
import json
import pathlib
import sys

import yaml  # PyYAML required

# query_type -> category, grouping the YAML_RUNNERS rows by runner:
#   InsertRunner/UpdateRunner/EraseRunner -> crud
#   SelectRunner / ValuesRunner           -> select
#   AggregateRunner (chain, group_<func>) / DistinctRunner -> grouped
#   AggregateRunner (scalar aggregates)   -> aggregate
WRITE_OPS = {"insert_one", "insert_batch", "update_batch", "erase_all", "erase_batch"}
SELECT_OPS = {"select", "first", "one", "rows", "values"}
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

# #869: a case on dataset `relations` (the m2m / reverse_fk / two-FK models of
# benchmarks/m2m_models.hpp) goes to its own slice whatever its query_type, so only the
# `join` TU creates those tables and instantiates statements on those models. What SQL it
# may pin is still decided by its runner group above.
RELATIONS_DATASET = "relations"
JOIN_CATEGORY = "join"

CATEGORIES = ("select", "grouped", "aggregate", "crud", JOIN_CATEGORY)

# Mirrors EXPECTED_SQL_CAPACITY in tests/test_parser.hpp (#818). Since #845 the C++
# parser rejects every over-long case at compile time; the script keeps its check so the
# error names the case and the key.
EXPECTED_SQL_CAPACITY = 160


# Categories whose runner compares the pinned SQL: AggregateRunner (#818) and SelectRunner on
# `select` (#844). On any other category the keys would be silently ignored and the case would
# pass without ever comparing SQL.
EXPECTED_SQL_CATEGORIES = {"aggregate", "select"}
# Within the select category only these query_types pin SQL: SelectRunner on `select`, ValuesRunner
# on `values` (#867). `first` / `one` / `rows` would ignore it.
SQL_PINNING_SELECT_OPS = {"select", "values"}

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
    # `first` / `one` / `rows` share the select category, but only select and values pin SQL.
    if category == "select" and case.get("query_type") not in SQL_PINNING_SELECT_OPS:
        raise SystemExit(
            f"Error: case {name!r}: {', '.join(present)} is supported on query_type select and values only, "
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


def reject_unquoted_temporal(name: str, value: object, path: str) -> None:
    """PyYAML reads an unquoted 2024-01-15 (or 2024-01-15T10:30:00) as a date/datetime, which JSON cannot
    carry. A date or time point is a STRING in the corpus, so say which case and key forgot the quotes
    instead of dying later in json.dumps with an unrelated TypeError (#853). A date used as a mapping key
    is caught too. A bare time of day or `0102` is not a date for PyYAML (it loads as an int), so it is
    not detectable here: quote those."""
    if isinstance(value, datetime.date):  # datetime.datetime is a date
        raise SystemExit(
            f"Error: case {name!r}: {path} is an unquoted {type(value).__name__} ({value.isoformat()}); "
            f"PyYAML reads it as {type(value).__name__}, not text. Write it as a quoted string: "
            f"'{value.isoformat()}'."
        )
    if isinstance(value, dict):
        for key, item in value.items():
            reject_unquoted_temporal(name, key, f"{path}.<key {key!s}>" if path else f"<key {key!s}>")
            reject_unquoted_temporal(name, item, f"{path}.{key}" if path else str(key))
    elif isinstance(value, list):
        for position, item in enumerate(value):
            reject_unquoted_temporal(name, item, f"{path}[{position}]")


def slice_of(case: dict, runner_category: str) -> str:
    """The slice a case compiles in: `join` for dataset `relations` (#869), else its runner group."""
    return JOIN_CATEGORY if case.get("dataset") == RELATIONS_DATASET else runner_category


def runner_category_of(case: dict, index: int) -> str:
    """Map one case to its runner group in YAML_RUNNERS, which also decides what SQL it may pin."""
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
        f"{qt!r}. Add it to the tables in {pathlib.Path(__file__).name} and "
        f"add a row to YAML_RUNNERS in tests/test_unified_yaml_body.h unless a prefix row "
        f"(group_) already covers it."
    )


def load_cases(path: pathlib.Path) -> list:
    """Read one authoring file: a top-level list of cases, or empty (a new topic file)."""
    if not path.is_file():
        raise SystemExit(f"Error: corpus {path} is not an existing file")
    with path.open(encoding="utf-8") as fh:
        cases = yaml.safe_load(fh) or []
    if not isinstance(cases, list):
        raise SystemExit(f"Error: {path}: expected a top-level list of cases in the YAML corpus")
    return cases


def bucket_corpus(inputs: list[pathlib.Path]) -> dict[str, list]:
    """Bucket every case of every file by category; reject a repeated case name anywhere."""
    buckets: dict[str, list] = {name: [] for name in CATEGORIES}
    seen_in: dict[str, pathlib.Path] = {}
    for path in inputs:
        for index, case in enumerate(load_cases(path)):
            try:
                runner_category = runner_category_of(case, index)
                category = slice_of(case, runner_category)
                validate_expected_sql(case, runner_category)
            except SystemExit as err:
                raise SystemExit(f"{err} (in {path})") from None
            name = case.get("name")
            if not isinstance(name, str) or not name:
                raise SystemExit(f"Error: a case in {path} has no `name` string: {case!r:.80}")
            try:
                reject_unquoted_temporal(name, case, "")
            except SystemExit as err:
                raise SystemExit(f"{err} (in {path})") from None
            if name in seen_in:
                raise SystemExit(
                    f"Error: duplicate case name {name!r} in {path} (first defined in "
                    f"{seen_in[name]}). Names must be unique across all corpus files."
                )
            seen_in[name] = path
            buckets[category].append(case)
    return buckets


def write_if_changed(out: pathlib.Path, text: str) -> None:
    """Leave an unchanged file alone so its mtime does not trigger a recompile of its TU."""
    if out.is_file() and out.read_text(encoding="utf-8") == text:
        return
    out.write_text(text, encoding="utf-8")


def main() -> None:
    if len(sys.argv) < 3:
        print("Usage: split_unified_cases.py output_dir input.yaml [input.yaml ...]", file=sys.stderr)
        sys.exit(1)

    # Resolve every path up front and require the output directory to already exist: this
    # script only ever rewrites files CMake generates, so a mistyped argument should fail
    # here rather than read or write somewhere unexpected.
    output_dir = pathlib.Path(sys.argv[1]).resolve()
    inputs = [pathlib.Path(arg).resolve() for arg in sys.argv[2:]]

    if not output_dir.is_dir():
        raise SystemExit(f"Error: output directory {output_dir} does not exist")

    buckets = bucket_corpus(inputs)

    # A truncated, empty or all-comments corpus otherwise yields five empty JSONs,
    # five suites registering zero tests, and a green ctest that verifies nothing.
    # Every category maps to runners that have cases today, so an empty
    # bucket means the corpus or the tables above are wrong, not that the corpus
    # legitimately shrank.
    empty = [name for name in CATEGORIES if not buckets[name]]
    if empty:
        raise SystemExit(
            f"Error: no cases for {', '.join(empty)} in {len(inputs)} corpus file(s). An empty "
            f"category would register a suite with zero tests and still pass. Check the corpus, "
            f"or drop the category from CATEGORIES here, from SPLIT_CATEGORIES in "
            f"tests/CMakeLists.txt, and delete its test TU."
        )

    for name in CATEGORIES:
        # The basename comes from the fixed CATEGORIES tuple, never from input, but
        # resolve and confirm containment anyway so the write target is provably
        # inside the directory that was passed in.
        out = (output_dir / f"unified_cases_{name}.json").resolve()
        if out.parent != output_dir:
            raise SystemExit(f"Error: refusing to write outside {output_dir}: {out}")
        write_if_changed(out, json.dumps(buckets[name], separators=(",", ":")) + "\n")

    total = sum(len(b) for b in buckets.values())
    parts = ", ".join(f"{name}={len(buckets[name])}" for name in CATEGORIES)
    print(f"Split {total} cases from {len(inputs)} file(s) -> {parts}")


if __name__ == "__main__":
    main()
