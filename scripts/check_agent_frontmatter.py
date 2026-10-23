#!/usr/bin/env python3
"""Validate Claude Code agent frontmatter (issue #543).

Claude Code parses each .claude/agents/*.md file's YAML frontmatter to
register the agent. In YAML, an unquoted scalar ends at the first line
starting in column 0 — so a `description:` written across real newlines
terminates early, the next line is read as a key, the parse fails, and the
agent is dropped SILENTLY: it simply never appears in the available-agents
list. That is what made storm-sql-reviewer and storm-buildsystem-reviewer
undispatchable from the day they merged, leaving CLAUDE.md rule #13
partly unenforceable.

The fix, and the invariant this script enforces: keep `description:` on ONE
physical line, escaping newlines as literal \\n (the form used by every agent
file that loads correctly).

Note this is deliberately NOT a strict YAML validation — the descriptions
contain unquoted ':' characters that a strict parser rejects, yet Claude
Code's lenient parser accepts them. The single-line span is the property
that actually distinguishes loading from non-loading files.

    scripts/check_agent_frontmatter.py [agents-dir]   (default: .claude/agents)

Exit: 0 = all agent files valid, 1 = at least one invalid.
"""
import os
import pathlib
import re
import sys

DEFAULT_AGENTS_DIR = pathlib.Path(__file__).resolve().parent.parent / ".claude" / "agents"

# A frontmatter key is a column-0 `lowercase-word:` — deliberately narrow so it
# does NOT match the prose inside a broken multi-line description: `Context:`
# and `<example>` fail the lowercase/leading-char test, and `user:`/`assistant:`
# are excluded by name. Without those exclusions a 28-line description measures
# as 3, understating the span; with a fixed key whitelist instead, an unlisted
# key (`origin:`) would make a VALID file measure to the closing '---' and
# false-positive.
FRONTMATTER_KEY = re.compile(r"^(?!(?:user|assistant):)[a-z][a-z0-9_-]*:")

FIX_HINT = """\
    Claude Code will silently DROP this agent; it will not appear
    in the available-agents list at all.
    Fix: fold the description onto a single line, writing newlines
    as literal \\n escapes (see reviewer.md for the working form)."""


def problems_in(text):
    """Return the frontmatter problems in one agent file's text ([] if valid)."""
    # Only \n ends a physical line (str.splitlines would also split on form
    # feeds and Unicode separators inside prose); a CRLF file is still checked.
    lines = [line.removesuffix("\r") for line in text.split("\n")]
    # A file without opening frontmatter is not an agent (e.g. rule-standards.md,
    # which is shared prose included by the agent files). Skip it.
    if not lines or lines[0] != "---":
        return []

    fm_end = next((i for i in range(1, len(lines)) if lines[i] == "---"), None)
    if fm_end is None:
        return ["frontmatter opened with '---' but never closed"]

    desc_start = next(
        (i for i in range(1, fm_end) if lines[i].startswith("description:")), None)
    if desc_start is None:
        return ["no 'description:' field in frontmatter — the agent cannot be registered"]

    # The description spans every line until the next frontmatter key or the
    # closing '---'. Anything past the first line means the YAML scalar
    # terminates early and the parse breaks.
    desc_end = next(
        (i for i in range(desc_start + 1, fm_end) if FRONTMATTER_KEY.match(lines[i])),
        fm_end)
    span = desc_end - desc_start
    if span > 1:
        return [f"description: spans {span} physical lines — must be 1"]
    return []


def report(path, problem):
    # Under GitHub Actions, emit a workflow annotation so the failure surfaces
    # on the file itself in the PR diff rather than only in the raw job log.
    if os.environ.get("GITHUB_ACTIONS"):
        cwd = pathlib.Path.cwd()
        shown = path.relative_to(cwd) if path.is_relative_to(cwd) else path
        print(f"::error file={shown}::{problem}", file=sys.stderr)
    print(f"✗ {path.name}: {problem}", file=sys.stderr)
    if "spans" in problem:
        print(FIX_HINT, file=sys.stderr)


def main(argv):
    agents_dir = pathlib.Path(argv[1]).absolute() if len(argv) > 1 else DEFAULT_AGENTS_DIR
    if not agents_dir.is_dir():
        print(f"check-agent-frontmatter: no such directory: {agents_dir}", file=sys.stderr)
        return 1

    invalid = 0
    # Hidden files are skipped, as the shell glob `*.md` this replaced did.
    for path in sorted(p for p in agents_dir.glob("*.md") if not p.name.startswith(".")):
        try:
            problems = problems_in(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError) as error:
            problems = [f"cannot read: {error}"]
        for problem in problems:
            report(path, problem)
        invalid += bool(problems)

    if invalid:
        print(f"\ncheck-agent-frontmatter: {invalid} agent file(s) invalid", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
