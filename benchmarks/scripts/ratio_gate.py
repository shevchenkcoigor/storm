#!/usr/bin/env python3
"""storm/raw ratio gate over interleaved Google Benchmark rounds (issue #752).

Input: one gbench JSON per round and side (`--benchmark_report_aggregates_only=true`, so each
file holds `median` and `cv` aggregate rows), N raw files and N storm files in the same round
order. Per benchmark it reports the median over rounds of `raw / storm * 100` (`% of raw`),
the round-to-round spread of that ratio (`ratio cv`), and gbench's own worst within-round cv.

The exit status is the gate verdict, so CI and a local run execute the same code:
    0  every benchmark is at or above its threshold
    1  at least one benchmark is below its threshold
    2  the data cannot be compared (empty/partial pairing, drifted names, mixed time units,
       unreadable JSON). Never degraded to a short table: that is the failure mode #551's
       post-mortem names.

Ratios cancel runner noise only because both binaries ran in the same job, interleaved
(raw, storm, raw, storm, ...), so this script does not and cannot compare across runs.

    ratio_gate.py --raw raw_1.json raw_2.json --storm storm_1.json storm_2.json \\
        --threshold 95 --override Storm/INSERT/=80 --summary "$GITHUB_STEP_SUMMARY"

Stdlib only, no syntax newer than Python 3.7.
"""
import argparse
import collections
import json
import statistics
import sys

EXIT_PASS = 0
EXIT_BELOW_THRESHOLD = 1
EXIT_BAD_DATA = 2

Row = collections.namedtuple(
    "Row", "name raw_time storm_time pct_of_raw ratio_cv max_gbench_cv unit")


class GateError(Exception):
    """The inputs cannot be compared, which is a different verdict from 'too slow'."""


def load_json(path):
    try:
        with open(path) as handle:
            return json.load(handle)
    except (OSError, ValueError) as error:
        raise GateError("cannot read %s: %s" % (path, error))


def load_aggregate(data, aggregate_name):
    """{run_name: real_time} for one aggregate (median / cv) of one gbench JSON."""
    return {b["run_name"]: b["real_time"] for b in data["benchmarks"]
            if b.get("aggregate_name") == aggregate_name}


def time_units(data):
    # RMS rows (from ->Complexity) report a percentage and carry no time_unit at all.
    return {b["time_unit"] for b in data["benchmarks"] if "time_unit" in b}


def check_paired(raw, storm, what):
    paired = sorted(set(raw) & set(storm))
    if not paired:
        raise GateError("no paired %s: raw=%d storm=%d - filter or names drifted"
                        % (what, len(raw), len(storm)))
    if len(paired) != len(raw) or len(paired) != len(storm):
        raise GateError("partial %s match: raw=%s storm=%s - filter or names drifted "
                        "between the two binaries" % (what, sorted(raw), sorted(storm)))
    return paired


def ratio_cv(ratios):
    mean = statistics.mean(ratios)
    if len(ratios) < 2 or not mean:
        return float("nan")
    return statistics.stdev(ratios) / mean


def compute(raw_rounds, storm_rounds):
    """Rows (sorted by name) from per-round parsed gbench JSON; raises GateError."""
    if not raw_rounds or len(raw_rounds) != len(storm_rounds):
        raise GateError("need the same non-zero number of raw and storm rounds: raw=%d storm=%d"
                        % (len(raw_rounds), len(storm_rounds)))

    units = set()
    medians = {"raw": [], "storm": []}
    cvs = {"raw": [], "storm": []}
    names = None
    for index, (raw, storm) in enumerate(zip(raw_rounds, storm_rounds), start=1):
        # A ->Unit(kMicrosecond) on one side only would skew every ratio 1000x while still
        # printing a clean table, so every timed entry of every round must agree.
        units |= time_units(raw) | time_units(storm)
        raw_median, storm_median = load_aggregate(raw, "median"), load_aggregate(storm, "median")
        raw_cv, storm_cv = load_aggregate(raw, "cv"), load_aggregate(storm, "cv")
        paired = check_paired(raw_median, storm_median, "round %d median" % index)
        check_paired(raw_cv, storm_cv, "round %d cv" % index)
        if names is not None and paired != names:
            raise GateError("round %d benchmark set differs from round 1 - filter or gbench "
                            "nondeterminism" % index)
        names = paired
        medians["raw"].append(raw_median)
        medians["storm"].append(storm_median)
        cvs["raw"].append(raw_cv)
        cvs["storm"].append(storm_cv)

    if len(units) != 1:
        raise GateError("time_unit mismatch or non-uniform across rounds: %s - ratios would "
                        "be nonsense" % sorted(units))
    unit = next(iter(units))

    rows = []
    for name in names:
        raw_times = [m[name] for m in medians["raw"]]
        storm_times = [m[name] for m in medians["storm"]]
        ratios = [r / s * 100.0 for r, s in zip(raw_times, storm_times)]
        worst_cv = max(c[name] for side in cvs.values() for c in side)
        rows.append(Row(name, statistics.median(raw_times), statistics.median(storm_times),
                        statistics.median(ratios), ratio_cv(ratios), worst_cv, unit))
    return rows


def parse_override(text):
    """'PREFIX=THRESHOLD' -> (PREFIX, float). PREFIX is matched against the benchmark name."""
    prefix, separator, threshold = text.rpartition("=")
    if not separator or not prefix:
        raise GateError("override must look like PREFIX=THRESHOLD, got %r" % text)
    try:
        return prefix, float(threshold)
    except ValueError:
        raise GateError("override threshold must be a number, got %r" % text)


def threshold_for(name, default_threshold, overrides):
    for prefix, threshold in overrides:
        if name.startswith(prefix):
            return threshold
    return default_threshold


def evaluate(rows, default_threshold, overrides):
    """The rows strictly below their threshold; the first matching override wins."""
    return [r for r in rows
            if r.pct_of_raw < threshold_for(r.name, default_threshold, overrides)]


def format_table(rows, rounds):
    unit = rows[0].unit if rows else "ns"
    lines = [
        "| benchmark | raw_%s (median-of-%d) | storm_%s (median-of-%d) | %% of raw | "
        "ratio cv (rounds) | raw/storm cv (max) |" % (unit, rounds, unit, rounds),
        "|---|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        lines.append("| `%s` | %.0f | %.0f | %.1f%% | %.1f%% | %.1f%% |" % (
            r.name, r.raw_time, r.storm_time, r.pct_of_raw, r.ratio_cv * 100.0,
            r.max_gbench_cv * 100.0))
    return "\n".join(lines)


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--raw", nargs="+", required=True, metavar="JSON",
                        help="raw anchor gbench JSON, one per round")
    parser.add_argument("--storm", nargs="+", required=True, metavar="JSON",
                        help="storm_bench gbench JSON, one per round, same order as --raw")
    parser.add_argument("--threshold", type=float, default=95.0,
                        help="fail below this %% of raw (default 95, the project target)")
    parser.add_argument("--override", action="append", default=[], metavar="PREFIX=PCT",
                        help="threshold for benchmarks whose name starts with PREFIX; "
                             "repeatable, first match wins")
    parser.add_argument("--summary", metavar="FILE",
                        help="append the markdown table here (e.g. $GITHUB_STEP_SUMMARY)")
    parser.add_argument("--title", default="storm/raw ratio",
                        help="heading for the --summary section")
    return parser


def run(args):
    overrides = [parse_override(text) for text in args.override]
    rows = compute([load_json(p) for p in args.raw], [load_json(p) for p in args.storm])
    table = format_table(rows, rounds=len(args.raw))
    print(table)
    if args.summary:
        with open(args.summary, "a") as handle:
            handle.write("## %s\n\n%d interleaved rounds (raw then storm, repeated).\n\n%s\n"
                         % (args.title, len(args.raw), table))
    failed = evaluate(rows, args.threshold, overrides)
    for r in failed:
        sys.stderr.write("FAIL: %s is %.1f%% of raw, below %.1f%%\n" % (
            r.name, r.pct_of_raw, threshold_for(r.name, args.threshold, overrides)))
    return EXIT_BELOW_THRESHOLD if failed else EXIT_PASS


def main(argv=None):
    try:
        return run(build_parser().parse_args(argv))
    except GateError as error:
        sys.stderr.write("ERROR: %s\n" % error)
        return EXIT_BAD_DATA
    except (KeyError, TypeError, ZeroDivisionError, OSError) as error:
        # JSON of the wrong shape, a zero storm time, an unwritable --summary: all are
        # data/environment problems, and must not leak out as exit 1 ("too slow").
        sys.stderr.write("ERROR: unusable input (%s: %s)\n" % (type(error).__name__, error))
        return EXIT_BAD_DATA


if __name__ == "__main__":
    sys.exit(main())
