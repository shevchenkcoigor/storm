#!/bin/bash
# Tests for scripts/check-orm-backend-imports.sh.
#
# Strategy: build a minimal fake repo (src/orm + src/db) in a temp dir, run the
# checker against it, and assert the exit code and message per scenario. A
# final scenario runs the checker against the real repository, which must pass.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHECKER="$REPO_ROOT/scripts/check-orm-backend-imports.sh"

PASS=0
FAIL=0
SQLITE_IMPORT='import storm_db_sqlite;'

check() {
    local tag="$1" want_rc="$2" want_text="$3" root="$4"
    local out rc
    out="$("$CHECKER" "$root" 2>&1)"
    rc=$?
    if [[ "$rc" -ne "$want_rc" ]]; then
        echo "  FAIL [$tag]: exit $rc, want $want_rc"
        echo "$out" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return 0
    fi
    if [[ -n "$want_text" && "$out" != *"$want_text"* ]]; then
        echo "  FAIL [$tag]: output lacks '$want_text'"
        echo "$out" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return 0
    fi
    echo "  PASS [$tag]"
    PASS=$((PASS + 1))
    return 0
}

# Fresh fake repo with clean, backend-neutral sources.
make_repo() {
    local dir
    dir="$(mktemp -d)" || exit 1
    mkdir -p "$dir/src/orm/statements" "$dir/src/db"
    printf 'export module storm_orm_queryset;\nimport storm_db_concept;\n' > "$dir/src/orm/queryset.cppm"
    printf 'export module storm_orm_statements_select;\n' > "$dir/src/orm/statements/select.cppm"
    printf 'export module storm_db_default_connection;\nimport storm_db_pool;\n' > "$dir/src/db/default_connection.cppm"
    printf 'export module storm_db_pool;\n' > "$dir/src/db/pool.cppm"
    printf 'export module storm_model;\n' > "$dir/src/storm_model.cppm"
    # A backend module may of course name itself — it is not checked.
    printf 'export module storm_db_sqlite;\nnamespace storm::db::sqlite {}\n' > "$dir/src/db/sqlite.cppm"
    echo "$dir"
}

TMPS=()
cleanup() {
    rm -rf "${TMPS[@]}"
    return 0
}
trap cleanup EXIT

r="$(make_repo)"; TMPS+=("$r")
check clean 0 "names no concrete database backend" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo "$SQLITE_IMPORT" >> "$r/src/orm/statements/select.cppm"
check nested-orm-import 1 "src/orm/statements/select.cppm:2" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo 'template <class C = storm::db::sqlite::Connection> class X;' >> "$r/src/orm/queryset.cppm"
check qualified-default 1 "src/orm/queryset.cppm:3" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo "$SQLITE_IMPORT" >> "$r/src/db/default_connection.cppm"
check default-connection 1 "src/db/default_connection.cppm" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo 'import storm_db_postgresql;' >> "$r/src/db/pool.cppm"
check pool-postgres 1 "src/db/pool.cppm" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo 'import storm_defaults;' >> "$r/src/storm_model.cppm"
check model-imports-defaults 1 "src/storm_model.cppm:2" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo "$SQLITE_IMPORT" >> "$r/src/orm/queryset.cppm"
echo "$SQLITE_IMPORT" >> "$r/src/orm/statements/select.cppm"
check counts-all 1 "2 backend reference(s)" "$r"

r="$(make_repo)"; TMPS+=("$r")
mv "$r/src/db/pool.cppm" "$r/src/db/pool_v2.cppm"
echo "$SQLITE_IMPORT" >> "$r/src/db/pool_v2.cppm"
check renamed-listed-file 1 "listed path missing: src/db/pool.cppm" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo 'using namespace storm::db::sqlite;' >> "$r/src/orm/queryset.cppm"
check using-namespace 1 "src/orm/queryset.cppm:3" "$r"

r="$(make_repo)"; TMPS+=("$r")
echo 'import storm_defaults;' >> "$r/src/orm/statements/select.cppm"
check imports-defaults 1 "src/orm/statements/select.cppm:2" "$r"

check missing-root 1 "no src/orm" "/nonexistent-storm-root"

check real-repo 0 "names no concrete database backend" "$REPO_ROOT"

echo ""
echo "check-orm-backend-imports self-test: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
