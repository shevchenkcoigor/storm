#!/usr/bin/env python3
"""Tests for scripts/tidy_prereqs.py (issues #557, #688).

The verdict answers "does clang-tidy have every module BMI it needs to parse the
staged files?". It must fail CLOSED: a missing BMI does not make clang-tidy
error, it degrades it to a partial parse with FALSE warnings, and the hook runs
clang-tidy with --fix. So every uncertain outcome must read `incomplete`.
Stdlib unittest only, no syntax newer than Python 3.7.

    python3 scripts/tests/test_tidy_prereqs.py
"""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = REPO_ROOT / "scripts" / "tidy_prereqs.py"

sys.path.insert(0, str(HELPER.parent))
from tidy_prereqs import prereqs_complete  # noqa: E402


class Tree:
    """A throwaway build dir with a compile_commands.json and module files."""

    def __init__(self, tmp):
        self.root = pathlib.Path(tmp).resolve()
        self.build = self.root / "build"
        self.build.mkdir()
        self.entries = []

    def source(self, name):
        path = self.root / name
        path.write_text("")
        return path

    def entry(self, source, modmap_text=None, bmi_refs=(), form="arguments"):
        """Register a TU; modmap_text=None means no modmap file is written."""
        modmap = "%s.modmap" % source.name
        args = ["clang++", "@%s" % modmap, "-c", str(source)]
        if modmap_text is not None or bmi_refs:
            text = "".join('-fmodule-file=m=%s\n' % ref for ref in bmi_refs) if modmap_text is None else modmap_text
            (self.build / modmap).write_text(text)
        record = {"file": str(source), "directory": str(self.build)}
        if form == "arguments":
            record["arguments"] = args
        else:
            record["command"] = " ".join(args)
        self.entries.append(record)

    def bmi(self, name):
        (self.build / name).write_text("")
        return name

    def write(self):
        (self.build / "compile_commands.json").write_text(json.dumps(self.entries))
        return self.build


class VerdictTest(unittest.TestCase):
    def tree(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Tree(tmp.name)

    def test_complete_when_modmap_and_bmis_exist(self):
        t = self.tree()
        src = t.source("a.cpp")
        t.entry(src, bmi_refs=[t.bmi("std.pcm")])
        self.assertTrue(prereqs_complete(t.write(), [src]))

    def test_incomplete_when_a_referenced_bmi_is_missing(self):
        t = self.tree()
        src = t.source("a.cpp")
        t.entry(src, bmi_refs=["missing.pcm"])
        self.assertFalse(prereqs_complete(t.write(), [src]))

    def test_incomplete_when_the_modmap_file_is_missing(self):
        t = self.tree()
        src = t.source("a.cpp")
        t.entry(src)  # modmap argument present, file never written
        self.assertFalse(prereqs_complete(t.write(), [src]))

    def test_incomplete_when_a_staged_file_has_no_entry(self):
        t = self.tree()
        listed, unlisted = t.source("a.cpp"), t.source("b.h")
        t.entry(listed, bmi_refs=[t.bmi("std.pcm")])
        self.assertFalse(prereqs_complete(t.write(), [listed, unlisted]))

    def test_unstaged_entries_are_ignored(self):
        t = self.tree()
        staged, other = t.source("a.cpp"), t.source("other.cpp")
        t.entry(staged, bmi_refs=[t.bmi("std.pcm")])
        t.entry(other, bmi_refs=["never-built.pcm"])
        self.assertTrue(prereqs_complete(t.write(), [staged]))

    def test_command_string_form_is_parsed(self):
        t = self.tree()
        src = t.source("a.cpp")
        t.entry(src, bmi_refs=[t.bmi("std.pcm")], form="command")
        self.assertTrue(prereqs_complete(t.write(), [src]))

    def test_entry_without_modmap_argument_needs_nothing(self):
        t = self.tree()
        src = t.source("a.cpp")
        t.entries.append({"file": str(src), "directory": str(t.build), "arguments": ["clang++", "-c"]})
        self.assertTrue(prereqs_complete(t.write(), [src]))

    def test_quoted_and_unprefixed_module_file_flags(self):
        t = self.tree()
        src = t.source("a.cpp")
        t.bmi("q.pcm")
        t.bmi("p.pcm")
        t.entry(src, modmap_text='-fmodule-file="n=q.pcm"\n-fmodule-file=p.pcm\n')
        self.assertTrue(prereqs_complete(t.write(), [src]))

    def test_no_staged_files_is_incomplete_not_vacuously_complete(self):
        t = self.tree()
        self.assertFalse(prereqs_complete(t.write(), []))


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(HELPER)] + [str(a) for a in args],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              universal_newlines=True)

    def test_prints_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Tree(tmp)
            src = t.source("a.cpp")
            t.entry(src, bmi_refs=[t.bmi("std.pcm")])
            result = self.run_cli(t.write(), src)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "complete"))

    def test_missing_compile_commands_is_incomplete(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_cli(tmp, pathlib.Path(tmp) / "a.cpp")
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "incomplete"))

    def test_corrupt_compile_commands_is_incomplete(self):
        with tempfile.TemporaryDirectory() as tmp:
            (pathlib.Path(tmp) / "compile_commands.json").write_text("{not json")
            result = self.run_cli(tmp, pathlib.Path(tmp) / "a.cpp")
        self.assertEqual(result.stdout.strip(), "incomplete")

    def test_no_files_is_incomplete(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_cli(tmp)
        self.assertEqual(result.stdout.strip(), "incomplete")


if __name__ == "__main__":
    unittest.main()
