#!/bin/bash
# Cold-cache CPM policy test (issue #709).
#
# CPM shallow-clones (`git clone --depth 1 --no-single-branch` + `git checkout
# <tag>`) any GIT_TAG that is not a commit hash, and derives GIT_TAG v<VERSION>
# when none is given. A depth-1 clone only carries branch tips, so against GitHub
# a tag that is NOT a branch tip (stduuid v1.2.3) is missing and a fresh worktree
# (empty .cache/CPM) cannot configure. Warm caches hide it, so nothing red ever
# points at the declaration. A hash pin is already full-cloned by CPM; it carries
# GIT_SHALLOW FALSE too only to keep this policy uniform.
#
# A behavioural fixture cannot catch this: a local `git clone` (file:// or path)
# hands over the tags regardless, so the failure reproduces only against GitHub.
# The test is therefore a policy check over every tracked CMake file: each
# CPMAddPackage/CPMFindPackage/CPMDeclarePackage/FetchContent_Declare call that
# pins a version must end with `GIT_SHALLOW FALSE` (last value wins). It also
# fails when it finds NO such call, so a moved file cannot make it a silent pass.
# Known limitation: a `)` inside an argument before the pin ends the call early.
# Needs git and perl; no network, no cmake.

set -u -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); return 0; }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); return 0; }

# One line per call: "<NAME>\t<collapsed args>". Comments are stripped first so a
# `# GIT_SHALLOW FALSE` note cannot satisfy the check.
extract_calls() {
    perl -0777 -ne '
        s/#[^\n]*//g;
        while (/\b(?:cpm(?:add|find|declare)package|fetchcontent_declare)\s*\((.*?)\)/gis) {
            (my $c = $1) =~ s/\s+/ /g;
            print "$c\n";
        }' "$1"
}

is_pinned() {
    grep -qE 'GIT_TAG |VERSION |gh:[^ ]*@' <<< "$1"
}

last_shallow_is_false() {
    local last
    last="$(grep -oE 'GIT_SHALLOW [A-Za-z0-9_]+' <<< "$1" | tail -n 1)"
    [[ "$last" == "GIT_SHALLOW FALSE" ]]
}

checked=0
while IFS= read -r f; do
    [[ "$f" == "cmake/cpm.cmake" ]] && continue   # only bootstraps CPM
    calls="$(extract_calls "$REPO_ROOT/$f")" || { fail "$f: perl could not read the file"; continue; }
    while IFS= read -r call; do
        [[ -z "$call" ]] && continue
        is_pinned "$call" || continue
        checked=$((checked+1))
        name="$(sed -nE 's/.*NAME ([^ ]+).*/\1/p' <<< "$call")"
        [[ -z "$name" ]] && name="${call:0:40}"
        if last_shallow_is_false "$call"; then
            pass "$f: $name is pinned with GIT_SHALLOW FALSE"
        else
            fail "$f: $name is pinned without a final GIT_SHALLOW FALSE (cold cache cannot fetch a non-tip tag)"
        fi
    done <<< "$calls"
done < <(git -C "$REPO_ROOT" ls-files '*.cmake' '*CMakeLists.txt')

if [[ $checked -eq 0 ]]; then
    fail "found no pinned CPM/FetchContent call — the file scan is stale"
fi

echo
echo "Passed: $PASS  Failed: $FAIL"
[[ $FAIL -eq 0 ]]
