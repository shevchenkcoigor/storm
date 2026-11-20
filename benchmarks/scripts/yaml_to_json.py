#!/usr/bin/env python3
"""
Convert benchmark_tests.yaml to benchmark_tests.json

Reads the nested-format YAML benchmark definitions and emits JSON that the
compile-time C++ parser (parser.hpp) consumes via #embed.

Usage:
    python yaml_to_json.py [input.yaml] [output.json]

If no arguments provided, uses default paths:
    Input:  benchmarks/tests/benchmark_tests.yaml
    Output: benchmarks/tests/benchmark_tests.json

Requires PyYAML.
"""

import json
import sys
from pathlib import Path

try:
    import yaml  # type: ignore
except ImportError:
    print("Error: PyYAML is required. Install with: pip install pyyaml", file=sys.stderr)
    sys.exit(1)


# Top-level keys from YAML to JSON (with 'test_' prefix where parser.hpp expects it)
TOP_LEVEL_RENAME = {
    "name": "test_name",
    "category": "test_category",
}

# Nested spec keys that are passed through as-is
NESTED_KEYS = {"where", "order_by", "group_by", "distinct", "limit", "aggregate", "join", "setop"}

# Scalar top-level keys that are passed through as-is
SCALAR_PASSTHROUGH = {
    "description",
    "model",
    "operation",
    "iterations",
    "init_dataset_size",
    "batch_size",
    "size_profile",
}


def transform_condition(c: dict) -> dict:
    """Transform a single WHERE condition: {field, op, value} → {field, op, values: [...]}."""
    out = {}
    for k, v in c.items():
        if k == "value":
            # Emit raw values — parse_typed_value sniffs type from JSON token
            out["values"] = list(v) if isinstance(v, list) else [v]
        else:
            out[k] = v
    return out


def transform_where(w: dict) -> dict:
    """Transform WHERE spec.

    Supports two formats:
    - Single condition: {field, op, value} → {conditions: [{field, op, values: [...]}]}
    - Multi-condition (AND/OR): {conditions: [...], combine: "and"/"or"}
    """
    if "conditions" in w:
        out = {"conditions": [transform_condition(c) for c in w["conditions"]]}
        if "combine" in w:  # absent means AND, as the C++ parser reads it
            out["combine"] = w["combine"]
        return out

    return {"conditions": [transform_condition(w)]}


def transform_group_by(g: dict) -> dict:
    out = {}
    for k, v in g.items():
        if k == "having":
            out[k] = transform_where(v)  # HAVING is the WHERE tree (#868)
        else:
            out[k] = v
    return out


def transform_test(test: dict) -> dict:
    """Transform one YAML test entry into the JSON shape parser.hpp expects."""
    out: dict = {}

    for yaml_key, json_key in TOP_LEVEL_RENAME.items():
        if yaml_key in test and test[yaml_key] is not None:
            out[json_key] = test[yaml_key]

    for key in SCALAR_PASSTHROUGH:
        if key in test and test[key] is not None:
            out[key] = test[key]

    if "where" in test and test["where"]:
        out["where"] = transform_where(test["where"])

    if "order_by" in test and test["order_by"]:
        out["order_by"] = list(test["order_by"])  # array of OrderBySpec dicts

    if "group_by" in test and test["group_by"]:
        out["group_by"] = transform_group_by(test["group_by"])

    if "distinct" in test and test["distinct"]:
        out["distinct"] = test["distinct"]

    if "limit" in test and test["limit"]:
        out["limit"] = test["limit"]

    if "aggregate" in test and test["aggregate"]:
        out["aggregate"] = test["aggregate"]

    if "join" in test and test["join"]:
        out["join"] = test["join"]

    if "setop" in test and test["setop"]:
        out["setop"] = test["setop"]

    return out


def convert_yaml_to_json(yaml_path: Path, json_path: Path) -> None:
    with open(yaml_path) as f:
        data = yaml.safe_load(f)

    if not isinstance(data, dict) or "tests" not in data:
        print("Error: YAML file must have a top-level 'tests' key", file=sys.stderr)
        sys.exit(1)

    tests = data["tests"]
    if tests is None:
        print("Error: 'tests' is None — parsing failed", file=sys.stderr)
        sys.exit(1)

    json_tests = [transform_test(t) for t in tests if isinstance(t, dict)]

    with open(json_path, "w") as f:
        json.dump(json_tests, f, indent=2)
        f.write("\n")

    print(f"Converted {len(json_tests)} tests: {yaml_path} -> {json_path}")


def main():
    script_dir = Path(__file__).parent
    benchmarks_dir = script_dir.parent

    if len(sys.argv) >= 3:
        yaml_path = Path(sys.argv[1])
        json_path = Path(sys.argv[2])
    elif len(sys.argv) == 2:
        yaml_path = Path(sys.argv[1])
        json_path = yaml_path.with_suffix(".json")
    else:
        yaml_path = benchmarks_dir / "tests" / "benchmark_tests.yaml"
        json_path = benchmarks_dir / "tests" / "benchmark_tests.json"

    if not yaml_path.exists():
        print(f"Error: YAML file not found: {yaml_path}", file=sys.stderr)
        sys.exit(1)

    convert_yaml_to_json(yaml_path, json_path)


if __name__ == "__main__":
    main()
