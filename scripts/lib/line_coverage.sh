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
#
# Also holds storm_format_line_ranges (below), which commit.sh's uncovered-lines
# report uses when that gate fails.

storm_parse_line_coverage() {
    grep 'lines\.\.\.\.\.\.\.' | tail -1 | grep -oE '[0-9.]+%' | head -1 | tr -d '%'
    return 0
}

# Compresses ascending line numbers into ranges for the uncovered-lines report:
# `storm_format_line_ranges 1 3 4 5 9` prints `1, 3-5, 9`, and nothing for no
# arguments. The numbers are arguments rather than an array name on purpose
# (#689): commit.sh runs under macOS /bin/bash 3.2, which has no `local -n`.
#
# Usage: storm_format_line_ranges "${lines[@]}"

storm_format_line_ranges() {
    local result="" range_start="" range_end="" ln
    for ln in "$@"; do
        if [[ -z "$range_start" ]]; then
            range_start=$ln; range_end=$ln
        elif [[ $ln -eq $((range_end + 1)) ]]; then
            range_end=$ln
        else
            if [[ "$range_start" == "$range_end" ]]; then
                result+="${result:+, }$range_start"
            else
                result+="${result:+, }${range_start}-${range_end}"
            fi
            range_start=$ln; range_end=$ln
        fi
    done
    if [[ -n "$range_start" ]]; then
        if [[ "$range_start" == "$range_end" ]]; then
            result+="${result:+, }$range_start"
        else
            result+="${result:+, }${range_start}-${range_end}"
        fi
    fi
    echo "$result"
    return 0
}
