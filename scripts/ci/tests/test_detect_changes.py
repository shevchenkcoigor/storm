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

FLAGS = ("SRC", "TEST", "CPP", "CMAKE", "BENCH")

# (description, changed files, flags that must be true)
SCENARIOS = (
    ("src/foo.cppm sets SRC + CPP", ["src/foo.cppm"], {"SRC", "CPP"}),
    ("tests/test_foo.cpp sets TEST + CPP", ["tests/test_foo.cpp"], {"TEST", "CPP"}),
    # A shared header outside src/ and tests/ is still compiled: CPP must catch it.
    ("include/storm/storm.h sets CPP only", ["include/storm/storm.h"], {"CPP"}),
    ("cmake/foo.cmake sets CMAKE only", ["cmake/foo.cmake"], {"CMAKE"}),
    ("src/CMakeLists.txt sets SRC + CMAKE", ["src/CMakeLists.txt"], {"SRC", "CMAKE"}),
    ("benchmarks/foo.cpp sets BENCH + CPP", ["benchmarks/foo.cpp"], {"BENCH", "CPP"}),
    ("docs/README.md sets nothing", ["docs/README.md"], set()),
    ("empty input sets nothing", [], set()),
    ("docs + src together still sets SRC + CPP", ["docs/README.md", "src/foo.cppm"], {"SRC", "CPP"}),
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


if __name__ == "__main__":
    unittest.main()
