# shellcheck shell=bash
# Sourced, not executed. Provides the default STORM_PG_CONNSTR for local runs.
#
# The connection string names a libpq unix-socket DIRECTORY, and the right one
# depends on the host: a Linux PostgreSQL package (and the CI service
# container, and scripts/dev-container.sh's sidecar) uses /var/run/postgresql,
# while a Homebrew PostgreSQL on macOS listens in /tmp. commit.sh and
# scripts/coverage-run-batched.sh used to hardcode the Linux path, so on macOS
# every PG-backed test skipped and the 100% coverage gate failed at ~99.8%.
#
# The choice is an explicit per-OS branch, not a probe of which socket happens
# to exist: the branch documents intent, and an unsupported host fails with a
# message naming it instead of silently picking a path where PG tests skip.
#
# An already-exported STORM_PG_CONNSTR still wins; each caller keeps its own
# unset-vs-empty rule and only asks this file for the default.
#
# Usage: storm_pg_default_connstr [os-name]   (os-name defaults to `uname -s`)

storm_pg_default_connstr() {
    local os="${1:-$(uname -s)}"
    local socket_dir

    case "$os" in
        Linux)  socket_dir="/var/run/postgresql" ;;
        Darwin) socket_dir="/tmp" ;;
        *)
            echo "ERROR: no default PostgreSQL socket directory for OS [$os];" \
                 "export STORM_PG_CONNSTR explicitly" >&2
            return 1
            ;;
    esac

    echo "host=$socket_dir dbname=storm_db user=storm_db"
}
