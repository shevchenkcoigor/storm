#ifndef STORM_TESTS_TEST_DB_HELPERS_H
#define STORM_TESTS_TEST_DB_HELPERS_H

/**
 * @file test_db_helpers.h
 * @brief Backend abstraction helpers for Storm ORM TYPED_TEST infrastructure
 *
 * Provides template helpers keyed on ConnType (SQLite or PostgreSQL) so that
 * all ORM tests can run against both backends from a single binary.
 *
 * PostgreSQL test isolation uses per-process schemas (test_<pid>), giving each
 * CTest process its own namespace. This enables fully parallel execution with
 * zero lock contention. Per-test isolation uses DROP SCHEMA + CREATE SCHEMA in
 * TearDown to reset state. The schema is created once per process and reused
 * (never cleared between tests).
 *
 * PostgreSQL tests auto-skip when STORM_PG_CONNSTR env var is not set.
 *
 * Usage in test files:
 *   #include "test_db_helpers.h"
 *   template <typename ConnType>
 *   class MyTest : public ::testing::Test { ... };
 *   TYPED_TEST_SUITE(MyTest, DatabaseTypes);
 */

#include <cstdlib>
#include <gtest/gtest.h>
#include <libpq-fe.h>
#include <string>
#include <type_traits>

// Forward-declare connection types (actual import in .cpp files via `import storm;`)
// These are used in std::is_same_v checks within if constexpr branches.
namespace storm::db::sqlite {
class Connection;
}
namespace storm::db::postgresql {
class Connection;
}

// Type list for TYPED_TEST_SUITE — runs each test against both backends
using DatabaseTypes = ::testing::Types<storm::db::sqlite::Connection, storm::db::postgresql::Connection>;

// Single-backend halves of DatabaseTypes, for splitting a slow TYPED_TEST TU into two
// TUs (one per backend) so ninja can compile them in parallel instead of serially
// instantiating both backends in one compiler process. Pair each with the matching
// NameGenerator below so gtest's generated test IDs don't collide between the two TUs
// when linked into the same binary — by default gtest suffixes a suite's tests by the
// type's index in the list, and both halves have a single-element list starting at
// index 0 (e.g. both would register "FooTest/0.Bar" without the NameGenerator).
//
// Usage:
//   TYPED_TEST_SUITE(FooTest, DatabaseTypesSqliteHalf, DatabaseTypesSqliteHalfNames);
using DatabaseTypesSqliteHalf = ::testing::Types<storm::db::sqlite::Connection>;
using DatabaseTypesPgHalf = ::testing::Types<storm::db::postgresql::Connection>;

class DatabaseTypesSqliteHalfNames {
  public:
    template <typename T> static auto GetName(int /*index*/) -> std::string { return "Sqlite"; }
};

class DatabaseTypesPgHalfNames {
  public:
    template <typename T> static auto GetName(int /*index*/) -> std::string { return "Postgresql"; }
};

namespace storm::test {

namespace detail {
// Tracks the current per-process test schema name.
// Empty means no schema has been created yet for this process.
inline std::string current_test_schema; // NOLINT // NOSONAR(cpp:S5421)

// "<head><current test schema><tail>" — the admin DDL below. Appends rather than
// std::format: see numbered().
inline auto schema_sql(std::string_view head, std::string_view tail = {}) -> std::string {
    std::string sql{head};
    sql += current_test_schema;
    sql += tail;
    return sql;
}
} // namespace detail

// "<prefix><n>" — e.g. numbered("Person", 7) == "Person7". Used instead of
// std::format in shared test headers: one std::format call reachable from a header
// makes every including TU generate the whole formatting stack (~0.4 s per TU).
template <typename Integer>
    requires std::is_integral_v<Integer>
inline auto numbered(std::string_view prefix, Integer n) -> std::string {
    std::string out{prefix};
    out += std::to_string(n);
    return out;
}

// Check if the backend is available (PG requires STORM_PG_CONNSTR set and server reachable;
// SQLite is always available)
template <typename ConnType> auto backend_available() -> bool {
    if constexpr (std::is_same_v<ConnType, storm::db::postgresql::Connection>) {
        const char *connstr = std::getenv("STORM_PG_CONNSTR"); // NOLINT(concurrency-mt-unsafe)
        if (!connstr) {
            return false;
        }
        PGconn *conn = PQconnectdb(connstr);
        const bool connected = PQstatus(conn) == CONNECTION_OK;
        PQfinish(conn);
        return connected;
    }
    return true;
}

// Get the connection string for the current backend
template <typename ConnType> auto get_connection_string() -> std::string {
    if constexpr (std::is_same_v<ConnType, storm::db::postgresql::Connection>) {
        const char *connstr = std::getenv("STORM_PG_CONNSTR"); // NOLINT(concurrency-mt-unsafe)
        return connstr ? std::string(connstr) : "";
    } else {
        return ":memory:";
    }
}

// Non-template overload for backward compatibility with existing code
inline auto get_connection_string() -> std::string { return ":memory:"; }

// Initialise the per-process PostgreSQL test schema (one-time per process).
// No-op for SQLite. Uses conn->execute for PG admin DDL (CREATE/DROP SCHEMA,
// SET search_path) — there is no ORM equivalent for these admin operations.
template <typename ConnType> auto pg_schema_init(auto &conn) -> void {
    if constexpr (std::is_same_v<ConnType, storm::db::postgresql::Connection>) {
        if (detail::current_test_schema.empty()) {
            detail::current_test_schema = numbered("test_", getpid());
            (void)conn->execute(detail::schema_sql("DROP SCHEMA IF EXISTS ", " CASCADE"));
            (void)conn->execute(detail::schema_sql("CREATE SCHEMA "));
        }
        // Always set search_path — each test gets a fresh connection
        (void)conn->execute(detail::schema_sql("SET search_path TO "));
    }
}

// End test isolation. For PG: drops the per-process schema (instant cleanup).
// For SQLite: no-op (:memory: is destroyed with the connection).
template <typename ConnType> auto rollback_test_txn(const auto &conn) -> void {
    if constexpr (std::is_same_v<ConnType, storm::db::postgresql::Connection>) {
        if (!detail::current_test_schema.empty()) {
            (void)conn->execute(detail::schema_sql("DROP SCHEMA IF EXISTS ", " CASCADE"));
            (void)conn->execute(detail::schema_sql("CREATE SCHEMA "));
            // DON'T clear current_test_schema — schema is recreated and ready for next test.
            // pg_schema_init will skip CREATE (schema exists) and just SET search_path.
        }
    }
    (void)conn;
}

// Check if we're running against PostgreSQL
template <typename ConnType> constexpr auto is_postgresql() -> bool {
    return std::is_same_v<ConnType, storm::db::postgresql::Connection>;
}

// Check if we're running against SQLite
template <typename ConnType> constexpr auto is_sqlite() -> bool {
    return std::is_same_v<ConnType, storm::db::sqlite::Connection>;
}

} // namespace storm::test

#endif // STORM_TESTS_TEST_DB_HELPERS_H
