#!/usr/bin/env python3
"""Tests for scripts/check_orm_backend_imports.py (issue #692).

The checker keeps the generic ORM layer free of any concrete database backend.
Strategy: build a minimal fake repo (src/orm + src/db) in a temp dir, run the
checker against it, and assert the exit code and message per scenario; a final
scenario runs it against the real repository, which must pass. These are the
twelve scenarios of the bash self-test this replaces, plus the cases the shell
version never covered. Stdlib unittest only, no syntax newer than Python 3.9.

    python3 scripts/tests/test_check_orm_backend_imports.py
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts" / "check_orm_backend_imports.py"

SQLITE_IMPORT = "import storm_db_sqlite;"
CLEAN_MESSAGE = "names no concrete database backend"


class FakeRepoTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = pathlib.Path(tmp.name)
        self.write("src/orm/queryset.cppm", "export module storm_orm_queryset;\nimport storm_db_concept;\n")
        self.write("src/orm/statements/select.cppm", "export module storm_orm_statements_select;\n")
        self.write("src/db/default_connection.cppm",
                   "export module storm_db_default_connection;\nimport storm_db_pool;\n")
        self.write("src/db/pool.cppm", "export module storm_db_pool;\n")
        self.write("src/storm_model.cppm", "export module storm_model;\n")
        # A backend module may of course name itself - it is not checked.
        self.write("src/db/sqlite.cppm", "export module storm_db_sqlite;\nnamespace storm::db::sqlite {}\n")

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def append(self, relative, line):
        with open(str(self.root / relative), "a") as handle:
            handle.write(line + "\n")

    def run_checker(self, root=None, env=None):
        environment = dict(os.environ)
        environment.pop("GITHUB_ACTIONS", None)
        environment.update(env or {})
        return subprocess.run([sys.executable, str(CHECKER), str(root if root is not None else self.root)],
                              capture_output=True, text=True, env=environment)

    def assert_fails(self, text, proc=None):
        proc = proc or self.run_checker()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(text, proc.stderr)
        return proc


class ScenarioTest(FakeRepoTest):
    """The twelve scenarios of the retired bash self-test."""

    def test_clean_tree_passes(self):
        proc = self.run_checker()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(CLEAN_MESSAGE, proc.stdout)

    def test_nested_orm_import_is_flagged_with_its_line(self):
        self.append("src/orm/statements/select.cppm", SQLITE_IMPORT)
        self.assert_fails("src/orm/statements/select.cppm:2")

    def test_qualified_default_is_flagged(self):
        self.append("src/orm/queryset.cppm", "template <class C = storm::db::sqlite::Connection> class X;")
        self.assert_fails("src/orm/queryset.cppm:3")

    def test_default_connection_facade_is_checked(self):
        self.append("src/db/default_connection.cppm", SQLITE_IMPORT)
        self.assert_fails("src/db/default_connection.cppm")

    def test_pool_importing_postgresql_is_flagged(self):
        self.append("src/db/pool.cppm", "import storm_db_postgresql;")
        self.assert_fails("src/db/pool.cppm")

    def test_model_header_importing_defaults_is_flagged(self):
        self.append("src/storm_model.cppm", "import storm_defaults;")
        self.assert_fails("src/storm_model.cppm:2")

    def test_every_violation_is_counted(self):
        self.append("src/orm/queryset.cppm", SQLITE_IMPORT)
        self.append("src/orm/statements/select.cppm", SQLITE_IMPORT)
        self.assert_fails("2 backend reference(s)")

    def test_renamed_listed_file_fails_instead_of_dropping_out_of_the_scan(self):
        (self.root / "src/db/pool.cppm").rename(self.root / "src/db/pool_v2.cppm")
        self.append("src/db/pool_v2.cppm", SQLITE_IMPORT)
        self.assert_fails("listed path missing: src/db/pool.cppm")

    def test_using_namespace_of_a_backend_is_flagged(self):
        self.append("src/orm/queryset.cppm", "using namespace storm::db::sqlite;")
        self.assert_fails("src/orm/queryset.cppm:3")

    def test_importing_defaults_from_a_statement_is_flagged(self):
        self.append("src/orm/statements/select.cppm", "import storm_defaults;")
        self.assert_fails("src/orm/statements/select.cppm:2")

    def test_missing_root_fails(self):
        self.assert_fails("no src/orm", self.run_checker(root="/nonexistent-storm-root"))

    def test_real_repository_passes(self):
        proc = self.run_checker(root=REPO_ROOT)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(CLEAN_MESSAGE, proc.stdout)


class PatternTest(FakeRepoTest):
    def test_each_forbidden_spelling_is_flagged(self):
        for spelling in ("import storm_db_sqlite;", "import storm_db_postgresql;", "x = sqlite::Connection;",
                         "x = postgresql::Connection;", "using namespace storm::db::postgresql;",
                         "import storm;", "import storm_defaults;"):
            with self.subTest(spelling=spelling):
                self.write("src/orm/queryset.cppm", "export module m;\n%s\n" % spelling)
                self.assert_fails("src/orm/queryset.cppm:2")

    def test_look_alike_names_are_not_flagged(self):
        for line in ("import storm_db_concept;", "import storm_orm_queryset;", "import storm_model;",
                     "// unrelated sqlite3_stmt handle", "auto storm_db = 1;"):
            with self.subTest(line=line):
                self.write("src/orm/queryset.cppm", "export module m;\n%s\n" % line)
                self.assertEqual(self.run_checker().returncode, 0)

    def test_a_comment_naming_a_backend_is_flagged_on_purpose(self):
        self.append("src/orm/queryset.cppm", "// default is sqlite::Connection")
        self.assert_fails("src/orm/queryset.cppm:3")

    def test_only_cppm_files_are_scanned(self):
        self.write("src/orm/notes.md", SQLITE_IMPORT + "\n")
        self.write("src/orm/helper.cpp", SQLITE_IMPORT + "\n")
        self.assertEqual(self.run_checker().returncode, 0)

    def test_deeply_nested_orm_file_is_scanned(self):
        self.write("src/orm/a/b/c/deep.cppm", "export module d;\n" + SQLITE_IMPORT + "\n")
        self.assert_fails("src/orm/a/b/c/deep.cppm:2")

    def test_backend_module_may_name_itself(self):
        self.append("src/db/sqlite.cppm", "import storm_db_sqlite;")
        self.assertEqual(self.run_checker().returncode, 0)


class ReportTest(FakeRepoTest):
    def test_violation_line_shows_path_line_number_and_source_text(self):
        self.append("src/orm/queryset.cppm", SQLITE_IMPORT)
        proc = self.assert_fails("✗ src/orm/queryset.cppm:3: %s" % SQLITE_IMPORT)
        self.assertEqual(proc.stdout, "")

    def test_violations_are_reported_in_a_stable_order(self):
        self.append("src/orm/statements/select.cppm", SQLITE_IMPORT)
        self.append("src/orm/queryset.cppm", SQLITE_IMPORT)
        lines = [l for l in self.run_checker().stderr.splitlines() if l.startswith("✗")]
        self.assertEqual([l.split(":")[0] for l in lines],
                         ["✗ src/orm/queryset.cppm", "✗ src/orm/statements/select.cppm"])

    def test_summary_tells_the_fix(self):
        self.append("src/orm/queryset.cppm", SQLITE_IMPORT)
        self.assert_fails("add the default alias to src/storm_defaults.cppm")

    def test_github_actions_annotates_the_offending_line(self):
        self.append("src/orm/queryset.cppm", SQLITE_IMPORT)
        proc = self.run_checker(env={"GITHUB_ACTIONS": "true"})
        self.assertIn("::error file=src/orm/queryset.cppm,line=3::backend-neutral module names a concrete backend",
                      proc.stderr)

    def test_no_annotation_outside_github_actions(self):
        self.append("src/orm/queryset.cppm", SQLITE_IMPORT)
        self.assertNotIn("::error", self.run_checker().stderr)


class FailureModeTest(FakeRepoTest):
    def test_unreadable_file_is_an_error_not_a_pass(self):
        target = self.root / "src/orm/queryset.cppm"
        target.chmod(0o000)
        self.addCleanup(target.chmod, 0o644)
        if os.access(str(target), os.R_OK):
            self.skipTest("running as a user that ignores file modes")
        self.assert_fails("check-orm-backend-imports: cannot read")

    def test_missing_orm_directory_names_the_root(self):
        empty = self.root / "empty"
        empty.mkdir()
        self.assert_fails("no src/orm under: %s" % empty, self.run_checker(root=empty))

    def test_every_listed_neutral_path_is_required(self):
        for relative in ("src/db/default_connection.cppm", "src/db/pool.cppm", "src/storm_model.cppm"):
            with self.subTest(relative=relative):
                path = self.root / relative
                saved = path.read_text()
                path.unlink()
                self.assert_fails("listed path missing: %s" % relative)
                path.write_text(saved)

    def test_non_utf8_bytes_do_not_crash_the_scan(self):
        (self.root / "src/orm/queryset.cppm").write_bytes(b"export module m;\n// caf\xe9\n")
        self.assertEqual(self.run_checker().returncode, 0)

    def test_default_root_is_the_repository_containing_the_script(self):
        proc = subprocess.run([sys.executable, str(CHECKER)], capture_output=True, text=True, cwd=self.root)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(CLEAN_MESSAGE, proc.stdout)


if __name__ == "__main__":
    unittest.main()
