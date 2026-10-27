#include <gtest/gtest.h>

// person.h includes these textually; they must precede `import std;` or the two
// std::string declarations collide (see COMPILER_ISSUES.md section 9).
#include <cstdint>
#include <meta>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// Backend layering: the generic ORM templates (`Basic*`) carry NO default
// backend; the SQLite default lives only in the `storm_defaults` aliases. These
// static_asserts pin both halves of that contract, so re-adding a
// `= sqlite::Connection` default to a generic module (which would need it to
// import storm_db_sqlite again) fails to compile here.
// scripts/check_orm_backend_imports.py is the textual twin of this guard.

import std;
import storm_db_concept;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_defaults;
import storm_model;
import storm_orm_indexes;
import storm_orm_queryset;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;

#include "../../shared/models/person.h" // NOSONAR cpp:S954

namespace {

    using SqliteConn = storm::db::sqlite::Connection;
    using PgConn     = storm::db::postgresql::Connection;

    namespace st = storm::orm::statements;

    // --- The default aliases resolve to the generic template + SQLite ---------
    static_assert(std::same_as<storm::QuerySet<Person>, storm::BasicQuerySet<Person, SqliteConn, false>>);
    static_assert(std::same_as<st::SelectStatement<Person>, st::BasicSelectStatement<Person, SqliteConn>>);
    static_assert(std::same_as<st::InsertStatement<Person>, st::BasicInsertStatement<Person, SqliteConn>>);
    static_assert(std::same_as<st::UpdateStatement<Person>, st::BasicUpdateStatement<Person, SqliteConn>>);
    static_assert(std::same_as<st::EraseStatement<Person>, st::BasicEraseStatement<Person, SqliteConn>>);
    static_assert(std::same_as<storm::db::DefaultConnection<>, storm::db::BasicDefaultConnection<SqliteConn>>);

    // --- An explicit backend passes straight through the alias -----------------
    static_assert(std::same_as<storm::QuerySet<Person, PgConn>, storm::BasicQuerySet<Person, PgConn, false>>);
    static_assert(std::same_as<storm::QuerySet<Person, PgConn, true>, storm::BasicQuerySet<Person, PgConn, true>>);
    static_assert(std::same_as<st::SelectStatement<Person, PgConn>, st::BasicSelectStatement<Person, PgConn>>);
    static_assert(std::same_as<st::InsertStatement<Person, PgConn>, st::BasicInsertStatement<Person, PgConn>>);
    static_assert(std::same_as<st::UpdateStatement<Person, PgConn>, st::BasicUpdateStatement<Person, PgConn>>);
    static_assert(std::same_as<st::EraseStatement<Person, PgConn>, st::BasicEraseStatement<Person, PgConn>>);
    static_assert(std::same_as<storm::db::DefaultConnection<PgConn>, storm::db::BasicDefaultConnection<PgConn>>);

    // --- The generic templates have no backend default -------------------------
    // Each probe takes a PACK so the argument count stays dependent: a fixed
    // `BasicQuerySet<T>` would be rejected when the concept is defined rather than
    // reported as an unsatisfied requirement when it is checked.
    template <class... A>
    concept QuerySetNameableWith = requires { typename storm::BasicQuerySet<A...>; };
    template <class... A>
    concept SelectNameableWith = requires { typename st::BasicSelectStatement<A...>; };
    template <class... A>
    concept InsertNameableWith = requires { typename st::BasicInsertStatement<A...>; };
    template <class... A>
    concept UpdateNameableWith = requires { typename st::BasicUpdateStatement<A...>; };
    template <class... A>
    concept EraseNameableWith = requires { typename st::BasicEraseStatement<A...>; };
    template <class... A>
    concept DefaultConnectionNameableWith = requires { typename storm::db::BasicDefaultConnection<A...>; };

    static_assert(!QuerySetNameableWith<Person>);
    static_assert(!SelectNameableWith<Person>);
    static_assert(!InsertNameableWith<Person>);
    static_assert(!UpdateNameableWith<Person>);
    static_assert(!EraseNameableWith<Person>);
    static_assert(!DefaultConnectionNameableWith<>);

    // Positive controls: the same probes accept an explicit backend, so the
    // negative asserts above fail for the missing default, not a broken probe.
    static_assert(QuerySetNameableWith<Person, SqliteConn>);
    static_assert(SelectNameableWith<Person, PgConn>);
    static_assert(InsertNameableWith<Person, PgConn>);
    static_assert(UpdateNameableWith<Person, PgConn>);
    static_assert(EraseNameableWith<Person, PgConn>);
    static_assert(DefaultConnectionNameableWith<SqliteConn>);

} // namespace

// The static_asserts above are the real test; this runtime case only makes the
// file show up as a test so a reader running the suite sees the guard exists.
TEST(BackendLayeringTest, DefaultAliasesBindSqliteAndGenericTemplatesStayBackendFree) {
    EXPECT_TRUE((std::same_as<storm::QuerySet<Person>, storm::BasicQuerySet<Person, SqliteConn, false>>));
    EXPECT_FALSE(QuerySetNameableWith<Person>);
}
