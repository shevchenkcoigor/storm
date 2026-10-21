#include <gtest/gtest.h>

import storm_db_concept;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

// ── #477: dialect-support concepts are compile-time backend-capability gates ──
// These name the ad-hoc `if constexpr (requires { ConnType::trait; })` probes
// used for dialect selection, plus the previously-unconstrained transaction
// contract. Verification is compile-time only (the issue: "static_assert per
// backend"). Each backend's own definition also static-asserts these (see
// sqlite.cppm / postgresql_connection.cppm); this TU adds the CROSS-BACKEND
// contrast and the NEGATIVE gates a non-connection type is rejected. It stays a
// runnable GTest target so the suite reports it.

using SqliteConn = storm::db::sqlite::Connection;
using PgConn     = storm::db::postgresql::Connection;

// SupportsPgDialect IS the dialect switch: PG declares uses_pg_dialect, SQLite
// does not — so the two backends must disagree, and a non-connection is out.
static_assert(storm::db::SupportsPgDialect<PgConn>);
static_assert(!storm::db::SupportsPgDialect<SqliteConn>);
static_assert(!storm::db::SupportsPgDialect<int>);

// SupportsLimitAll is an EXISTENCE probe — true for BOTH backends (both declare
// the member); call sites read the bool VALUE to pick "LIMIT ALL" vs "LIMIT -1".
static_assert(storm::db::SupportsLimitAll<PgConn>);
static_assert(storm::db::SupportsLimitAll<SqliteConn>);
static_assert(!storm::db::SupportsLimitAll<int>);

// TransactionCapable holds for real connections and rejects a non-connection.
static_assert(storm::db::TransactionCapable<PgConn>);
static_assert(storm::db::TransactionCapable<SqliteConn>);
static_assert(!storm::db::TransactionCapable<int>);

// The TransactionCapable constraint is load-bearing: storm::begin is callable
// for a real connection type and NOT for a non-connection. The probe is wrapped
// in a variable template so the failed-constraint case is a dependent SFINAE
// soft-fail rather than a hard error at namespace scope.
template <class C> constexpr bool begin_callable = requires(std::shared_ptr<C> conn) { storm::begin(conn); };
static_assert(begin_callable<SqliteConn>);
static_assert(!begin_callable<int>);

TEST(DialectConcepts, CompileTimeOnly) {
    // The static_asserts above are the real test; this keeps the TU a runnable
    // GTest target so the suite reports it.
    SUCCEED();
}
