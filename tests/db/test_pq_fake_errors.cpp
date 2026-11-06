/**
 * @file test_pq_fake_errors.cpp
 * @brief PostgreSQL backend error paths, driven by the fake libpq API policy (#787)
 *
 * Ported from the removed preloaded libpq mock (tests/mock_libpq). The fake
 * (fake_pq_api.hpp) forwards every call to a live PostgreSQL server and only
 * substitutes a result where a test armed one, so these tests need a server and
 * skip without one (STORM_PG_CONNSTR), like every other PG test.
 *
 * Real PostgreSQL is stricter than the old fabricated mock: SQL is genuinely
 * prepared and executed, so the fixture creates the tables the statements name,
 * and each armed failure uses SQL that would otherwise SUCCEED — a failure can only
 * come from the fake, never vacuously from the server.
 *
 * Error paths tested:
 * - Connection failures (null PGconn, CONNECTION_BAD)
 * - Prepare failures (null result, PGRES_FATAL_ERROR)
 * - Execute failures (null result, SQL errors)
 * - Statement move assignment, DEALLOCATE on destruction
 * - BLOB extraction (null, non-hex, upper/lowercase hex, invalid digits)
 * - Cache utilities, connection accessors
 * - Quote/comment handling in placeholder translation
 * - ORM INSERT RETURNING / LIMIT ALL / schema paths on the PG dialect
 */

#include <gtest/gtest.h>
#include <meta>

// Textual includes must precede `import std;` (COMPILER_ISSUES.md section 9).
#include "test_db_helpers.h"
#include <libpq-fe.h>
#include <unistd.h>

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR(cpp:S125)
// NOLINTBEGIN(misc-const-correctness,bugprone-unused-return-value,readability-identifier-length) // NOSONAR(cpp:S125)

import std;
import storm;
import storm_db_default_connection;
using storm::primary;
using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;

#include "fake_pq_api.hpp" // NOSONAR cpp:S954 - must follow the imports

namespace {

    namespace pg = storm::db::postgresql;
    using fake_pq::FakeConn;
    using fake_pq::FakePqApi;
    using fake_pq::FakePqConfig;
    using PgStatement = FakeConn::Statement;
    using PgError     = FakeConn::Error;
    using FakeDefault = storm::db::BasicDefaultConnection<FakeConn>;

    // Tables every statement-level test may name. `"col?"` exists so the quote
    // handling tests can prepare a quoted identifier containing a `?`.
    constexpr std::array<std::string_view, 5> kTableDdl = {
            "CREATE TABLE test (id INTEGER, name TEXT, big BIGINT, price DOUBLE PRECISION, note TEXT, data BYTEA)",
            R"(CREATE TABLE t (id INTEGER, col TEXT, name TEXT, age INTEGER, a INTEGER, b INTEGER, "col?" TEXT))",
            "CREATE TABLE persons (id INTEGER)",
            "CREATE TABLE messages (id INTEGER)",
            "CREATE TABLE person_addresses (id INTEGER)",
    };

    /**
     * Base fixture: skips without a reachable server, gives the process its own
     * schema with the tables above, and resets the fake plan on both ends. Helpers
     * open connections WITHOUT the fake counting (or failing) the setup calls.
     */
    class PqFakeTest : public ::testing::Test {
      public:
        std::string connstr_;
        std::string schema_;

        auto SetUp() -> void override {
            FakePqConfig::reset();
            if (!storm::test::backend_available<pg::Connection>()) {
                GTEST_SKIP() << "PostgreSQL not available (STORM_PG_CONNSTR unset / unreachable)";
            }
            connstr_   = storm::test::get_connection_string<pg::Connection>();
            schema_    = std::format("pqfake_{}", getpid());
            auto admin = open_admin();
            ASSERT_TRUE(admin.has_value());
            ASSERT_TRUE(admin->execute(std::format("DROP SCHEMA IF EXISTS {} CASCADE", schema_)).has_value());
            ASSERT_TRUE(admin->execute(std::format("CREATE SCHEMA {}", schema_)).has_value());
            ASSERT_TRUE(admin->execute(std::format("SET search_path TO {}", schema_)).has_value());
            for (const auto ddl : kTableDdl) {
                ASSERT_TRUE(admin->execute(ddl).has_value()) << ddl;
            }
        }

        auto TearDown() -> void override {
            FakePqConfig::reset();
            FakeDefault::clear();
            if (!schema_.empty()) {
                if (auto admin = open_admin(); admin.has_value()) {
                    std::ignore = admin->execute(std::format("DROP SCHEMA IF EXISTS {} CASCADE", schema_));
                }
            }
        }

        // Production connection for schema setup/teardown; its DROP ... CASCADE
        // NOTICEs are noise in the test log.
        [[nodiscard]] auto open_admin() const -> std::expected<pg::Connection, PgError> {
            auto admin = pg::Connection::open(connstr_);
            if (admin.has_value()) {
                std::ignore = admin->execute("SET client_min_messages TO warning");
            }
            return admin;
        }

        // A FakeConn on the test schema. The armed plan (and its counters) is set
        // aside while connecting, so an arming made before this call applies only
        // to what the test does next.
        auto open_test_conn() -> std::expected<FakeConn, PgError> {
            const auto armed = FakePqApi::plan;
            FakePqApi::reset();
            auto conn = FakeConn::open(connstr_);
            if (conn.has_value()) {
                std::ignore = conn->execute(std::format("SET search_path TO {}", schema_));
            }
            FakePqApi::plan = armed;
            return conn;
        }

        // Install a FakeConn on the test schema as this thread's default connection.
        auto set_default_conn() -> void {
            const auto armed = FakePqApi::plan;
            FakePqApi::reset();
            ASSERT_TRUE(FakeDefault::set(connstr_).has_value());
            ASSERT_TRUE(FakeDefault::get()->execute(std::format("SET search_path TO {}", schema_)).has_value());
            FakePqApi::plan = armed;
        }

        // Single value of a one-row scalar query, read with the production extractor.
        static auto scalar(FakeConn& conn, std::string_view sql) -> std::int64_t {
            auto stmt = conn.prepare(sql);
            EXPECT_TRUE(stmt.has_value()) << sql;
            if (!stmt.has_value()) {
                return -1;
            }
            const auto row = stmt->step();
            EXPECT_TRUE(row.has_value() && *row) << sql;
            return stmt->extract_int64(0);
        }
    };

    // Prepares `sql` and steps to its first row; the statement borrows `conn`.
    auto first_row(FakeConn& conn, std::string_view sql) -> std::optional<PgStatement> {
        auto stmt = conn.prepare(sql);
        if (!stmt.has_value()) {
            ADD_FAILURE() << "prepare failed: " << sql;
            return std::nullopt;
        }
        const auto row = stmt->step();
        if (!row.has_value() || !*row) {
            ADD_FAILURE() << "no first row: " << sql;
            return std::nullopt;
        }
        return std::move(*stmt);
    }

    // ============================================================================
    // Connection Error Tests
    // ============================================================================

    class PgConnectionErrorTest : public PqFakeTest {};

    TEST_F(PgConnectionErrorTest, ConnectdbReturnsNull) {
        FakePqConfig::connectdb_returns_null();
        auto result = FakeConn::open(connstr_);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Failed to allocate PGconn");
    }

    TEST_F(PgConnectionErrorTest, StatusReturnsBad) {
        // The server is reachable, so only the armed status can make open() fail.
        FakePqConfig::status_returns(CONNECTION_BAD);
        FakePqConfig::error_message("Connection refused");
        auto result = FakeConn::open(connstr_);

        ASSERT_FALSE(result.has_value());
        // Issue #351: the status and message MUST be captured before PQfinish(raw_conn).
        // The fake calls the real PQstatus/PQerrorMessage on the handle before returning
        // the armed value, so a read after PQfinish is a genuine use-after-free that the
        // CI ASAN job reports (the assertions alone cannot see it).
        EXPECT_EQ(result.error().code(), CONNECTION_BAD);
        EXPECT_EQ(result.error().message(), "Connection refused");
    }

    TEST_F(PgConnectionErrorTest, ErrorCodeAndMessage) {
        FakePqConfig::connectdb_returns_null();
        auto result = FakeConn::open(connstr_);

        ASSERT_FALSE(result.has_value());
        const PgError& err = result.error();
        EXPECT_EQ(err.code(), -1);
        EXPECT_EQ(err.message(), "Failed to allocate PGconn");
        EXPECT_EQ(FakePqConfig::get_connectdb_call_count(), 1);
    }

    // ============================================================================
    // Prepare Error Tests
    // ============================================================================

    class PgPrepareErrorTest : public PqFakeTest {};

    TEST_F(PgPrepareErrorTest, PrepareOnClosedConnection) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto moved = std::move(*conn_result); // conn_result is now moved-from (is_open() == false)

        auto result = conn_result->prepare("SELECT 1"); // NOLINT(bugprone-use-after-move)
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Connection not open");
    }

    // Issue #271: the target of a move assignment takes over the source's open
    // handle and the source becomes closed.
    TEST_F(PgPrepareErrorTest, MoveAssignmentTransfersHandle) {
        auto src = open_test_conn();
        ASSERT_TRUE(src.has_value());
        auto dst = open_test_conn();
        ASSERT_TRUE(dst.has_value());

        *dst = std::move(*src);
        EXPECT_TRUE(dst->is_open());
        EXPECT_FALSE(src->is_open()); // NOLINT(bugprone-use-after-move) -- intentional moved-from check
        EXPECT_EQ(scalar(*dst, "SELECT 5"), 5);
    }

    TEST_F(PgPrepareErrorTest, PrepareReturnsNull) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        FakePqConfig::prepare_returns_null();
        auto result = conn_result->prepare("SELECT 1");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(FakePqConfig::get_prepare_call_count(), 1);
    }

    TEST_F(PgPrepareErrorTest, PrepareReturnsFatalError) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        FakePqConfig::prepare_status(PGRES_FATAL_ERROR);
        FakePqConfig::error_message("syntax error");
        auto result = conn_result->prepare("SELECT 1"); // valid SQL: only the fake can fail it

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "syntax error");
    }

    TEST_F(PgPrepareErrorTest, PrepareCachedOnClosedConnection) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto moved = std::move(*conn_result);

        auto result = conn_result->prepare_cached("SELECT 1"); // NOLINT(bugprone-use-after-move)
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Connection not open");
    }

    TEST_F(PgPrepareErrorTest, PrepareCachedReturnsNull) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        FakePqConfig::prepare_returns_null();
        auto result = conn_result->prepare_cached("SELECT 1");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(conn_result->cached_statement_count(), 0U); // a failed prepare is not cached
    }

    TEST_F(PgPrepareErrorTest, PrepareCachedReturnsFatalError) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        FakePqConfig::prepare_status(PGRES_FATAL_ERROR);
        FakePqConfig::error_message("relation not found");
        auto result = conn_result->prepare_cached("SELECT * FROM persons");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "relation not found");
    }

    // ============================================================================
    // Execute (Direct SQL) Error Tests
    // ============================================================================

    class PgExecuteErrorTest : public PqFakeTest {};

    TEST_F(PgExecuteErrorTest, ExecuteOnClosedConnection) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto moved = std::move(*conn_result);

        auto result = conn_result->execute("SELECT 1"); // NOLINT(bugprone-use-after-move)
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Connection not open");
    }

    TEST_F(PgExecuteErrorTest, ExecuteReturnsNull) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        FakePqConfig::exec_returns_null();
        auto result = conn_result->execute("SELECT 1");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    TEST_F(PgExecuteErrorTest, ExecuteReturnsSqlError) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        FakePqConfig::exec_status(PGRES_FATAL_ERROR);
        FakePqConfig::error_message("table already exists");
        auto result = conn_result->execute("SELECT 1");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), PGRES_FATAL_ERROR);
        EXPECT_EQ(result.error().message(), "table already exists");
    }

    TEST_F(PgExecuteErrorTest, ExecuteSuccess) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto result = conn_result->execute("CREATE TABLE exec_ok (id INTEGER)");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(scalar(*conn_result, "SELECT count(*) FROM exec_ok"), 0);
    }

    // ============================================================================
    // Statement Execute / Step Error Tests
    // ============================================================================

    class PgStatementErrorTest : public PqFakeTest {};

    TEST_F(PgStatementErrorTest, ExecPreparedReturnsFatalError) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test (id) VALUES (1)");
        ASSERT_TRUE(stmt_result.has_value());

        FakePqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        FakePqConfig::error_message("constraint violation");

        auto exec_result = stmt_result->execute();
        ASSERT_FALSE(exec_result.has_value());
        EXPECT_EQ(exec_result.error().code(), PGRES_FATAL_ERROR);
        EXPECT_EQ(exec_result.error().message(), "constraint violation");
        FakePqConfig::reset();
        EXPECT_EQ(scalar(*conn_result, "SELECT count(*) FROM test"), 0); // never reached the server
    }

    TEST_F(PgStatementErrorTest, StepAutoExecuteError) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT * FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // step() auto-executes if result_ is null
        FakePqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        auto step_result = stmt_result->step();

        ASSERT_FALSE(step_result.has_value());
        EXPECT_EQ(step_result.error().code(), PGRES_FATAL_ERROR);
    }

    TEST_F(PgStatementErrorTest, StepReturnsFalseAtEnd) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        auto step1 = stmt_result->step();
        ASSERT_TRUE(step1.has_value());
        EXPECT_TRUE(step1.value()); // First row available

        auto step2 = stmt_result->step();
        ASSERT_TRUE(step2.has_value());
        EXPECT_FALSE(step2.value()); // No more rows
    }

    TEST_F(PgStatementErrorTest, StepRawAutoExecuteError) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT * FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        FakePqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        int raw_result = stmt_result->step_raw();

        EXPECT_EQ(raw_result, -1); // Error
    }

    TEST_F(PgStatementErrorTest, StatementMoveAssignment) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt1_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt1_result.has_value());

        auto stmt2_result = conn_result->prepare("SELECT 2");
        ASSERT_TRUE(stmt2_result.has_value());

        *stmt1_result = std::move(*stmt2_result);

        // stmt1 now runs stmt2's server-side statement
        auto step = stmt1_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());
        EXPECT_EQ(stmt1_result->extract_int(0), 2);
    }

    TEST_F(PgStatementErrorTest, GetErrorMessage) {
        FakePqConfig::error_message("test error from PQ");
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        const char* msg = stmt_result->get_error_message();
        EXPECT_STREQ(msg, "test error from PQ");
    }

    TEST_F(PgStatementErrorTest, PrepareAndExecuteSuccess) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1)");
        ASSERT_TRUE(stmt_result.has_value());
        ASSERT_TRUE(stmt_result->bind_int(1, 5).has_value());

        auto exec_result = stmt_result->execute();
        ASSERT_TRUE(exec_result.has_value());
        EXPECT_EQ(scalar(*conn_result, "SELECT id FROM test"), 5);
    }

    TEST_F(PgStatementErrorTest, PrepareAndStepRawSuccess) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        int raw = stmt_result->step_raw();
        EXPECT_EQ(raw, PgStatement::ROW_AVAILABLE);

        raw = stmt_result->step_raw();
        EXPECT_EQ(raw, PgStatement::NO_MORE_ROWS);
    }

    TEST_F(PgStatementErrorTest, BindMethodsSucceed) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1, $2, $3, $4, $5, $6)");
        ASSERT_TRUE(stmt_result.has_value());

        EXPECT_TRUE(stmt_result->bind_int(1, 42).has_value());
        EXPECT_TRUE(stmt_result->bind_text(2, "hello").has_value());
        EXPECT_TRUE(stmt_result->bind_int64(3, 123456789LL).has_value());
        EXPECT_TRUE(stmt_result->bind_double(4, std::numbers::pi).has_value());
        EXPECT_TRUE(stmt_result->bind_null(5).has_value());
        std::vector<std::uint8_t> blob_data = {0xDE, 0xAD};
        EXPECT_TRUE(stmt_result->bind_blob(6, blob_data.data(), blob_data.size()).has_value());

        // The server accepts every bound value with its column's type.
        ASSERT_TRUE(stmt_result->execute().has_value());
        EXPECT_EQ(scalar(*conn_result, "SELECT big FROM test WHERE id = 42 AND note IS NULL"), 123456789LL);
        EXPECT_EQ(scalar(*conn_result, "SELECT length(data) FROM test WHERE name = 'hello'"), 2);
    }

    TEST_F(PgStatementErrorTest, ResetStatement) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_NE(stmt_result->handle(), nullptr);

        stmt_result->reset();

        EXPECT_EQ(stmt_result->handle(), nullptr);
    }

    TEST_F(PgStatementErrorTest, HandleReturnsNullBeforeExecute) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        EXPECT_EQ(stmt_result->handle(), nullptr);
    }

    // ============================================================================
    // expanded_sql() edge cases
    // ============================================================================

    TEST_F(PgStatementErrorTest, ExpandedSqlWithSingleQuote) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT 'literal' FROM t WHERE id = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        ASSERT_TRUE(stmt->bind_int(1, 42).has_value());

        EXPECT_EQ(stmt->expanded_sql(), "SELECT 'literal' FROM t WHERE id = '42'");
    }

    TEST_F(PgStatementErrorTest, ExpandedSqlWithDoubleQuote) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT \"col\" FROM t WHERE id = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        ASSERT_TRUE(stmt->bind_int(1, 42).has_value());

        EXPECT_EQ(stmt->expanded_sql(), "SELECT \"col\" FROM t WHERE id = '42'");
    }

    TEST_F(PgStatementErrorTest, ExpandedSqlWithNullParam) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT ? FROM t");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        ASSERT_TRUE(stmt->bind_null(1).has_value());

        EXPECT_EQ(stmt->expanded_sql(), "SELECT NULL FROM t");
    }

    TEST_F(PgStatementErrorTest, ExpandedSqlWithExtraPlaceholder) {
        // More ? in SQL than bound params: the unmatched ? stays as-is.
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT ? FROM t WHERE id = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        ASSERT_TRUE(stmt->bind_int(1, 42).has_value()); // Only bind first parameter

        EXPECT_EQ(stmt->expanded_sql(), "SELECT '42' FROM t WHERE id = ?");
    }

    TEST_F(PgStatementErrorTest, ExpandedSqlBoundParamWithSingleQuote) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT * FROM t WHERE name = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        ASSERT_TRUE(stmt->bind_text(1, "O'Brien").has_value());

        EXPECT_EQ(stmt->expanded_sql(), "SELECT * FROM t WHERE name = 'O''Brien'");
    }

    // ============================================================================
    // Statement finalize / destructor DEALLOCATE (issue #417)
    //
    // On cache eviction the unique_ptr<Statement> is destroyed. PG must DEALLOCATE
    // the server-side prepared statement, or it leaks for the connection lifetime.
    // ============================================================================

    TEST_F(PgStatementErrorTest, FinalizeStatement) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        stmt_result->finalize();

        EXPECT_EQ(stmt_result->handle(), nullptr);
        EXPECT_EQ(FakePqConfig::get_deallocate_call_count(), 1);
        EXPECT_EQ(scalar(*conn_result, "SELECT count(*) FROM pg_prepared_statements"), 1); // the probe itself
    }

    TEST_F(PgStatementErrorTest, DestructorDeallocatesPreparedStatement) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        std::string stmt_name;
        {
            auto stmt_result = conn_result->prepare("SELECT 1");
            ASSERT_TRUE(stmt_result.has_value());
            stmt_name = FakePqConfig::get_last_prepare_name();
            // No explicit finalize() — rely on the destructor.
        }

        EXPECT_EQ(FakePqConfig::get_deallocate_call_count(), 1);
        EXPECT_EQ(FakePqConfig::get_last_exec_query(), std::format("DEALLOCATE {}", stmt_name));
        EXPECT_EQ(scalar(*conn_result, "SELECT count(*) FROM pg_prepared_statements"), 1); // the probe itself
    }

    TEST_F(PgStatementErrorTest, MovedFromStatementDoesNotDeallocate) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        {
            auto stmt_result = conn_result->prepare("SELECT 1");
            ASSERT_TRUE(stmt_result.has_value());

            // `moved` deallocates once; the moved-from `*stmt_result` must not.
            PgStatement moved = std::move(*stmt_result);
        }

        EXPECT_EQ(FakePqConfig::get_deallocate_call_count(), 1);
    }

    // ============================================================================
    // Bind + execute (rebuild_param_ptrs)
    // ============================================================================

    TEST_F(PgStatementErrorTest, BindAndExecuteCoversRebuildParamPtrs) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1, $2)");
        ASSERT_TRUE(stmt_result.has_value());

        EXPECT_TRUE(stmt_result->bind_int(1, 42).has_value());
        EXPECT_TRUE(stmt_result->bind_text(2, "hello").has_value());

        ASSERT_TRUE(stmt_result->execute().has_value());
        EXPECT_EQ(scalar(*conn_result, "SELECT count(*) FROM test WHERE id = 42 AND name = 'hello'"), 1);
    }

    TEST_F(PgStatementErrorTest, BindNullAndExecuteCoversRebuildPtrsBranch) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1, $2)");
        ASSERT_TRUE(stmt_result.has_value());

        EXPECT_TRUE(stmt_result->bind_int(1, 42).has_value());
        EXPECT_TRUE(stmt_result->bind_null(2).has_value());

        ASSERT_TRUE(stmt_result->execute().has_value());
        EXPECT_EQ(scalar(*conn_result, "SELECT count(*) FROM test WHERE id = 42 AND name IS NULL"), 1);
    }

    TEST_F(PgStatementErrorTest, BindBlobNull) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test (data) VALUES ($1)");
        ASSERT_TRUE(stmt_result.has_value());

        EXPECT_TRUE(stmt_result->bind_blob(1, nullptr, 0).has_value());

        // An empty blob is stored as a zero-length, NON-null BYTEA.
        ASSERT_TRUE(stmt_result->execute().has_value());
        EXPECT_EQ(scalar(*conn_result, "SELECT length(data) FROM test WHERE data IS NOT NULL"), 0);
    }

    // ============================================================================
    // BLOB Extraction Tests (text result format, real server values)
    // ============================================================================

    class PgBlobExtractionTest : public PqFakeTest {};

    TEST_F(PgBlobExtractionTest, ExtractBlobPtrNull) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT NULL::bytea");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_EQ(stmt->extract_blob_ptr(0), nullptr);
        EXPECT_EQ(stmt->extract_bytes(0), 0);
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobNonHexFallback) {
        // A value without the \x prefix is copied through as raw bytes.
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 'rawdata'::text");
        ASSERT_TRUE(stmt.has_value());

        const void* blob = stmt->extract_blob_ptr(0);
        ASSERT_NE(blob, nullptr);
        EXPECT_EQ(stmt->extract_bytes(0), 7);
        EXPECT_EQ(std::string_view(static_cast<const char*>(blob), 7), "rawdata");
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobUppercaseHex) {
        // The server always emits lowercase BYTEA hex; a text column carries uppercase.
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, R"(SELECT '\xDEADBEEF'::text)");
        ASSERT_TRUE(stmt.has_value());

        const void* blob = stmt->extract_blob_ptr(0);
        ASSERT_NE(blob, nullptr);
        EXPECT_EQ(stmt->extract_bytes(0), 4);

        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0xDE);
        EXPECT_EQ(data[1], 0xAD);
        EXPECT_EQ(data[2], 0xBE);
        EXPECT_EQ(data[3], 0xEF);
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobWithNumericHexDigits) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, R"(SELECT '\x0123456789'::bytea)");
        ASSERT_TRUE(stmt.has_value());

        const void* blob = stmt->extract_blob_ptr(0);
        ASSERT_NE(blob, nullptr);
        EXPECT_EQ(stmt->extract_bytes(0), 5); // 10 hex chars = 5 bytes

        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0x01);
        EXPECT_EQ(data[1], 0x23);
        EXPECT_EQ(data[2], 0x45);
        EXPECT_EQ(data[3], 0x67);
        EXPECT_EQ(data[4], 0x89);
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobInvalidHexDigitFallback) {
        // Invalid hex characters decode to 0 (hex_digit() fallback).
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, R"(SELECT '\xGG'::text)");
        ASSERT_TRUE(stmt.has_value());

        const void* blob = stmt->extract_blob_ptr(0);
        ASSERT_NE(blob, nullptr);
        EXPECT_EQ(stmt->extract_bytes(0), 1); // 2 hex chars = 1 byte

        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0x00);
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobLowercaseHex) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, R"(SELECT '\xdeadbeef'::bytea)");
        ASSERT_TRUE(stmt.has_value());

        const void* blob = stmt->extract_blob_ptr(0);
        ASSERT_NE(blob, nullptr);
        EXPECT_EQ(stmt->extract_bytes(0), 4);

        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0xDE);
        EXPECT_EQ(data[1], 0xAD);
        EXPECT_EQ(data[2], 0xBE);
        EXPECT_EQ(data[3], 0xEF);
    }

    // ============================================================================
    // Column extraction (text result format, real server values)
    // ============================================================================

    class PgColumnExtractionTest : public PqFakeTest {};

    TEST_F(PgColumnExtractionTest, ExtractIntValue) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 42");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_EQ(stmt->extract_int(0), 42);
    }

    TEST_F(PgColumnExtractionTest, ExtractInt64Value) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 9876543210::bigint");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_EQ(stmt->extract_int64(0), 9876543210LL);
    }

    TEST_F(PgColumnExtractionTest, ExtractDoubleValue) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 3.14159::float8");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_NEAR(stmt->extract_double(0), std::numbers::pi, 0.001);
    }

    TEST_F(PgColumnExtractionTest, ExtractTextPtr) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 'hello'::text");
        ASSERT_TRUE(stmt.has_value());

        const unsigned char* text = stmt->extract_text_ptr(0);
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(text)), "hello");
    }

    TEST_F(PgColumnExtractionTest, ExtractTextView) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 'world'::text");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_EQ(stmt->extract_text_view(0), "world");
    }

    TEST_F(PgColumnExtractionTest, ExtractTextViewNull) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT NULL::text");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_TRUE(stmt->extract_text_view(0).empty());
    }

    TEST_F(PgColumnExtractionTest, ExtractBoolTrue) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT true");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_TRUE(stmt->extract_bool(0));
    }

    TEST_F(PgColumnExtractionTest, ExtractBoolFalse) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT false");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_FALSE(stmt->extract_bool(0));
    }

    TEST_F(PgColumnExtractionTest, ExtractFloatValue) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 2.5::float4");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_NEAR(stmt->extract_float(0), 2.5F, 0.01F);
    }

    TEST_F(PgColumnExtractionTest, ExtractBytes) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT 'hello'::text");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_EQ(stmt->extract_bytes(0), 5);
    }

    TEST_F(PgColumnExtractionTest, IsNull) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        auto stmt = first_row(*conn_result, "SELECT NULL::text, 'x'::text");
        ASSERT_TRUE(stmt.has_value());

        EXPECT_TRUE(stmt->is_null(0));
        EXPECT_FALSE(stmt->is_null(1));
    }

    // ============================================================================
    // Cache Utility Tests
    // ============================================================================

    class PgCacheUtilityTest : public PqFakeTest {};

    TEST_F(PgCacheUtilityTest, CachedStatementCount) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        EXPECT_EQ(conn_result->cached_statement_count(), 0U);

        ASSERT_TRUE(conn_result->prepare_cached("SELECT 1").has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 1U);

        ASSERT_TRUE(conn_result->prepare_cached("SELECT 2").has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 2U);

        // Same SQL reuses cached statement
        ASSERT_TRUE(conn_result->prepare_cached("SELECT 1").has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 2U);
        EXPECT_EQ(FakePqConfig::get_prepare_call_count(), 2);
    }

    // Issue #273: cache_stats() snapshot — hits/misses/size track the same way as
    // SQLite (shared cache_* helpers).
    TEST_F(PgCacheUtilityTest, CacheStatsSnapshot) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        ASSERT_TRUE(conn_result->prepare_cached("SELECT 1").has_value()); // miss
        ASSERT_TRUE(conn_result->prepare_cached("SELECT 2").has_value()); // miss
        ASSERT_TRUE(conn_result->prepare_cached("SELECT 1").has_value()); // hit

        const auto stats = conn_result->cache_stats();
        EXPECT_EQ(stats.misses, 2U);
        EXPECT_EQ(stats.hits, 1U);
        EXPECT_EQ(stats.evictions, 0U);
        EXPECT_EQ(stats.current_size, 2U);
    }

    TEST_F(PgCacheUtilityTest, ClearStatementCache) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        ASSERT_TRUE(conn_result->prepare_cached("SELECT 1").has_value());
        ASSERT_TRUE(conn_result->prepare_cached("SELECT 2").has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 2U);

        conn_result->clear_statement_cache();
        EXPECT_EQ(conn_result->cached_statement_count(), 0U);
        EXPECT_EQ(FakePqConfig::get_deallocate_call_count(), 2); // #417: evicted entries DEALLOCATE
    }

    // Per-table clear (issue #215): word-boundary aware, so clearing "persons"
    // does not touch "person_addresses".
    TEST_F(PgCacheUtilityTest, ClearStatementCachePerTable) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        ASSERT_TRUE(conn_result->prepare_cached("SELECT id FROM persons").has_value());
        ASSERT_TRUE(conn_result->prepare_cached("SELECT id FROM messages").has_value());
        ASSERT_TRUE(conn_result->prepare_cached("SELECT id FROM person_addresses").has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 3U);

        conn_result->clear_statement_cache(std::string_view{"persons"});
        EXPECT_EQ(conn_result->cached_statement_count(), 2U)
                << "only the persons entry must be dropped — messages and person_addresses survive";
    }

    // ============================================================================
    // Connection Accessor Tests
    // ============================================================================

    class PgConnectionAccessorTest : public PqFakeTest {};

    TEST_F(PgConnectionAccessorTest, GetReturnsRawPointer) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        PGconn* raw = conn_result->get();
        ASSERT_NE(raw, nullptr);
        EXPECT_EQ(PQstatus(raw), CONNECTION_OK);
    }

    TEST_F(PgConnectionAccessorTest, PrepareCommonStatements) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());

        // A no-op for PostgreSQL: nothing is prepared, the connection still works.
        conn_result->prepare_common_statements();

        EXPECT_TRUE(conn_result->is_open());
        EXPECT_EQ(conn_result->cached_statement_count(), 0U);
    }

    TEST_F(PgConnectionAccessorTest, OpenSucceeds) {
        auto conn_result = open_test_conn();
        ASSERT_TRUE(conn_result.has_value());
        EXPECT_TRUE(conn_result->is_open());
    }

    // ============================================================================
    // Quote / comment handling in placeholder translation (#418). Each test
    // asserts the exact translated SQL the server was asked to prepare.
    // ============================================================================

    class PgQuoteHandlingTest : public PqFakeTest {
      public:
        // Prepares `sql` for real and returns the translated text sent to PQprepare.
        auto translated(std::string_view sql) -> std::string {
            auto conn = open_test_conn();
            EXPECT_TRUE(conn.has_value());
            if (!conn.has_value()) {
                return {};
            }
            EXPECT_TRUE(conn->prepare(sql).has_value()) << sql;
            return FakePqConfig::get_last_prepare_query();
        }
    };

    TEST_F(PgQuoteHandlingTest, SingleQuotedQuestionMarkNotTranslated) {
        EXPECT_EQ(translated("SELECT * FROM t WHERE col = 'literal?'"), "SELECT * FROM t WHERE col = 'literal?'");
    }

    TEST_F(PgQuoteHandlingTest, DoubleQuotedQuestionMarkNotTranslated) {
        EXPECT_EQ(translated("SELECT \"col?\" FROM t WHERE id = ?"), "SELECT \"col?\" FROM t WHERE id = $1");
    }

    TEST_F(PgQuoteHandlingTest, MixedQuotesAndPlaceholders) {
        EXPECT_EQ(
                translated("SELECT * FROM t WHERE name = 'test?' AND age = ? AND \"col?\" = ?"),
                "SELECT * FROM t WHERE name = 'test?' AND age = $1 AND \"col?\" = $2"
        );
    }

    TEST_F(PgQuoteHandlingTest, RealPlaceholdersStillTranslated) {
        EXPECT_EQ(translated("SELECT * FROM t WHERE a = ? AND b = ?"), "SELECT * FROM t WHERE a = $1 AND b = $2");
    }

    TEST_F(PgQuoteHandlingTest, AnonymousDollarQuoteNotTranslated) {
        // ? inside $$ ... $$ must survive; the real ? after it becomes $1.
        EXPECT_EQ(translated("SELECT $$ a ? b $$, ?"), "SELECT $$ a ? b $$, $1");
    }

    TEST_F(PgQuoteHandlingTest, TaggedDollarQuoteNotTranslated) {
        // A nested $$ inside a $tag$ body must NOT close the tag early.
        EXPECT_EQ(translated("SELECT $tag$ a ? $$ ? b $tag$, ?"), "SELECT $tag$ a ? $$ ? b $tag$, $1");
    }

    TEST_F(PgQuoteHandlingTest, EscapeStringQuestionMarkNotTranslated) {
        // E'it\'s ?' — the backslash-escaped quote must not close the string.
        EXPECT_EQ(translated("SELECT E'it\\'s ?', ?"), "SELECT E'it\\'s ?', $1");
    }

    TEST_F(PgQuoteHandlingTest, LineCommentQuestionMarkNotTranslated) {
        EXPECT_EQ(translated("SELECT ? -- pick ? one\n, ?"), "SELECT $1 -- pick ? one\n, $2");
    }

    TEST_F(PgQuoteHandlingTest, BlockCommentQuestionMarkNotTranslated) {
        // Nested block comment: the inner */ must not end the outer comment.
        EXPECT_EQ(translated("SELECT ? /* a ? /* b ? */ c ? */, ?"), "SELECT $1 /* a ? /* b ? */ c ? */, $2");
    }

    // ============================================================================
    // ORM-level PostgreSQL tests (QuerySet over the fake-policy connection):
    // INSERT RETURNING (insert.cppm), LIMIT ALL (base.cppm), PG schema DDL.
    // ============================================================================

    struct MockPgPerson {
        [[= primary]] std::int64_t id{};
        std::string                name;
        int                        age{};
    };

    using PgQuerySet = storm::BasicQuerySet<MockPgPerson, FakeConn>;

    class PgOrmInsertReturningTest : public PqFakeTest {
      public:
        auto SetUp() -> void override {
            PqFakeTest::SetUp();
            if (IsSkipped() || HasFatalFailure()) {
                return;
            }
            set_default_conn();
            ASSERT_TRUE(SchemaStatement<MockPgPerson>::create_table_if_not_exists(FakeDefault::get()).has_value());
            FakePqConfig::reset();
        }
    };

    TEST_F(PgOrmInsertReturningTest, InsertReturningSuccess) {
        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 1); // first IDENTITY value of the fresh table
    }

    TEST_F(PgOrmInsertReturningTest, InsertReturningPrepareError) {
        FakePqConfig::prepare_returns_null();

        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Bob", .age = 25};

        auto result = qs.insert(person).execute();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    TEST_F(PgOrmInsertReturningTest, InsertReturningStepError) {
        FakePqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        FakePqConfig::error_message("constraint violation");

        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Charlie", .age = 35};

        auto result = qs.insert(person).execute();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().message(), "constraint violation");
        FakePqConfig::reset();
        EXPECT_EQ(qs.count().execute(), 0); // the row never reached the server
    }

    TEST_F(PgOrmInsertReturningTest, InsertToSqlPrepareError) {
        FakePqConfig::prepare_returns_null();

        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).to_sql();
        ASSERT_FALSE(result.has_value());
    }

    TEST_F(PgOrmInsertReturningTest, InsertToSqlPgPathSuccess) {
        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).to_sql();
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->contains("'Alice'")) << *result;
        EXPECT_TRUE(result->contains("RETURNING")) << *result;
    }

    // LIMIT ALL path (offset without limit) — base.cppm append_limit_offset.
    TEST_F(PgOrmInsertReturningTest, SelectWithOffsetOnlyUsesLimitAll) {
        PgQuerySet qs;
        for (int i = 0; i < 7; ++i) {
            ASSERT_TRUE(qs.insert(MockPgPerson{.id = 0, .name = std::format("P{}", i), .age = i}).execute());
        }

        auto results = qs.offset(5).select().execute();
        ASSERT_TRUE(results.has_value());
        EXPECT_EQ(results->size(), 2U);
    }

    // QuerySet::reset(): clearing query state must leave the QuerySet usable.
    TEST_F(PgOrmInsertReturningTest, ResetInvalidatesSelectCache) {
        PgQuerySet qs;
        auto       results = qs.select().execute();
        ASSERT_TRUE(results.has_value());

        qs.reset();

        auto results2 = qs.select().execute();
        ASSERT_TRUE(results2.has_value());
        EXPECT_TRUE(results2->empty());
    }

    // ============================================================================
    // Concept-based mock connection with failing binds: the INSERT RETURNING
    // bind-error path, which real PG binds (text, never failing) cannot reach.
    // No server involved, so these tests do not skip.
    // ============================================================================

    struct BindFailError {
        int         code_;
        std::string message_;

        [[nodiscard]] constexpr auto code() const noexcept -> int {
            return code_;
        }
        [[nodiscard]] constexpr auto message() const noexcept -> std::string_view {
            return message_;
        }
    };

    class BindFailStatement {
      public:
        using Error = BindFailError;

        static constexpr int ROW_AVAILABLE = 1;
        static constexpr int NO_MORE_ROWS  = 0;

        template <typename = void>
        [[nodiscard]] auto bind_int(int /*index*/, int /*value*/) noexcept -> std::expected<void, Error> {
            return check_fail();
        }

        template <typename = void>
        [[nodiscard]] auto bind_text(int /*index*/, std::string_view /*value*/) noexcept -> std::expected<void, Error> {
            return check_fail();
        }

        template <typename = void>
        [[nodiscard]] auto bind_int64(int /*index*/, std::int64_t /*value*/) noexcept -> std::expected<void, Error> {
            return check_fail();
        }

        template <typename = void>
        [[nodiscard]] auto bind_double(int /*index*/, double /*value*/) noexcept -> std::expected<void, Error> {
            return check_fail();
        }

        template <typename = void> [[nodiscard]] auto bind_null(int /*index*/) noexcept -> std::expected<void, Error> {
            return check_fail();
        }

        template <typename = void>
        [[nodiscard]] auto
        bind_blob(int /*index*/, const void* /*data*/, std::size_t /*size*/) noexcept // NOSONAR(cpp:S5008)
                -> std::expected<void, Error> {
            return check_fail();
        }

        template <typename = void> [[nodiscard]] auto execute() noexcept -> std::expected<void, Error> {
            return {};
        }

        template <typename = void> [[nodiscard]] auto step() noexcept -> std::expected<bool, Error> {
            return true;
        }

        template <typename = void> [[nodiscard]] auto step_raw() noexcept -> int {
            return ROW_AVAILABLE;
        }

        template <typename = void> auto reset() noexcept -> void {
            // Intentionally empty.
        }

        template <typename = void> auto finalize() noexcept -> void {
            // Intentionally empty.
        }

        template <typename = void> [[nodiscard]] auto handle() const noexcept -> void* { // NOSONAR(cpp:S5008)
            return nullptr;
        }

        template <typename = void> [[nodiscard]] auto extract_int(int /*col_index*/) const noexcept -> int {
            return 0;
        }

        template <typename = void> [[nodiscard]] auto extract_int64(int /*col_index*/) const noexcept -> std::int64_t {
            return 0;
        }

        template <typename = void> [[nodiscard]] auto extract_double(int /*col_index*/) const noexcept -> double {
            return 0.0;
        }

        template <typename = void>
        [[nodiscard]] auto extract_text_ptr(int /*col_index*/) const noexcept -> const unsigned char* {
            return nullptr;
        }

        template <typename = void>
        [[nodiscard]] auto extract_text_view(int /*col_index*/) const noexcept -> std::string_view {
            return {};
        }

        template <typename = void> [[nodiscard]] auto extract_bytes(int /*col_index*/) noexcept -> int {
            return 0;
        }

        template <typename = void> [[nodiscard]] auto extract_bool(int /*col_index*/) const noexcept -> bool {
            return false;
        }

        template <typename = void> [[nodiscard]] auto extract_float(int /*col_index*/) const noexcept -> float {
            return 0.0F;
        }

        template <typename = void>
        [[nodiscard]] auto extract_blob_ptr(int /*col_index*/) noexcept -> const void* { // NOSONAR(cpp:S5008)
            return nullptr;
        }

        template <typename = void> [[nodiscard]] auto is_null(int /*col_index*/) const noexcept -> bool {
            return true;
        }

        template <typename = void> [[nodiscard]] auto get_error_message() const noexcept -> const char* {
            return "mock error";
        }

        template <typename = void> [[nodiscard]] auto expanded_sql() const noexcept -> std::string {
            return {};
        }

        auto set_fail(bool fail) noexcept -> void {
            fail_ = fail;
        }

      private:
        bool fail_ = false;

        [[nodiscard]] auto check_fail() const noexcept -> std::expected<void, Error> {
            if (fail_) {
                return std::unexpected(Error{-1, "mock bind failure"});
            }
            return {};
        }
    };

    class BindFailConnection {
      public:
        using Error     = BindFailError;
        using Statement = BindFailStatement;

        static constexpr bool supports_limit_all = true;
        static constexpr bool supports_returning = true;

        [[nodiscard]] static auto open(std::string_view /*conninfo*/) -> std::expected<BindFailConnection, Error> {
            return BindFailConnection{};
        }

        auto prepare_common_statements() -> void {
            // Intentionally empty.
        }

        [[nodiscard]] constexpr auto is_open() const noexcept -> bool {
            return true;
        }

        [[nodiscard]] auto prepare(std::string_view /*sql*/) -> std::expected<Statement, Error> {
            return Statement{};
        }

        [[nodiscard]] auto prepare_cached(std::string_view /*sql*/) -> std::expected<Statement*, Error> {
            cached_stmt_.set_fail(fail_binds_);
            return &cached_stmt_;
        }

        auto clear_statement_cache() noexcept -> void {
            // Intentionally empty.
        }

        auto clear_statement_cache(std::string_view /*table*/) noexcept -> void {
            // Intentionally empty.
        }

        [[nodiscard]] static auto cached_statement_count() noexcept -> std::size_t {
            return 0;
        }

        [[nodiscard]] static auto cache_stats() noexcept -> storm::db::CacheStats { // Issue #273
            return {};
        }

        [[nodiscard]] auto execute(std::string_view /*sql*/) -> std::expected<void, Error> {
            return {};
        }

        [[nodiscard]] auto get() const noexcept -> void* { // NOSONAR(cpp:S5008)
            return nullptr;
        }

        auto set_fail_binds(bool fail) noexcept -> void {
            fail_binds_ = fail;
        }

      private:
        BindFailConnection() = default;
        Statement cached_stmt_;
        bool      fail_binds_ = false;
    };

    static_assert(storm::db::DatabaseConnection<BindFailConnection>);
    static_assert(storm::db::CachedDatabaseConnection<BindFailConnection>);
    static_assert(storm::db::DatabaseStatement<BindFailStatement>);
    static_assert(storm::db::DatabaseError<BindFailError>);

    using BindFailQuerySet = storm::BasicQuerySet<MockPgPerson, BindFailConnection>;
    using BindFailDefault  = storm::db::BasicDefaultConnection<BindFailConnection>;

    TEST(PgInsertReturningBindErrorTest, BindFailureReturnsError) {
        ASSERT_TRUE(BindFailDefault::set("mock").has_value());
        BindFailDefault::get()->set_fail_binds(true);

        BindFailQuerySet   qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();
        BindFailDefault::clear();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "mock bind failure");
    }

    TEST(PgInsertReturningBindErrorTest, InsertToSqlBindFailureReturnsError) {
        ASSERT_TRUE(BindFailDefault::set("mock").has_value());
        BindFailDefault::get()->set_fail_binds(true);

        BindFailQuerySet   qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).to_sql();
        BindFailDefault::clear();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    // ============================================================================
    // schema.cppm: PostgreSQL dialect schema generation
    // ============================================================================

    class PgSchemaDetailTest : public PqFakeTest {};

    TEST_F(PgSchemaDetailTest, CreateTableIfNotExistsPgDialect) {
        set_default_conn();

        auto result = SchemaStatement<MockPgPerson>::create_table_if_not_exists(FakeDefault::get());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(PgQuerySet{}.count().execute(), 0); // the table now exists in the test schema
    }

    // Pure DDL text: needs no server, so it is a plain TEST outside the PG fixture.
    TEST(PgSchemaDdlTest, PgDialectSqlHasNativeTypes) {
        const std::string& sql = SchemaStatement<MockPgPerson>::create_table_sql<Dialect::PostgreSQL>();
        EXPECT_NE(sql.find("BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY"), std::string::npos);
        // age is a plain `int`: PG's width-matched integer mapping (#603) maps it to
        // INTEGER — the id PK stays BIGINT (pinned, DB-generated) regardless.
        EXPECT_NE(sql.find("age INTEGER NOT NULL"), std::string::npos);
        EXPECT_EQ(sql.find("AUTOINCREMENT"), std::string::npos);
        // SQLite's single-PK spelling must never leak into PG DDL.
        EXPECT_EQ(sql.find("INTEGER PRIMARY KEY"), std::string::npos);
    }

} // namespace

// NOLINTEND(misc-const-correctness,bugprone-unused-return-value,readability-identifier-length) // NOSONAR(cpp:S125)
// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR(cpp:S125)
