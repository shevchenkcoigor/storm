#!/usr/bin/env python3
"""Capture and summarise a clang -ftime-trace of one TU from an existing build.

Replays the TU's exact command from compile_commands.json with -ftime-trace and
-ftime-trace-granularity=0 appended, writing nothing the build owns: the output
goes to a temporary directory and the trace to --out-dir, so the replay never
rewrites the build's BMI (which would make ninja rebuild every importer). A .cppm
is traced as its BMI build (--precompile, see capture). Default builds are
untouched — no preset, no cache variable.

    scripts/time_trace.py src/orm/schema.cppm
    scripts/time_trace.py tests/schema/test_orm_schema.cpp --top 15 --filter SchemaStatement

Reported: clang's own "Total <event>" aggregates (which do not double-count a
nested event of the same name; "Total ExecuteCompiler" is the whole compile), and
the N longest single events of the expensive kinds with their detail string, from
the fastest of --runs compiles. A single TU still moves by ~0.3 s between rebuilt
BMIs, so read an A/B delta across alternated rounds, never one pair — and prefer
the event COUNTS, which are deterministic. See
docs/internals/performance/COMPILE_TIME.md#method.
"""
import argparse
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), "lib"))
from compile_replay import REPO_ROOT, command_for, load_compile_db, safe_path  # noqa: E402

# The event kinds whose individual instances are worth naming: template
# instantiation, class parsing, and constant evaluation (where consteval DDL
# generation lands).
DETAIL_EVENTS = ("InstantiateFunction", "InstantiateClass", "ParseClass",
                 "EvaluateAsConstantExpr", "EvaluateAsRValue", "EvaluateAsInitializer")


def expand_modmap(argv, cwd):
    """Inline each @response file, dropping -fmodule-output (see capture)."""
    expanded = []
    for arg in argv:
        if not arg.startswith("@"):
            expanded.append(arg)
            continue
        response = pathlib.Path(cwd) / arg[1:]
        text = pathlib.Path(safe_path(response)).read_text(encoding="utf-8")
        for flag in shlex.split(text):
            if not flag.startswith("-fmodule-output"):
                expanded.append(flag)
    return expanded


def trace_command(cmd, cwd, source_file, trace_path, out_dir):
    """The replay argv for one TU: its own flags, a granularity-0 trace, outputs in out_dir.

    A module interface unit compiles as TWO cc1 jobs (-emit-module-interface,
    then -emit-obj FROM that BMI), and both would write the one trace file —
    the second, which never parses the source, last. --precompile runs only
    the first: the BMI build, which is where a module's frontend cost lives.
    Its BMI lands in out_dir; the modmap's -fmodule-output is dropped so it can
    never land over the build's own.
    """
    is_module = source_file.endswith(".cppm")
    output = pathlib.Path(out_dir) / ("trace.pcm" if is_module else "trace.o")
    return expand_modmap(cmd, cwd) + [
        f"-ftime-trace={trace_path}", "-ftime-trace-granularity=0",
        "--precompile" if is_module else "-c", source_file, "-o", str(output)]


def capture(build_dir, source, trace_path):
    """Compile `source` once with a granularity-0 trace; return an error text or None.

    The source is looked up in the database's own file set before its command is
    assembled, in this one frame — the same allow-list shape as compile_replay's
    measure(), so what constrains the executed argv is visible here.
    """
    db = load_compile_db(build_dir)
    source_file = safe_path(pathlib.Path(REPO_ROOT) / source)
    entry = next((e for e in db if e["file"] == source_file), None)
    if entry is None:
        return f"{source}: not in the compile database"

    cmd, cwd = command_for(entry)
    with tempfile.TemporaryDirectory() as tmp:
        full = trace_command(cmd, cwd, source_file, trace_path, tmp)
        proc = subprocess.run(full, cwd=cwd, capture_output=True,
                              text=True, encoding="utf-8", errors="replace")
    return proc.stderr[-2000:] if proc.returncode != 0 else None


def summarise(trace_path, top, pattern):
    events = json.loads(pathlib.Path(trace_path).read_text(encoding="utf-8"))["traceEvents"]
    timed = [e for e in events if e.get("ph") == "X"]

    totals = sorted((e for e in timed if e["name"].startswith("Total ")),
                    key=lambda e: e["dur"], reverse=True)
    print(f"{'aggregate':40} {'sec':>8} {'count':>8}")
    for event in totals[:top]:
        count = event.get("args", {}).get("count", "")
        print(f"{event['name']:40} {event['dur'] / 1e6:8.3f} {count!s:>8}")

    regex = re.compile(pattern) if pattern else None
    for kind in DETAIL_EVENTS:
        rows = [e for e in timed if e["name"] == kind
                and (regex is None or regex.search(e.get("args", {}).get("detail", "")))]
        if not rows:
            continue
        rows.sort(key=lambda e: e["dur"], reverse=True)
        print(f"\nlongest {kind} ({len(rows)} events{', filtered' if regex else ''})")
        for event in rows[:top]:
            detail = event.get("args", {}).get("detail", "")
            print(f"  {event['dur'] / 1e6:8.3f}  {detail[:150]}")


def total_seconds(trace_path, name):
    """Duration of the named event in a trace, in seconds (e.g. "Total ExecuteCompiler")."""
    events = json.loads(pathlib.Path(trace_path).read_text(encoding="utf-8"))["traceEvents"]
    return next(e["dur"] for e in events if e.get("name") == name) / 1e6


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="TU to trace, relative to the repository root")
    ap.add_argument("--build-dir", default="build/debug",
                    help="configured AND built tree, relative to the repository root")
    ap.add_argument("--out-dir", default="build/time-trace",
                    help="where the trace JSON is written (default build/time-trace)")
    ap.add_argument("--runs", type=int, default=3, help="best of N compiles (default 3)")
    ap.add_argument("--top", type=int, default=10, help="rows per table (default 10)")
    ap.add_argument("--filter", default="",
                    help="regex on the event detail for the per-event tables")
    args = ap.parse_args()
    if args.runs < 1:
        ap.error("--runs must be >= 1")

    out_dir = pathlib.Path(safe_path(pathlib.Path(REPO_ROOT) / args.out_dir))
    out_dir.mkdir(parents=True, exist_ok=True)
    # Whole repo-relative path, separators flattened: two TUs sharing a basename
    # (tests/crud/ vs tests/schema/) must not overwrite each other's trace.
    stem = re.sub(r"[^A-Za-z0-9_.-]", "_", args.source)
    run_path = pathlib.Path(safe_path(out_dir / f"{stem}.last.json"))
    best_path = pathlib.Path(safe_path(out_dir / f"{stem}.json"))

    # Best-of-N, keeping the fastest run's trace: the distribution is one true
    # cost plus scheduler noise, so the minimum is what noise cannot inflate.
    best = None
    for _ in range(args.runs):
        err = capture(args.build_dir, args.source, run_path)
        if err:
            print(err, file=sys.stderr)
            return 1
        seconds = total_seconds(run_path, "Total ExecuteCompiler")
        if best is None or seconds < best:
            best = seconds
            shutil.copyfile(run_path, best_path)
    run_path.unlink()
    print(f"trace: {best_path}   ExecuteCompiler best of {args.runs}: {best:.3f} s\n")
    summarise(best_path, args.top, args.filter)
    return 0


if __name__ == "__main__":
    sys.exit(main())
