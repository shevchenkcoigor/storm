#!/usr/bin/env python3
"""Tests for scripts/ci/detect_changes.py.

The script classifies a changed-file list into HAS_*_CHANGES flags that both commit.sh
(pre-commit smart skips) and the `changes` job in .github/workflows/ci.yml (skipping the
test/coverage/clang-p2996-host-layout jobs on a PR that doesn't touch src/tests/cmake) rely
on to decide whether to run anything at all. A silent misclassification here is invisible in
a diff and would either skip real coverage or force every PR through the full matrix — the
table below pins the exact flag output for each file class. Each row names every flag that
is true; all others must be false. Stdlib unittest, no syntax newer than Python 3.7.

    python3 scripts/ci/tests/test_detect_changes.py
"""
import pathlib
import subprocess
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
DETECTOR = REPO_ROOT / "scripts" / "ci" / "detect_changes.py"

FLAGS = ("SRC", "TEST", "CPP", "CMAKE", "BENCH", "TIDY")

# (description, changed files, flags that must be true)
SCENARIOS = (
    ("src/foo.cppm sets SRC + CPP + TIDY", ["src/foo.cppm"], {"SRC", "CPP", "TIDY"}),
    ("tests/test_foo.cpp sets TEST + CPP + TIDY", ["tests/test_foo.cpp"], {"TEST", "CPP", "TIDY"}),
    # A shared header outside src/ and tests/ is still compiled: CPP must catch it.
    ("include/storm/storm.h sets CPP + TIDY", ["include/storm/storm.h"], {"CPP", "TIDY"}),
    ("cmake/foo.cmake sets CMAKE only", ["cmake/foo.cmake"], {"CMAKE"}),
    ("src/CMakeLists.txt sets SRC + CMAKE", ["src/CMakeLists.txt"], {"SRC", "CMAKE"}),
    ("benchmarks/foo.cpp sets BENCH + CPP + TIDY", ["benchmarks/foo.cpp"], {"BENCH", "CPP", "TIDY"}),
    ("docs/README.md sets nothing", ["docs/README.md"], set()),
    ("empty input sets nothing", [], set()),
    ("docs + src together still sets SRC + CPP + TIDY", ["docs/README.md", "src/foo.cppm"], {"SRC", "CPP", "TIDY"}),
    # clang-tidy --diff lints changed C++ lines only (#913): a change with no C++ gives it nothing
    # to do, so cmake, scripts, workflows and docs alone must not set TIDY ...
    ("cmake-only change does not set TIDY", ["CMakeLists.txt", "cmake/foo.cmake"], {"CMAKE"}),
    ("a script outside scripts/tidy does not set TIDY", ["scripts/commit/smart_skip.py"], set()),
    ("another workflow does not set TIDY", [".github/workflows/bench-ratio.yml"], set()),
    ("the weekly sweep workflow does not set TIDY", [".github/workflows/clang-tidy-sweep.yml"], set()),
    ("a .clang-tidy config change does not set TIDY (the weekly sweep validates it)", [".clang-tidy"], set()),
    # ... except the tidy plumbing itself, so an edit to the job or the runner is verified by it.
    ("scripts/tidy/run_clang_tidy.py sets TIDY only", ["scripts/tidy/run_clang_tidy.py"], {"TIDY"}),
    ("scripts/tidy/tests/ sets TIDY only", ["scripts/tidy/tests/test_run_clang_tidy.py"], {"TIDY"}),
    ("ci.yml sets TIDY only", [".github/workflows/ci.yml"], {"TIDY"}),
    # The prefix and the exact name are what keep the plumbing rule from over- or under-matching.
    ("a sibling of scripts/tidy/ does not set TIDY", ["scripts/tidyfoo/x.py"], set()),
    ("a path ending in scripts/tidy/ elsewhere does not set TIDY", ["xscripts/tidy/x.py"], set()),
    ("a backup of ci.yml does not set TIDY", [".github/workflows/ci.yml.bak"], set()),
)


def detect(files):
    """Run the real detector over `files`; return (returncode, {flag: bool}, raw stdout)."""
    stdin = "".join(name + "\n" for name in files)
    result = subprocess.run([str(DETECTOR)], input=stdin, stdout=subprocess.PIPE,
                            universal_newlines=True)
    parsed = {}
    for line in result.stdout.splitlines():
        key, _, value = line.partition("=")
        parsed[key] = value == "true"
    return result.returncode, parsed, result.stdout


class DetectChangesTest(unittest.TestCase):
    def test_detector_is_executable(self):
        self.assertTrue(DETECTOR.is_file(), "%s is missing" % DETECTOR)
        self.assertTrue(DETECTOR.stat().st_mode & 0o111, "%s is not executable" % DETECTOR)

    def test_scenarios(self):
        for description, files, expected_true in SCENARIOS:
            with self.subTest(description):
                code, parsed, raw = detect(files)
                self.assertEqual(code, 0, "detector exited %d. Output: %s" % (code, raw))
                expected = {"HAS_%s_CHANGES" % flag: flag in expected_true for flag in FLAGS}
                self.assertEqual(parsed, expected, "raw output: %s" % raw)


class SharedPatternTest(unittest.TestCase):
    def test_the_cpp_extensions_match_the_ones_the_tidy_runner_lints(self):
        # HAS_TIDY_CHANGES decides whether the tidy job runs at all: if run_clang_tidy.py learns a
        # new extension and this does not, a PR touching only such files skips the gate silently.
        import importlib.util
        import sys
        sys.path.insert(0, str(REPO_ROOT / "scripts" / "tidy"))
        self.addCleanup(sys.path.remove, str(REPO_ROOT / "scripts" / "tidy"))
        specs = {}
        for name, path in (("detect_changes", DETECTOR), ("run_clang_tidy", REPO_ROOT / "scripts" / "tidy" / "run_clang_tidy.py")):
            spec = importlib.util.spec_from_file_location(name, str(path))
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            specs[name] = module
        self.assertEqual(specs["detect_changes"].CPP_FILE.pattern, specs["run_clang_tidy"].CPP_FILE.pattern)


if __name__ == "__main__":
    unittest.main()
