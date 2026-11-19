#!/usr/bin/env python3
"""Tests for the expected_sql validation in scripts/split_unified_cases.py (#818, #844).

A case may pin the SQL text it emits: one `expected_sql` for both backends, or BOTH
`expected_sql_sqlite` and `expected_sql_pg` when the text differs per dialect
(NULLS FIRST/LAST, LIMIT ALL vs LIMIT -1). The script is the one place that sees the
YAML before the C++ parser (which cannot report an overflow or a stray key), so it
must reject what the parser would silently mangle or never compare. Strategy: run the
real script over a throwaway corpus (one valid case per category, plus the case under
test) and assert the exit status and message; a last scenario runs it over the real
corpus. Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/ci/tests/test_split_unified_cases.py
"""
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

try:
    import yaml  # noqa: F401  (the splitter under test imports it)
except ImportError:
    sys.exit("PyYAML is required by split_unified_cases.py and is missing from " + sys.executable +
             "; install it (pip install pyyaml) or put an interpreter that has it first on PATH")

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
SPLITTER = REPO_ROOT / "scripts" / "split_unified_cases.py"
REAL_CORPUS = REPO_ROOT / "tests" / "test_cases" / "unified_cases.yaml"

# One case per category, so no category is empty (an empty one is itself an error).
BASE_CASES = [
    {"name": "base_select", "query_type": "select"},
    {"name": "base_grouped", "query_type": "group_count"},
    {"name": "base_aggregate", "query_type": "count"},
    {"name": "base_crud", "query_type": "insert_one"},
]

SELECT_QUERY = {"query_type": "select"}
AGGREGATE_QUERY = {"query_type": "count"}
GROUPED_QUERY = {"query_type": "group_count"}
CRUD_QUERY = {"query_type": "insert_one"}

PAIR = {
    "expected_sql_sqlite": 'SELECT "id" FROM "t" LIMIT -1 OFFSET 2',
    "expected_sql_pg": 'SELECT "id" FROM "t" LIMIT ALL OFFSET 2',
}
PARSER_HEADER = REPO_ROOT / "tests" / "test_parser.hpp"


def capacity_of_the_script():
    """EXPECTED_SQL_CAPACITY as the script states it, so a raised limit moves this test with it."""
    text = SPLITTER.read_text(encoding="utf-8")
    match = re.search(r"^EXPECTED_SQL_CAPACITY\s*=\s*(\d+)", text, re.MULTILINE)
    assert match, "EXPECTED_SQL_CAPACITY not found in " + str(SPLITTER)
    return int(match.group(1))


CAPACITY = capacity_of_the_script()


class Run:
    def __init__(self, completed, outputs):
        self.returncode = completed.returncode
        self.stderr = completed.stderr
        self.stdout = completed.stdout
        self.outputs = outputs


def run_splitter(case):
    """Split BASE_CASES + `case`; returns the exit status, streams and the written JSON per category."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        corpus = root / "corpus.yaml"
        out_dir = root / "out"
        out_dir.mkdir()
        corpus.write_text(json.dumps(BASE_CASES + [case]), encoding="utf-8")  # JSON is YAML
        completed = subprocess.run(
            [sys.executable, str(SPLITTER), str(corpus), str(out_dir)],
            capture_output=True,
            text=True,
            check=False,
        )
        outputs = {}
        for path in out_dir.glob("unified_cases_*.json"):
            outputs[path.stem[len("unified_cases_"):]] = json.loads(path.read_text(encoding="utf-8"))
        return Run(completed, outputs)


def case(name, query, **keys):
    out = {"name": name}
    out.update(query)
    out.update(keys)
    return out


class SharedKeyTest(unittest.TestCase):
    def test_shared_expected_sql_is_accepted_on_aggregate(self):
        run = run_splitter(case("c", AGGREGATE_QUERY, expected_sql='SELECT COUNT(*) FROM "t"'))
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_shared_expected_sql_is_accepted_on_select(self):
        # SelectRunner pins SQL too (#844), so select cases can state ORDER BY / LIMIT text.
        run = run_splitter(case("c", SELECT_QUERY, expected_sql='SELECT "id" FROM "t"'))
        self.assertEqual(run.returncode, 0, run.stderr)


class DialectPairTest(unittest.TestCase):
    def test_pair_is_accepted_on_select_and_reaches_the_json(self):
        run = run_splitter(case("pair_select", SELECT_QUERY, **PAIR))
        self.assertEqual(run.returncode, 0, run.stderr)
        written = [c for c in run.outputs["select"] if c["name"] == "pair_select"]
        self.assertEqual(len(written), 1)
        self.assertEqual(written[0]["expected_sql_sqlite"], PAIR["expected_sql_sqlite"])
        self.assertEqual(written[0]["expected_sql_pg"], PAIR["expected_sql_pg"])

    def test_pair_is_accepted_on_aggregate(self):
        run = run_splitter(case("pair_aggregate", AGGREGATE_QUERY, **PAIR))
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_pair_with_identical_text_is_allowed(self):
        same = 'SELECT "id" FROM "t" LIMIT 5 OFFSET 2'
        run = run_splitter(
            case("same", SELECT_QUERY, expected_sql_sqlite=same, expected_sql_pg=same)
        )
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_shared_key_together_with_the_pair_is_rejected(self):
        run = run_splitter(case("mixed", SELECT_QUERY, expected_sql="SELECT 1", **PAIR))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("mixed", run.stderr)
        self.assertIn("expected_sql", run.stderr)

    def test_shared_key_with_one_dialect_key_is_rejected(self):
        run = run_splitter(
            case("mixed_one", SELECT_QUERY, expected_sql="SELECT 1", expected_sql_pg="SELECT 2")
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("mixed_one", run.stderr)

    def test_sqlite_key_alone_is_rejected(self):
        # The other backend would run with no SQL check at all.
        run = run_splitter(
            case("only_sqlite", SELECT_QUERY, expected_sql_sqlite=PAIR["expected_sql_sqlite"])
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("only_sqlite", run.stderr)
        self.assertIn("expected_sql_pg", run.stderr)

    def test_pg_key_alone_is_rejected(self):
        run = run_splitter(case("only_pg", SELECT_QUERY, expected_sql_pg=PAIR["expected_sql_pg"]))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("only_pg", run.stderr)
        self.assertIn("expected_sql_sqlite", run.stderr)


class UnsupportedCategoryTest(unittest.TestCase):
    """A category whose runner never reads the key would pass without comparing any SQL."""

    def test_every_form_is_rejected_where_no_runner_reads_it(self):
        forms = {
            "shared": {"expected_sql": "SELECT 1"},
            "pair": PAIR,
        }
        for category, query in (("grouped", GROUPED_QUERY), ("crud", CRUD_QUERY)):
            for form, keys in forms.items():
                with self.subTest(category=category, form=form):
                    run = run_splitter(case("unsupported", query, **keys))
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn("unsupported", run.stderr)
                    self.assertIn(category, run.stderr)

    def test_first_and_one_are_rejected_in_the_select_category(self):
        # They share the `select` category with `select` but SelectRunner pins SQL only on
        # `select`; the C++ static_assert would catch it later, this fails before the build.
        for query_type in ("first", "one"):
            with self.subTest(query_type=query_type):
                run = run_splitter(case("pinned", {"query_type": query_type}, expected_sql="SELECT 1"))
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(query_type, run.stderr)

    def test_a_lone_dialect_key_in_an_unsupported_category_is_still_rejected(self):
        run = run_splitter(case("lone", CRUD_QUERY, expected_sql_pg="SELECT 2"))
        self.assertNotEqual(run.returncode, 0)


class TextRulesTest(unittest.TestCase):
    """The C++ parser unescapes only a quote and a backslash, ignores string boundaries when
    skipping braces, and stops copying at capacity -- so every key is held to the same rules
    as expected_sql."""

    KEYS = ("expected_sql_sqlite", "expected_sql_pg")

    def _pair_with(self, key, text):
        keys = dict(PAIR)
        keys[key] = text
        return case("text_rules", SELECT_QUERY, **keys)

    def test_each_dialect_key_rejects_what_the_parser_would_mangle(self):
        bad = {
            "empty": "",
            "non-string": 7,
            "non-ascii": "SELECT 'é'",
            "newline": "SELECT 1\nFROM t",
            "brace": "SELECT {1}",
            "bracket": "SELECT [1]",
            "over capacity": "S" * CAPACITY,
        }
        for key in self.KEYS:
            for label, text in bad.items():
                with self.subTest(key=key, text=label):
                    run = run_splitter(self._pair_with(key, text))
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn(key, run.stderr)

    def test_each_dialect_key_accepts_text_one_under_capacity(self):
        for key in self.KEYS:
            with self.subTest(key=key):
                run = run_splitter(self._pair_with(key, "S" * (CAPACITY - 1)))
                self.assertEqual(run.returncode, 0, run.stderr)

    def test_quoted_identifiers_survive_the_json_hop(self):
        run = run_splitter(case("quoted", SELECT_QUERY, **PAIR))
        written = [c for c in run.outputs["select"] if c["name"] == "quoted"][0]
        self.assertIn('"id"', written["expected_sql_pg"])


class CapacityTest(unittest.TestCase):
    def test_the_script_and_the_cpp_parser_agree_on_the_capacity(self):
        # The C++ parser stops copying at capacity and cannot report an overflow, so the script's
        # limit must be exactly the buffer the three keys are parsed into.
        header = PARSER_HEADER.read_text(encoding="utf-8")
        match = re.search(r"EXPECTED_SQL_CAPACITY\s*=\s*(\d+)\s*;", header)
        self.assertIsNotNone(match, "EXPECTED_SQL_CAPACITY not found in tests/test_parser.hpp")
        self.assertEqual(int(match.group(1)), CAPACITY)

    def test_every_pinned_sql_key_is_parsed_into_that_buffer(self):
        header = PARSER_HEADER.read_text(encoding="utf-8")
        for key in ("expected_sql", "expected_sql_sqlite", "expected_sql_pg"):
            with self.subTest(key=key):
                self.assertRegex(header, r"ConstexprString<EXPECTED_SQL_CAPACITY>\s+" + key + r"\s*;")


class RealCorpusTest(unittest.TestCase):
    def test_the_real_corpus_splits_and_pins_per_dialect_sql(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp)
            completed = subprocess.run(
                [sys.executable, str(SPLITTER), str(REAL_CORPUS), str(out_dir)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            select = json.loads((out_dir / "unified_cases_select.json").read_text(encoding="utf-8"))
            pairs = [c for c in select if "expected_sql_pg" in c]
            self.assertTrue(
                any("NULLS FIRST" in c["expected_sql_pg"] for c in pairs),
                "one case must pin NULLS FIRST/LAST per dialect",
            )
            self.assertTrue(
                any("LIMIT ALL" in c["expected_sql_pg"] and "LIMIT -1" in c["expected_sql_sqlite"] for c in pairs),
                "one case must pin LIMIT ALL / LIMIT -1 per dialect",
            )


if __name__ == "__main__":
    unittest.main()
