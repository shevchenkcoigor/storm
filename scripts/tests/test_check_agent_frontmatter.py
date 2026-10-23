#!/usr/bin/env python3
"""Tests for scripts/check_agent_frontmatter.py (issue #543).

The validator guards against the bug that made storm-sql-reviewer and
storm-buildsystem-reviewer silently undispatchable: a `description:` field
spanning multiple physical lines breaks Claude Code's frontmatter parse, and
the agent is dropped with no warning.

Two layers: `problems_in` is exercised directly on fixture text, and the CLI
is run as a subprocess for what only a real process shows — exit codes and
which stream the GitHub Actions annotation lands on. Stdlib unittest only, so
CI and the pre-commit hook need nothing beyond python3.

    python3 scripts/tests/test_check_agent_frontmatter.py
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts" / "check_agent_frontmatter.py"

sys.path.insert(0, str(CHECKER.parent))
from check_agent_frontmatter import problems_in  # noqa: E402

# A single-line, \n-escaped description: the form that loads.
GOOD_DESC = r'description: Does a thing. Examples:\n\n<example>\nContext: A context.\nuser: "do it"\n</example>'
# The same description written across real newlines: the #543 bug, 6 lines.
MULTILINE_DESC = GOOD_DESC.replace(r"\n", "\n")


def agent(description=GOOD_DESC, trailing_key="model: opus"):
    """Agent file text; description=None omits the field entirely."""
    fields = ["name: fixture-agent", description, trailing_key]
    frontmatter = "\n".join(f for f in fields if f is not None)
    return f"---\n{frontmatter}\n---\n\nBody text here.\n"


class ProblemsInTest(unittest.TestCase):
    def test_single_line_description_is_valid(self):
        self.assertEqual(problems_in(agent()), [])

    def test_multiline_description_reports_true_span(self):
        # Prose inside the broken description (`Context:`, `user:`) must not
        # truncate the measurement: all 6 lines are counted.
        [problem] = problems_in(agent(MULTILINE_DESC))
        self.assertIn("spans 6 physical lines", problem)

    def test_missing_description_is_reported(self):
        [problem] = problems_in(agent(description=None))
        self.assertIn("no 'description:' field", problem)

    def test_unclosed_frontmatter_is_reported(self):
        [problem] = problems_in("---\nname: x\n" + GOOD_DESC + "\n")
        self.assertIn("never closed", problem)

    def test_non_agent_markdown_is_ignored(self):
        self.assertEqual(problems_in("# Just a markdown doc\n\nNo frontmatter.\n"), [])

    def test_uncommon_trailing_key_is_valid(self):
        # Writing the span scan as a fixed key whitelist regresses exactly
        # here: an unlisted key would let a valid span run to the closing '---'.
        self.assertEqual(problems_in(agent(trailing_key="origin: ECC")), [])

    def test_description_as_last_key_is_valid(self):
        self.assertEqual(problems_in(agent(trailing_key=None)), [])

    def test_crlf_multiline_description_is_reported(self):
        # YAML accepts CRLF, so a CRLF file breaks the same way an LF one does.
        [problem] = problems_in(agent(MULTILINE_DESC).replace("\n", "\r\n"))
        self.assertIn("spans 6 physical lines", problem)

    def test_unicode_line_separators_do_not_split_a_line(self):
        # Only \n ends a physical line: a form feed or NEL inside prose must
        # not count as a second description line.
        self.assertEqual(problems_in(agent(GOOD_DESC + " \x0c \x85   end")), [])


class CliTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def write(self, name, text):
        (self.dir / name).write_text(text)

    def run_checker(self, directory=None, github_actions=False):
        env = {k: v for k, v in os.environ.items() if k != "GITHUB_ACTIONS"}
        if github_actions:
            env["GITHUB_ACTIONS"] = "true"
        return subprocess.run(
            [sys.executable, str(CHECKER), str(directory or self.dir)],
            capture_output=True, text=True, env=env, check=False,
        )

    def test_all_valid_exits_zero(self):
        self.write("good.md", agent())
        self.write("also-good.md", agent())
        self.write("rule-standards.md", "# Shared prose, not an agent\n")
        self.assertEqual(self.run_checker().returncode, 0)

    def test_empty_dir_exits_zero(self):
        self.assertEqual(self.run_checker().returncode, 0)

    def test_missing_dir_exits_nonzero(self):
        result = self.run_checker(self.dir / "absent")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no such directory", result.stderr)

    def test_invalid_file_fails_names_file_and_explains_fix(self):
        self.write("good.md", agent())
        self.write("bad.md", agent(MULTILINE_DESC))
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("bad.md", result.stderr)
        self.assertIn(r"\n", result.stderr)
        self.assertIn("1 agent file(s) invalid", result.stderr)

    def test_hidden_md_files_are_ignored(self):
        self.write(".scratch.md", agent(MULTILINE_DESC))
        self.assertEqual(self.run_checker().returncode, 0)

    def test_unreadable_file_is_reported_per_file(self):
        (self.dir / "latin1.md").write_bytes(b"---\nname: caf\xe9\n---\n")
        result = self.run_checker(github_actions=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("::error file=", result.stderr)
        self.assertIn("latin1.md: cannot read", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_gha_annotation_is_relative_to_cwd(self):
        self.write("bad.md", agent(MULTILINE_DESC))
        env = {**os.environ, "GITHUB_ACTIONS": "true"}
        result = subprocess.run(
            [sys.executable, str(CHECKER), "."], cwd=self.dir,
            capture_output=True, text=True, env=env, check=False,
        )
        self.assertIn("::error file=bad.md::", result.stderr)

    def test_gha_annotation_goes_to_stderr_only(self):
        self.write("bad.md", agent(MULTILINE_DESC))
        result = self.run_checker(github_actions=True)
        self.assertIn("::error file=", result.stderr)
        self.assertNotIn("::error", result.stdout)

    def test_no_annotation_outside_gha(self):
        self.write("bad.md", agent(MULTILINE_DESC))
        result = self.run_checker()
        self.assertNotIn("::error", result.stderr + result.stdout)
        self.assertIn("bad.md", result.stderr)

    def test_real_repo_agents_pass(self):
        # The regression guard proper: the repo's own agents must stay clean.
        result = self.run_checker(REPO_ROOT / ".claude" / "agents")
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
