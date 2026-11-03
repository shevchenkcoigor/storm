#!/usr/bin/env python3
"""Cold-cache CPM policy test (issue #709).

CPM shallow-clones (`git clone --depth 1 --no-single-branch` + `git checkout <tag>`) any
GIT_TAG that is not a commit hash, and derives GIT_TAG v<VERSION> when none is given. A
depth-1 clone only carries branch tips, so against GitHub a tag that is NOT a branch tip
(stduuid v1.2.3) is missing and a fresh worktree (empty .cache/CPM) cannot configure. Warm
caches hide it, so nothing red ever points at the declaration. A hash pin is already
full-cloned by CPM; it carries GIT_SHALLOW FALSE too only to keep this policy uniform.

A behavioural fixture cannot catch this: a local `git clone` (file:// or path) hands over the
tags regardless, so the failure reproduces only against GitHub. The test is therefore a policy
check over every tracked CMake file: each CPMAddPackage/CPMFindPackage/CPMDeclarePackage/
FetchContent_Declare call that pins a version must end with `GIT_SHALLOW FALSE` (last value
wins). It also fails when it finds NO such call, so a moved file cannot make it a silent pass.
Known limitation: a `)` inside an argument before the pin ends the call early.

Needs git only; no network, no cmake. The behavioural check against GitHub is ci.yml's
`cold-cpm-configure` job (#702): a real configure with no CPM cache. Stdlib unittest, no
syntax newer than Python 3.7.

    python3 scripts/ci/tests/test_cpm_cold_cache.py
"""
import pathlib
import re
import subprocess
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
CPM_BOOTSTRAP = "cmake/cpm.cmake"  # only bootstraps CPM, declares no package

CALL = re.compile(r"\b(?:cpmaddpackage|cpmfindpackage|cpmdeclarepackage|fetchcontent_declare)"
                  r"\s*\((.*?)\)", re.IGNORECASE | re.DOTALL)
COMMENT = re.compile(r"#[^\n]*")
PIN = re.compile(r"GIT_TAG |VERSION |gh:[^ ]*@")
SHALLOW = re.compile(r"GIT_SHALLOW [A-Za-z0-9_]+")
NAME = re.compile(r"NAME ([^ ]+)")


def calls_in(text):
    """Each package call as one whitespace-collapsed line. Comments are stripped first so a
    `# GIT_SHALLOW FALSE` note cannot satisfy the check."""
    return [" ".join(match.split()) for match in CALL.findall(COMMENT.sub("", text))]


def is_pinned(call):
    return PIN.search(call) is not None


def last_shallow_is_false(call):
    values = SHALLOW.findall(call)
    return bool(values) and values[-1] == "GIT_SHALLOW FALSE"


def label(call):
    found = NAME.search(call)
    return found.group(1) if found else call[:40]


def tracked_cmake_files():
    listing = subprocess.run(["git", "-C", str(REPO_ROOT), "ls-files", "*.cmake", "*CMakeLists.txt"],
                             stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
    return [name for name in listing.splitlines() if name and name != CPM_BOOTSTRAP]


class RepoPolicyTest(unittest.TestCase):
    def test_every_pinned_call_ends_with_shallow_false(self):
        checked = 0
        for name in tracked_cmake_files():
            text = (REPO_ROOT / name).read_text(encoding="utf-8")
            for call in filter(is_pinned, calls_in(text)):
                checked += 1
                with self.subTest(file=name, package=label(call)):
                    self.assertTrue(last_shallow_is_false(call),
                                    "pinned without a final GIT_SHALLOW FALSE "
                                    "(a cold cache cannot fetch a non-tip tag)")
        self.assertGreater(checked, 0, "found no pinned CPM/FetchContent call — the file scan is stale")


class CheckerTest(unittest.TestCase):
    """The policy check itself, on inputs built to fail it — a checker that cannot fail proves nothing."""

    def pinned(self, text):
        return [call for call in calls_in(text) if is_pinned(call)]

    def test_pin_without_shallow_is_flagged(self):
        (call,) = self.pinned("CPMAddPackage(NAME a GITHUB_REPOSITORY x/y GIT_TAG v1)")
        self.assertFalse(last_shallow_is_false(call))

    def test_shallow_true_is_flagged(self):
        (call,) = self.pinned("CPMAddPackage(NAME a GIT_TAG v1 GIT_SHALLOW TRUE)")
        self.assertFalse(last_shallow_is_false(call))

    def test_last_shallow_value_wins(self):
        (late_false,) = self.pinned("CPMAddPackage(NAME a GIT_TAG v1 GIT_SHALLOW TRUE GIT_SHALLOW FALSE)")
        (late_true,) = self.pinned("CPMAddPackage(NAME a GIT_TAG v1 GIT_SHALLOW FALSE GIT_SHALLOW TRUE)")
        self.assertTrue(last_shallow_is_false(late_false))
        self.assertFalse(last_shallow_is_false(late_true))

    def test_comment_cannot_satisfy_the_check(self):
        (call,) = self.pinned("CPMAddPackage(NAME a GIT_TAG v1 # GIT_SHALLOW FALSE\n)")
        self.assertFalse(last_shallow_is_false(call))

    def test_multiline_call_and_case_are_handled(self):
        (call,) = self.pinned("cpmaddpackage(\n  NAME a\n  VERSION 1.2\n  GIT_SHALLOW FALSE\n)")
        self.assertTrue(last_shallow_is_false(call))
        self.assertEqual(label(call), "a")

    def test_gh_shorthand_counts_as_a_pin(self):
        self.assertEqual(len(self.pinned("CPMAddPackage(gh:owner/repo@v1)")), 1)

    def test_unpinned_call_is_not_checked(self):
        self.assertEqual(self.pinned("CPMAddPackage(NAME a SOURCE_DIR /tmp/a)"), [])

    def test_all_four_declaration_commands_are_scanned(self):
        text = "\n".join("%s(NAME n%d GIT_TAG v1)" % (command, index) for index, command in enumerate(
            ("CPMAddPackage", "CPMFindPackage", "CPMDeclarePackage", "FetchContent_Declare")))
        self.assertEqual(len(self.pinned(text)), 4)

    def test_file_with_no_calls_yields_nothing(self):
        self.assertEqual(calls_in("add_library(x)\n"), [])


if __name__ == "__main__":
    unittest.main()
