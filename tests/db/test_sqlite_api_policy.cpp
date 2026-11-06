#include <gtest/gtest.h>

// Textual includes must precede `import std;` (COMPILER_ISSUES.md section 9).
#include <cstdint>
#include <meta>
#include <sqlite3.h>

// #760/#784: structural checks on the compile-time sqlite3 API policy. The error
// paths themselves are exercised by test_sqlite_fake_errors.cpp.

import std;
import storm_db_concept;
import storm_db_sqlite;

#include "fake_sqlite_api.hpp" // NOSONAR cpp:S954 - must follow the imports

namespace {

    namespace sq = storm::db::sqlite;
    using fake_sqlite::FakeConn;
    using fake_sqlite::FakeSqliteApi;

    // The fake is a distinct backend type, not the production one.
    static_assert(!std::same_as<FakeConn, sq::Connection>);
    static_assert(std::derived_from<sq::Connection, sq::BasicConnection<sq::RealApi>>);
    static_assert(storm::db::CachedDatabaseConnection<FakeConn>);
    static_assert(storm::db::DatabaseStatement<sq::BasicStatement<FakeSqliteApi>>);

    TEST(SqliteApiPolicyTest, ProductionConnectionIgnoresAnArmedFakePlan) {
        // A failing fake plan must never reach the production connection type.
        FakeSqliteApi::reset();
        FakeSqliteApi::plan.open_rc = SQLITE_CANTOPEN;

        auto conn = sq::Connection::open(":memory:");

        FakeSqliteApi::reset();
        EXPECT_TRUE(conn.has_value());
    }

    TEST(SqliteApiPolicyTest, FakeForwardsToRealSqliteWhenNothingIsArmed) {
        FakeSqliteApi::reset();

        auto conn = FakeConn::open(":memory:");

        ASSERT_TRUE(conn.has_value());
        EXPECT_TRUE(conn->execute("CREATE TABLE t (id INTEGER)").has_value());
    }

    // --- The fake's own semantics: all ported error tests depend on them. ---------

    TEST(SqliteApiPolicyTest, FailOnCallNBeatsSequenceAndSequenceBeatsFixedCode) {
        FakeSqliteApi::reset();
        FakeSqliteApi::plan.step_rc       = SQLITE_IOERR;
        FakeSqliteApi::plan.step_sequence = {SQLITE_ROW, SQLITE_ROW, SQLITE_ROW};
        FakeSqliteApi::plan.step_fail_on  = 2;
        FakeSqliteApi::plan.step_fail_rc  = SQLITE_CORRUPT;

        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_ROW);     // call 1: sequence[0], not the fixed code
        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_CORRUPT); // call 2: fail-on-call-N wins
        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_ROW);     // call 3: sequence resumes at [1]
        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_ROW);     // call 4: sequence[2]
        FakeSqliteApi::reset();
    }

    TEST(SqliteApiPolicyTest, ExhaustedStepSequenceReturnsDone) {
        FakeSqliteApi::reset();
        FakeSqliteApi::plan.step_sequence = {SQLITE_ROW};

        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_ROW);
        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_DONE);
        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_DONE);
        FakeSqliteApi::reset();
    }

    TEST(SqliteApiPolicyTest, FixedStepCodeAppliesWhenNoSequenceIsArmed) {
        FakeSqliteApi::reset();
        FakeSqliteApi::plan.step_rc = SQLITE_IOERR;

        EXPECT_EQ(FakeSqliteApi::step(nullptr), SQLITE_IOERR);
        EXPECT_EQ(FakeSqliteApi::plan.step_calls, 1);
        FakeSqliteApi::reset();
    }

    TEST(SqliteApiPolicyTest, ResetClearsEveryArmedFailureAndCounter) {
        FakeSqliteApi::plan.prepare_rc        = SQLITE_ERROR;
        FakeSqliteApi::plan.bind_int_fail_on  = 3;
        FakeSqliteApi::plan.step_sequence     = {SQLITE_ROW};
        FakeSqliteApi::plan.expanded_sql_null = true;
        FakeSqliteApi::plan.exec_calls        = 7;

        FakeSqliteApi::reset();

        const auto& plan = FakeSqliteApi::plan;
        EXPECT_EQ(plan.prepare_rc, SQLITE_OK);
        EXPECT_EQ(plan.bind_int_fail_on, -1);
        EXPECT_TRUE(plan.step_sequence.empty());
        EXPECT_FALSE(plan.expanded_sql_null);
        EXPECT_EQ(plan.exec_calls, 0);
    }

    TEST(SqliteApiPolicyTest, NthBindFailureHitsOnlyThatCall) {
        FakeSqliteApi::reset();
        sqlite3*      raw  = nullptr;
        sqlite3_stmt* stmt = nullptr;
        ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_prepare_v2(raw, "SELECT ?1", -1, &stmt, nullptr), SQLITE_OK);
        FakeSqliteApi::plan.bind_int_fail_on = 2;
        FakeSqliteApi::plan.bind_int_fail_rc = SQLITE_NOMEM;

        EXPECT_EQ(FakeSqliteApi::bind_int(stmt, 1, 10), SQLITE_OK);
        EXPECT_EQ(FakeSqliteApi::bind_int(stmt, 1, 20), SQLITE_NOMEM);
        EXPECT_EQ(FakeSqliteApi::bind_int(stmt, 1, 30), SQLITE_OK);

        sqlite3_finalize(stmt);
        sqlite3_close(raw);
        FakeSqliteApi::reset();
    }

} // namespace
