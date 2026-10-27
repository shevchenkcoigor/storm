"""Fake toolchain shared by the run_clang_tidy.py tests (issue #692).

The runner shells out to clang-tidy, clang-tidy-diff.py and git, so its tests build
a throwaway repo next to a fake toolchain at the relative paths the script
hardcodes (`../clang-p2996/...`) and run it end to end:

  * the fake clang-tidy prints a canned diagnostic file per source file and logs
    its argv;
  * the fake clang-tidy-diff.py logs argv, stdin and the compile database it was
    pointed at, and prints a canned output per call.

That pins the contract the shell script had - exit codes, messages, counts, what
reaches the tools - without a real clang-tidy. Not a test module itself.
Stdlib only, no syntax newer than Python 3.9.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "run_clang_tidy.py"

FAKE_CLANG_TIDY = """#!/bin/sh
for last; do :; done
printf '%s\\n' "$*" >> "$FAKE_LOG/tidy_calls"
out="$FAKE_TIDY_OUT/$(printf '%s' "$last" | tr / _)"
[ -f "$out" ] && cat "$out"
exit 0
"""

FAKE_CLANG_TIDY_DIFF = """import os, pathlib, shutil, sys
log = pathlib.Path(os.environ["FAKE_LOG"])
args = sys.argv[1:]
with open(str(log / "diff_calls"), "a") as handle:
    handle.write(" ".join(args) + "\\n")
call = len((log / "diff_calls").read_text().splitlines())
with open(str(log / ("diff_stdin.%d" % call)), "w") as handle:
    handle.write(sys.stdin.read())
database = pathlib.Path(args[args.index("-path") + 1]) / "compile_commands.json"
shutil.copy(str(database), str(log / ("database.%d" % call)))
touch = os.environ.get("FAKE_DIFF_TOUCH")
if touch and "-fix" in args:
    with open(touch, "a") as handle:
        handle.write("// fixed\\n")
canned = pathlib.Path(os.environ["FAKE_DIFF_OUT"]) / str(call)
if canned.exists():
    sys.stdout.write(canned.read_text())
"""

WARNING = "src/orm/fields.cppm:70:12: warning: enum 'Color' uses a larger base type [performance-enum-size]"
ERROR = "src/orm/fields.cppm:3:1: error: unknown type name 'foo' [clang-diagnostic-error]"
CRASH = "PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/"
PCH_COMMAND = ("clang++ -c -Xclang -include-pch -Xclang gtest.pch -Xclang -include -Xclang gtest.h "
               "-Winvalid-pch -o t.o t.cpp")


def clean_env():
    """The environment minus GIT_*: under the pre-commit hook GIT_INDEX_FILE points
    at the real repo, and every git call below must act on the throwaway one."""
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.pop("GITHUB_ACTIONS", None)
    return env


class ToolchainTest(unittest.TestCase):
    """A throwaway repo plus a fake toolchain at the paths the script hardcodes."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = pathlib.Path(tmp.name).resolve()
        self.repo = self.root / "repo"
        self.log = self.root / "log"
        self.tidy_out = self.root / "tidy_out"
        self.diff_out = self.root / "diff_out"
        for directory in (self.repo, self.log, self.tidy_out, self.diff_out):
            directory.mkdir()

        tidy = self.root / "clang-p2996" / "build" / "bin" / "clang-tidy"
        tidy.parent.mkdir(parents=True)
        tidy.write_text(FAKE_CLANG_TIDY)
        tidy.chmod(0o755)
        diff = self.root / "clang-p2996" / "clang-tools-extra" / "clang-tidy" / "tool" / "clang-tidy-diff.py"
        diff.parent.mkdir(parents=True)
        diff.write_text(FAKE_CLANG_TIDY_DIFF)

        (self.repo / ".clang-tidy").write_text("Checks: '*'\n")
        self.write_database([])
        self.git("init", "-q")

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo)] + list(args), capture_output=True, text=True,
                              env=clean_env(), check=True).stdout

    def write_database(self, commands):
        build = self.repo / "build" / "release"
        build.mkdir(parents=True, exist_ok=True)
        entries = [{"directory": str(build), "command": command, "file": "t.cpp"} for command in commands]
        (build / "compile_commands.json").write_text(json.dumps(entries))

    def stage(self, relative, text="int x;\n"):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        self.git("add", relative)

    def canned_tidy(self, relative, text):
        (self.tidy_out / relative.replace("/", "_")).write_text(text)

    def canned_diff(self, call, text):
        (self.diff_out / str(call)).write_text(text)

    def run_script(self, *args, extra_env=None):
        env = clean_env()
        env.update({"FAKE_LOG": str(self.log), "FAKE_TIDY_OUT": str(self.tidy_out),
                    "FAKE_DIFF_OUT": str(self.diff_out)})
        env.update(extra_env or {})
        return subprocess.run([sys.executable, str(SCRIPT)] + list(args), cwd=str(self.repo),
                              capture_output=True, text=True, env=env)

    def calls(self, name):
        path = self.log / name
        return path.read_text().splitlines() if path.exists() else []

    def assert_fails(self, proc, text):
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(text, proc.stdout + proc.stderr)

    def assert_passes(self, proc, text):
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(text, proc.stdout + proc.stderr)
