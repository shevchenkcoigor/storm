#!/usr/bin/env python3
"""Tests for benchmarks/scripts/ratio_gate.py (issue #752).

The storm/raw ratio gate used to live inline in bench-ratio-spike.yml as a heredoc, where
nothing could test it and a local run could not reuse it. The computation is plain functions
tested on fixture gbench JSON; the CLI runs as a subprocess for what only a process shows
(exit status = the gate verdict: 0 pass, 1 below threshold, 2 unusable data).

Stdlib unittest only, no syntax newer than Python 3.7.

    python3 scripts/ci/tests/test_ratio_gate.py
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "benchmarks" / "scripts" / "ratio_gate.py"
sys.path.insert(0, str(SCRIPT.parent))
import ratio_gate  # noqa: E402

WHERE = "Storm/WHERE/where_bool_equality/N:10000"
INSERT = "Storm/INSERT/insert/N:1000"


def entry(run_name, aggregate, real_time, unit="ns"):
    return {"run_name": run_name, "aggregate_name": aggregate, "real_time": real_time,
            "time_unit": unit}


def run_json(times, cv=0.01, unit="ns"):
    """One gbench --benchmark_report_aggregates_only JSON: median + cv per benchmark."""
    rows = []
    for name, value in times.items():
        rows.append(entry(name, "median", value, unit))
        rows.append(entry(name, "cv", cv, unit))
    return {"benchmarks": rows}


class ComputeTest(unittest.TestCase):
    def test_pct_of_raw_is_raw_over_storm(self):
        rows = ratio_gate.compute([run_json({WHERE: 100.0})], [run_json({WHERE: 125.0})])

        self.assertEqual(len(rows), 1)
        self.assertAlmostEqual(rows[0].pct_of_raw, 80.0)

    def test_pct_of_raw_is_median_of_per_round_ratios(self):
        raw = [run_json({WHERE: 100.0}) for _ in range(3)]
        storm = [run_json({WHERE: 100.0}), run_json({WHERE: 125.0}), run_json({WHERE: 200.0})]

        rows = ratio_gate.compute(raw, storm)

        self.assertAlmostEqual(rows[0].pct_of_raw, 80.0)

    def test_ratio_cv_is_spread_of_round_ratios(self):
        raw = [run_json({WHERE: 100.0}) for _ in range(2)]
        storm = [run_json({WHERE: 100.0}), run_json({WHERE: 50.0})]

        rows = ratio_gate.compute(raw, storm)

        # ratios 100 and 200: stdev 70.71 / mean 150
        self.assertAlmostEqual(rows[0].ratio_cv, 0.4714, places=3)

    def test_single_round_has_no_ratio_cv(self):
        rows = ratio_gate.compute([run_json({WHERE: 100.0})], [run_json({WHERE: 100.0})])

        self.assertNotEqual(rows[0].ratio_cv, rows[0].ratio_cv)  # NaN

    def test_max_gbench_cv_is_worst_of_either_side_across_rounds(self):
        raw = [run_json({WHERE: 100.0}, cv=0.01), run_json({WHERE: 100.0}, cv=0.02)]
        storm = [run_json({WHERE: 100.0}, cv=0.05), run_json({WHERE: 100.0}, cv=0.03)]

        rows = ratio_gate.compute(raw, storm)

        self.assertAlmostEqual(rows[0].max_gbench_cv, 0.05)

    def test_rows_are_sorted_by_name(self):
        times = {WHERE: 100.0, INSERT: 100.0}

        rows = ratio_gate.compute([run_json(times)], [run_json(times)])

        self.assertEqual([r.name for r in rows], [INSERT, WHERE])

    def test_rms_rows_without_time_unit_are_ignored(self):
        # ->Complexity(oN) adds an RMS row that reports a percentage: no time_unit, and an
        # aggregate_name that is neither median nor cv.
        rms = {"run_name": "Storm/INSERT/insert_RMS", "aggregate_name": "RMS",
               "real_time": 0.03}
        raw, storm = run_json({WHERE: 100.0}), run_json({WHERE: 100.0})
        raw["benchmarks"].append(rms)
        storm["benchmarks"].append(rms)

        rows = ratio_gate.compute([raw], [storm])

        self.assertEqual([r.name for r in rows], [WHERE])
        self.assertEqual(rows[0].unit, "ns")

    def test_empty_pairing_is_an_error(self):
        with self.assertRaises(ratio_gate.GateError) as caught:
            ratio_gate.compute([run_json({WHERE: 100.0})], [run_json({INSERT: 100.0})])

        self.assertIn("no paired", str(caught.exception))

    def test_partial_pairing_is_an_error(self):
        with self.assertRaises(ratio_gate.GateError) as caught:
            ratio_gate.compute([run_json({WHERE: 100.0, INSERT: 100.0})],
                               [run_json({WHERE: 100.0})])

        self.assertIn("partial", str(caught.exception))

    def test_missing_cv_aggregate_is_an_error_not_a_dash(self):
        raw = {"benchmarks": [entry(WHERE, "median", 100.0)]}

        with self.assertRaises(ratio_gate.GateError):
            ratio_gate.compute([raw], [run_json({WHERE: 100.0})])

    def test_benchmark_set_changing_between_rounds_is_an_error(self):
        raw = [run_json({WHERE: 100.0}), run_json({INSERT: 100.0})]
        storm = [run_json({WHERE: 100.0}), run_json({INSERT: 100.0})]

        with self.assertRaises(ratio_gate.GateError) as caught:
            ratio_gate.compute(raw, storm)

        self.assertIn("round 2", str(caught.exception))

    def test_time_unit_mismatch_between_sides_is_an_error(self):
        with self.assertRaises(ratio_gate.GateError) as caught:
            ratio_gate.compute([run_json({WHERE: 100.0}, unit="ns")],
                               [run_json({WHERE: 100.0}, unit="us")])

        self.assertIn("time_unit", str(caught.exception))

    def test_different_round_counts_are_an_error(self):
        with self.assertRaises(ratio_gate.GateError):
            ratio_gate.compute([run_json({WHERE: 100.0})] * 2, [run_json({WHERE: 100.0})])

    def test_no_rounds_is_an_error(self):
        with self.assertRaises(ratio_gate.GateError):
            ratio_gate.compute([], [])


def row(name, pct):
    return ratio_gate.Row(name=name, raw_time=100.0, storm_time=100.0 / pct * 100.0,
                          pct_of_raw=pct, ratio_cv=0.01, max_gbench_cv=0.01, unit="ns")


class EvaluateTest(unittest.TestCase):
    def test_below_default_threshold_fails(self):
        failed = ratio_gate.evaluate([row(WHERE, 94.9)], 95.0, [])

        self.assertEqual([r.name for r in failed], [WHERE])

    def test_exactly_at_threshold_passes(self):
        self.assertEqual(ratio_gate.evaluate([row(WHERE, 95.0)], 95.0, []), [])

    def test_override_prefix_replaces_the_default_for_matching_rows_only(self):
        rows = [row(INSERT, 85.0), row(WHERE, 85.0)]

        failed = ratio_gate.evaluate(rows, 95.0, [("Storm/INSERT/", 80.0)])

        self.assertEqual([r.name for r in failed], [WHERE])

    def test_override_can_still_fail(self):
        failed = ratio_gate.evaluate([row(INSERT, 79.0)], 95.0, [("Storm/INSERT/", 80.0)])

        self.assertEqual([r.name for r in failed], [INSERT])

    def test_first_matching_override_wins(self):
        overrides = [("Storm/INSERT/insert/", 70.0), ("Storm/INSERT/", 90.0)]

        self.assertEqual(ratio_gate.evaluate([row(INSERT, 75.0)], 95.0, overrides), [])


class FormatTest(unittest.TestCase):
    def test_table_has_the_spike_columns_and_one_line_per_row(self):
        table = ratio_gate.format_table([row(WHERE, 80.0)], rounds=3)

        lines = table.splitlines()
        self.assertIn("% of raw", lines[0])
        self.assertIn("ratio cv (rounds)", lines[0])
        self.assertEqual(len(lines), 3)
        self.assertIn("`%s`" % WHERE, lines[2])
        self.assertIn("80.0%", lines[2])


class OverrideParsingTest(unittest.TestCase):
    def test_parses_prefix_equals_threshold(self):
        self.assertEqual(ratio_gate.parse_override("Storm/INSERT/=80"),
                         ("Storm/INSERT/", 80.0))

    def test_rejects_missing_equals(self):
        with self.assertRaises(ratio_gate.GateError):
            ratio_gate.parse_override("Storm/INSERT/")

    def test_rejects_non_numeric_threshold(self):
        with self.assertRaises(ratio_gate.GateError):
            ratio_gate.parse_override("Storm/INSERT/=lots")


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)

    def write(self, name, payload):
        path = self.dir / name
        path.write_text(json.dumps(payload))
        return str(path)

    def invoke(self, raw, storm, *extra):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--raw", raw, "--storm", storm] + list(extra),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)

    def gate(self, raw_times, storm_times, *extra):
        raw = self.write("raw_1.json", run_json(raw_times))
        storm = self.write("storm_1.json", run_json(storm_times))
        return self.invoke(raw, storm, *extra)

    def test_passes_when_every_benchmark_meets_the_threshold(self):
        result = self.gate({WHERE: 100.0}, {WHERE: 100.0}, "--threshold", "95")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(WHERE, result.stdout)

    def test_exit_1_when_a_benchmark_is_below_the_threshold(self):
        result = self.gate({WHERE: 100.0}, {WHERE: 125.0}, "--threshold", "95")

        self.assertEqual(result.returncode, 1)
        self.assertIn("below", result.stderr)
        self.assertIn(WHERE, result.stderr)

    def test_override_lets_a_known_gap_pass(self):
        result = self.gate({INSERT: 100.0}, {INSERT: 125.0}, "--threshold", "95",
                           "--override", "Storm/INSERT/=75")

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_exit_2_on_unusable_data(self):
        result = self.gate({WHERE: 100.0}, {INSERT: 100.0})

        self.assertEqual(result.returncode, 2)
        self.assertIn("no paired", result.stderr)

    def test_exit_2_on_unreadable_json(self):
        broken = self.dir / "raw_1.json"
        broken.write_text("{not json")
        storm = self.write("storm_1.json", run_json({WHERE: 100.0}))

        result = self.invoke(str(broken), storm)

        self.assertEqual(result.returncode, 2)

    def test_exit_2_when_json_has_the_wrong_shape(self):
        raw = self.write("raw_1.json", {})
        storm = self.write("storm_1.json", run_json({WHERE: 100.0}))

        result = self.invoke(raw, storm)

        self.assertEqual(result.returncode, 2)
        self.assertIn("unusable input", result.stderr)

    def test_exit_2_when_a_storm_time_is_zero(self):
        result = self.gate({WHERE: 100.0}, {WHERE: 0.0})

        self.assertEqual(result.returncode, 2)

    def test_exit_2_when_the_summary_cannot_be_written(self):
        missing = self.dir / "no_such_dir" / "summary.md"

        result = self.gate({WHERE: 100.0}, {WHERE: 100.0}, "--summary", str(missing))

        self.assertEqual(result.returncode, 2)

    def test_summary_file_gets_the_table_appended(self):
        summary = self.dir / "summary.md"
        summary.write_text("existing\n")

        result = self.gate({WHERE: 100.0}, {WHERE: 100.0}, "--threshold", "95",
                           "--summary", str(summary), "--title", "where")

        self.assertEqual(result.returncode, 0, result.stderr)
        text = summary.read_text()
        self.assertTrue(text.startswith("existing\n"))
        self.assertIn("where", text)
        self.assertIn(WHERE, text)

    def test_summary_is_written_even_when_the_gate_fails(self):
        summary = self.dir / "summary.md"

        result = self.gate({WHERE: 100.0}, {WHERE: 200.0}, "--threshold", "95",
                           "--summary", str(summary))

        self.assertEqual(result.returncode, 1)
        self.assertIn(WHERE, summary.read_text())


WORKFLOW = REPO_ROOT / ".github" / "workflows" / "bench-ratio-spike.yml"
GATE_STEP_NAME = "- name: Compute storm/raw ratio and apply the gate"


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def workflow_gate_script():
    """The `run: |` block of the workflow's gate step, dedented, exactly as the runner gets it."""
    lines = WORKFLOW.read_text().split("\n")
    step = next(i for i, line in enumerate(lines) if line.strip() == GATE_STEP_NAME)
    run = next(i for i in range(step, len(lines)) if lines[i].strip() == "run: |")
    body_indent = indent_of(lines[run + 1])
    body = []
    for line in lines[run + 1:]:
        if line.strip() and indent_of(line) < body_indent:
            break
        body.append(line[body_indent:])
    return "\n".join(body)


class WorkflowStepTest(unittest.TestCase):
    """The workflow's call of the script, run for real: a renamed flag or a moved script
    would otherwise surface only when someone dispatches the workflow by hand."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)
        (self.dir / "benchmarks").symlink_to(REPO_ROOT / "benchmarks")
        self.summary = self.dir / "summary.md"

    def run_step(self, raw_times, storm_times):
        for index in (1, 2, 3):
            (self.dir / ("raw_%d.json" % index)).write_text(json.dumps(run_json(raw_times)))
            (self.dir / ("storm_%d.json" % index)).write_text(json.dumps(run_json(storm_times)))
        environment = {"PATH": "/usr/bin:/bin:/usr/local/bin:" + str(pathlib.Path(sys.executable).parent),
                       "GITHUB_RUN_NUMBER": "7", "MATRIX_CATEGORY": "where",
                       "GITHUB_STEP_SUMMARY": str(self.summary)}
        return subprocess.run(["bash", "-e", "-c", workflow_gate_script()], cwd=str(self.dir),
                              env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)

    def test_step_passes_and_writes_the_summary_when_ratios_are_fine(self):
        result = self.run_step({WHERE: 100.0}, {WHERE: 100.0})

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(WHERE, self.summary.read_text())
        self.assertIn("run 7 / where", self.summary.read_text())

    def test_step_only_warns_when_a_benchmark_is_below_its_threshold(self):
        result = self.run_step({WHERE: 100.0}, {WHERE: 125.0})

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("::warning::", result.stdout)
        self.assertIn(WHERE, self.summary.read_text())

    def test_step_stays_fatal_on_unusable_data(self):
        result = self.run_step({WHERE: 100.0}, {INSERT: 100.0})

        self.assertEqual(result.returncode, 2)

    def test_step_applies_the_insert_override(self):
        result = self.run_step({INSERT: 100.0}, {INSERT: 125.0})  # 80%: at the override

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("::warning::", result.stdout)

    def test_sparse_checkout_fetches_the_script_the_step_runs(self):
        text = WORKFLOW.read_text()
        sparse = [line.split(":", 1)[1].strip() for line in text.split("\n")
                  if line.strip().startswith("sparse-checkout:")]

        self.assertEqual(sparse, ["benchmarks/scripts/ratio_gate.py"])
        self.assertTrue((REPO_ROOT / sparse[0]).is_file())
        self.assertIn(sparse[0], workflow_gate_script())


if __name__ == "__main__":
    unittest.main()
