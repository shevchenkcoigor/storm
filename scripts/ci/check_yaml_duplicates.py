#!/usr/bin/env python3
"""Fail when two cases of the YAML test corpus have the same input.

The corpus is split by topic into tests/test_cases/<topic>.yaml, so the same query is easy to
add twice under two names, in one file or in two. The checker hashes each case's INPUT and
reports every hash shared by more than one case, naming each as <file>:<case name>.

A case's input is the case minus the keys that do not change what it runs: `name`,
`description`, the result keys `expected*` and `allow_duplicate`. A pinned `expected_sql*` stays
in: the text it pins depends on how the query was written, so two cases pinning different text
are two tests. The input is normalised before hashing, so the way it was written does not matter:
  * key order, at every depth;
  * `where` (a `conditions` list plus `combine`, default and) and `where_expr` (a left/right
    tree) are both turned into one tree, so the same filter in either form hashes alike;
  * AND/OR are commutative and associative: nested same-operator nodes are flattened and
    the operands sorted;
  * IN lists are sorted; BETWEEN bounds are not (the order is the range).
Typed values stay distinct (`value_int: 30` is not `value_dbl: 30.0`, #844), and a case that
differs in any other key (model, dataset, limit, ...) is a different case.

A deliberate repeat carries `allow_duplicate: <reason>` and is left out of the comparison; an
empty reason is an error, since the reason is what a reader needs.

    scripts/ci/check_yaml_duplicates.py [repo-root]   (default: this repository)

Exit: 0 = no duplicate inputs, 1 = duplicates or a malformed corpus. Needs PyYAML.
"""
import hashlib
import json
import pathlib
import sys
from collections import defaultdict

import yaml

DEFAULT_ROOT = pathlib.Path(__file__).resolve().parents[2]
CORPUS_DIR = pathlib.Path("tests") / "test_cases"
ALLOW_KEY = "allow_duplicate"
NON_INPUT_KEYS = frozenset({"name", "description", ALLOW_KEY, "where", "where_expr"})
DEFAULT_COMBINE = "and"
SORTED_VALUE_OPS = frozenset({"IN"})
TYPED_VALUE_KEYS = ("value_int", "value_dbl", "value")
EXPECTED_PREFIX = "expected"
# A pinned `expected_sql*` is part of the input: condition order and the where/where_expr shape
# change the emitted text, so two cases pinning different text are two SQL-text tests.
PINNED_SQL_PREFIX = "expected_sql"


class CorpusError(Exception):
    """A malformed corpus: the message names the file and, where known, the case."""


def dump(node):
    return json.dumps(node, sort_keys=True, separators=(",", ":"))


def leaf(field, op, values):
    ordered = sorted(values, key=dump) if op in SORTED_VALUE_OPS else list(values)
    return {"field": field, "op": op, "values": ordered}


def group(combine, children):
    """AND/OR node: same-operator children flattened, operands sorted; one operand is itself."""
    flat = []
    for child in children:
        flat.extend(child["of"] if child.get("combine") == combine else [child])
    if len(flat) == 1:
        return flat[0]
    return {"combine": combine, "of": sorted(flat, key=dump)}


def expr_values(node):
    """Every typed operand of a where_expr leaf, in the order written (BETWEEN's bounds)."""
    return [node[key] for key in node if key in TYPED_VALUE_KEYS]


def tree_of_expr(node):
    if "left" in node and "right" in node:
        return group(str(node["op"]).lower(), [tree_of_expr(node["left"]), tree_of_expr(node["right"])])
    return leaf(node["field"], node["op"], expr_values(node))


def tree_of_conditions(block):
    combine = str(block.get("combine", DEFAULT_COMBINE)).lower()
    return group(combine, [leaf(c["field"], c["op"], c.get("values", [])) for c in block["conditions"]])


def is_input_key(key):
    if key in NON_INPUT_KEYS:
        return False
    return key.startswith(PINNED_SQL_PREFIX) or not key.startswith(EXPECTED_PREFIX)


def canonical_input(case):
    kept = {key: value for key, value in case.items() if is_input_key(key)}
    filters = [tree_of_conditions(case["where"])] if "where" in case else []
    filters += [tree_of_expr(case["where_expr"])] if "where_expr" in case else []
    if filters:
        kept["where"] = group(DEFAULT_COMBINE, filters)
    return dump(kept)


def load_cases(path):
    cases = yaml.safe_load(path.read_text())
    if not isinstance(cases, list):
        raise CorpusError("%s: expected a list of cases" % path.name)
    for index, case in enumerate(cases):
        if not isinstance(case, dict) or not isinstance(case.get("name"), str):
            raise CorpusError("%s: case #%d has no string `name`" % (path.name, index + 1))
        reason = case.get(ALLOW_KEY, "x")
        if not isinstance(reason, str) or not reason.strip():
            raise CorpusError("%s:%s: `%s` needs a non-empty reason" % (path.name, case["name"], ALLOW_KEY))
    return cases


def collect(root):
    corpus = pathlib.Path(root) / CORPUS_DIR
    files = sorted(corpus.glob("*.yaml"))
    if not files:
        raise CorpusError("no %s/*.yaml under %s" % (CORPUS_DIR.as_posix(), root))
    by_digest = defaultdict(list)
    total = 0
    for path in files:
        for case in load_cases(path):
            total += 1
            if ALLOW_KEY in case:
                continue
            label = "%s:%s" % (path.name, case["name"])
            try:
                canonical = canonical_input(case)
            except (KeyError, TypeError, AttributeError) as error:
                raise CorpusError("%s: malformed case (%s: %s)" % (label, type(error).__name__, error))
            by_digest[hashlib.sha256(canonical.encode()).hexdigest()[:12]].append(label)
    return total, {digest: names for digest, names in by_digest.items() if len(names) > 1}


def report(duplicates):
    sys.stderr.write("duplicate test input in %d group(s):\n" % len(duplicates))
    for digest, names in sorted(duplicates.items()):
        sys.stderr.write("  %s\n" % digest)
        for name in names:
            sys.stderr.write("    %s\n" % name)
    sys.stderr.write("Remove the repeat, or mark a deliberate one `%s: <reason>`.\n" % ALLOW_KEY)


def main(argv):
    root = argv[1] if len(argv) > 1 else DEFAULT_ROOT
    try:
        total, duplicates = collect(root)
    except CorpusError as error:
        sys.stderr.write("%s\n" % error)
        return 1
    if duplicates:
        report(duplicates)
        return 1
    print("%d cases, no duplicate test inputs" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
