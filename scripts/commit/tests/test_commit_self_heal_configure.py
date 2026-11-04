#!/usr/bin/env python3
"""Tests for commit.sh's self-heal configure (issue #674, ported from bash in #767).

Before a step that needs a build tree, commit.sh configures the tree when it is absent
(#489): build/release for the clang-tidy prerequisites and the bench release build,
build/debug for clang-format, cmake-format, tests and coverage. Those configures used to run
as `cmake --preset ... > /dev/null 2>&1` with the exit status ignored, so a failed configure
surfaced one step later as `ninja: error: loading 'build.ninja': No such file or directory`
- with the real CMake error logged nowhere.

What is asserted, per configure site:
  - a failed configure fails the step that needed it, and blocks the commit;
  - the CMake error is in that step's log, and the log path is printed;
  - the misleading ninja error is not what the log reports.
And for the warm path: an already-configured tree never runs a configure.

Strategy: run the REAL commit.sh (a bash script, executed through its own shebang - bash 3.2
on macOS) in a throwaway git repo, with `cmake`, `ninja` and `pg_isready` stubbed on PATH, so
no compiler is needed. The stub `cmake --preset P` fails with a recognizable CMake error when
P is listed in FAIL_PRESETS, and otherwise writes the files a real configure leaves behind.
The stub `cmake --build` behaves like ninja on an unconfigured tree. Logs go to a
per-scenario directory via STORM_COMMIT_LOG_DIR.

Stdlib unittest, no syntax newer than Python 3.7. Needs `git`.

    python3 scripts/commit/tests/test_commit_self_heal_configure.py
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "lib"))
import testing  # noqa: E402

CMAKE_ERROR = "CMake Error at CMakeLists.txt:1 (message): FAKE_CONFIGURE_ERROR"
NINJA_ERROR = "ninja: error: loading 'build.ninja': No such file or directory"
# commit.sh's name for the clang-tidy prerequisites step.
PREREQS_STEP = "release prereqs"
ANSI = re.compile(r"\x1b\[[0-9;]*m")

# What commit.sh and the guards it runs need in the fixture repo (relative to the repo root).
FIXTURE_FILES = (
    "commit.sh",
    "scripts/ci/detect_changes.py",
    "scripts/commit/pre_commit_guards.py",
    "scripts/commit/smart_skip.py",
    "scripts/coverage/coverage_gate.py",
    "scripts/coverage/uncovered_lines.py",
    "scripts/tidy/tidy_prereqs.py",
    "scripts/tidy/clang_tidy_skiplist.py",
    # A staged cmake file triggers the CPM cold-cache guard (#709); it needs its script and
    # one compliant pin in the tree, or it fails before the step under test is reached.
    "scripts/ci/tests/test_cpm_cold_cache.py",
)
COMPLIANT_PIN = "cpmaddpackage(NAME pinned GITHUB_REPOSITORY a/b GIT_TAG v1 GIT_SHALLOW FALSE)\n"

CMAKE_STUB = '''#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
with open(os.environ["STUB_CALLS"], "a") as calls:
    calls.write("cmake " + " ".join(args) + "\\n")

PRESET_DIRS = {"ninja-debug": "build/debug", "ninja-debug-coverage": "build/debug",
               "ninja-release": "build/release"}

def preset_dir(preset):
    if preset not in PRESET_DIRS:
        sys.stderr.write("stub cmake: unknown preset %s\\n" % preset)
        sys.exit(2)
    return PRESET_DIRS[preset]

if args[:1] == ["--preset"]:
    if args[1] in os.environ.get("FAIL_PRESETS", "").split():
        print("-- Configuring incomplete, errors occurred!")
        sys.stderr.write("CMake Error at CMakeLists.txt:1 (message): FAKE_CONFIGURE_ERROR (%s)\\n" % args[1])
        sys.exit(1)
    directory = preset_dir(args[1])
    os.makedirs(directory, exist_ok=True)
    for name in ("build.ninja", "compile_commands.json"):
        open(os.path.join(directory, name), "a").close()
    sys.exit(0)

if args[:2] == ["--build", "--preset"]:
    directory = preset_dir(args[2])
    if not os.path.isfile(os.path.join(directory, "build.ninja")):
        sys.stderr.write("ninja: error: loading 'build.ninja': No such file or directory\\n")
        sys.exit(1)
    if args[2] == "ninja-debug" and len(args) == 3:
        for binary in ("tests/storm_tests", "tests/mock_sqlite/storm_mock_tests",
                       "tests/mock_libpq/storm_pq_mock_tests"):
            path = os.path.join(directory, binary)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as script:
                script.write("#!/bin/sh\\nexit 0\\n")
            os.chmod(path, 0o755)
    joined = " ".join(args)
    if "--target coverage" in joined and "coverage-clean" not in joined:
        print("  lines.......: 100.0% (1 of 1 lines)")
    sys.exit(0)

sys.stderr.write("stub cmake: unexpected invocation: %s\\n" % " ".join(args))
sys.exit(2)
'''

# Empty `ninja -t targets` output sends build_tidy_prereqs down its full-build fallback, which
# the cmake stub handles. A scenario that wants the targeted path writes a target list to
# $NINJA_TARGETS.
NINJA_STUB = '''#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
with open(os.environ["STUB_CALLS"], "a") as calls:
    calls.write("ninja " + " ".join(args) + "\\n")
if "-t targets" in " ".join(args) and os.path.isfile(os.environ["NINJA_TARGETS"]):
    with open(os.environ["NINJA_TARGETS"]) as targets:
        sys.stdout.write(targets.read())
sys.exit(0)
'''

PG_ISREADY_STUB = "#!/bin/sh\nexit 0\n"


def environment_without_git_hook_variables():
    """os.environ minus what a git hook exports.

    commit.sh runs this test from the pre-commit hook, where git exports GIT_INDEX_FILE (and,
    for some invocations, GIT_DIR). Left set, every `git` call below would act on the REAL
    repository's index - staging scenario files into the commit being checked. The hook also
    exports STORM_PRE_COMMIT_HOOK; left set, "manual run" scenarios would run in hook mode.
    """
    names = subprocess.check_output(["git", "rev-parse", "--local-env-vars"],
                                    universal_newlines=True).split()
    drop = set(names) | {"STORM_PRE_COMMIT_HOOK"}
    return {key: value for key, value in os.environ.items() if key not in drop}


BASE_ENVIRONMENT = environment_without_git_hook_variables()


class SelfHealConfigureTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.tmp = pathlib.Path(os.path.realpath(scratch.name))
        self.logs = self.tmp / "logs"
        self.calls_file = self.tmp / "calls"
        self.calls_file.write_text("")
        self.write_stubs()
        self.make_repo()

    # --- Fixtures ---

    def write_stubs(self):
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for name, text in (("cmake", CMAKE_STUB), ("ninja", NINJA_STUB), ("pg_isready", PG_ISREADY_STUB)):
            path = self.bin / name
            path.write_text(text)
            path.chmod(0o755)

    def git(self, *args):
        result = testing.run(["git", "-C", self.repo] + list(args), env=BASE_ENVIRONMENT, merge_stderr=True)
        self.assertEqual(result.returncode, 0, "git %s failed: %s" % (" ".join(args), result.stdout))

    def make_repo(self):
        """A throwaway repo holding the real commit.sh and the scripts it runs, committed so
        that only each scenario's own file is staged."""
        self.repo = self.tmp / "repo"
        for relative in FIXTURE_FILES:
            target = self.repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(str(REPO_ROOT / relative), str(target))
        (self.repo / "cmake").mkdir()
        (self.repo / "cmake" / "pinned.cmake").write_text(COMPLIANT_PIN)
        self.write_executable(self.repo / "scripts" / "tidy" / "run_clang_tidy.py", "#!/bin/sh\nexit 0\n")
        # A failed fixture commit (e.g. a global signing setup) would leave every file staged,
        # and the scenarios would pass or fail for the wrong reason - hence the assertions.
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false",
                 "commit", "-q", "-m", "init")

    @staticmethod
    def write_executable(path, text):
        path.write_text(text)
        path.chmod(0o755)

    def stage(self, *paths):
        for relative in paths:
            target = self.repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("// staged\n")
            self.git("add", relative)

    def run_commit(self, fail_presets, extra_env=None):
        """Run ./commit.sh in the fixture repo; returns (returncode, output without colours)."""
        env = dict(BASE_ENVIRONMENT)
        env.update(PATH="%s%s%s" % (self.bin, os.pathsep, os.environ["PATH"]),
                   STUB_CALLS=str(self.calls_file), NINJA_TARGETS=str(self.tmp / "ninja_targets"),
                   FAIL_PRESETS=fail_presets, STORM_COMMIT_LOG_DIR=str(self.logs))
        env.update(extra_env or {})
        result = testing.run(["./commit.sh"], env=env, cwd=str(self.repo), merge_stderr=True)
        return result.returncode, ANSI.sub("", result.stdout)

    def calls(self):
        return self.calls_file.read_text().splitlines()

    def log_text(self, name):
        path = self.logs / name
        return path.read_text() if path.is_file() else None

    # --- Assertions ---

    def assert_step(self, output, verdict, step):
        """run_step's own verdict line ("v <name> passed" / "x <name> FAILED"), not the summary
        table, which pads the icon with two spaces."""
        icon = "✓" if verdict == "passed" else "✗"
        matching = [line for line in output.splitlines() if (icon + " " + step) in line]
        self.assertTrue(any(" " + verdict in line for line in matching),
                        "step '%s' did not report %s" % (step, verdict))

    def assert_configure_error_in(self, output, step, log_name):
        """The step that needed the failed configure carries the CMake error in its own log,
        names that log, and does not blame ninja."""
        self.assert_step(output, "FAILED", step)
        log = self.log_text(log_name)
        self.assertIsNotNone(log, "'%s' wrote no log %s" % (step, log_name))
        self.assertIn(CMAKE_ERROR, log, "'%s' log does not hold the CMake error" % step)
        self.assertNotIn(NINJA_ERROR, log, "'%s' log reports the misleading ninja error" % step)
        self.assertIn("full output: %s" % (self.logs / log_name), output,
                      "'%s' does not print its log path" % step)

    # --- Scenarios ---

    def test_tidy_release_configure_fails(self):
        # Cold build/release, release configure fails: the clang-tidy prerequisites.
        self.stage("src/a.cpp")
        code, output = self.run_commit("ninja-release")
        self.assertNotEqual(code, 0, "commit is blocked")
        self.assert_configure_error_in(output, PREREQS_STEP, "storm_tidy_bmi.log")

    def test_bench_release_configure_fails(self):
        # Cold build/release with a bench change: the bench release build.
        self.stage("benchmarks/b.cpp")
        code, output = self.run_commit("ninja-release")
        self.assertNotEqual(code, 0, "commit is blocked")
        self.assert_configure_error_in(output, "release build", "storm_release.log")

    def test_debug_configure_fails(self):
        # Cold build/debug, debug configure fails: clang-format, tests and coverage each need
        # it, and each must report the CMake error itself.
        self.stage("src/a.cpp")
        code, output = self.run_commit("ninja-debug")
        self.assertNotEqual(code, 0, "commit is blocked")
        for step, log_name in (("clang-format", "storm_format.log"), ("tests (SQLite", "storm_tests.log"),
                               ("coverage", "storm_coverage.log")):
            with self.subTest(step=step):
                self.assert_configure_error_in(output, step, log_name)

    def test_cmake_format_debug_configure_fails(self):
        # cmake-only commits run cmake-format without clang-format - the #643 path.
        self.stage("cmake/c.cmake")
        code, output = self.run_commit("ninja-debug")
        self.assertNotEqual(code, 0, "commit is blocked")
        self.assert_configure_error_in(output, "cmake-format", "storm_cmake_format.log")

    def test_tidy_tree_without_build_ninja(self):
        # compile_commands.json present but no build.ninja: the tidy self-heal sentinel says
        # "configured", so build_tidy_prereqs must say "not configured" rather than hand ninja
        # a missing build.ninja.
        (self.repo / "build" / "release").mkdir(parents=True)
        (self.repo / "build" / "release" / "compile_commands.json").touch()
        self.stage("src/a.cpp")
        code, output = self.run_commit("")
        self.assertNotEqual(code, 0, "commit is blocked")
        self.assert_step(output, "FAILED", PREREQS_STEP)
        log = self.log_text("storm_tidy_bmi.log")
        self.assertIsNotNone(log)
        self.assertIn("not configured", log)
        self.assertNotIn(NINJA_ERROR, log)

    def test_tidy_targeted_prereqs_under_system_bash(self):
        # A configured tree whose ninja lists synthesized BMIs takes the TARGETED prereq build,
        # under the hook's own `#!/bin/bash` - bash 3.2 on macOS. It used to probe for `mapfile`
        # (bash >= 4.0) and, on a Mac, always fall back to building the full release target
        # set (#689).
        release = self.repo / "build" / "release"
        release.mkdir(parents=True)
        (release / "build.ninja").touch()
        # One entry for the staged TU and no modmap, so the prereq verdict is "complete" and no
        # fallback of any kind should run.
        (release / "compile_commands.json").write_text(
            '[{"directory": "%s", "file": "%s/src/a.cpp", "arguments": ["clang++", "-c", "src/a.cpp"]}]\n'
            % (self.repo, self.repo))
        (self.tmp / "ninja_targets").write_text(
            "CMakeFiles/storm_tests@synth_1.dir/0123abcd.bmi: CXX_MODULE_BMI\n"
            "CMakeFiles/storm.dir/CXX.dd: CXX_DYNDEP\n")
        self.stage("src/a.cpp")
        _, output = self.run_commit("")
        self.assert_step(output, "passed", PREREQS_STEP)
        calls = self.calls()
        # Every fallback, whatever its message, is this one call.
        self.assertNotIn("cmake --build --preset ninja-release", calls,
                         "release prereqs took a full-build fallback: %s" % self.log_text("storm_tidy_bmi.log"))
        self.assertIn("ninja -C build/release CMakeFiles/storm_tests@synth_1.dir/0123abcd.bmi", calls,
                      "the synthesized BMI was not built by name")
        self.assertIn("ninja -C build/release CMakeFiles/storm.dir/CXX.dd", calls,
                      "the dyndep files were not generated by name")

    def test_warm_tree_never_configures(self):
        # Warm path: both trees configured. Every configure would fail if it ran, so a
        # configure on the warm path shows up as a failed step as well as a call.
        for tree in ("debug", "release"):
            (self.repo / "build" / tree).mkdir(parents=True)
            (self.repo / "build" / tree / "build.ninja").touch()
        (self.repo / "build" / "release" / "compile_commands.json").touch()
        self.stage("src/a.cpp", "benchmarks/b.cpp")
        _, output = self.run_commit("ninja-debug ninja-release")
        configures = [call for call in self.calls() if call.startswith("cmake --preset")]
        self.assertEqual(configures, [], "a configured tree ran a configure")
        for step in ("clang-format", PREREQS_STEP, "release build", "tests (SQLite", "coverage"):
            self.assert_step(output, "passed", step)

    def test_failing_smart_skip_blocks_commit(self):
        # A smart-skip helper that fails (lost exec bit, broken python3, import error) must
        # BLOCK the commit. Left to its own devices every RUN_* stays unset, TOTAL_STEPS is 0
        # and the hook would print "No checks needed" and pass (#688).
        self.write_executable(self.repo / "scripts" / "commit" / "smart_skip.py", "#!/bin/sh\nexit 1\n")
        self.stage("src/a.cpp")
        code, output = self.run_commit("")
        self.assertNotEqual(code, 0, "commit is blocked")
        self.assertNotIn("No checks needed", output,
                         "a failing smart-skip helper let the commit through with no checks")
        self.assertIn("smart-skip failed", output, "the block does not name the smart-skip failure")

    def test_hook_empty_index_skips_checks(self):
        # Nothing staged, run by the git hook (#706): a commit that changes nothing needs no
        # checks. The hook says so with STORM_PRE_COMMIT_HOOK=1 (.githooks/pre-commit).
        code, output = self.run_commit("ninja-debug ninja-release", {"STORM_PRE_COMMIT_HOOK": "1"})
        self.assertEqual(code, 0, "hook with an empty index did not skip: %s" % output)
        self.assertIn("No checks needed", output)

    def test_hook_empty_index_failing_helper_blocks(self):
        # The empty-index route to "No checks needed" must still fail closed: a broken
        # smart-skip helper blocks the commit instead of waving it through.
        self.write_executable(self.repo / "scripts" / "commit" / "smart_skip.py", "#!/bin/sh\nexit 1\n")
        code, output = self.run_commit("", {"STORM_PRE_COMMIT_HOOK": "1"})
        self.assertNotEqual(code, 0, "commit is blocked")
        self.assertNotIn("No checks needed", output,
                         "a failing helper let an empty-index hook commit through")


if __name__ == "__main__":
    unittest.main()
