#include <gtest/gtest.h>

// Textual includes must precede `import std;` (COMPILER_ISSUES.md section 9).
#include "test_db_helpers.h"
#include <libpq-fe.h>

// #787: structural checks on the compile-time libpq API policy, plus the fake's own
// semantics. The error paths themselves are exercised by test_pq_fake_errors.cpp.

import std;
import storm_db_concept;
import storm_db_postgresql;

#include "fake_pq_api.hpp" // NOSONAR cpp:S954 - must follow the imports

namespace {

    namespace pg = storm::db::postgresql;
    using fake_pq::FakeConn;
    using fake_pq::FakePqApi;
    using fake_pq::FakePqConfig;

    // The fake is a distinct backend type, not the production one.
    static_assert(!std::same_as<FakeConn, pg::Connection>);
    static_assert(std::derived_from<pg::Connection, pg::BasicConnection<pg::RealApi>>);
    static_assert(std::same_as<pg::Connection::Statement, pg::BasicStatement<pg::RealApi>>);
    static_assert(storm::db::CachedDatabaseConnection<FakeConn>);
    static_assert(storm::db::DatabaseStatement<pg::BasicStatement<FakePqApi>>);
    static_assert(storm::db::SupportsPgDialect<FakeConn>);
    static_assert(storm::db::TransactionCapable<FakeConn>);

    // Live tests: a reachable server (STORM_PG_CONNSTR), an unarmed plan, and an open
    // FakeConn. The plan is reset again on the way out so no arming leaks.
    class PqApiPolicyLiveTest : public ::testing::Test {
      public:
        std::string             connstr_;
        std::optional<FakeConn> conn_;

        auto SetUp() -> void override {
            if (!storm::test::backend_available<pg::Connection>()) {
                GTEST_SKIP() << "PostgreSQL not available (STORM_PG_CONNSTR unset / unreachable)";
            }
            connstr_ = storm::test::get_connection_string<pg::Connection>();
            FakePqConfig::reset();
            auto opened = FakeConn::open(connstr_);
            ASSERT_TRUE(opened.has_value());
            conn_.emplace(std::move(*opened));
        }

        auto TearDown() -> void override {
            FakePqConfig::reset();
        }
    };

    TEST_F(PqApiPolicyLiveTest, ProductionConnectionIgnoresAnArmedFakePlan) {
        // A failing fake plan must never reach the production connection type.
        FakePqConfig::reset();
        FakePqConfig::connectdb_returns_null();

        auto conn = pg::Connection::open(connstr_);

        ASSERT_TRUE(conn.has_value());
        EXPECT_EQ(FakePqConfig::get_connectdb_call_count(), 0);
    }

    TEST_F(PqApiPolicyLiveTest, FakeForwardsToRealLibpqWhenNothingIsArmed) {
        auto stmt = conn_->prepare("SELECT 7");
        ASSERT_TRUE(stmt.has_value());
        auto step = stmt->step();

        ASSERT_TRUE(step.has_value() && *step);
        EXPECT_EQ(stmt->extract_int(0), 7);
        EXPECT_EQ(FakePqConfig::get_prepare_call_count(), 1);
        EXPECT_EQ(FakePqConfig::get_exec_prepared_call_count(), 1);
    }

    // --- The fake's own semantics: all ported error tests depend on them. ---------

    TEST_F(PqApiPolicyLiveTest, FailOnCallNHitsOnlyThatPrepareCall) {
        FakePqConfig::prepare_fails_on_call(2);

        EXPECT_TRUE(conn_->prepare("SELECT 1").has_value());
        EXPECT_FALSE(conn_->prepare("SELECT 2").has_value());
        EXPECT_TRUE(conn_->prepare("SELECT 3").has_value());
        EXPECT_EQ(FakePqConfig::get_last_prepare_query(), "SELECT 3");
    }

    TEST_F(PqApiPolicyLiveTest, ExecFailOnCallNReturnsFatalErrorThenForwards) {
        FakePqConfig::exec_fails_on_call(1);

        const auto failed = conn_->execute("SELECT 1");
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), PGRES_FATAL_ERROR);
        EXPECT_TRUE(conn_->execute("SELECT 1").has_value());
        EXPECT_EQ(FakePqConfig::get_exec_call_count(), 2);
    }

    TEST(PqApiPolicyTest, ResetClearsEveryArmedFailureAndCounter) {
        FakePqConfig::status_returns(CONNECTION_BAD);
        FakePqConfig::prepare_fails_on_call(3);
        FakePqConfig::exec_returns_null();
        FakePqApi::plan.exec_calls = 7;

        FakePqConfig::reset();

        const auto& plan = FakePqApi::plan;
        EXPECT_FALSE(plan.status.has_value());
        EXPECT_EQ(plan.prepare_fail_on, -1);
        EXPECT_FALSE(plan.exec_null);
        EXPECT_EQ(plan.exec_calls, 0);
    }

} // namespace
