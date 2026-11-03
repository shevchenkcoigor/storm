#!/bin/bash
# SessionStart hook. Two independent jobs, and the ORDER between them matters:
#
# 1. Wire git's pre-commit hook (issue #651) — UNCONDITIONAL, before the
#    provisioning half can decide it has nothing to do. `core.hooksPath` is
#    local config that a clone never carries, and the only other place that
#    sets it is CMakeLists.txt, as a side effect of a successful cmake
#    configure — exactly what a session with no ../clang-p2996 cannot do. Such
#    a session therefore committed with commit.sh (format, clang-tidy, tests,
#    the 100% coverage gate, and the two self-tests it runs even for an
#    otherwise no-op commit — #543, #550) never firing, and with nothing in the
#    output to say so. Doing this after any of the three provisioning bail-outs
#    reintroduces that silent gap, which is why each bail-out has its own
#    scenario in scripts/commit/tests/test_session_start_hook.sh.
#
# 2. Provision scripts/dev-container.sh's Docker build environment (issue
#    #628): Claude Code's remote/sandboxed sessions start with no local
#    ../clang-p2996 toolchain, so no CMake preset can configure, build, run
#    tests, or run clang-format/clang-tidy without it. Provisioned in the
#    background, so it's ready by the time a session wants to build.
#
#    No-op when:
#      - not a remote session (only remote sessions lack the toolchain locally)
#      - ../clang-p2996 is already present (nothing to provision)
#      - docker isn't installed (nothing this hook can do)
#
# See scripts/dev-container.sh for the up/exec/status/down commands this
# provisions, and CLAUDE.md's Prerequisites section for how a session is
# expected to use it once ready.

set -euo pipefail

# CLAUDE_PROJECT_DIR is set by Claude Code for hooks; fall back to this
# script's own location so manual invocation (e.g. this skill's own
# validation step) doesn't fail on an unbound variable.
project_dir="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

# --- 1. Pre-commit hook wiring (#651) ---
#
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

# --- 2. Docker dev-container provisioning (#628) ---
#
# Decided before anything is reported, because the decision picks the channel:
# on the provisioning path stdout is Claude Code's protocol channel (the async
# JSON below) and must stay exactly that, so a warning there can only go to
# stderr. On the other three paths stdout is free, and for a SessionStart hook
# that is the channel that becomes session context — i.e. the one place a
# warning actually reaches whoever can act on it. A missing gate must never be
# as quiet as a passing one (#651), but it must not fail session start either:
# nothing below depends on the wiring.
provision=true
if [[ "${CLAUDE_CODE_REMOTE:-}" != "true" ]]; then
    provision=false           # not a remote session — toolchain is local
elif [[ -e "$project_dir/../clang-p2996" ]]; then
    provision=false           # toolchain already present — nothing to build
elif ! command -v docker >/dev/null 2>&1; then
    provision=false           # no docker — nothing this hook can do
fi

if [[ -n "$wiring_warning" ]]; then
    echo "$wiring_warning" >&2
    if [[ "$provision" == false ]]; then
        echo "$wiring_warning"
    fi
fi

if [[ "$provision" == false ]]; then
    exit 0
fi

# The image build (Manjaro base + pacman -Syu + package install) can take
# several minutes on a cold cache — async so session start isn't blocked on
# it. dev-container.sh's `up` serializes on a flock, so a build/test command
# run before this finishes just waits on the same provisioning via its own
# `exec` call rather than racing it.
echo '{"async": true, "asyncTimeout": 600000}'

"$project_dir/scripts/dev-container.sh" up >>/tmp/storm-dev-container-setup.log 2>&1 || true
