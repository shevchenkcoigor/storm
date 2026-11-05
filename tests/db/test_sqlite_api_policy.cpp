#include <gtest/gtest.h>

// Textual includes must precede `import std;` (COMPILER_ISSUES.md section 9).
#include <cstdint>
#include <meta>
#include <sqlite3.h>

// #760 spike: error-path tests driven by a compile-time API policy instead of the
// LD_PRELOAD mock. `FakeSqliteApi` forwards to the real RealApi and substitutes a
// return code where a test asks for one, so every test here runs in storm_tests on
// every platform (the LD_PRELOAD mock binaries are Linux-only).

import std;
import storm_db_concept;
import storm_db_default_connection;
import storm_db_sqlite;
import storm_model;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_insert;

namespace {

    namespace sq = storm::db::sqlite;

    // Failure plan for one test. thread_local so in-process threads never share it.
    struct FakePlan {
        int prepare_rc        = SQLITE_OK;
        int step_rc           = SQLITE_OK;
        int bind_text_rc      = SQLITE_OK;
        int bind_text_fail_on = 0; // 1-based call number; 0 = every call uses bind_text_rc
        int busy_timeout_rc   = SQLITE_OK;
        int exec_rc           = SQLITE_OK;
        int open_rc           = SQLITE_OK;
        int bind_text_calls   = 0;
    };

    struct FakeSqliteApi {
        static inline thread_local FakePlan plan{};

        static auto reset() -> void {
            plan = FakePlan{};
        }

        static auto open_v2(const char* path, sqlite3** db, int flags, const char* vfs) noexcept -> int {
            return plan.open_rc != SQLITE_OK ? plan.open_rc : sq::RealApi::open_v2(path, db, flags, vfs);
        }
        static auto busy_timeout(sqlite3* db, int timeout_ms) noexcept -> int {
            return plan.busy_timeout_rc != SQLITE_OK ? plan.busy_timeout_rc : sq::RealApi::busy_timeout(db, timeout_ms);
        }
        static auto exec(sqlite3* db, const char* sql, char** errmsg) noexcept -> int {
            return plan.exec_rc != SQLITE_OK ? plan.exec_rc : sq::RealApi::exec(db, sql, errmsg);
        }
        static auto prepare_v2(sqlite3* db, const char* sql, int len, sqlite3_stmt** out) noexcept -> int {
            return plan.prepare_rc != SQLITE_OK ? plan.prepare_rc : sq::RealApi::prepare_v2(db, sql, len, out);
        }
        static auto step(sqlite3_stmt* stmt) noexcept -> int {
            return plan.step_rc != SQLITE_OK ? plan.step_rc : sq::RealApi::step(stmt);
        }
        static auto bind_int(sqlite3_stmt* stmt, int idx, int value) noexcept -> int {
            return sq::RealApi::bind_int(stmt, idx, value);
        }
        static auto bind_int64(sqlite3_stmt* stmt, int idx, std::int64_t value) noexcept -> int {
            return sq::RealApi::bind_int64(stmt, idx, value);
        }
        static auto bind_double(sqlite3_stmt* stmt, int idx, double value) noexcept -> int {
            return sq::RealApi::bind_double(stmt, idx, value);
        }
        static auto bind_text(sqlite3_stmt* stmt, int idx, std::string_view value) noexcept -> int {
            ++plan.bind_text_calls;
            const bool hit = plan.bind_text_fail_on == 0 || plan.bind_text_calls == plan.bind_text_fail_on;
            return (plan.bind_text_rc != SQLITE_OK && hit) ? plan.bind_text_rc
                                                           : sq::RealApi::bind_text(stmt, idx, value);
        }
        static auto bind_null(sqlite3_stmt* stmt, int idx) noexcept -> int {
            return sq::RealApi::bind_null(stmt, idx);
        }
        static auto bind_blob(sqlite3_stmt* stmt, int idx, const void* data, std::size_t size) noexcept -> int {
            return sq::RealApi::bind_blob(stmt, idx, data, size);
        }
        static auto expanded_sql(sqlite3_stmt* stmt) noexcept -> char* {
            return sq::RealApi::expanded_sql(stmt);
        }
    };

    using FakeConn = sq::BasicConnection<FakeSqliteApi>;

    // The fake is a distinct backend type, not the default one.
    static_assert(!std::same_as<FakeConn, sq::Connection>);
    static_assert(std::derived_from<sq::Connection, sq::BasicConnection<sq::RealApi>>);
    static_assert(storm::db::CachedDatabaseConnection<FakeConn>);

    struct ApiPolicyPerson {
        [[= storm::primary]] std::int64_t id{};
        std::string                       name;
        int                               age{};
    };

    using FakeDefault  = storm::db::BasicDefaultConnection<FakeConn>;
    using FakeQuerySet = storm::BasicQuerySet<ApiPolicyPerson, FakeConn>;

    class SqliteApiPolicyTest : public ::testing::Test {
      public:
        void SetUp() override {
            FakeSqliteApi::reset();
            ASSERT_TRUE(FakeDefault::set(":memory:").has_value());
            // Real DDL, run before any failure is armed: the fake forwards to real
            // SQLite, so the table has to exist for update/select to reach step().
            const auto created =
                    storm::orm::schema::SchemaStatement<ApiPolicyPerson>::create_table_if_not_exists<FakeConn>();
            ASSERT_TRUE(created.has_value());
        }
        void TearDown() override {
            FakeSqliteApi::reset();
            FakeDefault::clear();
        }
    };

    const ApiPolicyPerson alice{.id = 0, .name = "Alice", .age = 30};

    TEST_F(SqliteApiPolicyTest, InsertFailsOnPrepareError) {
        FakeSqliteApi::plan.prepare_rc = SQLITE_ERROR;

        auto result = FakeQuerySet{}.insert(alice).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(SqliteApiPolicyTest, InsertFailsOnStepIoError) {
        FakeSqliteApi::plan.step_rc = SQLITE_IOERR;

        auto result = FakeQuerySet{}.insert(alice).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(SqliteApiPolicyTest, InsertFailsOnStepCorrupt) {
        FakeSqliteApi::plan.step_rc = SQLITE_CORRUPT;

        auto result = FakeQuerySet{}.insert(alice).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(SqliteApiPolicyTest, SelectFailsOnPrepareError) {
        FakeSqliteApi::plan.prepare_rc = SQLITE_ERROR;

        auto result = FakeQuerySet{}.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(SqliteApiPolicyTest, UpdateFailsOnTextBindNomem) {
        FakeSqliteApi::plan.bind_text_rc = SQLITE_NOMEM;
        const ApiPolicyPerson updated{.id = 1, .name = "Updated", .age = 35};

        auto result = FakeQuerySet{}.update(updated).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(SqliteApiPolicyTest, UpdateFailsOnStepBusy) {
        FakeSqliteApi::plan.step_rc = SQLITE_BUSY;
        const ApiPolicyPerson updated{.id = 1, .name = "Updated", .age = 35};

        auto result = FakeQuerySet{}.update(updated).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(SqliteApiPolicyTest, FailsOnlyTheNthTextBind) {
        // Second text bind fails: proves per-call selectivity, which the LD_PRELOAD
        // mock offered as bind_text_fails_on_call().
        FakeSqliteApi::plan.bind_text_rc      = SQLITE_NOMEM;
        FakeSqliteApi::plan.bind_text_fail_on = 2;

        auto first  = FakeQuerySet{}.insert(alice).execute();
        auto second = FakeQuerySet{}.insert(alice).execute();

        EXPECT_TRUE(first.has_value());
        ASSERT_FALSE(second.has_value());
        EXPECT_EQ(second.error().code(), SQLITE_NOMEM);
    }

    TEST_F(SqliteApiPolicyTest, ConnectionOpenFailsOnOpenError) {
        FakeSqliteApi::plan.open_rc = SQLITE_CANTOPEN;

        auto conn = FakeConn::open("nonexistent.db");

        ASSERT_FALSE(conn.has_value());
        EXPECT_EQ(conn.error().code(), SQLITE_CANTOPEN);
    }

    TEST_F(SqliteApiPolicyTest, ConnectionOpenFailsOnBusyTimeoutError) {
        FakeSqliteApi::plan.busy_timeout_rc = SQLITE_ERROR;

        auto conn = FakeConn::open(":memory:");

        ASSERT_FALSE(conn.has_value());
        EXPECT_EQ(conn.error().code(), SQLITE_ERROR);
    }

    TEST_F(SqliteApiPolicyTest, ConnectionOpenFailsOnWalPragmaError) {
        FakeSqliteApi::plan.exec_rc = SQLITE_ERROR;

        auto conn = FakeConn::open(":memory:", {.journal_mode = sq::JournalMode::WAL});

        ASSERT_FALSE(conn.has_value());
        EXPECT_EQ(conn.error().code(), SQLITE_ERROR);
    }

    TEST_F(SqliteApiPolicyTest, DefaultPolicyIsUnaffectedByFakePlan) {
        // A failing fake plan must not leak into the production connection type.
        FakeSqliteApi::plan.open_rc = SQLITE_CANTOPEN;

        auto conn = sq::Connection::open(":memory:");

        EXPECT_TRUE(conn.has_value());
    }

} // namespace
