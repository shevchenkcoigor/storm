#!/usr/bin/env python3
"""Tests for the expected_sql validation in scripts/split_unified_cases.py (#818, #844).

A case may pin the SQL text it emits: one `expected_sql` for both backends, or BOTH
`expected_sql_sqlite` and `expected_sql_pg` when the text differs per dialect
(NULLS FIRST/LAST, LIMIT ALL vs LIMIT -1). The script is the one place that sees the
YAML before the C++ parser. The parser cannot report a stray key; it does reject an
over-long string at compile time, but the script keeps that check so the error names the
case and the key. So the script must reject what the parser would mangle or never compare. Strategy: run the
real script over a throwaway corpus (one valid case per category, plus the case under
test) and assert the exit status and message; a last scenario runs it over the real
corpus. Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/ci/tests/test_split_unified_cases.py
"""
import json
import os
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
CORPUS_DIR = REPO_ROOT / "tests" / "test_cases"

# One case per category, so no category is empty (an empty one is itself an error).
BASE_CASES = [
    {"name": "base_select", "query_type": "select"},
    {"name": "base_grouped", "query_type": "group_count"},
    {"name": "base_aggregate", "query_type": "count"},
    {"name": "base_crud", "query_type": "insert_one"},
    {"name": "base_join", "query_type": "select", "dataset": "relations"},
]

SELECT_QUERY = {"query_type": "select"}
AGGREGATE_QUERY = {"query_type": "count"}
GROUPED_QUERY = {"query_type": "group_count"}
CRUD_QUERY = {"query_type": "insert_one"}
RELATIONS = {"dataset": "relations"}

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
            [sys.executable, str(SPLITTER), str(out_dir), str(corpus)],
            capture_output=True,
            text=True,
            check=False,
        )
        outputs = {}
        for path in out_dir.glob("unified_cases_*.json"):
            outputs[path.stem[len("unified_cases_"):]] = json.loads(path.read_text(encoding="utf-8"))
        return Run(completed, outputs)


def run_on_files(files, extra_args=()):
    """Split several authoring files (name -> list of cases) in the given order; returns
    (completed, out_dir path string, outputs per category, mtimes kept for a rerun)."""
    tmp = tempfile.TemporaryDirectory()
    root = pathlib.Path(tmp.name)
    out_dir = root / "out"
    out_dir.mkdir()
    paths = []
    for name, cases in files.items():
        path = root / name
        path.write_text(json.dumps(cases), encoding="utf-8")  # JSON is YAML
        paths.append(str(path))
    completed = subprocess.run(
        [sys.executable, str(SPLITTER), str(out_dir), *paths, *extra_args],
        capture_output=True,
        text=True,
        check=False,
    )
    return tmp, out_dir, paths, completed


def read_slices(out_dir):
    return {
        p.stem[len("unified_cases_"):]: json.loads(p.read_text(encoding="utf-8"))
        for p in out_dir.glob("unified_cases_*.json")
    }


def case(name, query, **keys):
    out = {"name": name}
    out.update(query)
    out.update(keys)
    return out


class AggregateShapeTest(unittest.TestCase):
    """#868: a GROUP BY + aggregate chain is a `chain` case, and multi-field aggregates keep their list."""

    def test_a_grouped_chain_is_bucketed_grouped_with_its_steps_intact(self):
        steps = [{"func": "count", "result": {"groups": [1, 2]}}, {"func": "sum", "fields": ["a", "b"]}]
        run = run_splitter(case("grouped_chain", {"query_type": "chain"}, group_by={"fields": ["k"]},
                                aggregations=steps))
        self.assertEqual(run.returncode, 0, run.stderr)
        written = [c for c in run.outputs["grouped"] if c["name"] == "grouped_chain"]
        self.assertEqual(written[0]["aggregations"], steps)

    def test_a_multi_field_scalar_aggregate_is_bucketed_aggregate(self):
        aggregate = {"func": "avg", "fields": ["a", "b"]}
        run = run_splitter(case("avg_two", {"query_type": "avg"}, aggregate=aggregate,
                                expected_sql='SELECT AVG("a" + "b") FROM "t"'))
        self.assertEqual(run.returncode, 0, run.stderr)
        written = [c for c in run.outputs["aggregate"] if c["name"] == "avg_two"]
        self.assertEqual(written[0]["aggregate"], aggregate)


class SharedKeyTest(unittest.TestCase):
    def test_shared_expected_sql_is_accepted_on_aggregate(self):
        run = run_splitter(case("c", AGGREGATE_QUERY, expected_sql='SELECT COUNT(*) FROM "t"'))
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_shared_expected_sql_is_accepted_on_values(self):
        # ValuesRunner pins SQL too (#867).
        run = run_splitter(case("c", {"query_type": "values"}, expected_sql='SELECT "id" FROM "t"'))
        self.assertEqual(run.returncode, 0, run.stderr)

    def test_shared_expected_sql_is_accepted_on_setop(self):
        # SetOpRunner pins SQL too (#870); a set-op case lands in the select slice.
        run = run_splitter(case("c", {"query_type": "setop"}, expected_sql='SELECT "id" FROM "t" UNION SELECT "id" FROM "t"'))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual([entry["name"] for entry in run.outputs["select"]].count("c"), 1)

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

    def test_pair_is_accepted_on_setop_and_reaches_the_json(self):
        run = run_splitter(case("pair_setop", {"query_type": "setop"}, **PAIR))
        self.assertEqual(run.returncode, 0, run.stderr)
        written = [entry for entry in run.outputs["select"] if entry["name"] == "pair_setop"]
        self.assertEqual(len(written), 1)
        self.assertEqual(written[0]["expected_sql_pg"], PAIR["expected_sql_pg"])

    def test_pair_is_accepted_on_values_and_reaches_the_json(self):
        run = run_splitter(case("pair_values", {"query_type": "values"}, **PAIR))
        self.assertEqual(run.returncode, 0, run.stderr)
        written = [c for c in run.outputs["select"] if c["name"] == "pair_values"]
        self.assertEqual(len(written), 1)
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

    def test_first_one_and_rows_are_rejected_in_the_select_category(self):
        # They share the `select` category with `select` and `values`, but only those two pin SQL
        # (SelectRunner on `select`, ValuesRunner on `values`, #867); the C++ static_assert would
        # catch it later, this fails before the build.
        for query_type in ("first", "one", "rows"):
            with self.subTest(query_type=query_type):
                run = run_splitter(case("pinned", {"query_type": query_type}, expected_sql="SELECT 1"))
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(query_type, run.stderr)

    def test_a_lone_dialect_key_in_an_unsupported_category_is_still_rejected(self):
        run = run_splitter(case("lone", CRUD_QUERY, expected_sql_pg="SELECT 2"))
        self.assertNotEqual(run.returncode, 0)


class TextRulesTest(unittest.TestCase):
    """The C++ parser unescapes only a quote and a backslash and holds a fixed capacity -- so
    every key is held to the same rules as expected_sql."""

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

    def test_braces_and_brackets_are_accepted(self):
        # #845: the C++ parser skips a string whole when it skips a braced value, so a brace or
        # bracket inside the text no longer miscounts the nesting.
        for key in self.KEYS:
            with self.subTest(key=key):
                run = run_splitter(self._pair_with(key, "SELECT '{1}', '[2]'"))
                self.assertEqual(run.returncode, 0, run.stderr)
        with self.subTest(key="expected_sql"):
            run = run_splitter(case("text_rules", SELECT_QUERY, expected_sql="SELECT '{1}', '[2]'"))
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_quoted_identifiers_survive_the_json_hop(self):
        run = run_splitter(case("quoted", SELECT_QUERY, **PAIR))
        written = [c for c in run.outputs["select"] if c["name"] == "quoted"][0]
        self.assertIn('"id"', written["expected_sql_pg"])


class CapacityTest(unittest.TestCase):
    def test_the_script_and_the_cpp_parser_agree_on_the_capacity(self):
        # The C++ parser rejects an over-long string at compile time; the script checks first so the
        # error names the case and the key, so its limit must be exactly the buffer the keys are parsed into.
        header = PARSER_HEADER.read_text(encoding="utf-8")
        match = re.search(r"EXPECTED_SQL_CAPACITY\s*=\s*(\d+)\s*;", header)
        self.assertIsNotNone(match, "EXPECTED_SQL_CAPACITY not found in tests/test_parser.hpp")
        self.assertEqual(int(match.group(1)), CAPACITY)

    def test_every_pinned_sql_key_is_parsed_into_that_buffer(self):
        header = PARSER_HEADER.read_text(encoding="utf-8")
        for key in ("expected_sql", "expected_sql_sqlite", "expected_sql_pg"):
            with self.subTest(key=key):
                self.assertRegex(header, r"ConstexprString<EXPECTED_SQL_CAPACITY>\s+" + key + r"\s*;")


class MultiFileTest(unittest.TestCase):
    """#846: the corpus is several topic files; the slices are still bucketed by query_type."""

    def test_cases_of_every_file_are_bucketed_by_query_type_in_argument_order(self):
        tmp, out_dir, _, completed = run_on_files(
            {
                "a.yaml": [case("a_select", SELECT_QUERY), case("a_count", AGGREGATE_QUERY)],
                "b.yaml": [case("b_select", SELECT_QUERY), case("b_insert", CRUD_QUERY),
                           case("b_group", GROUPED_QUERY), case("b_join", SELECT_QUERY, **RELATIONS)],
            }
        )
        with tmp:
            self.assertEqual(completed.returncode, 0, completed.stderr)
            slices = read_slices(out_dir)
            names = {k: [c["name"] for c in v] for k, v in slices.items()}
            self.assertEqual(names["select"], ["a_select", "b_select"])
            self.assertEqual(names["aggregate"], ["a_count"])
            self.assertEqual(names["crud"], ["b_insert"])
            self.assertEqual(names["grouped"], ["b_group"])
            self.assertEqual(names["join"], ["b_join"])

    def test_a_topic_file_may_feed_one_slice_only(self):
        tmp, _, _, completed = run_on_files(
            {
                "all.yaml": BASE_CASES,
                "selects_only.yaml": [case("only_select", SELECT_QUERY)],
            }
        )
        with tmp:
            self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_a_duplicate_name_across_files_is_a_hard_error_naming_both_files(self):
        tmp, _, _, completed = run_on_files(
            {
                "one.yaml": BASE_CASES + [case("dup", SELECT_QUERY)],
                "two.yaml": [case("dup", AGGREGATE_QUERY)],
            }
        )
        with tmp:
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("duplicate", completed.stderr.lower())
            self.assertIn("'dup'", completed.stderr)
            self.assertIn("one.yaml", completed.stderr)
            self.assertIn("two.yaml", completed.stderr)

    def test_a_duplicate_name_inside_one_file_is_a_hard_error(self):
        tmp, _, _, completed = run_on_files(
            {"one.yaml": BASE_CASES + [case("dup", SELECT_QUERY), case("dup", SELECT_QUERY)]}
        )
        with tmp:
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("duplicate", completed.stderr.lower())

    def test_an_unknown_query_type_in_a_later_file_is_a_hard_error_naming_the_file(self):
        tmp, _, _, completed = run_on_files(
            {
                "ok.yaml": BASE_CASES,
                "bad.yaml": [case("weird", {"query_type": "no_such_op"})],
            }
        )
        with tmp:
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("unknown query_type", completed.stderr)
            self.assertIn("bad.yaml", completed.stderr)

    def test_a_non_list_file_among_several_is_rejected_naming_it(self):
        tmp, _, _, completed = run_on_files({"ok.yaml": BASE_CASES, "map.yaml": {"name": "x"}})
        with tmp:
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("map.yaml", completed.stderr)

    def test_an_all_comments_file_contributes_nothing_and_is_not_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            out_dir = root / "out"
            out_dir.mkdir()
            good = root / "good.yaml"
            good.write_text(json.dumps(BASE_CASES), encoding="utf-8")
            empty = root / "empty.yaml"
            empty.write_text("# nothing here yet\n", encoding="utf-8")
            completed = subprocess.run(
                [sys.executable, str(SPLITTER), str(out_dir), str(good), str(empty)],
                capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_a_case_without_a_name_is_rejected_as_such(self):
        tmp, _, _, completed = run_on_files(
            {"a.yaml": BASE_CASES + [{"query_type": "select"}, {"query_type": "select"}]})
        with tmp:
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("name", completed.stderr)
            self.assertNotIn("duplicate", completed.stderr.lower())

    def test_no_input_file_is_a_usage_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            completed = subprocess.run(
                [sys.executable, str(SPLITTER), tmp], capture_output=True, text=True, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("Usage", completed.stderr)

    def test_a_missing_input_file_is_a_hard_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            completed = subprocess.run(
                [sys.executable, str(SPLITTER), tmp, str(pathlib.Path(tmp) / "nope.yaml")],
                capture_output=True, text=True, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("nope.yaml", completed.stderr)


class UnquotedTemporalScalarTest(unittest.TestCase):
    """PyYAML reads an unquoted 2024-01-15 as a date; json.dumps then dies with an unrelated TypeError
    (or, for a time point, would emit a value the C++ parser never sees as text). The corpus quotes them
    ('2024-01-15'), so the script says which case and which key forgot to (#853)."""

    def split_text(self, yaml_text):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            out_dir = root / "out"
            out_dir.mkdir()
            corpus = root / "corpus.yaml"
            corpus.write_text(yaml_text, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(SPLITTER), str(out_dir), str(corpus)],
                capture_output=True, text=True, check=False)

    BASE = (
        "- {name: base_grouped, query_type: group_count}\n"
        "- {name: base_aggregate, query_type: count}\n"
        "- {name: base_crud, query_type: insert_one}\n"
        "- {name: base_join, query_type: select, dataset: relations}\n"
    )

    def test_an_unquoted_date_is_rejected_naming_the_case_and_the_key(self):
        completed = self.split_text(
            self.BASE + "- name: d\n  query_type: select\n  where:\n    conditions:\n"
            "    - field: date_field\n      op: ==\n      values:\n      - 2024-01-15\n")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("'d'", completed.stderr)
        self.assertIn("values", completed.stderr)
        self.assertIn("quote", completed.stderr)
        self.assertNotIn("Traceback", completed.stderr)

    def test_an_unquoted_expected_first_date_is_rejected(self):
        completed = self.split_text(
            self.BASE + "- name: e\n  query_type: select\n  expected:\n    first:\n"
            "      date_field: 2024-01-15\n")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("date_field", completed.stderr)

    def test_an_unquoted_datetime_is_rejected(self):
        completed = self.split_text(
            self.BASE + "- name: t\n  query_type: select\n  expected:\n    first:\n"
            "      datetime_field: 2024-01-15T10:30:00\n")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("datetime_field", completed.stderr)

    def test_an_unquoted_date_used_as_a_key_is_rejected(self):
        completed = self.split_text(
            self.BASE + "- name: k\n  query_type: select\n  expected:\n    first:\n"
            "      2024-01-15: x\n")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("'k'", completed.stderr)
        self.assertIn("quote", completed.stderr)
        self.assertNotIn("Traceback", completed.stderr)

    def test_a_quoted_date_is_accepted(self):
        completed = self.split_text(
            self.BASE + "- name: q\n  query_type: select\n  expected:\n    first:\n"
            "      date_field: '2024-01-15'\n")
        self.assertEqual(completed.returncode, 0, completed.stderr)


class RewriteOnlyWhenChangedTest(unittest.TestCase):
    """#846: ninja runs the splitter whenever ANY yaml changes (restat). A JSON whose bytes did
    not change must keep its mtime, so only the slices an edit really touched recompile."""

    def test_only_the_slice_whose_content_changed_is_rewritten(self):
        files = {"a.yaml": BASE_CASES}
        tmp, out_dir, paths, completed = run_on_files(files)
        with tmp:
            self.assertEqual(completed.returncode, 0, completed.stderr)
            stale = 1_000_000_000  # an old, recognisable mtime
            for p in out_dir.glob("unified_cases_*.json"):
                os.utime(p, (stale, stale))
            extra = BASE_CASES + [case("new_select", SELECT_QUERY)]
            pathlib.Path(paths[0]).write_text(json.dumps(extra), encoding="utf-8")
            rerun = subprocess.run(
                [sys.executable, str(SPLITTER), str(out_dir), *paths],
                capture_output=True, text=True, check=False)
            self.assertEqual(rerun.returncode, 0, rerun.stderr)
            mtimes = {p.stem[len("unified_cases_"):]: int(p.stat().st_mtime)
                      for p in out_dir.glob("unified_cases_*.json")}
            self.assertGreater(mtimes["select"], stale)
            for untouched in ("grouped", "aggregate", "crud", "join"):
                self.assertEqual(mtimes[untouched], stale, untouched + " was rewritten unchanged")


class JoinSliceTest(unittest.TestCase):
    """#869: a case on dataset `relations` goes to the `join` slice whatever its query_type; the SQL it
    may pin is still its runner's rule."""

    def test_every_query_type_on_the_relations_dataset_is_bucketed_join(self):
        for query in (SELECT_QUERY, AGGREGATE_QUERY, GROUPED_QUERY, CRUD_QUERY):
            run = run_splitter(case("rel_case", query, **RELATIONS))
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual([c["name"] for c in run.outputs["join"]], ["base_join", "rel_case"], query)
            for other in ("select", "grouped", "aggregate", "crud"):
                self.assertNotIn("rel_case", [c["name"] for c in run.outputs[other]], other)

    def test_another_dataset_keeps_the_query_type_slice(self):
        run = run_splitter(case("main_select", SELECT_QUERY, dataset="main"))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("main_select", [c["name"] for c in run.outputs["select"]])

    def test_pinned_sql_follows_the_runner_of_a_join_case(self):
        sql = {"expected_sql": 'SELECT COUNT(*) FROM "t"'}
        run = run_splitter(case("rel_count", AGGREGATE_QUERY, **RELATIONS, **sql))
        self.assertEqual(run.returncode, 0, run.stderr)
        pinned = [c for c in run.outputs["join"] if c["name"] == "rel_count"]
        self.assertEqual(len(pinned), 1)
        self.assertEqual(pinned[0]["expected_sql"], sql["expected_sql"])
        run = run_splitter(case("rel_first", {"query_type": "first"}, **RELATIONS, **sql))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("rel_first", run.stderr)
        run = run_splitter(case("rel_group", GROUPED_QUERY, **RELATIONS, **sql))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("not supported", run.stderr)
        self.assertIn("(grouped)", run.stderr)

    def test_an_empty_join_slice_is_an_error(self):
        no_relations = [c for c in BASE_CASES if c.get("dataset") != "relations"]
        tmp, _, _, completed = run_on_files({"a.yaml": no_relations})
        with tmp:
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("no cases for join", completed.stderr)


class RealCorpusTest(unittest.TestCase):
    def test_the_real_corpus_is_split_into_topic_files_not_one_big_file(self):
        self.assertFalse((CORPUS_DIR / "unified_cases.yaml").exists(),
                         "unified_cases.yaml was replaced by topic files (#846)")
        self.assertGreater(len(sorted(CORPUS_DIR.glob("*.yaml"))), 1)

    def test_every_hand_written_file_in_the_corpus_dir_is_a_yaml_the_glob_sees(self):
        # CMake globs *.yaml; a foo.yml or a subdirectory would be skipped silently and its
        # cases would never run, with a green build.
        stray = [p.name for p in CORPUS_DIR.iterdir() if p.suffix not in (".yaml", ".json")]
        self.assertEqual(stray, [], "only *.yaml topic files (and generated *.json) belong here")

    def test_the_real_corpus_splits_and_pins_per_dialect_sql(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp)
            completed = subprocess.run(
                [sys.executable, str(SPLITTER), str(out_dir),
                 *map(str, sorted(CORPUS_DIR.glob("*.yaml")))],
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
