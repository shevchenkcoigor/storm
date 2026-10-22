#!/bin/bash
# Tests for scripts/time_trace.py's command assembly and trace parsing.
#
# The trace it captures lands in docs/internals/performance/COMPILE_TIME.md, and
# its first version had exactly the failure that cannot be seen from the output:
# replaying a module interface unit with -c traced only the SECOND of the two
# cc1 jobs clang runs for it (codegen from the BMI), reporting src/orm/schema.cppm
# at 0.1 s when the BMI build that parses it takes several seconds — a plausible
# number, wrong by two orders of magnitude. The other easy slip is the modmap's
# -fmodule-output surviving the replay and overwriting the build's own BMI, which
# makes the next ninja run rebuild every importer.
#
# So the scenarios below pin both, plus the trace parsing the summary rests on.
# None of them needs a compiler.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TRACE="$REPO_ROOT/scripts/time_trace.py"

PASS=0
FAIL=0
FAILED_TESTS=()
CURRENT_TAG=""

fail() {
    local msg="$1"
    echo "  FAIL: $msg"
    FAIL=$((FAIL+1))
    FAILED_TESTS+=("$CURRENT_TAG")
    return 0
}

pass() {
    local msg="$1"
    echo "  ok: $msg"
    PASS=$((PASS+1))
    return 0
}

# Run the python snippet on stdin against the module under test, loaded by path
# (no cwd/PYTHONPATH dependence), from a QUOTED heredoc so the shell cannot
# mangle the quotes inside it.
run_py() {
    {
        cat <<'PRELUDE'
import importlib.util, json, pathlib, sys, tempfile
spec = importlib.util.spec_from_file_location("time_trace", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
PRELUDE
        cat
    } | python3 - "$TRACE" 2>&1
    return 0
}

check() {
    local what="$1" expected="$2" actual
    actual="$(run_py)"
    if [[ "$actual" == "$expected" ]]; then
        pass "$what"
    else
        fail "$what: expected '$expected', got '$actual'"
    fi
    return 0
}

# --- the bug that shipped: a module unit traced as its codegen job -----------
scenario_module_unit_traced_as_bmi_build() {
    check "a .cppm is replayed with --precompile into a .pcm, never -c" "OK" <<'SNIPPET'
argv = mod.trace_command(["clang++", "-std=c++26"], "/w", "/src/orm/schema.cppm",
                         "/t/schema.json", "/tmp/out")
ok = ("--precompile" in argv and "-c" not in argv
      and argv[argv.index("-o") + 1] == "/tmp/out/trace.pcm"
      and "-ftime-trace=/t/schema.json" in argv and "-ftime-trace-granularity=0" in argv)
print("OK" if ok else argv)
SNIPPET
    return 0
}

scenario_regular_tu_compiled_to_object() {
    check "a .cpp is replayed with -c into an object in the output dir" "OK" <<'SNIPPET'
argv = mod.trace_command(["clang++"], "/w", "/tests/a.cpp", "/t/a.json", "/tmp/out")
ok = ("-c" in argv and "--precompile" not in argv
      and argv[argv.index("-o") + 1] == "/tmp/out/trace.o")
print("OK" if ok else argv)
SNIPPET
    return 0
}

# --- the build's BMI must never be an output of the replay -------------------
scenario_modmap_module_output_dropped() {
    check "the modmap is inlined with -fmodule-output dropped, the rest kept" "OK" <<'SNIPPET'
with tempfile.TemporaryDirectory() as tmp:
    pathlib.Path(tmp, "x.modmap").write_text(
        '-x c++-module -fmodule-output="CMakeFiles/storm.dir/storm_orm_schema.pcm" '
        '-fmodule-file="std=CMakeFiles/std.pcm"\n', encoding="utf-8")
    argv = mod.trace_command(["clang++", "@x.modmap", "-Werror"], tmp,
                             "/src/orm/schema.cppm", "/t/s.json", "/tmp/out")
ok = (not any(a.startswith("-fmodule-output") for a in argv)
      and argv[:6] == ["clang++", "-x", "c++-module",
                       "-fmodule-file=std=CMakeFiles/std.pcm", "-Werror",
                       "-ftime-trace=/t/s.json"])
print("OK" if ok else argv)
SNIPPET
    return 0
}

# --- the trace parsing the summary and best-of-N rest on ---------------------
scenario_trace_parsing() {
    check "totals are read by name and the per-event table honours --filter" "OK" <<'SNIPPET'
import contextlib, io
events = {"traceEvents": [
    {"ph": "X", "name": "Total ExecuteCompiler", "dur": 2500000, "args": {"count": 1}},
    {"ph": "X", "name": "Total InstantiateClass", "dur": 900000, "args": {"count": 2}},
    {"ph": "X", "name": "InstantiateClass", "dur": 700000, "args": {"detail": "SchemaStatement<P>"}},
    {"ph": "X", "name": "InstantiateClass", "dur": 200000, "args": {"detail": "std::vector<int>"}},
    {"ph": "M", "name": "process_name"},
]}
with tempfile.TemporaryDirectory() as tmp:
    path = pathlib.Path(tmp, "t.json")
    path.write_text(json.dumps(events), encoding="utf-8")
    seconds = mod.total_seconds(path, "Total ExecuteCompiler")
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        mod.summarise(path, 5, "SchemaStatement")
text = out.getvalue()
ok = (seconds == 2.5 and "Total InstantiateClass" in text
      and "SchemaStatement<P>" in text and "std::vector<int>" not in text
      and "(1 events, filtered)" in text)
print("OK" if ok else (seconds, text))
SNIPPET
    return 0
}

SCENARIOS=(module_unit_traced_as_bmi_build regular_tu_compiled_to_object \
           modmap_module_output_dropped trace_parsing)

for tag in "${SCENARIOS[@]}"; do
    CURRENT_TAG="$tag"
    echo ""
    echo "Scenario: $tag"
    "scenario_$tag"
done

echo ""
echo "================================================"
echo "Passed: $PASS, Failed: $FAIL"

# A scenario that returns before its assertion increments neither counter, and
# the suite would report green having run fewer tests than it lists.
if (( PASS + FAIL != ${#SCENARIOS[@]} )); then
    echo "expected ${#SCENARIOS[@]} scenarios, accounted for $((PASS + FAIL))"
    exit 1
fi

if [[ $FAIL -gt 0 ]]; then
    echo "Failed scenarios: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
