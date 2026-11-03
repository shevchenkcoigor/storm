#!/usr/bin/env python3
"""Tests for scripts/compile_time/time_trace.py's command assembly and trace parsing.

The trace it captures lands in docs/internals/performance/COMPILE_TIME.md, and its first
version had exactly the failure that cannot be seen from the output: replaying a module
interface unit with -c traced only the SECOND of the two cc1 jobs clang runs for it (codegen
from the BMI), reporting src/orm/schema.cppm at 0.1 s when the BMI build that parses it takes
several seconds — a plausible number, wrong by two orders of magnitude. The other easy slip is
the modmap's -fmodule-output surviving the replay and overwriting the build's own BMI, which
makes the next ninja run rebuild every importer.

So the tests below pin both, plus the trace parsing the summary rests on. None of them needs a
compiler. Stdlib unittest, no syntax newer than Python 3.7.

    python3 scripts/compile_time/tests/test_time_trace.py
"""
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "lib"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import testing  # noqa: E402
import time_trace  # noqa: E402


class TraceCommandTest(unittest.TestCase):
    def test_a_module_unit_is_traced_as_the_bmi_build(self):
        argv = time_trace.trace_command(["clang++", "-std=c++26"], "/w", "/src/orm/schema.cppm",
                                        "/t/schema.json", "/tmp/out")
        self.assertIn("--precompile", argv)
        self.assertNotIn("-c", argv)
        self.assertEqual(argv[argv.index("-o") + 1], "/tmp/out/trace.pcm")
        self.assertIn("-ftime-trace=/t/schema.json", argv)
        self.assertIn("-ftime-trace-granularity=0", argv)

    def test_a_regular_tu_is_compiled_to_an_object_in_the_output_dir(self):
        argv = time_trace.trace_command(["clang++"], "/w", "/tests/a.cpp", "/t/a.json", "/tmp/out")
        self.assertIn("-c", argv)
        self.assertNotIn("--precompile", argv)
        self.assertEqual(argv[argv.index("-o") + 1], "/tmp/out/trace.o")

    def test_the_modmap_is_inlined_with_module_output_dropped(self):
        # The build's BMI must never be an output of the replay.
        with testing.scratch_dir_in_repo("time-trace-test.") as tmp:
            (tmp / "x.modmap").write_text(
                '-x c++-module -fmodule-output="CMakeFiles/storm.dir/storm_orm_schema.pcm" '
                '-fmodule-file="std=CMakeFiles/std.pcm"\n', encoding="utf-8")
            argv = time_trace.trace_command(["clang++", "@x.modmap", "-Werror"], str(tmp),
                                            "/src/orm/schema.cppm", "/t/s.json", "/tmp/out")
        self.assertFalse([arg for arg in argv if arg.startswith("-fmodule-output")], argv)
        self.assertEqual(argv[:6], ["clang++", "-x", "c++-module", "-fmodule-file=std=CMakeFiles/std.pcm",
                                    "-Werror", "-ftime-trace=/t/s.json"])


class TraceParsingTest(unittest.TestCase):
    EVENTS = {"traceEvents": [
        {"ph": "X", "name": "Total ExecuteCompiler", "dur": 2500000, "args": {"count": 1}},
        {"ph": "X", "name": "Total InstantiateClass", "dur": 900000, "args": {"count": 2}},
        {"ph": "X", "name": "InstantiateClass", "dur": 700000, "args": {"detail": "SchemaStatement<P>"}},
        {"ph": "X", "name": "InstantiateClass", "dur": 200000, "args": {"detail": "std::vector<int>"}},
        {"ph": "M", "name": "process_name"},
    ]}

    def test_totals_are_read_by_name_and_the_event_table_honours_the_filter(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp, "t.json")
            path.write_text(json.dumps(self.EVENTS), encoding="utf-8")
            seconds = time_trace.total_seconds(path, "Total ExecuteCompiler")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                time_trace.summarise(path, 5, "SchemaStatement")
        text = out.getvalue()
        self.assertEqual(seconds, 2.5)
        self.assertIn("Total InstantiateClass", text)
        self.assertIn("SchemaStatement<P>", text)
        self.assertNotIn("std::vector<int>", text)
        self.assertIn("(1 events, filtered)", text)


if __name__ == "__main__":
    unittest.main()
