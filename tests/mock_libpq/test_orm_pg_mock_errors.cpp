/**
 * @file test_orm_pg_mock_errors.cpp
 * @brief Tests for Storm ORM PostgreSQL error paths using mock libpq
 *
 * These tests use the mock libpq library to test hard-to-trigger error paths
 * in the Storm ORM PostgreSQL backend that would be difficult or impossible
 * to test with a real PostgreSQL server.
 *
 * Error paths tested:
 * - Connection failures (null PGconn, CONNECTION_BAD)
 * - Prepare failures (null result, PGRES_FATAL_ERROR)
 * - Execute failures (null result, SQL errors)
 * - Statement move assignment
 * - BLOB extraction (null, non-hex, uppercase hex)
 * - Error accessor methods
 * - Cache utilities
 * - Connection accessors
 * - Quote handling in placeholder translation
 */

#include <gtest/gtest.h>

// LINT-EXCLUDE-FILE: file-size, duplicate
// Pre-existing structural debt — large mock-error test file with repeated TEST/setup
// boilerplate across many error scenarios. Tracked under issue #264 Phase 1.
// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR(cpp:S125)
// NOLINTBEGIN(misc-const-correctness) // NOSONAR(cpp:S125)
// NOLINTBEGIN(readability-identifier-length,bugprone-unused-return-value) // bind-result names r1..r6 + intentional (void)prepare_cached. Pre-existing; tracked under #262/#264.

#include "mock_libpq.h"

import storm;
import std;
using storm::primary;
using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;

using PgConnection = storm::db::postgresql::Connection;
using PgStatement  = storm::db::postgresql::Statement;
using PgError      = storm::db::postgresql::Error;
using storm::db::DefaultConnection;
using storm::test::MockPqConfig;
using storm::test::MockPqGuard;

namespace {

    // ============================================================================
    // Connection Error Tests (L520-530)
    // ============================================================================

    class PgConnectionErrorTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgConnectionErrorTest, ConnectdbReturnsNull) {
        MockPqConfig::connectdb_returns_null();
        auto result = PgConnection::open("host=localhost");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Failed to allocate PGconn");
    }

    TEST_F(PgConnectionErrorTest, StatusReturnsBad) {
        MockPqConfig::status_returns(CONNECTION_BAD);
        MockPqConfig::error_message("Connection refused");
        auto result = PgConnection::open("host=localhost");

        ASSERT_FALSE(result.has_value());
        // Issue #351: the status MUST be captured before PQfinish(raw_conn).
        // The mock poisons FakePGconn::status inside PQfinish, so a read of
        // PQstatus(raw_conn) after the free would yield the poison value, not
        // CONNECTION_BAD. Asserting the exact pre-finish status catches the
        // use-after-free that ASAN flags against real libpq.
        EXPECT_EQ(result.error().code(), CONNECTION_BAD);
        EXPECT_EQ(result.error().message(), "Connection refused");
    }

    // ============================================================================
    // Error Accessor Tests (L24-31)
    // ============================================================================

    TEST_F(PgConnectionErrorTest, ErrorCodeAndMessage) {
        MockPqConfig::connectdb_returns_null();
        auto result = PgConnection::open("host=localhost");

        ASSERT_FALSE(result.has_value());
        const PgError& err = result.error();
        EXPECT_EQ(err.code(), -1);
        EXPECT_EQ(err.message(), "Failed to allocate PGconn");
    }

    // ============================================================================
    // Prepare Error Tests (L553-572)
    // ============================================================================

    class PgPrepareErrorTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgPrepareErrorTest, PrepareOnClosedConnection) {
        // Create connection, then move-from it to simulate closed state
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto moved = std::move(*conn_result);
        // conn_result is now in moved-from state (is_open() == false)

        auto result = conn_result->prepare("SELECT 1");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Connection not open");
    }

    // Issue #271 replaced PG Connection's defaulted move ops with explicit ones
    // (std::shared_mutex is not movable). Exercise move assignment: the target
    // takes over the source's open handle and the source becomes closed.
    TEST_F(PgPrepareErrorTest, MoveAssignmentTransfersHandle) {
        auto src = PgConnection::open("host=localhost");
        ASSERT_TRUE(src.has_value());
        auto dst = PgConnection::open("host=localhost");
        ASSERT_TRUE(dst.has_value());

        *dst = std::move(*src);
        EXPECT_TRUE(dst->is_open());
        EXPECT_FALSE(src->is_open()); // NOLINT(bugprone-use-after-move) -- intentional moved-from check
    }

    TEST_F(PgPrepareErrorTest, PrepareReturnsNull) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        MockPqConfig::prepare_returns_null();
        auto result = conn_result->prepare("SELECT 1");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    TEST_F(PgPrepareErrorTest, PrepareReturnsFatalError) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        MockPqConfig::prepare_status(PGRES_FATAL_ERROR);
        MockPqConfig::error_message("syntax error");
        auto result = conn_result->prepare("INVALID SQL");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    // ============================================================================
    // Prepare Cached Error Tests (L580-607)
    // ============================================================================

    TEST_F(PgPrepareErrorTest, PrepareCachedOnClosedConnection) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto moved = std::move(*conn_result);

        auto result = conn_result->prepare_cached("SELECT 1");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Connection not open");
    }

    TEST_F(PgPrepareErrorTest, PrepareCachedReturnsNull) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        MockPqConfig::prepare_returns_null();
        auto result = conn_result->prepare_cached("SELECT 1");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    TEST_F(PgPrepareErrorTest, PrepareCachedReturnsFatalError) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        MockPqConfig::prepare_status(PGRES_FATAL_ERROR);
        MockPqConfig::error_message("relation not found");
        auto result = conn_result->prepare_cached("SELECT * FROM nonexistent");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    // ============================================================================
    // Execute (Direct SQL) Error Tests (L628-650)
    // ============================================================================

    class PgExecuteErrorTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgExecuteErrorTest, ExecuteOnClosedConnection) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto moved = std::move(*conn_result);

        auto result = conn_result->execute("CREATE TABLE test (id INTEGER)");
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "Connection not open");
    }

    TEST_F(PgExecuteErrorTest, ExecuteReturnsNull) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        MockPqConfig::exec_returns_null();
        auto result = conn_result->execute("CREATE TABLE test (id INTEGER)");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    TEST_F(PgExecuteErrorTest, ExecuteReturnsSqlError) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        MockPqConfig::exec_status(PGRES_FATAL_ERROR);
        MockPqConfig::error_message("table already exists");
        auto result = conn_result->execute("CREATE TABLE test (id INTEGER)");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), PGRES_FATAL_ERROR);
    }

    // ============================================================================
    // Statement Execute Error Tests (L199)
    // ============================================================================

    class PgStatementErrorTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgStatementErrorTest, ExecPreparedReturnsFatalError) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // Prepare succeeds, but exec_prepared fails
        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1)");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        MockPqConfig::error_message("constraint violation");

        auto exec_result = stmt_result->execute();
        ASSERT_FALSE(exec_result.has_value());
    }

    // ============================================================================
    // Statement Step Error Tests (L218-240)
    // ============================================================================

    TEST_F(PgStatementErrorTest, StepAutoExecuteError) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT * FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // step() auto-executes if result_ is null
        MockPqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        auto step_result = stmt_result->step();

        ASSERT_FALSE(step_result.has_value());
    }

    TEST_F(PgStatementErrorTest, StepReturnsFalseAtEnd) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT * FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // Execute returns 1 row
        MockPqConfig::exec_prepared_ntuples(1);
        auto step1 = stmt_result->step();
        ASSERT_TRUE(step1.has_value());
        EXPECT_TRUE(step1.value()); // First row available

        auto step2 = stmt_result->step();
        ASSERT_TRUE(step2.has_value());
        EXPECT_FALSE(step2.value()); // No more rows
    }

    TEST_F(PgStatementErrorTest, StepRawAutoExecuteError) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT * FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        int raw_result = stmt_result->step_raw();

        EXPECT_EQ(raw_result, -1); // Error
    }

    // ============================================================================
    // Statement Move Assignment Tests (L77-104)
    // ============================================================================

    TEST_F(PgStatementErrorTest, StatementMoveAssignment) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt1_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt1_result.has_value());

        auto stmt2_result = conn_result->prepare("SELECT 2");
        ASSERT_TRUE(stmt2_result.has_value());

        // Exercise move assignment
        *stmt1_result = std::move(*stmt2_result);

        // stmt1 should now hold stmt2's state
        // Verify it still works
        MockPqConfig::exec_prepared_ntuples(0);
        auto step = stmt1_result->step();
        ASSERT_TRUE(step.has_value());
        EXPECT_FALSE(step.value()); // 0 rows
    }

    // ============================================================================
    // BLOB Extraction Tests (L339-393)
    // ============================================================================

    class PgBlobExtractionTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgBlobExtractionTest, ExtractBlobPtrNull) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT data FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // Configure: 1 row, column 0 is null
        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_null(0, 0);
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());

        const void* blob = stmt_result->extract_blob_ptr(0);
        EXPECT_EQ(blob, nullptr);
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobNonHexFallback) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT data FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // Configure: 1 row, column 0 has raw data (no \x prefix)
        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "rawdata");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());

        const void* blob = stmt_result->extract_blob_ptr(0);
        EXPECT_NE(blob, nullptr);

        // extract_bytes should return the length of the raw data
        int bytes = stmt_result->extract_bytes(0);
        EXPECT_EQ(bytes, 7); // "rawdata" length
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobUppercaseHex) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT data FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // Configure: 1 row, column 0 has uppercase hex BYTEA
        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "\\xDEADBEEF");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());

        const void* blob = stmt_result->extract_blob_ptr(0);
        EXPECT_NE(blob, nullptr);

        // \xDEADBEEF is 4 bytes decoded
        int bytes = stmt_result->extract_bytes(0);
        EXPECT_EQ(bytes, 4);

        // Verify decoded bytes
        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0xDE);
        EXPECT_EQ(data[1], 0xAD);
        EXPECT_EQ(data[2], 0xBE);
        EXPECT_EQ(data[3], 0xEF);
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobWithNumericHexDigits) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT data FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // Hex string with numeric digits (0-9) to cover hex_digit() numeric branch
        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "\\x0123456789");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());

        const void* blob = stmt_result->extract_blob_ptr(0);
        EXPECT_NE(blob, nullptr);

        int bytes = stmt_result->extract_bytes(0);
        EXPECT_EQ(bytes, 5); // 10 hex chars = 5 bytes

        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0x01);
        EXPECT_EQ(data[1], 0x23);
        EXPECT_EQ(data[2], 0x45);
        EXPECT_EQ(data[3], 0x67);
        EXPECT_EQ(data[4], 0x89);
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobInvalidHexDigitFallback) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT data FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        // Hex string with invalid hex characters to cover hex_digit() return 0 fallback
        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "\\xGG");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());

        const void* blob = stmt_result->extract_blob_ptr(0);
        EXPECT_NE(blob, nullptr);

        int bytes = stmt_result->extract_bytes(0);
        EXPECT_EQ(bytes, 1); // 2 hex chars = 1 byte (both decode to 0)

        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0x00); // Invalid chars fall back to 0
    }

    TEST_F(PgBlobExtractionTest, ExtractBlobLowercaseHex) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT data FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "\\xdeadbeef");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());

        const void* blob = stmt_result->extract_blob_ptr(0);
        EXPECT_NE(blob, nullptr);

        int bytes = stmt_result->extract_bytes(0);
        EXPECT_EQ(bytes, 4);

        const auto* data = static_cast<const unsigned char*>(blob);
        EXPECT_EQ(data[0], 0xDE);
        EXPECT_EQ(data[1], 0xAD);
        EXPECT_EQ(data[2], 0xBE);
        EXPECT_EQ(data[3], 0xEF);
    }

    // ============================================================================
    // Statement get_error_message Test (L375-380)
    // ============================================================================

    TEST_F(PgStatementErrorTest, GetErrorMessage) {
        MockPqConfig::error_message("test error from PQ");
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        const char* msg = stmt_result->get_error_message();
        EXPECT_STREQ(msg, "test error from PQ");
    }

    // ============================================================================
    // Cache Utility Tests (L616-624)
    // ============================================================================

    class PgCacheUtilityTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgCacheUtilityTest, CachedStatementCount) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        EXPECT_EQ(conn_result->cached_statement_count(), 0U);

        auto stmt1 = conn_result->prepare_cached("SELECT 1");
        ASSERT_TRUE(stmt1.has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 1U);

        auto stmt2 = conn_result->prepare_cached("SELECT 2");
        ASSERT_TRUE(stmt2.has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 2U);

        // Same SQL reuses cached statement
        auto stmt3 = conn_result->prepare_cached("SELECT 1");
        ASSERT_TRUE(stmt3.has_value());
        EXPECT_EQ(conn_result->cached_statement_count(), 2U);
    }

    // Issue #273: cache_stats() snapshot on the real PostgreSQL Connection —
    // hits/misses/size track the same way as SQLite (shared cache_* helpers).
    TEST_F(PgCacheUtilityTest, CacheStatsSnapshot) {
        auto conn_result = PgConnection::open("host=localhost");
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
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        (void)conn_result->prepare_cached("SELECT 1");
        (void)conn_result->prepare_cached("SELECT 2");
        EXPECT_EQ(conn_result->cached_statement_count(), 2U);

        conn_result->clear_statement_cache();
        EXPECT_EQ(conn_result->cached_statement_count(), 0U);
    }

    // Per-table clear (issue #215): drop entries that reference the given
    // table, leave the rest. Word-boundary aware so clearing "persons" does
    // not touch "person_addresses".
    TEST_F(PgCacheUtilityTest, ClearStatementCachePerTable) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        (void)conn_result->prepare_cached("SELECT id FROM persons");
        (void)conn_result->prepare_cached("SELECT id FROM messages");
        (void)conn_result->prepare_cached("SELECT id FROM person_addresses");
        EXPECT_EQ(conn_result->cached_statement_count(), 3U);

        conn_result->clear_statement_cache(std::string_view{"persons"});
        EXPECT_EQ(conn_result->cached_statement_count(), 2U)
                << "only the persons entry must be dropped — messages and person_addresses survive";
    }

    // ============================================================================
    // Connection Accessor Tests (L656-668)
    // ============================================================================

    class PgConnectionAccessorTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgConnectionAccessorTest, GetReturnsRawPointer) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        PGconn* raw = conn_result->get();
        EXPECT_NE(raw, nullptr);
    }

    // ============================================================================
    // Quote Handling in Placeholder Translation Tests (L688-693)
    // ============================================================================

    class PgQuoteHandlingTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgQuoteHandlingTest, SingleQuotedQuestionMarkNotTranslated) {
        // SQL with '?' inside single quotes should NOT translate the ?
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // This calls translate_placeholders internally
        // The ? inside 'literal?' should not be translated
        auto result = conn_result->prepare("SELECT * FROM t WHERE col = 'literal?'");
        ASSERT_TRUE(result.has_value());
    }

    TEST_F(PgQuoteHandlingTest, DoubleQuotedQuestionMarkNotTranslated) {
        // SQL with ? inside double quotes should NOT translate the ?
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto result = conn_result->prepare("SELECT \"col?\" FROM t WHERE id = ?");
        ASSERT_TRUE(result.has_value());
    }

    TEST_F(PgQuoteHandlingTest, MixedQuotesAndPlaceholders) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // Mix of quoted ? and real ? placeholders
        auto result = conn_result->prepare("SELECT * FROM t WHERE name = 'test?' AND age = ? AND \"col?\" = ?");
        ASSERT_TRUE(result.has_value());
    }

    // ------------------------------------------------------------------------
    // #418: translate_placeholders must NOT rewrite `?` inside dollar-quoted
    // bodies, E'...' escape strings, or SQL comments. Assert the exact
    // translated SQL captured by the mock PQprepare.
    // ------------------------------------------------------------------------

    TEST_F(PgQuoteHandlingTest, RealPlaceholdersStillTranslated) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto result = conn_result->prepare("SELECT * FROM t WHERE a = ? AND b = ?");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(MockPqConfig::get_last_prepare_query(), "SELECT * FROM t WHERE a = $1 AND b = $2");
    }

    TEST_F(PgQuoteHandlingTest, AnonymousDollarQuoteNotTranslated) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // ? inside $$ ... $$ must survive; the real ? after it becomes $1.
        auto result = conn_result->prepare("SELECT $$ a ? b $$, ?");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(MockPqConfig::get_last_prepare_query(), "SELECT $$ a ? b $$, $1");
    }

    TEST_F(PgQuoteHandlingTest, TaggedDollarQuoteNotTranslated) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // A nested $$ inside a $tag$ body must NOT close the tag early.
        auto result = conn_result->prepare("SELECT $tag$ a ? $$ ? b $tag$, ?");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(MockPqConfig::get_last_prepare_query(), "SELECT $tag$ a ? $$ ? b $tag$, $1");
    }

    TEST_F(PgQuoteHandlingTest, EscapeStringQuestionMarkNotTranslated) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // E'it\'s ?' — the backslash-escaped quote must not close the string,
        // so the ? stays literal; the trailing ? becomes $1.
        auto result = conn_result->prepare("SELECT E'it\\'s ?', ?");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(MockPqConfig::get_last_prepare_query(), "SELECT E'it\\'s ?', $1");
    }

    TEST_F(PgQuoteHandlingTest, LineCommentQuestionMarkNotTranslated) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto result = conn_result->prepare("SELECT ? -- pick ? one\n, ?");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(MockPqConfig::get_last_prepare_query(), "SELECT $1 -- pick ? one\n, $2");
    }

    TEST_F(PgQuoteHandlingTest, BlockCommentQuestionMarkNotTranslated) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // Nested block comment: the inner */ must not end the outer comment.
        auto result = conn_result->prepare("SELECT ? /* a ? /* b ? */ c ? */, ?");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(MockPqConfig::get_last_prepare_query(), "SELECT $1 /* a ? /* b ? */ c ? */, $2");
    }

    // ============================================================================
    // Connection open success test (covers normal path)
    // ============================================================================

    TEST_F(PgConnectionAccessorTest, PrepareCommonStatements) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // Call prepare_common_statements() directly - it's a no-op for PostgreSQL
        conn_result->prepare_common_statements();

        // Verify connection still works after calling it
        EXPECT_TRUE(conn_result->is_open());
    }

    TEST_F(PgConnectionAccessorTest, OpenSucceeds) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());
        EXPECT_TRUE(conn_result->is_open());
    }

    // ============================================================================
    // Statement prepare and execute success (covers normal paths)
    // ============================================================================

    TEST_F(PgStatementErrorTest, PrepareAndExecuteSuccess) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1)");
        ASSERT_TRUE(stmt_result.has_value());

        auto exec_result = stmt_result->execute();
        ASSERT_TRUE(exec_result.has_value());
    }

    TEST_F(PgStatementErrorTest, PrepareAndStepRawSuccess) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT * FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        int raw = stmt_result->step_raw();
        EXPECT_EQ(raw, PgStatement::ROW_AVAILABLE);

        raw = stmt_result->step_raw();
        EXPECT_EQ(raw, PgStatement::NO_MORE_ROWS);
    }

    // ============================================================================
    // Statement bind methods tests (verify they compile and work with mock)
    // ============================================================================

    TEST_F(PgStatementErrorTest, BindMethodsSucceed) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1, $2, $3, $4, $5, $6)");
        ASSERT_TRUE(stmt_result.has_value());

        auto r1 = stmt_result->bind_int(1, 42);
        EXPECT_TRUE(r1.has_value());

        auto r2 = stmt_result->bind_text(2, "hello");
        EXPECT_TRUE(r2.has_value());

        auto r3 = stmt_result->bind_int64(3, 123456789LL);
        EXPECT_TRUE(r3.has_value());

        auto r4 = stmt_result->bind_double(4, std::numbers::pi);
        EXPECT_TRUE(r4.has_value());

        auto r5 = stmt_result->bind_null(5);
        EXPECT_TRUE(r5.has_value());

        std::vector<std::uint8_t> blob_data = {0xDE, 0xAD};
        auto                      r6        = stmt_result->bind_blob(6, blob_data.data(), blob_data.size());
        EXPECT_TRUE(r6.has_value());
    }

    // ============================================================================
    // Statement reset and handle tests
    // ============================================================================

    TEST_F(PgStatementErrorTest, ResetStatement) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        // Execute and step
        MockPqConfig::exec_prepared_ntuples(0);
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        // Reset should clear state
        stmt_result->reset();

        // After reset, handle should be null
        EXPECT_EQ(stmt_result->handle(), nullptr);
    }

    TEST_F(PgStatementErrorTest, HandleReturnsNullBeforeExecute) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        EXPECT_EQ(stmt_result->handle(), nullptr);
    }

    // ============================================================================
    // expanded_sql() edge case tests (postgresql.cppm lines 279-288, 303-304)
    // ============================================================================

    TEST_F(PgStatementErrorTest, ExpandedSqlWithSingleQuote) {
        // SQL with single-quoted literal exercises the ' branch (lines 279-280)
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT 'literal' FROM t WHERE id = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        (void)stmt->bind_int(1, 42);

        std::string sql = stmt->expanded_sql();
        EXPECT_FALSE(sql.empty());
        EXPECT_NE(sql.find("'literal'"), std::string::npos);
    }

    TEST_F(PgStatementErrorTest, ExpandedSqlWithDoubleQuote) {
        // SQL with double-quoted identifier exercises the " branch (lines 282-283)
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT \"col\" FROM t WHERE id = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        (void)stmt->bind_int(1, 42);

        std::string sql = stmt->expanded_sql();
        EXPECT_FALSE(sql.empty());
        EXPECT_NE(sql.find("\"col\""), std::string::npos);
    }

    TEST_F(PgStatementErrorTest, ExpandedSqlWithNullParam) {
        // NULL-bound parameter exercises the null branch (line 288)
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT ? FROM t");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        (void)stmt->bind_null(1);

        std::string sql = stmt->expanded_sql();
        EXPECT_NE(sql.find("NULL"), std::string::npos);
    }

    TEST_F(PgStatementErrorTest, ExpandedSqlWithExtraPlaceholder) {
        // More ? in SQL than bound params exercises the unmatched-? branch (lines 303-304)
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        // SQL has 2 ? but we only bind 1
        auto stmt_result = conn_result->prepare_cached("SELECT ? FROM t WHERE id = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        (void)stmt->bind_int(1, 42); // Only bind first parameter

        std::string sql = stmt->expanded_sql();
        // Second ? should remain as-is
        EXPECT_NE(sql.find('?'), std::string::npos);
    }

    // ============================================================================
    // Column extraction method tests
    // ============================================================================

    class PgColumnExtractionTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    TEST_F(PgColumnExtractionTest, ExtractIntValue) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT age FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "42");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());

        EXPECT_EQ(stmt_result->extract_int(0), 42);
    }

    TEST_F(PgColumnExtractionTest, ExtractInt64Value) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT bigid FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "9876543210");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        EXPECT_EQ(stmt_result->extract_int64(0), 9876543210LL);
    }

    TEST_F(PgColumnExtractionTest, ExtractDoubleValue) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT price FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "3.14159");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        EXPECT_NEAR(stmt_result->extract_double(0), std::numbers::pi, 0.001);
    }

    TEST_F(PgColumnExtractionTest, ExtractTextPtr) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT name FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "hello");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        const unsigned char* text = stmt_result->extract_text_ptr(0);
        EXPECT_NE(text, nullptr);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(text)), "hello");
    }

    TEST_F(PgColumnExtractionTest, ExtractTextView) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT name FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "world");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        auto view = stmt_result->extract_text_view(0);
        EXPECT_EQ(view, "world");
    }

    TEST_F(PgColumnExtractionTest, ExtractTextViewNull) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT name FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_null(0, 0);
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        auto view = stmt_result->extract_text_view(0);
        EXPECT_TRUE(view.empty());
    }

    TEST_F(PgColumnExtractionTest, ExtractBoolTrue) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT active FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "t");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        EXPECT_TRUE(stmt_result->extract_bool(0));
    }

    TEST_F(PgColumnExtractionTest, ExtractBoolFalse) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT active FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "f");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        EXPECT_FALSE(stmt_result->extract_bool(0));
    }

    TEST_F(PgColumnExtractionTest, ExtractFloatValue) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT score FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "2.5");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        EXPECT_NEAR(stmt_result->extract_float(0), 2.5F, 0.01F);
    }

    TEST_F(PgColumnExtractionTest, ExtractBytes) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT name FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "hello");
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        EXPECT_EQ(stmt_result->extract_bytes(0), 5);
    }

    TEST_F(PgColumnExtractionTest, IsNull) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT name FROM test");
        ASSERT_TRUE(stmt_result.has_value());

        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_null(0, 0);
        auto step = stmt_result->step();
        ASSERT_TRUE(step.has_value());

        EXPECT_TRUE(stmt_result->is_null(0));
    }

    // ============================================================================
    // Statement finalize test
    // ============================================================================

    TEST_F(PgStatementErrorTest, FinalizeStatement) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("SELECT 1");
        ASSERT_TRUE(stmt_result.has_value());

        // finalize() calls PQexec("DEALLOCATE ...") and clears the statement
        stmt_result->finalize();

        // After finalize, handle should be null
        EXPECT_EQ(stmt_result->handle(), nullptr);
    }

    // ============================================================================
    // Statement destructor DEALLOCATE (issue #417)
    //
    // On cache eviction the unique_ptr<Statement> is destroyed. SQLite finalizes
    // via the unique_ptr deleter; PG must symmetrically DEALLOCATE the server-side
    // prepared statement, or it leaks on the backend for the connection lifetime.
    // ============================================================================

    TEST_F(PgStatementErrorTest, DestructorDeallocatesPreparedStatement) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        {
            auto stmt_result = conn_result->prepare("SELECT 1");
            ASSERT_TRUE(stmt_result.has_value());
            // No explicit finalize() — rely on the destructor when stmt_result
            // (the std::expected holding the Statement) leaves this scope.
        }

        // The destructor must have issued exactly one DEALLOCATE for the
        // server-side prepared statement.
        EXPECT_EQ(MockPqConfig::get_deallocate_call_count(), 1);
        EXPECT_TRUE(MockPqConfig::get_last_exec_query().starts_with("DEALLOCATE"))
                << "last PQexec was: " << MockPqConfig::get_last_exec_query();
    }

    TEST_F(PgStatementErrorTest, MovedFromStatementDoesNotDeallocate) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        {
            auto stmt_result = conn_result->prepare("SELECT 1");
            ASSERT_TRUE(stmt_result.has_value());

            // Move the statement out; the moved-from source must NOT deallocate
            // when it is destroyed (its stmt_name_ was swapped away).
            PgStatement moved = std::move(*stmt_result);
            // `moved` deallocates once here; `*stmt_result` (moved-from) must not.
        }

        // Exactly one DEALLOCATE total — no double-free / error spam from the
        // moved-from husk.
        EXPECT_EQ(MockPqConfig::get_deallocate_call_count(), 1);
    }

    // ============================================================================
    // Execute success test
    // ============================================================================

    TEST_F(PgExecuteErrorTest, ExecuteSuccess) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto result = conn_result->execute("CREATE TABLE test (id INTEGER)");
        ASSERT_TRUE(result.has_value());
    }

    // ============================================================================
    // Bind blob with null data
    // ============================================================================

    // Test bind + execute to cover rebuild_param_ptrs() loop body
    TEST_F(PgStatementErrorTest, BindAndExecuteCoversRebuildParamPtrs) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1, $2)");
        ASSERT_TRUE(stmt_result.has_value());

        auto r1 = stmt_result->bind_int(1, 42);
        EXPECT_TRUE(r1.has_value());

        auto r2 = stmt_result->bind_text(2, "hello");
        EXPECT_TRUE(r2.has_value());

        // Execute with bound params - this calls rebuild_param_ptrs()
        auto exec_result = stmt_result->execute();
        ASSERT_TRUE(exec_result.has_value());
    }

    // Test bind with NULL param + execute to cover rebuild_param_ptrs() nullptr branch
    TEST_F(PgStatementErrorTest, BindNullAndExecuteCoversRebuildPtrsBranch) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1, $2)");
        ASSERT_TRUE(stmt_result.has_value());

        auto r1 = stmt_result->bind_int(1, 42);
        EXPECT_TRUE(r1.has_value());

        auto r2 = stmt_result->bind_null(2);
        EXPECT_TRUE(r2.has_value());

        // Execute - rebuild_param_ptrs will see non-null for param 1 and null for param 2
        auto exec_result = stmt_result->execute();
        ASSERT_TRUE(exec_result.has_value());
    }

    TEST_F(PgStatementErrorTest, BindBlobNull) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare("INSERT INTO test VALUES ($1)");
        ASSERT_TRUE(stmt_result.has_value());

        auto r = stmt_result->bind_blob(1, nullptr, 0);
        EXPECT_TRUE(r.has_value());
    }

    // ============================================================================
    // ORM-level PostgreSQL tests (QuerySet with PgConnection)
    // These tests exercise the INSERT RETURNING path in insert.cppm
    // and the LIMIT ALL path in base.cppm
    // ============================================================================

    struct MockPgPerson {
        [[= primary]] std::int64_t id{};
        std::string                name;
        int                        age{};
    };

    using PgQuerySet = storm::QuerySet<MockPgPerson, PgConnection>;

    class PgOrmInsertReturningTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
            // Set up PG default connection via mock
            (void)DefaultConnection<PgConnection>::set("host=localhost");
        }
    };

    // Covers insert.cppm lines 251-285: INSERT RETURNING happy path
    TEST_F(PgOrmInsertReturningTest, InsertReturningSuccess) {
        // Configure mock: exec_prepared returns 1 row with the generated ID
        MockPqConfig::exec_prepared_ntuples(1);
        MockPqConfig::set_column_value(0, 0, "42");

        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 42);
    }

    // Covers insert.cppm LCOV_EXCL lines: INSERT RETURNING prepare error
    TEST_F(PgOrmInsertReturningTest, InsertReturningPrepareError) {
        MockPqConfig::prepare_returns_null();

        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Bob", .age = 25};

        auto result = qs.insert(person).execute();
        ASSERT_FALSE(result.has_value());
    }

    // Covers insert.cppm LCOV_EXCL lines: INSERT RETURNING step error
    TEST_F(PgOrmInsertReturningTest, InsertReturningStepError) {
        MockPqConfig::exec_prepared_status(PGRES_FATAL_ERROR);
        MockPqConfig::error_message("constraint violation");

        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Charlie", .age = 35};

        auto result = qs.insert(person).execute();
        ASSERT_FALSE(result.has_value());
    }

    // Covers insert.cppm lines 256-257: to_sql() prepare failure in PG branch
    TEST_F(PgOrmInsertReturningTest, InsertToSqlPrepareError) {
        MockPqConfig::prepare_returns_null();

        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).to_sql();
        ASSERT_FALSE(result.has_value());
    }

    // Covers base.cppm line 583: LIMIT ALL path (offset without limit)
    TEST_F(PgOrmInsertReturningTest, SelectWithOffsetOnlyUsesLimitAll) {
        // Configure mock: exec_prepared returns 0 rows for the SELECT
        MockPqConfig::exec_prepared_ntuples(0);

        PgQuerySet qs;
        auto       results = qs.offset(5).select().execute();
        ASSERT_TRUE(results.has_value());
        EXPECT_TRUE(results.value().empty());
    }

    // Covers QuerySet::reset(): clearing query state must leave the QuerySet
    // usable for a fresh select.
    TEST_F(PgOrmInsertReturningTest, ResetInvalidatesSelectCache) {
        MockPqConfig::exec_prepared_ntuples(0);

        PgQuerySet qs;
        // Execute a select to populate select_stmt_ cache
        auto results = qs.select().execute();
        ASSERT_TRUE(results.has_value());

        // Reset should invalidate the cached select statement
        qs.reset();

        // Verify the queryset works correctly after reset
        auto results2 = qs.select().execute();
        ASSERT_TRUE(results2.has_value());
    }

    // ============================================================================
    // Concept-based mock connection with failing binds
    // Tests INSERT RETURNING bind error path (insert.cppm line 264-267)
    // ============================================================================

    // Minimal mock connection where bind_text fails — triggers the bind error
    // path in INSERT RETURNING that real PG binds can never hit.
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

    // Verify concepts are satisfied
    static_assert(storm::db::DatabaseConnection<BindFailConnection>);
    static_assert(storm::db::CachedDatabaseConnection<BindFailConnection>);
    static_assert(storm::db::DatabaseStatement<BindFailStatement>);
    static_assert(storm::db::DatabaseError<BindFailError>);

    using BindFailQuerySet = storm::QuerySet<MockPgPerson, BindFailConnection>;

    TEST(PgInsertReturningBindErrorTest, BindFailureReturnsError) {
        // Set up connection with failing binds
        (void)DefaultConnection<BindFailConnection>::set("mock");

        // Get the connection and configure it to fail binds
        const auto& conn = DefaultConnection<BindFailConnection>::get();
        conn->set_fail_binds(true);

        BindFailQuerySet   qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
        EXPECT_EQ(result.error().message(), "mock bind failure");
    }

    // Covers insert.cppm lines 263-264: to_sql() bind failure in PG branch
    TEST(PgInsertReturningBindErrorTest, InsertToSqlBindFailureReturnsError) {
        (void)DefaultConnection<BindFailConnection>::set("mock");

        const auto& conn = DefaultConnection<BindFailConnection>::get();
        conn->set_fail_binds(true);

        BindFailQuerySet   qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).to_sql();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);
    }

    // ============================================================================
    // insert.cppm line 267: to_sql() success path in PG branch (INSERT RETURNING)
    // ============================================================================

    // Covers insert.cppm line 267: return stmt->expanded_sql() in PG success path
    TEST_F(PgOrmInsertReturningTest, InsertToSqlPgPathSuccess) {
        PgQuerySet         qs;
        MockPgPerson const person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).to_sql();
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result.value().empty());
    }

    // ============================================================================
    // postgresql.cppm line 294: single-quote escaping in expanded_sql()
    // ============================================================================

    // Covers postgresql.cppm line 294: result += "''" when bound param value contains '
    TEST_F(PgStatementErrorTest, ExpandedSqlBoundParamWithSingleQuote) {
        auto conn_result = PgConnection::open("host=localhost");
        ASSERT_TRUE(conn_result.has_value());

        auto stmt_result = conn_result->prepare_cached("SELECT * FROM t WHERE name = ?");
        ASSERT_TRUE(stmt_result.has_value());

        auto* stmt = *stmt_result;
        // Bind a value containing a single quote — triggers the '' escaping branch
        (void)stmt->bind_text(1, "O'Brien");

        std::string sql = stmt->expanded_sql();
        // The single quote must be escaped as '' in the output
        EXPECT_NE(sql.find("O''Brien"), std::string::npos);
    }

    // ============================================================================
    // schema.cppm: PostgreSQL dialect schema generation
    // ============================================================================

    class PgSchemaDetailTest : public ::testing::Test {
      public:
        [[no_unique_address]] MockPqGuard guard_;

        auto SetUp() -> void override {
            MockPqConfig::reset();
        }
    };

    // Exercises the PostgreSQL dialect path of create_table_if_not_exists
    TEST_F(PgSchemaDetailTest, CreateTableIfNotExistsPgDialect) {
        (void)DefaultConnection<PgConnection>::set("host=localhost");

        const auto& conn   = DefaultConnection<PgConnection>::get();
        auto        result = SchemaStatement<MockPgPerson>::create_table_if_not_exists(conn);
        ASSERT_TRUE(result.has_value());
    }

    TEST_F(PgSchemaDetailTest, PgDialectSqlHasNativeTypes) {
        using storm::orm::schema::Dialect;
        using storm::orm::schema::SchemaStatement;
        const std::string& sql = SchemaStatement<MockPgPerson>::create_table_sql<Dialect::PostgreSQL>();
        EXPECT_NE(sql.find("BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY"), std::string::npos);
        // age is a plain `int` (4-byte signed): PG's width-matched integer mapping
        // (#603) maps it to INTEGER, not BIGINT — the id PK stays BIGINT (pinned,
        // DB-generated) regardless.
        EXPECT_NE(sql.find("age INTEGER NOT NULL"), std::string::npos);
        EXPECT_EQ(sql.find("AUTOINCREMENT"), std::string::npos);
        // SQLite's single-PK spelling must never leak into PG DDL — still a valid
        // guard even though PG legitimately contains "INTEGER" now (#603).
        EXPECT_EQ(sql.find("INTEGER PRIMARY KEY"), std::string::npos);
    }

} // namespace

// NOLINTEND(readability-identifier-length,bugprone-unused-return-value) // bind-result names r1..r6 + intentional (void)prepare_cached. Pre-existing; tracked under #262/#264.
// NOLINTEND(misc-const-correctness) // NOSONAR(cpp:S125)
// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR(cpp:S125)
