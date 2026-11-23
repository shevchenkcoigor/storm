#!/usr/bin/env python3
"""Tests for benchmarks/scripts/yaml_to_json.py (#868).

The converter turns benchmarks/tests/benchmark_tests.yaml into the JSON the compile-time parser
#embeds. A key it does not pass through used to be dropped silently: the benchmark then ran
without it (an `aggregations` list lost this way ran a plain SELECT, and `values` was lost from
#867 to #868). It now rejects any key it does not know. Strategy: run the real script over a
throwaway YAML file and assert the exit status, the message and the JSON written; a last
scenario converts the real benchmark corpus. Needs PyYAML, like the converter. Stdlib unittest
only, no syntax newer than Python 3.9.

    python3 scripts/ci/tests/test_yaml_to_json.py
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

try:
    import yaml  # noqa: F401  (the converter under test imports it)
except ImportError:
    sys.exit("PyYAML is required by yaml_to_json.py and is missing from " + sys.executable +
             "; install it (pip install pyyaml) or put an interpreter that has it first on PATH")

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
CONVERTER = REPO_ROOT / "benchmarks" / "scripts" / "yaml_to_json.py"
CORPUS = REPO_ROOT / "benchmarks" / "tests" / "benchmark_tests.yaml"


class Run:
    def __init__(self, completed, written):
        self.returncode = completed.returncode
        self.stderr = completed.stderr
        self.written = written  # the parsed JSON, or None when no file was written


def convert(source_path):
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "out.json"
        completed = subprocess.run([sys.executable, str(CONVERTER), str(source_path), str(out)],
                                   capture_output=True, text=True, check=False)
        written = json.loads(out.read_text(encoding="utf-8")) if out.exists() else None
        return Run(completed, written)


def convert_tests(tests):
    with tempfile.TemporaryDirectory() as tmp:
        source = pathlib.Path(tmp) / "in.yaml"
        source.write_text(json.dumps({"tests": tests}), encoding="utf-8")  # JSON is YAML
        return convert(source)


def bench(**keys):
    out = {"name": "case", "category": "AGGREGATE", "model": "Person"}
    out.update(keys)
    return out


class UnknownKeyTest(unittest.TestCase):
    def test_a_misspelled_key_exits_1_and_writes_nothing(self):
        run = convert_tests([bench(agregate={"func": "sum", "field": "age"})])
        self.assertEqual(run.returncode, 1)
        self.assertIn("unknown key(s) agregate", run.stderr)
        self.assertIn("'case'", run.stderr)
        self.assertIsNone(run.written)


class PassThroughTest(unittest.TestCase):
    def test_aggregations_pass_through(self):
        steps = [{"func": "count"}, {"func": "avg", "fields": ["age", "salary"]}]
        run = convert_tests([bench(group_by={"fields": ["age"]}, aggregations=steps)])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.written[0]["aggregations"], steps)

    def test_values_pass_through(self):
        run = convert_tests([bench(values={"fields": ["name", "age"]})])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.written[0]["values"], {"fields": ["name", "age"]})

    def test_the_renamed_keys_still_get_their_prefix(self):
        run = convert_tests([bench(aggregate={"func": "max", "field": "salary"})])
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.written[0]["test_name"], "case")
        self.assertEqual(run.written[0]["aggregate"], {"func": "max", "field": "salary"})


class RealCorpusTest(unittest.TestCase):
    def test_the_benchmark_corpus_converts(self):
        run = convert(CORPUS)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertGreater(len(run.written), 0)
        by_name = {t["test_name"]: t for t in run.written}
        # A `#` inside an unquoted description would be cut off as a YAML comment.
        self.assertTrue(by_name["aggregate_avg_two_fields"]["description"].endswith("#868)"))


if __name__ == "__main__":
    unittest.main()
