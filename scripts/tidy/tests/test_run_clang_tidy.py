#!/usr/bin/env python3
"""Tests for scripts/tidy/run_clang_tidy.py: prerequisites, --diff mode, PCH stripping (issue #692).

Port of the --diff half of the shell runner. The fake toolchain lives in
tidy_toolchain.py; the skip-list itself is covered by test_clang_tidy_skiplist.py
and --full/--all by test_run_clang_tidy_full.py. Stdlib unittest only, no syntax
newer than Python 3.9.

    python3 scripts/tidy/tests/test_run_clang_tidy.py
"""
import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from tidy_toolchain import (CRASH, ERROR, PCH_COMMAND, SCRIPT, WARNING,  # noqa: E402
                            ToolchainTest)

sys.path.insert(0, str(SCRIPT.parent))
import run_clang_tidy as rct  # noqa: E402


class PrerequisiteTest(ToolchainTest):
    def test_missing_compile_database_fails_with_the_fix(self):
        (self.repo / "build" / "release" / "compile_commands.json").unlink()
        proc = self.run_script("--full")
        self.assert_fails(proc, "compile_commands.json not found at build/release/compile_commands.json")
        self.assertIn("Run: cmake --preset ninja-release", proc.stderr)

    def test_missing_clang_tidy_fails(self):
        (self.root / "clang-p2996" / "build" / "bin" / "clang-tidy").unlink()
        self.assert_fails(self.run_script("--full"), "clang-tidy not found at ../clang-p2996/build/bin/clang-tidy")

    def test_non_executable_clang_tidy_fails(self):
        (self.root / "clang-p2996" / "build" / "bin" / "clang-tidy").chmod(0o644)
        self.assert_fails(self.run_script("--full"), "clang-tidy not found at")

    def test_diff_mode_needs_clang_tidy_diff(self):
        (self.root / "clang-p2996" / "clang-tools-extra" / "clang-tidy" / "tool" / "clang-tidy-diff.py").unlink()
        proc = self.run_script("--diff")
        self.assert_fails(proc, "clang-tidy-diff.py not found at")
        self.assertIn("Required for --diff mode. Re-run with --full or --all, or fix the path.", proc.stderr)

    def test_full_mode_does_not_need_clang_tidy_diff(self):
        (self.root / "clang-p2996" / "clang-tools-extra" / "clang-tidy" / "tool" / "clang-tidy-diff.py").unlink()
        self.assert_passes(self.run_script("--full"), "No staged C++ files")

    def test_missing_clang_tidy_config_fails(self):
        (self.repo / ".clang-tidy").unlink()
        self.assert_fails(self.run_script("--full"), ".clang-tidy config not found at .clang-tidy")

    def test_prerequisites_are_checked_in_order(self):
        (self.repo / "build" / "release" / "compile_commands.json").unlink()
        (self.repo / ".clang-tidy").unlink()
        self.assertIn("compile_commands.json not found", self.run_script("--full").stderr)


class BannerTest(ToolchainTest):
    def test_default_mode_is_diff(self):
        proc = self.run_script("-j", "3")
        self.assertIn("Scope: staged lines only (git diff --cached, lines added/changed)", proc.stdout)

    def test_banner_names_config_build_and_jobs(self):
        out = self.run_script("--full", "-j", "3").stdout
        for line in ("Running clang-tidy using .clang-tidy configuration", "Config file: .clang-tidy",
                     "Build directory: build/release", "Parallel jobs: 3",
                     "Scope: staged files (whole-file scan)", "Mode: Check only (use --fix to apply fixes)"):
            self.assertIn(line, out)

    def test_all_mode_scope(self):
        self.assertIn("Scope: ALL C++ source files (sweep)", self.run_script("--all", "-j", "1").stdout)

    def test_fix_banner(self):
        self.assertIn("Mode: AUTO-FIX enabled", self.run_script("--full", "--fix").stdout)

    def test_unknown_arguments_are_ignored(self):
        self.assert_passes(self.run_script("--full", "--include-tests", "whatever"), "No staged C++ files")

    def test_last_mode_flag_wins(self):
        self.assertIn("Scope: staged files", self.run_script("--all", "--full").stdout)

    def test_non_numeric_job_count_is_rejected(self):
        proc = self.run_script("--full", "-j", "many")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("-j expects a positive integer", proc.stderr)

    def test_dangling_job_flag_is_rejected(self):
        self.assertEqual(self.run_script("--full", "-j").returncode, 2)


class DiffModeTest(ToolchainTest):
    def setUp(self):
        super().setUp()
        self.stage("src/orm/fields.cppm")

    def test_nothing_relevant_is_a_clean_pass(self):
        self.canned_diff(1, "No relevant changes found.\n")
        self.assert_passes(self.run_script("--diff"), "✅ clang-tidy --diff: no staged C++ lines to check")

    def test_clean_run_passes(self):
        proc = self.run_script("--diff")
        self.assert_passes(proc, "✅ clang-tidy --diff: staged lines are clean")
        self.assertIn("Warnings on staged lines: 0", proc.stdout)
        self.assertIn("Errors in staged files:   0", proc.stdout)

    def test_a_warning_blocks(self):
        self.canned_diff(1, WARNING + "\n")
        proc = self.run_script("--diff")
        self.assert_fails(proc, "❌ clang-tidy --diff: new warnings/errors on staged lines")
        self.assertIn("Warnings on staged lines: 1", proc.stdout)

    def test_an_error_blocks(self):
        self.canned_diff(1, ERROR + "\n")
        proc = self.run_script("--diff")
        self.assert_fails(proc, "❌ clang-tidy --diff: new warnings/errors on staged lines")
        self.assertIn("Errors in staged files:   1", proc.stdout)

    def test_warnings_and_errors_are_counted_the_same_way(self):
        # #550 gap 1: both are plain counts of matching lines, neither filtered.
        self.canned_diff(1, "\n".join([WARNING, WARNING.replace(":70:", ":71:"), ERROR, ERROR.replace(":3:", ":4:")]) + "\n")
        proc = self.run_script("--diff")
        self.assertIn("Warnings on staged lines: 2", proc.stdout)
        self.assertIn("Errors in staged files:   2", proc.stdout)

    def test_a_concept_error_on_a_real_file_still_blocks(self):
        # #550 gap 2: no message-text regex may swallow this.
        self.canned_diff(1, "src/orm/fields.cppm:9:1: error: unknown type name 'concept' [clang-diagnostic-error]\n")
        self.assert_fails(self.run_script("--diff"), "Errors in staged files:   1")

    def test_a_crash_is_a_failure_not_a_clean_pass(self):
        self.canned_diff(1, "Stack dump:\n" + CRASH + "\n")
        proc = self.run_script("--diff")
        self.assert_fails(proc, "❌ clang-tidy --diff: crashed on 1 translation unit(s)")
        self.assertIn("Diagnostics are incomplete — treating as failure rather than clean.", proc.stdout)
        self.assertNotIn("staged lines are clean", proc.stdout)

    def test_a_tidy_timeout_is_a_failure_not_a_clean_pass(self):
        # clang-tidy-diff.py SIGKILLs a TU after -timeout and prints only these lines: no
        # warning, no error, no crash marker - so it used to read as "clean" (#910).
        self.canned_diff(1, "Terminated by signal 9 : clang-tidy -p x src/orm/fields.cppm\n"
                            "Terminated by timeout: clang-tidy -p x src/orm/fields.cppm\n")
        proc = self.run_script("--diff")
        self.assert_fails(proc, "❌ clang-tidy --diff: clang-tidy was terminated")
        self.assertNotIn("staged lines are clean", proc.stdout)

    def test_a_kill_by_signal_alone_is_a_failure(self):
        self.canned_diff(1, "Terminated by signal 9 : clang-tidy -p x src/orm/fields.cppm\n")
        self.assert_fails(self.run_script("--diff"), "clang-tidy was terminated")

    def test_every_crash_is_counted(self):
        self.canned_diff(1, CRASH + "\n" + CRASH + "\n")
        self.assert_fails(self.run_script("--diff"), "crashed on 2 translation unit(s)")

    def test_tool_output_is_echoed(self):
        self.canned_diff(1, WARNING + "\n")
        self.assertIn(WARNING, self.run_script("--diff").stdout)

    def test_arguments_reach_clang_tidy_diff(self):
        self.run_script("--diff", "-j", "5")
        (call,) = self.calls("diff_calls")
        self.assertEqual(call, "-clang-tidy-binary ../clang-p2996/build/bin/clang-tidy -p1 -path build/release "
                               "-iregex .*\\.(cpp|cppm|h|hpp) -j 5 -timeout 240 -extra-arg=-DSTORM_CLANG_TIDY -quiet")

    def test_fix_adds_the_fix_flag(self):
        self.run_script("--diff", "--fix")
        self.assertTrue(self.calls("diff_calls")[0].endswith("-quiet -fix"))

    def test_no_config_file_is_forced(self):
        # Forcing the root .clang-tidy defeats tests/.clang-tidy overrides (#326).
        self.run_script("--diff")
        self.assertNotIn("-config-file", self.calls("diff_calls")[0])

    def test_skip_listed_sections_never_reach_clang_tidy_diff(self):
        self.stage("shared/models.h")
        self.stage("src/orm/generator.cppm")
        self.run_script("--diff")
        stdin = (self.log / "diff_stdin.1").read_text()
        self.assertIn("src/orm/fields.cppm", stdin)
        self.assertNotIn("shared/models.h", stdin)
        self.assertNotIn("generator.cppm", stdin)

    def test_the_staged_diff_is_what_is_piped_in(self):
        self.run_script("--diff")
        stdin = (self.log / "diff_stdin.1").read_text()
        self.assertTrue(stdin.startswith("diff --git a/src/orm/fields.cppm b/src/orm/fields.cppm"), stdin)
        self.assertIn("+int x;", stdin)

    def test_unstaged_edits_are_not_checked(self):
        (self.repo / "src" / "orm" / "fields.cppm").write_text("int unstaged;\n")
        self.run_script("--diff")
        self.assertNotIn("unstaged", (self.log / "diff_stdin.1").read_text())


class BaseRefTest(ToolchainTest):
    """`--base <ref>` lints what a branch changed since <ref> (#910): CI has nothing staged."""

    def setUp(self):
        super().setUp()
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.stage("src/orm/fields.cppm", "int base;\n")
        self.git("commit", "-q", "-m", "base")
        self.git("tag", "base")
        self.stage("src/orm/fields.cppm", "int base;\nint changed;\n")
        self.git("commit", "-q", "-m", "change")

    def test_the_commits_since_base_are_what_is_piped_in(self):
        self.run_script("--diff", "--base", "base")
        stdin = (self.log / "diff_stdin.1").read_text()
        self.assertTrue(stdin.startswith("diff --git a/src/orm/fields.cppm b/src/orm/fields.cppm"), stdin)
        self.assertIn("+int changed;", stdin)
        self.assertNotIn("+int base;", stdin)

    def test_a_clean_tree_with_nothing_staged_is_still_linted(self):
        self.canned_diff(1, WARNING + "\n")
        self.assert_fails(self.run_script("--diff", "--base", "base"), "new warnings/errors on changed lines")

    def test_a_branch_with_no_cpp_changes_is_a_clean_pass(self):
        self.canned_diff(1, "No relevant changes found.\n")
        self.assert_passes(self.run_script("--diff", "--base", "HEAD"), "no changed C++ lines to check")

    def test_staged_edits_are_not_what_is_checked(self):
        self.stage("src/orm/fields.cppm", "int staged_only;\n")
        self.run_script("--diff", "--base", "base")
        self.assertNotIn("staged_only", (self.log / "diff_stdin.1").read_text())

    def test_banner_names_the_base(self):
        self.assertIn("Scope: lines changed since base (git diff base...HEAD)",
                      self.run_script("--diff", "--base", "base").stdout)

    def test_the_staged_default_banner_is_unchanged_without_base(self):
        self.assertIn("Scope: staged lines only", self.run_script("-j", "1").stdout)

    def test_full_mode_lints_the_files_changed_since_base(self):
        self.canned_tidy("src/orm/fields.cppm", WARNING + "\n")
        self.assert_fails(self.run_script("--full", "--base", "base"), "fields.cppm")
        self.assertEqual(len(self.calls("tidy_calls")), 1)

    def test_the_unshallow_hint_is_shown_for_a_base(self):
        self.git("checkout", "-q", "--orphan", "other")
        proc = self.run_script("--diff", "--base", "base")
        self.assertIn("git fetch --unshallow", proc.stderr)

    def test_a_dangling_base_flag_is_rejected(self):
        proc = self.run_script("--diff", "--base")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("--base expects a git ref", proc.stderr)

    def test_an_unknown_ref_is_a_failure_not_a_clean_pass(self):
        proc = self.run_script("--diff", "--base", "no-such-ref")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("no-such-ref", proc.stderr)

    def test_base_cannot_be_combined_with_fix(self):
        proc = self.run_script("--diff", "--base", "base", "--fix")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("--base cannot be combined with --fix", proc.stderr)


class BaseRefFailClosedTest(ToolchainTest):
    """An unusable base must never read as a clean pass (#910): CI has nothing staged."""

    def setUp(self):
        super().setUp()
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.stage("src/orm/fields.cppm", "int base;\n")
        self.git("commit", "-q", "-m", "base")
        self.git("tag", "base")
        self.git("checkout", "-q", "--orphan", "other")
        self.stage("src/orm/fields.cppm", "int other;\n")
        self.git("commit", "-q", "-m", "unrelated history")

    def test_a_base_with_no_merge_base_fails_diff_mode(self):
        proc = self.run_script("--diff", "--base", "base")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("no merge base", proc.stderr)

    def test_a_base_with_no_merge_base_fails_full_mode(self):
        proc = self.run_script("--full", "--base", "base")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertNotIn("clang-tidy skipped", proc.stdout)

    def test_the_equals_spelling_is_rejected_not_ignored(self):
        proc = self.run_script("--diff", "--base=base")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("--base expects", proc.stderr)

    def test_base_with_all_is_rejected(self):
        proc = self.run_script("--all", "--base", "base")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("--base has no effect with --all", proc.stderr)

    def test_full_mode_skips_files_the_branch_deleted(self):
        self.git("checkout", "-q", "-B", "work", "base")
        self.git("rm", "-q", "src/orm/fields.cppm")
        self.git("commit", "-q", "-m", "delete")
        proc = self.run_script("--full", "--base", "base")
        self.assertEqual(self.calls("tidy_calls"), [], proc.stdout + proc.stderr)


class FragmentDiffTest(ToolchainTest):
    """A staged #include fragment (#671) is linted through its parent TU."""

    FRAGMENT = "src/orm/statements/select_query.hpp"
    PARENT = "src/orm/statements/select.cppm"
    FILTER = ("-header-filter=.*/(src/orm/statements/select_query\\.hpp)$ "
              '-line-filter=[{"name": "src/orm/statements/select_query.hpp", "lines": [[1, 1]]}]')

    def setUp(self):
        super().setUp()
        self.stage(self.FRAGMENT)
        self.canned_diff(1, "No relevant changes found.\n")  # the fragment section was dropped

    def test_the_parent_is_linted_with_the_fragment_lines_only(self):
        self.run_script("--diff")
        self.assertEqual(self.calls("tidy_calls"), ["-p build/release -quiet -extra-arg=-DSTORM_CLANG_TIDY %s %s" % (self.FILTER, self.PARENT)])

    def test_a_warning_in_a_fragment_blocks(self):
        self.canned_tidy(self.PARENT, WARNING.replace("src/orm/fields.cppm", self.FRAGMENT) + "\n")
        proc = self.run_script("--diff")
        self.assert_fails(proc, "❌ clang-tidy --diff: new warnings/errors on staged lines")
        self.assertIn("Warnings on staged lines: 1", proc.stdout)

    def test_a_clean_fragment_is_not_reported_as_nothing_to_check(self):
        proc = self.run_script("--diff")
        self.assert_passes(proc, "✅ clang-tidy --diff: staged lines are clean")
        self.assertNotIn("no staged C++ lines to check", proc.stdout)

    def test_a_silent_nonzero_exit_blocks(self):
        proc = self.run_script("--diff", extra_env={"FAKE_TIDY_EXIT": "3"})
        self.assert_fails(proc, "Errors in staged files:   1")
        self.assertIn("clang-tidy exited 3 with no diagnostic", proc.stdout)

    def test_fix_reaches_the_parent_run(self):
        self.run_script("--diff", "--fix")
        self.assertIn("-quiet -extra-arg=-DSTORM_CLANG_TIDY %s -fix %s" % (self.FILTER, self.PARENT), self.calls("tidy_calls")[0])


class DiffFixTest(ToolchainTest):
    def setUp(self):
        super().setUp()
        self.stage("src/orm/fields.cppm")

    def test_fixed_warnings_are_rechecked_and_pass(self):
        self.canned_diff(1, WARNING + "\n")
        proc = self.run_script("--diff", "--fix")
        self.assert_passes(proc, "🔄 Re-checking after auto-fix...")
        self.assertIn("✅ clang-tidy --diff: staged lines are clean", proc.stdout)
        first, second = self.calls("diff_calls")
        self.assertTrue(first.endswith("-fix"))
        self.assertFalse(second.endswith("-fix"), "the re-check must be check-only")

    def test_warnings_that_survive_the_fix_still_block(self):
        self.canned_diff(1, WARNING + "\n")
        self.canned_diff(2, WARNING + "\n")
        proc = self.run_script("--diff", "--fix")
        self.assert_fails(proc, "❌ clang-tidy --diff: new warnings/errors on staged lines")
        self.assertEqual(len(self.calls("diff_calls")), 2)

    def test_a_clean_fix_run_does_not_recheck(self):
        self.run_script("--diff", "--fix")
        self.assertEqual(len(self.calls("diff_calls")), 1)

    def test_check_only_runs_do_not_recheck(self):
        self.canned_diff(1, WARNING + "\n")
        self.run_script("--diff")
        self.assertEqual(len(self.calls("diff_calls")), 1)

    def test_fixes_are_restaged(self):
        self.git("-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", "commit", "-qm", "base")
        edited = self.repo / "src" / "orm" / "fields.cppm"
        edited.write_text("int y;\n")
        self.git("add", "src/orm/fields.cppm")
        self.canned_diff(1, WARNING + "\n")
        self.run_script("--diff", "--fix", extra_env={"FAKE_DIFF_TOUCH": str(edited)})
        self.assertIn("// fixed", self.git("diff", "--cached"))

    def test_a_crash_is_not_rechecked(self):
        self.canned_diff(1, CRASH + "\n")
        self.assert_fails(self.run_script("--diff", "--fix"), "crashed on 1 translation unit(s)")
        self.assertEqual(len(self.calls("diff_calls")), 1)


class PchStripTest(ToolchainTest):
    def setUp(self):
        super().setUp()
        self.stage("src/orm/fields.cppm")

    def test_pch_flags_are_stripped_from_a_temp_copy(self):
        self.write_database([PCH_COMMAND])
        proc = self.run_script("--diff")
        self.assertIn("PCH flags stripped from a temp copy of compile_commands.json", proc.stdout)
        path = self.calls("diff_calls")[0].split("-path ")[1].split(" ")[0]
        self.assertNotEqual(path, "build/release")
        seen = json.loads((self.log / "database.1").read_text())[0]["command"]
        self.assertNotIn("include-pch", seen)
        self.assertNotIn("-Winvalid-pch", seen)
        self.assertIn("-c", seen)
        self.assertIn("t.cpp", seen)

    def test_the_original_database_is_left_alone(self):
        self.write_database([PCH_COMMAND])
        before = (self.repo / "build" / "release" / "compile_commands.json").read_text()
        self.run_script("--diff")
        self.assertEqual((self.repo / "build" / "release" / "compile_commands.json").read_text(), before)

    def test_the_temp_copy_is_removed_afterwards(self):
        self.write_database([PCH_COMMAND])
        self.run_script("--diff")
        path = self.calls("diff_calls")[0].split("-path ")[1].split(" ")[0]
        self.assertFalse(pathlib.Path(path).exists())

    def test_no_pch_means_the_real_database_is_used(self):
        self.write_database(["clang++ -c t.cpp"])
        proc = self.run_script("--diff")
        self.assertNotIn("PCH flags stripped", proc.stdout)
        self.assertIn("-path build/release ", self.calls("diff_calls")[0])

    def test_full_mode_points_clang_tidy_at_the_stripped_copy(self):
        self.write_database([PCH_COMMAND])
        self.run_script("--full")
        (call,) = self.calls("tidy_calls")
        self.assertTrue(call.startswith("-p "), call)
        self.assertNotIn("-p build/release", call)

    def test_strip_handles_the_arguments_form(self):
        argv = ["clang++", "-Xclang", "-include-pch", "-Xclang", "a.pch", "-Xclang", "-include", "-Xclang", "a.h",
                "-Winvalid-pch", "-c", "t.cpp"]
        self.assertEqual(rct.strip_pch(argv), ["clang++", "-c", "t.cpp"])

    def test_strip_keeps_unrelated_xclang_pairs(self):
        argv = ["clang++", "-Xclang", "-fmodules-embed-all-files", "-c", "t.cpp"]
        self.assertEqual(rct.strip_pch(argv), argv)

    def test_strip_database_rewrites_both_entry_forms(self):
        source = self.root / "in.json"
        target = self.root / "out.json"
        source.write_text(json.dumps([
            {"directory": "d", "file": "a.cpp", "command": PCH_COMMAND},
            {"directory": "d", "file": "b.cpp", "arguments": ["c++", "-Xclang", "-include-pch", "-Xclang", "p", "-c", "b.cpp"]},
            {"directory": "d", "file": "c.cpp"},
        ]))
        rct.strip_pch_database(source, target)
        a, b, c = json.loads(target.read_text())
        self.assertNotIn("include-pch", a["command"])
        self.assertEqual(b["arguments"], ["c++", "-c", "b.cpp"])
        self.assertEqual(c, {"directory": "d", "file": "c.cpp"})



if __name__ == "__main__":
    unittest.main()
