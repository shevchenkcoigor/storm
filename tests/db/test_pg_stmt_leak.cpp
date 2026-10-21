#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length)

import storm_db_postgresql;
import storm_db_sqlite;
import std;

// ============================================================================
// Issue #417 — server-side prepared-statement leak on cache eviction
//
// The bounded L3 statement cache (#273) evicts entries via CLOCK once it is
// full. SQLite finalizes the evicted statement through its unique_ptr deleter;
// PostgreSQL must symmetrically DEALLOCATE the server-side `_storm_N` prepared
// statement, or it accumulates on the backend for the connection's lifetime.
//
// This test runs ONLY against a real PostgreSQL server (auto-skips without
// STORM_PG_CONNSTR). It churns far more distinct SQL than the cache holds, then
// reads pg_prepared_statements to assert the server-side count stays bounded by
// the cache capacity rather than growing with the churn count.
// ============================================================================

namespace {

    using PgConnection = storm::db::postgresql::Connection;

    // Read the number of live server-side prepared statements on this connection.
    auto count_server_prepared(PgConnection& conn) -> std::int64_t {
        auto stmt = conn.prepare("SELECT count(*) FROM pg_prepared_statements");
        EXPECT_TRUE(stmt.has_value());
        auto step = stmt->step();
        EXPECT_TRUE(step.has_value());
        EXPECT_TRUE(step.value());
        return stmt->extract_int64(0);
    }

    TEST(PgStatementLeak, CacheEvictionDeallocatesServerSide) {
        if (!storm::test::backend_available<PgConnection>()) {
            GTEST_SKIP() << "PostgreSQL not available (STORM_PG_CONNSTR unset / unreachable)";
        }

        constexpr std::size_t kCapacity = 8;
        constexpr int         kChurn    = 200; // >> kCapacity → forces many evictions

        PgConnection::Config config;
        config.statement_cache_capacity = kCapacity;

        auto conn_result = PgConnection::open(storm::test::get_connection_string<PgConnection>(), config);
        ASSERT_TRUE(conn_result.has_value());
        PgConnection& conn = conn_result.value();

        // Churn many distinct SELECTs through prepare_cached(). Each unique SQL is
        // a fresh server-side prepared statement; once the cache is full, every
        // new one evicts (and must DEALLOCATE) an old one.
        for (int i = 0; i < kChurn; ++i) {
            auto stmt = conn.prepare_cached(std::format("SELECT {}", i));
            ASSERT_TRUE(stmt.has_value()) << "iteration " << i;
        }

        // The pg_prepared_statements probe itself uses prepare() (unnamed lifetime
        // here is a fresh `_storm_N`), so allow a small slack above capacity.
        const std::int64_t live = count_server_prepared(conn);
        EXPECT_LE(live, static_cast<std::int64_t>(kCapacity) + 4)
                << "server-side prepared statements grew without bound: " << live << " after churning " << kChurn
                << " distinct SQL through a cache of capacity " << kCapacity;
    }

} // namespace

// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length)
