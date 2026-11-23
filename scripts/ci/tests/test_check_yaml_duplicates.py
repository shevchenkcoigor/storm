#!/usr/bin/env python3
"""Tests for scripts/ci/check_yaml_duplicates.py.

The checker fails when two cases of the YAML test corpus (tests/test_cases/*.yaml) have the
same INPUT, wherever they live: the corpus is split by topic, so the same query is easy to
add twice under two names in two files. The input is the case minus its name, description and
`expected*` keys, normalised so that spelling does not matter (key order, condition order,
`where` vs `where_expr`). Strategy: build a throwaway corpus per scenario and assert the exit
code and message; a last scenario runs the checker over the real corpus, which must pass.
Needs PyYAML, like the checker.

    python3 scripts/ci/tests/test_check_yaml_duplicates.py
"""
import pathlib
import subprocess
import sys
import unittest

import yaml

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fake_repo import FakeRepoBase  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
CHECKER = REPO_ROOT / "scripts" / "ci" / "check_yaml_duplicates.py"

CLEAN_MESSAGE = "no duplicate test inputs"


def leaf(field, op, *values):
    return {"field": field, "op": op, "values": list(values)}


def where(*conditions, combine=None):
    block = {"conditions": list(conditions)}
    if combine is not None:
        block["combine"] = combine
    return block


def expr_leaf(field, op, **value):
    return {"field": field, "op": op, **value}


def expr(op, left, right):
    return {"op": op, "left": left, "right": right}


def case(name, **fields):
    base = {"name": name, "model": "person", "query_type": "select", "dataset": "main"}
    base.update(fields)
    base.setdefault("expected", {"count": 1})
    return base


class CheckerTest(FakeRepoBase):
    CHECKER = CHECKER

    def corpus(self, **files):
        for filename, cases in files.items():
            self.write("tests/test_cases/" + filename + ".yaml", yaml.safe_dump(cases, sort_keys=False))

    def assert_passes(self):
        proc = self.run_checker()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(CLEAN_MESSAGE, proc.stdout)

    def assert_duplicates(self, *members):
        proc = self.assert_fails("duplicate test input")
        for member in members:
            self.assertIn(member, proc.stderr)

    # --- what counts as the same input ---

    def test_distinct_inputs_pass(self):
        self.corpus(a=[case("one", where=where(leaf("age", ">", 30))),
                       case("two", where=where(leaf("age", ">", 50)))])
        self.assert_passes()

    def test_same_input_under_two_names_in_one_file_fails(self):
        self.corpus(a=[case("one", limit={"value": 2}), case("two", limit={"value": 2})])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_same_input_in_two_files_fails(self):
        self.corpus(a=[case("age_gt_30", where=where(leaf("age", ">", 30)))],
                    b=[case("select_age_gt_30", where=where(leaf("age", ">", 30)))])
        self.assert_duplicates("a.yaml:age_gt_30", "b.yaml:select_age_gt_30")

    def test_expected_results_do_not_make_inputs_distinct(self):
        self.corpus(a=[case("one", expected={"count": 1}, description="first"),
                       case("two", expected={"count": 2}, expected_first={"age": 22})])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_pinned_sql_text_makes_inputs_distinct(self):
        # Condition order changes the emitted SQL text, so two cases that each pin their own
        # `expected_sql` are two SQL-text tests, not one.
        self.corpus(a=[case("ab", where=where(leaf("a", "==", 1), leaf("b", "==", 2)),
                            expected_sql='SELECT * FROM "t" WHERE "a" = ? AND "b" = ?'),
                       case("ba", where=where(leaf("b", "==", 2), leaf("a", "==", 1)),
                            expected_sql='SELECT * FROM "t" WHERE "b" = ? AND "a" = ?')])
        self.assert_passes()

    def test_the_same_pinned_sql_text_is_still_a_duplicate(self):
        self.corpus(a=[case("one", expected_sql="SELECT 1", expected_sql_pg="SELECT 1"),
                       case("two", expected_sql="SELECT 1", expected_sql_pg="SELECT 1")])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_key_order_does_not_matter(self):
        first = {"name": "one", "model": "person", "query_type": "select", "limit": {"value": 2, "offset": 1}}
        second = {"limit": {"offset": 1, "value": 2}, "query_type": "select", "model": "person", "name": "two"}
        self.corpus(a=[first, second])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_a_different_model_query_type_or_dataset_is_distinct(self):
        self.corpus(a=[case("one"), case("two", model="order"), case("three", query_type="first"),
                       case("four", dataset="empty")])
        self.assert_passes()

    # --- where normalisation ---

    def test_condition_order_does_not_matter(self):
        self.corpus(a=[case("one", where=where(leaf("age", ">", 30), leaf("name", "LIKE", "A%"))),
                       case("two", where=where(leaf("name", "LIKE", "A%"), leaf("age", ">", 30)))])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_missing_combine_means_and(self):
        pair = (leaf("age", ">", 30), leaf("age", "<", 50))
        self.corpus(a=[case("one", where=where(*pair)), case("two", where=where(*pair, combine="and"))])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_and_and_or_are_distinct(self):
        pair = (leaf("age", ">", 30), leaf("age", "<", 50))
        self.corpus(a=[case("one", where=where(*pair, combine="and")),
                       case("two", where=where(*pair, combine="or"))])
        self.assert_passes()

    def test_in_list_order_does_not_matter(self):
        self.corpus(a=[case("one", where=where(leaf("age", "IN", 1, 2, 3))),
                       case("two", where=where(leaf("age", "IN", 3, 1, 2)))])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_between_bound_order_matters(self):
        self.corpus(a=[case("one", where=where(leaf("age", "BETWEEN", 1, 5))),
                       case("two", where=where(leaf("age", "BETWEEN", 5, 1)))])
        self.assert_passes()

    def test_where_expr_equals_the_same_conditions(self):
        self.corpus(a=[case("listed", where=where(leaf("age", ">", 30), leaf("salary", "<", 9), combine="or")),
                       case("tree", where_expr=expr("OR", expr_leaf("salary", "<", value_int=9),
                                                    expr_leaf("age", ">", value_int=30)))])
        self.assert_duplicates("a.yaml:listed", "a.yaml:tree")

    def test_nested_same_operator_tree_equals_a_flat_list(self):
        flat = where(leaf("age", "==", 25), leaf("age", "==", 30), leaf("age", "==", 35), combine="or")
        nested = expr("OR", expr_leaf("age", "==", value_int=25),
                      expr("OR", expr_leaf("age", "==", value_int=30), expr_leaf("age", "==", value_int=35)))
        self.corpus(a=[case("flat", where=flat), case("nested", where_expr=nested)])
        self.assert_duplicates("a.yaml:flat", "a.yaml:nested")

    def test_mixed_operator_trees_are_distinct_from_a_flat_list(self):
        flat = where(leaf("a", "==", 1), leaf("b", "==", 2), leaf("c", "==", 3), combine="or")
        mixed = expr("OR", expr_leaf("a", "==", value_int=1),
                     expr("AND", expr_leaf("b", "==", value_int=2), expr_leaf("c", "==", value_int=3)))
        self.corpus(a=[case("flat", where=flat), case("mixed", where_expr=mixed)])
        self.assert_passes()

    def test_typed_values_stay_distinct(self):
        self.corpus(a=[case("int", where_expr=expr_leaf("salary", ">", value_int=30)),
                       case("double", where_expr=expr_leaf("salary", ">", value_dbl=30.0))])
        self.assert_passes()

    def test_both_typed_operands_of_a_between_leaf_count(self):
        # A where_expr leaf may carry value_int AND value_dbl: two operands, the BETWEEN bounds.
        self.corpus(a=[case("low", where_expr=expr_leaf("age", "BETWEEN", value_int=1, value_dbl=5.0)),
                       case("high", where_expr=expr_leaf("age", "BETWEEN", value_int=1, value_dbl=9.0))])
        self.assert_passes()

    def test_between_operand_order_in_a_where_expr_leaf_matters(self):
        self.corpus(a=[case("int_first", where_expr=expr_leaf("age", "BETWEEN", value_int=1, value_dbl=5.0)),
                       case("dbl_first", where_expr=expr_leaf("age", "BETWEEN", value_dbl=5.0, value_int=1))])
        self.assert_passes()

    def test_a_collate_makes_inputs_distinct(self):
        # #866: NOCASE and BINARY compare differently, so the same condition under each is two tests.
        nocase = {**leaf("name", "==", "alice"), "collate": "NOCASE"}
        binary = {**leaf("name", "==", "alice"), "collate": "BINARY"}
        self.corpus(a=[case("nocase", where=where(nocase)), case("binary", where=where(binary)),
                       case("none", where=where(leaf("name", "==", "alice")))])
        self.assert_passes()

    def test_the_same_collate_is_still_a_duplicate(self):
        nocase = {**leaf("name", "==", "alice"), "collate": "NOCASE"}
        self.corpus(a=[case("one", where=where(nocase)), case("two", where=where(dict(nocase)))])
        self.assert_fails("one")

    def test_collate_spelling_case_does_not_matter(self):
        upper = {**leaf("name", "==", "alice"), "collate": "NOCASE"}
        lower = {**leaf("name", "==", "alice"), "collate": "nocase"}
        self.corpus(a=[case("upper", where=where(upper)), case("lower", where=where(lower))])
        self.assert_fails("upper")

    def test_a_values_list_in_a_where_expr_leaf_counts(self):
        # #866: a where_expr leaf may carry `values:` like the flat form, and equals it.
        tree = {"field": "age", "op": "BETWEEN", "values": [1, 5]}
        self.corpus(a=[case("low", where_expr=dict(tree)),
                       case("high", where_expr={**tree, "values": [1, 9]}),
                       case("flat", where=where(leaf("age", "BETWEEN", 1, 5)))])
        self.assert_fails("flat")

    # --- aggregate normalisation (#868) ---

    def test_field_and_a_one_element_fields_list_are_the_same_aggregate(self):
        self.corpus(a=[case("field", query_type="avg", aggregate={"func": "avg", "field": "age"}),
                       case("fields", query_type="avg", aggregate={"func": "avg", "fields": ["age"]})])
        self.assert_duplicates("a.yaml:field", "a.yaml:fields")

    def test_an_empty_field_is_no_field(self):
        # COUNT(*) is written `field: ''` or with no field at all.
        self.corpus(a=[case("empty", query_type="count", aggregate={"func": "count", "field": ""}),
                       case("absent", query_type="count", aggregate={"func": "count"})])
        self.assert_duplicates("a.yaml:empty", "a.yaml:absent")

    def test_the_shorthand_equals_a_one_element_aggregations_list(self):
        self.corpus(a=[case("short", query_type="chain", aggregate={"func": "sum", "field": "age"}),
                       case("listed", query_type="chain", aggregations=[{"func": "sum", "fields": ["age"]}])])
        self.assert_duplicates("a.yaml:short", "a.yaml:listed")

    def test_an_aggregation_result_is_not_input(self):
        step = {"func": "sum", "field": "age"}
        self.corpus(a=[case("one", query_type="chain", aggregations=[{**step, "result": {"type": "int", "value": 1}}]),
                       case("two", query_type="chain", aggregations=[{**step, "result": {"type": "int", "value": 2}}])])
        self.assert_duplicates("a.yaml:one", "a.yaml:two")

    def test_the_last_field_key_wins_like_the_parser(self):
        # parser.cppm reads the keys in order and each field key replaces the list before it, so
        # `fields: [a, b]` then `field: ''` is COUNT(*), and `field: age` then `fields: [...]` is the list.
        self.corpus(a=[case("star", query_type="count", aggregate={"func": "count", "fields": ["age", "score"], "field": ""}),
                       case("plain", query_type="count", aggregate={"func": "count"}),
                       case("list", query_type="sum", aggregate={"func": "sum", "field": "salary", "fields": ["age", "score"]}),
                       case("same", query_type="sum", aggregate={"func": "sum", "fields": ["age", "score"]})])
        self.assert_duplicates("a.yaml:star", "a.yaml:plain")
        self.assert_duplicates("a.yaml:list", "a.yaml:same")

    def test_a_non_mapping_step_is_an_empty_step(self):
        # parser.cppm counts a non-object element of `aggregations:` as one empty step, and `fields: null`
        # is no field; neither may crash the checker with a traceback instead of a verdict.
        self.corpus(a=[case("scalar", query_type="chain", aggregations=[{"func": "count"}, 7]),
                       case("empty", query_type="chain", aggregations=[{"func": "count"}, {}]),
                       case("null", query_type="count", aggregate={"func": "count", "fields": None}),
                       case("plain", query_type="count", aggregate={"func": "count"})])
        self.assert_duplicates("a.yaml:scalar", "a.yaml:empty")
        self.assert_duplicates("a.yaml:null", "a.yaml:plain")

    def test_an_empty_name_in_fields_is_no_field(self):
        # Like `field: ''`, an empty name in `fields:` is skipped by the parser (#868).
        self.corpus(a=[case("gap", query_type="sum", aggregate={"func": "sum", "fields": ["", "age"]}),
                       case("one", query_type="sum", aggregate={"func": "sum", "field": "age"})])
        self.assert_duplicates("a.yaml:gap", "a.yaml:one")

    def test_different_aggregate_fields_are_distinct(self):
        self.corpus(a=[case("one", query_type="avg", aggregate={"func": "avg", "fields": ["age"]}),
                       case("two", query_type="avg", aggregate={"func": "avg", "fields": ["age", "salary"]})])
        self.assert_passes()

    # --- the escape hatch ---

    def test_allow_duplicate_with_a_reason_exempts_the_case(self):
        self.corpus(a=[case("one", limit={"value": 2}),
                       case("two", limit={"value": 2}, allow_duplicate="same query, other runner")])
        self.assert_passes()

    def test_allow_duplicate_without_a_reason_fails(self):
        self.corpus(a=[case("one", limit={"value": 2}, allow_duplicate="")])
        self.assert_fails("allow_duplicate")

    # --- robustness ---

    def test_missing_corpus_fails(self):
        self.assert_fails("tests/test_cases")

    def test_a_malformed_where_names_the_case(self):
        for broken in ("not a mapping", {"combine": "or"}):
            self.corpus(a=[case("broken", where=broken)])
            self.assert_fails("a.yaml:broken")

    def test_a_malformed_where_expr_names_the_case(self):
        self.corpus(a=[case("half_tree", where_expr={"op": "AND", "left": expr_leaf("a", "==", value_int=1)})])
        self.assert_fails("a.yaml:half_tree")

    def test_a_case_without_a_name_fails(self):
        self.corpus(a=[{"model": "person", "query_type": "select"}])
        self.assert_fails("name")


class RealCorpusTest(unittest.TestCase):
    def test_the_real_corpus_has_no_duplicate_inputs(self):
        proc = subprocess.run([sys.executable, str(CHECKER), str(REPO_ROOT)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(CLEAN_MESSAGE, proc.stdout)


if __name__ == "__main__":
    unittest.main()
