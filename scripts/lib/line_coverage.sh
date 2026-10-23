# shellcheck shell=bash
# Sourced, not executed. Reads the line-coverage percentage out of the coverage
# target's `lcov --summary` output, shared by commit.sh's coverage step and the
# `coverage` job in .github/workflows/ci.yml so the two gates cannot drift.
#
# Anchors on the LINE row ("lines.......: 100.0% (8613 of 8613 lines)"), not the
# functions/branches rows, and takes the last one. Prints the number without the
# `%` (e.g. `100.0`), or nothing when there is no such row; always returns 0, so
# a no-match under `set -eo pipefail` reaches the caller's own empty-value error.
#
# Portable on purpose (#680): `grep -oP` with a lookahead used to do this, and
# macOS BSD grep has no -P, so the pre-commit coverage step failed on every Mac.
# Use only flags BSD and GNU grep share.
#
# Usage: storm_parse_line_coverage < summary.txt

storm_parse_line_coverage() {
    grep 'lines\.\.\.\.\.\.\.' | tail -1 | grep -oE '[0-9.]+%' | head -1 | tr -d '%'
    return 0
}
