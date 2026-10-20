#!/bin/bash
# Tests for scripts/pg-default-connstr.sh and the three scripts that depend on it:
# commit.sh, scripts/coverage-run-batched.sh and scripts/dev-container.sh.
#
# The default STORM_PG_CONNSTR names a libpq unix-socket directory, and that
# directory differs by host: CI (Linux) runs PostgreSQL at /var/run/postgresql,
# a Homebrew install on macOS listens in /tmp. commit.sh and
# scripts/coverage-run-batched.sh each hardcoded the Linux path, so on macOS the
# coverage gate silently skipped every PG test and failed at ~99.8%.
#
# The default is chosen by an explicit OS branch (not by probing which socket
# exists), so the helper takes the OS name as an argument and both branches can
# be exercised from any host.
#
# Every check is behavioural: it runs the code and looks at the value that comes
# out. Grepping the scripts for strings was tried first and passed with the real
# `source` line deleted, because the comments in those files name the helper too.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HELPER="$REPO_ROOT/scripts/pg-default-connstr.sh"

PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "  PASS: $name"
        PASS=$((PASS+1))
    else
        echo "  FAIL: $name"
        echo "        expected: [$expected]"
        echo "        actual:   [$actual]"
        FAIL=$((FAIL+1))
    fi
    return 0
}

if [[ ! -f "$HELPER" ]]; then
    echo "  FAIL: $HELPER does not exist"
    echo "0 passed, 1 failed"
    exit 1
fi

# shellcheck source=../pg-default-connstr.sh
source "$HELPER"

# --- The helper ---

check "Linux uses the system socket directory" \
    "host=/var/run/postgresql dbname=storm_db user=storm_db" \
    "$(storm_pg_default_connstr Linux)"

check "Darwin uses /tmp (Homebrew PostgreSQL)" \
    "host=/tmp dbname=storm_db user=storm_db" \
    "$(storm_pg_default_connstr Darwin)"

# An unknown host must fail loudly and name the OS, not fall through to one of
# the two paths and skip PG tests silently.
unknown_out="$(storm_pg_default_connstr FreeBSD 2>&1)"
unknown_rc=$?
check "unknown OS fails and names the OS" "rc=1 named=yes" \
    "rc=$unknown_rc named=$([[ "$unknown_out" == *FreeBSD* ]] && echo yes || echo no)"

# No argument resolves from the running host, so it must equal that host's branch.
case "$(uname -s)" in
    Darwin) host_expected="host=/tmp dbname=storm_db user=storm_db" ;;
    Linux)  host_expected="host=/var/run/postgresql dbname=storm_db user=storm_db" ;;
    *)      host_expected="" ;;
esac
if [[ -n "$host_expected" ]]; then
    check "no argument resolves from uname" "$host_expected" "$(storm_pg_default_connstr)"
fi

# --- The default blocks in commit.sh and coverage-run-batched.sh ---
# Extract each script's real default block, run it under `set -e` with the
# variable unset / exported / empty, and read back what it left behind. Running
# the real text means a deleted `source`, a hardcoded path or a changed
# unset-vs-empty rule all change the output.
#
# BASH_SOURCE[0] is swapped for the script's path by parameter expansion, not sed,
# so a repo path containing `#` or `&` cannot break the substitution.
run_default_block() { # <script> <start-regex> <setup command>
    local script="$1" start="$2" setup="$3" block
    block="$(sed -n "/$start/,/^[[:space:]]*fi\$/p" "$script")"
    block="${block//'${BASH_SOURCE[0]}'/$script}"
    # The block is wrapped in a function because commit.sh's `return 1` is only
    # legal inside one; the variable it sets is global, so it is still readable.
    bash -c "set -e; $setup; f() {
$block
}; f; printf '%s' \"\${STORM_PG_CONNSTR-<unset>}\""
}

COVERAGE_START='^if \[\[ -z "\${STORM_PG_CONNSTR+set}" \]\]'
COMMIT_START='^[[:space:]]*if \[\[ -z "\${STORM_PG_CONNSTR:-}" \]\]; then'
EXPORTED='host=ci-pg dbname=x user=y'
LINUX_DEFAULT="$(storm_pg_default_connstr Linux)"
DARWIN_DEFAULT="$(storm_pg_default_connstr Darwin)"

# Each block is run as a Linux host and as a Darwin host by stubbing `uname` with a
# shell function (it takes precedence over the real command inside the helper).
# Comparing against THIS machine's default instead would let a hardcoded path
# pass on the one OS where it happens to be right: a hardcoded /tmp is correct on
# a Mac and wrong on CI, so it must be checked against both.
as_linux='uname() { echo Linux; }'
as_darwin='uname() { echo Darwin; }'

# coverage-run-batched.sh defaults only when the variable is genuinely UNSET: an
# empty value is passed through untouched (documented there — libpq reads "" as
# "use the PG* environment defaults", so it is not a way to disable PostgreSQL).
COV="$REPO_ROOT/scripts/coverage-run-batched.sh"
check "coverage: unset on Linux takes the Linux default" "$LINUX_DEFAULT" \
    "$(run_default_block "$COV" "$COVERAGE_START" "$as_linux; unset STORM_PG_CONNSTR")"
check "coverage: unset on Darwin takes the Darwin default" "$DARWIN_DEFAULT" \
    "$(run_default_block "$COV" "$COVERAGE_START" "$as_darwin; unset STORM_PG_CONNSTR")"
check "coverage: an exported value wins" "$EXPORTED" \
    "$(run_default_block "$COV" "$COVERAGE_START" "export STORM_PG_CONNSTR='$EXPORTED'")"
check "coverage: an empty value is passed through" "" \
    "$(run_default_block "$COV" "$COVERAGE_START" 'export STORM_PG_CONNSTR=')"

# commit.sh differs deliberately: it treats a set-but-empty value as unset.
COMMIT="$REPO_ROOT/commit.sh"
check "commit.sh: unset on Linux takes the Linux default" "$LINUX_DEFAULT" \
    "$(run_default_block "$COMMIT" "$COMMIT_START" "$as_linux; unset STORM_PG_CONNSTR")"
check "commit.sh: unset on Darwin takes the Darwin default" "$DARWIN_DEFAULT" \
    "$(run_default_block "$COMMIT" "$COMMIT_START" "$as_darwin; unset STORM_PG_CONNSTR")"
check "commit.sh: an exported value wins" "$EXPORTED" \
    "$(run_default_block "$COMMIT" "$COMMIT_START" "export STORM_PG_CONNSTR='$EXPORTED'")"
check "commit.sh: an empty value is treated as unset" "$DARWIN_DEFAULT" \
    "$(run_default_block "$COMMIT" "$COMMIT_START" "$as_darwin; export STORM_PG_CONNSTR=")"

# --- scripts/dev-container.sh ---
# `exec` runs the command natively when ../clang-p2996 exists (a macOS developer
# with a local toolchain). It used to export the Linux container's connstr
# unconditionally, so `dev-container.sh exec ./commit.sh` handed the gate
# /var/run/postgresql on a Mac and every PG test skipped. On the native path it
# must leave STORM_PG_CONNSTR exactly as the caller had it. A fake repo with a
# sibling clang-p2996 directory selects the native path without needing docker.
DEV_TMP="$(mktemp -d)"
trap 'rm -rf "$DEV_TMP"' EXIT
mkdir -p "$DEV_TMP/repo/scripts" "$DEV_TMP/clang-p2996"
cp "$REPO_ROOT/scripts/dev-container.sh" "$DEV_TMP/repo/scripts/"
native_exec_connstr() {
    env "$@" bash "$DEV_TMP/repo/scripts/dev-container.sh" exec \
        bash -c 'printf %s "${STORM_PG_CONNSTR-<unset>}"'
}

check "dev-container: native exec leaves an unset value unset" "<unset>" \
    "$(native_exec_connstr -u STORM_PG_CONNSTR)"
check "dev-container: native exec passes an exported value through" "$EXPORTED" \
    "$(native_exec_connstr STORM_PG_CONNSTR="$EXPORTED")"

echo "$PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
