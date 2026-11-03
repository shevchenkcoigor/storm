#!/bin/bash
# Classifies a list of changed file paths into HAS_*_CHANGES flags used to
# skip work that can't be affected by the commit/PR — one place shared by
# commit.sh (staged files, pre-commit) and .github/workflows/ci.yml (diff
# against the PR base / previous push), so the two never drift apart.
#
# Usage: printf '%s\n' "${files[@]}" | scripts/ci/detect-changes.sh
#        (or: git diff --name-only ... | scripts/ci/detect-changes.sh)
#
# Reads file paths on stdin (one per line), prints `KEY=true|false` lines on
# stdout — eval that output to get the flags as shell variables:
#   eval "$(git diff --cached --name-only | ./scripts/ci/detect-changes.sh)"

set -euo pipefail

HAS_SRC_CHANGES=false
HAS_TEST_CHANGES=false
HAS_CPP_CHANGES=false
HAS_CMAKE_CHANGES=false
HAS_BENCH_CHANGES=false

while IFS= read -r file; do
    [[ -z "$file" ]] && continue
    [[ "$file" == src/* ]] && HAS_SRC_CHANGES=true
    [[ "$file" == tests/* ]] && HAS_TEST_CHANGES=true
    [[ "$file" =~ \.(cpp|cppm|h|hpp)$ ]] && HAS_CPP_CHANGES=true
    [[ "$file" =~ (CMakeLists\.txt|\.cmake)$ ]] && HAS_CMAKE_CHANGES=true
    [[ "$file" == benchmarks/* ]] && HAS_BENCH_CHANGES=true
done

printf 'HAS_SRC_CHANGES=%s\n' "$HAS_SRC_CHANGES"
printf 'HAS_TEST_CHANGES=%s\n' "$HAS_TEST_CHANGES"
printf 'HAS_CPP_CHANGES=%s\n' "$HAS_CPP_CHANGES"
printf 'HAS_CMAKE_CHANGES=%s\n' "$HAS_CMAKE_CHANGES"
printf 'HAS_BENCH_CHANGES=%s\n' "$HAS_BENCH_CHANGES"
