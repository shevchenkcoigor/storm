#!/bin/bash
# Keep the generic ORM layer free of any concrete database backend.
#
# The ORM templates (BasicQuerySet, Basic*Statement, BasicDefaultConnection,
# SchemaStatement, ...) are written against the DatabaseConnection concept
# only. The SQLite default that `QuerySet<T>` / `SelectStatement<T>` / ...
# carry lives in ONE place — the `storm_defaults` alias module — so no generic
# module's import graph reaches the SQLite backend. (This is module-level
# decoupling; the single `storm` CMake target still builds every backend.)
#
# This guard fails if a backend-neutral module names a concrete backend again,
# which is how the coupling crept in originally: a `= sqlite::Connection`
# template default needs `import storm_db_sqlite;` in the generic module.
# tests/db/test_backend_layering.cpp is the compile-time twin of this check
# (it pins that the generic templates have no default backend at all).
#
# Comments are checked too, deliberately: a plain grep cannot tell a comment
# from code reliably across raw strings and trailing comments, and prose about
# a backend belongs in that backend's module or in storm_defaults.
#
# Usage: check-orm-backend-imports.sh [repo-root]   (default: this repo)
# Exit:  0 = no backend reference in a neutral module, 1 = at least one found.

set -uo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [[ ! -d "$ROOT/src/orm" ]]; then
    echo "check-orm-backend-imports: no src/orm under: $ROOT" >&2
    exit 1
fi

# Backend-neutral sources: the whole ORM tree plus the generic db-layer
# facades that sit beside the backends in src/db/.
NEUTRAL_PATHS=(
    "$ROOT/src/orm"
    "$ROOT/src/db/default_connection.cppm"
    "$ROOT/src/db/pool.cppm"
    "$ROOT/src/storm_meta.cppm"
)

# Every listed path must exist: a renamed or split file would otherwise drop
# out of the scan silently and the guard would pass while checking nothing.
for path in "${NEUTRAL_PATHS[@]}"; do
    if [[ ! -e "$path" ]]; then
        echo "check-orm-backend-imports: listed path missing: ${path#"$ROOT/"} — update NEUTRAL_PATHS" >&2
        exit 1
    fi
done

# A backend module import, a qualified backend name, the backend namespace
# itself (`using namespace storm::db::sqlite;`), or the defaults/umbrella
# modules that bind a backend (`import storm_defaults;` / `import storm;`).
PATTERN='storm_db_(sqlite|postgresql)|(sqlite|postgresql)::|storm::db::(sqlite|postgresql)\b|import storm(_defaults)?;'

matches="$(grep -rnE --include='*.cppm' "$PATTERN" "${NEUTRAL_PATHS[@]}")"
grep_rc=$?

# grep: 0 = matches, 1 = none, >=2 = error (unreadable file, bad pattern).
if [[ "$grep_rc" -ge 2 ]]; then
    echo "check-orm-backend-imports: grep failed (rc=$grep_rc)" >&2
    exit 1
fi

if [[ "$grep_rc" -eq 1 ]]; then
    echo "✓ ORM layer names no concrete database backend"
    exit 0
fi

count=0
while IFS= read -r line; do
    file="${line%%:*}"
    rest="${line#*:}"
    lineno="${rest%%:*}"
    rel="${file#"$ROOT/"}"
    # Under GitHub Actions, annotate the offending line in the PR diff.
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
        echo "::error file=$rel,line=$lineno::backend-neutral module names a concrete backend — move the default into storm_defaults" >&2
    fi
    echo "✗ $rel:$lineno: ${rest#*:}" >&2
    count=$((count + 1))
done <<< "$matches"

echo "" >&2
echo "check-orm-backend-imports: $count backend reference(s) in backend-neutral modules." >&2
echo "Generic templates must not default to a backend; add the default alias to src/storm_defaults.cppm." >&2
exit 1
