#!/bin/bash
# SessionStart hook: wire git's pre-commit hook (issue #651).
#
# `core.hooksPath` is local config that a clone never carries, and the only other
# place that sets it is CMakeLists.txt, as a side effect of a successful cmake
# configure. A session that has not configured a build (a fresh clone or
# worktree, or a machine with no ../clang-p2996) would therefore commit with
# commit.sh — format, clang-tidy, tests, and the two
# self-tests it runs even for an otherwise no-op commit (#543, #550) — never
# firing, with nothing in the output to say so. This hook sets it on every
# session start.
#
# Covered by scripts/commit/tests/test_session_start_hook.py.

set -euo pipefail

# CLAUDE_PROJECT_DIR is set by Claude Code for hooks; fall back to this
# script's own location so manual invocation (e.g. this skill's own
# validation step) doesn't fail on an unbound variable.
project_dir="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# Empty unless the wiring failed; reported below on whichever channel is free.
wiring_warning=""

# `git -C <dir> config` walks UP to an enclosing repository, so a project_dir
# that is not itself a repo root would silently re-point an ANCESTOR repo's
# hooks. Resolve the top level first and refuse to write unless it is this
# directory. Both sides are compared physically (`pwd -P` vs git's already
# physical --show-toplevel) so a symlinked path is not mistaken for a mismatch.
project_real="$(cd "$project_dir" 2>/dev/null && pwd -P || true)"
git_toplevel="$(git -C "$project_dir" rev-parse --show-toplevel 2>/dev/null || true)"

if [[ -n "$project_real" && "$git_toplevel" == "$project_real" ]]; then
    if ! git -C "$project_dir" config core.hooksPath .githooks 2>/dev/null; then
        wiring_warning="session-start: could not set core.hooksPath in $project_dir — commits will NOT run commit.sh (see .githooks/pre-commit)"
    fi
else
    wiring_warning="session-start: $project_dir is not a git repository root (git says '${git_toplevel:-none}') — core.hooksPath left alone, so commits will NOT run commit.sh"
fi

# A wiring failure must be loud (#651's whole point is that a missing gate looked
# identical to a passing one) but must not fail session start. For a SessionStart
# hook at exit 0, stdout is what becomes session context — the one place a
# warning reaches whoever can act on it — so it goes there as well as to stderr.
if [[ -n "$wiring_warning" ]]; then
    echo "$wiring_warning" >&2
    echo "$wiring_warning"
fi
