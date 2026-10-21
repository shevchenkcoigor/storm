#include <gtest/gtest.h>
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <numbers>
#include "test_db_helpers.h"

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR
// NOLINTBEGIN(misc-const-correctness,misc-unused-alias-decls,modernize-use-std-numbers) // NOSONAR
// NOLINTBEGIN(readability-uppercase-literal-suffix) // NOSONAR
// NOLINTBEGIN(readability-identifier-length,cppcoreguidelines-init-variables) // NOSONAR
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result) // NOSONAR

import storm_db_sqlite;
import storm_db_default_connection;
import storm_orm_statements_insert;

namespace fs = std::filesystem;
using storm::db::DefaultConnection;
using storm::db::sqlite::Connection;
using storm::db::sqlite::Statement;

// ============================================================================
// Connection Error Tests
// ============================================================================

class ConnectionErrorTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        // Clean up any leftover test files
        cleanup_test_files();
    }

    auto TearDown() -> void override {
        cleanup_test_files();
    }

    static auto cleanup_test_files() -> void {
        const auto                     tmp_dir = fs::temp_directory_path(); // NOSONAR(cpp:S5443) - test cleanup
        const std::vector<std::string> test_files =
                {(tmp_dir / "storm_test_readonly.db").string(),
                 (tmp_dir / "storm_test_locked.db").string(),
                 (tmp_dir / "storm_test_locked.db-journal").string(),
                 (tmp_dir / "storm_test_locked.db-wal").string()};
        for (const auto& file : test_files) {
            std::error_code ec;
            fs::remove(file, ec);
        }
    }
};

TEST_F(ConnectionErrorTest, OpenInvalidPath) {
    // Attempt to open database in non-existent directory
    auto result = Connection::open("/nonexistent/directory/that/does/not/exist/test.db");

    ASSERT_FALSE(result.has_value()) << "Should fail to open database in non-existent directory";
    EXPECT_EQ(result.error().code(), SQLITE_CANTOPEN) << "Error code should be SQLITE_CANTOPEN";
    EXPECT_FALSE(result.error().message().empty()) << "Error message should not be empty";
}

TEST_F(ConnectionErrorTest, OpenEmptyPath) {
    // Opening empty path should fail
    auto result = Connection::open("");

    // SQLite may handle empty string differently - it might create in-memory or fail
    // The key is to verify the connection is either properly created or properly reports error
    if (!result.has_value()) {
        EXPECT_NE(result.error().code(), SQLITE_OK) << "Error code should indicate failure";
    }
}

TEST_F(ConnectionErrorTest, ConnectionNotOpenPrepare) {
    // Create a connection and move from it to simulate "closed" state
    auto result = Connection::open(":memory:");
    ASSERT_TRUE(result.has_value());

    auto conn       = std::move(result.value());
    auto moved_conn = std::move(conn);

    // Original conn is now in moved-from state. Intentional moved-from reads:
    // these tests assert the API rejects a moved-from (closed) connection.
    const bool moved_open = conn.is_open(); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_FALSE(moved_open) << "Moved-from connection should not be open";

    // prepare() on closed connection should return error
    auto prep_result = conn.prepare("SELECT 1"); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    ASSERT_FALSE(prep_result.has_value()) << "prepare() on closed connection should fail";
    EXPECT_EQ(prep_result.error().code(), SQLITE_MISUSE);
}

TEST_F(ConnectionErrorTest, ConnectionNotOpenPrepareCached) {
    auto result = Connection::open(":memory:");
    ASSERT_TRUE(result.has_value());

    auto conn       = std::move(result.value());
    auto moved_conn = std::move(conn);

    // prepare_cached() on closed connection should return error (intentional moved-from read)
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    auto prep_result = conn.prepare_cached("SELECT 1");
    ASSERT_FALSE(prep_result.has_value()) << "prepare_cached() on closed connection should fail";
    EXPECT_EQ(prep_result.error().code(), SQLITE_MISUSE);
}

TEST_F(ConnectionErrorTest, ConnectionNotOpenExecute) {
    auto result = Connection::open(":memory:");
    ASSERT_TRUE(result.has_value());

    auto conn       = std::move(result.value());
    auto moved_conn = std::move(conn);

    // execute() on closed connection should return error
    auto exec_result = conn.execute("SELECT 1"); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    ASSERT_FALSE(exec_result.has_value()) << "execute() on closed connection should fail";
    EXPECT_EQ(exec_result.error().code(), SQLITE_MISUSE);
}

TEST_F(ConnectionErrorTest, PrepareInvalidSQL) {
    auto result = Connection::open(":memory:");
    ASSERT_TRUE(result.has_value());

    auto conn = std::move(result.value());

    // Prepare statement with invalid SQL syntax
    auto stmt_result = conn.prepare("THIS IS NOT VALID SQL !!!");

    ASSERT_FALSE(stmt_result.has_value()) << "Should fail to prepare invalid SQL";
    EXPECT_EQ(stmt_result.error().code(), SQLITE_ERROR) << "Error code should be SQLITE_ERROR";
}

TEST_F(ConnectionErrorTest, PrepareCachedInvalidSQL) {
    auto result = Connection::open(":memory:");
    ASSERT_TRUE(result.has_value());

    auto conn = std::move(result.value());

    // Prepare cached statement with invalid SQL syntax
    auto stmt_result = conn.prepare_cached("SELECT * FROM nonexistent_table_xyz");

    ASSERT_FALSE(stmt_result.has_value()) << "Should fail to prepare invalid SQL";
    // SQLite returns SQLITE_ERROR for non-existent table
    EXPECT_EQ(stmt_result.error().code(), SQLITE_ERROR);
}

TEST_F(ConnectionErrorTest, ExecuteInvalidSQL) {
    auto result = Connection::open(":memory:");
    ASSERT_TRUE(result.has_value());

    auto conn = std::move(result.value());

    // Execute invalid SQL
    auto exec_result = conn.execute("DROP TABLE nonexistent_table_xyz");

    ASSERT_FALSE(exec_result.has_value()) << "Should fail to execute invalid SQL";
    EXPECT_EQ(exec_result.error().code(), SQLITE_ERROR);
}

TEST_F(ConnectionErrorTest, ExecuteSyntaxError) {
    auto result = Connection::open(":memory:");
    ASSERT_TRUE(result.has_value());

    auto conn = std::move(result.value());

    // Execute SQL with syntax error
    auto exec_result = conn.execute("SELEKT * FORM table");

    ASSERT_FALSE(exec_result.has_value()) << "Should fail on SQL syntax error";
    EXPECT_EQ(exec_result.error().code(), SQLITE_ERROR);
}

// ============================================================================
// Statement Bind Error Tests
// ============================================================================

class StatementBindErrorTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};

    auto SetUp() -> void override {
        // Create test table
        auto result = conn_.execute(
                "CREATE TABLE test_table ("
                "id INTEGER PRIMARY KEY, "
                "name TEXT, "
                "value REAL, "
                "data BLOB"
                ")"
        );
        ASSERT_TRUE(result.has_value());
    }
};

TEST_F(StatementBindErrorTest, BindIntOutOfRange) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, name) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Bind to out-of-range index (valid indices are 1 and 2)
    auto bind_result = stmt.bind_int(999, 42);

    ASSERT_FALSE(bind_result.has_value()) << "Binding to out-of-range index should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

TEST_F(StatementBindErrorTest, BindTextOutOfRange) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, name) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Bind text to out-of-range index
    auto bind_result = stmt.bind_text(100, "test");

    ASSERT_FALSE(bind_result.has_value()) << "Binding text to out-of-range index should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

TEST_F(StatementBindErrorTest, BindInt64OutOfRange) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, name) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Bind int64 to out-of-range index
    auto bind_result = stmt.bind_int64(50, 9223372036854775807LL);

    ASSERT_FALSE(bind_result.has_value()) << "Binding int64 to out-of-range index should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

TEST_F(StatementBindErrorTest, BindDoubleOutOfRange) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, name, value) VALUES (?, ?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Bind double to out-of-range index
    auto bind_result = stmt.bind_double(999, std::numbers::pi);

    ASSERT_FALSE(bind_result.has_value()) << "Binding double to out-of-range index should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

TEST_F(StatementBindErrorTest, BindNullOutOfRange) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, name) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Bind null to out-of-range index
    auto bind_result = stmt.bind_null(999);

    ASSERT_FALSE(bind_result.has_value()) << "Binding null to out-of-range index should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

TEST_F(StatementBindErrorTest, BindBlobOutOfRange) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, data) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Bind blob to out-of-range index
    const std::vector<std::uint8_t> blob_data   = {0x01, 0x02, 0x03, 0x04};
    auto                            bind_result = stmt.bind_blob(999, blob_data.data(), blob_data.size());

    ASSERT_FALSE(bind_result.has_value()) << "Binding blob to out-of-range index should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

TEST_F(StatementBindErrorTest, BindZeroIndex) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, name) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Index 0 is out of range (SQLite uses 1-based indexing)
    auto bind_result = stmt.bind_int(0, 42);

    ASSERT_FALSE(bind_result.has_value()) << "Binding to index 0 should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

TEST_F(StatementBindErrorTest, BindNegativeIndex) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, name) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Negative index is out of range
    auto bind_result = stmt.bind_int(-1, 42);

    ASSERT_FALSE(bind_result.has_value()) << "Binding to negative index should fail";
    EXPECT_EQ(bind_result.error().code(), SQLITE_RANGE);
}

// ============================================================================
// Statement Execute/Step Error Tests
// ============================================================================

class StatementExecuteErrorTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};

    auto SetUp() -> void override {
        // Create test table with unique constraint
        auto result = conn_.execute(
                "CREATE TABLE test_table ("
                "id INTEGER PRIMARY KEY, "
                "email TEXT UNIQUE NOT NULL"
                ")"
        );
        ASSERT_TRUE(result.has_value());

        // Insert initial data
        result = conn_.execute("INSERT INTO test_table (id, email) VALUES (1, 'test@example.com')");
        ASSERT_TRUE(result.has_value());
    }
};

TEST_F(StatementExecuteErrorTest, ExecuteUniqueConstraintViolation) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, email) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Try to insert duplicate email (unique constraint violation)
    auto bind1 = stmt.bind_int(1, 2);
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_text(2, "test@example.com");
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();

    ASSERT_FALSE(exec_result.has_value()) << "Unique constraint violation should fail";
    // SQLITE_CONSTRAINT or SQLITE_CONSTRAINT_UNIQUE
    EXPECT_TRUE(
            exec_result.error().code() == SQLITE_CONSTRAINT ||
            exec_result.error().code() == (SQLITE_CONSTRAINT | (6 << 8))
    );
}

TEST_F(StatementExecuteErrorTest, ExecutePrimaryKeyViolation) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, email) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Try to insert duplicate primary key
    auto bind1 = stmt.bind_int(1, 1); // id=1 already exists
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_text(2, "other@example.com");
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();

    ASSERT_FALSE(exec_result.has_value()) << "Primary key violation should fail";
    // SQLITE_CONSTRAINT or SQLITE_CONSTRAINT_PRIMARYKEY
    EXPECT_TRUE(
            exec_result.error().code() == SQLITE_CONSTRAINT || (exec_result.error().code() & 0xFF) == SQLITE_CONSTRAINT
    );
}

TEST_F(StatementExecuteErrorTest, ExecuteNotNullViolation) {
    auto stmt_result = conn_.prepare("INSERT INTO test_table (id, email) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Try to insert NULL into NOT NULL column
    auto bind1 = stmt.bind_int(1, 2);
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_null(2); // email is NOT NULL
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();

    ASSERT_FALSE(exec_result.has_value()) << "NOT NULL constraint violation should fail";
    EXPECT_TRUE((exec_result.error().code() & 0xFF) == SQLITE_CONSTRAINT);
}

TEST_F(StatementExecuteErrorTest, StepOnSelectReturnsRowNotDone) {
    // Insert some data first
    auto insert_result = conn_.execute("INSERT INTO test_table (id, email) VALUES (2, 'other@example.com')");
    ASSERT_TRUE(insert_result.has_value());

    auto stmt_result = conn_.prepare("SELECT * FROM test_table");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // execute() expects SQLITE_DONE, but SELECT returns SQLITE_ROW
    auto exec_result = stmt.execute();

    // This should fail because execute() expects SQLITE_DONE but SELECT returns SQLITE_ROW
    ASSERT_FALSE(exec_result.has_value()) << "execute() on SELECT should fail (expects DONE, gets ROW)";
    EXPECT_EQ(exec_result.error().code(), SQLITE_ROW);
}

TEST_F(StatementExecuteErrorTest, StepReturnsError) {
    // Trigger step() error path by altering schema while statement is active
    auto stmt_result = conn_.prepare("SELECT * FROM test_table");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Drop the table while the statement is prepared - causes SQLITE_ERROR on step
    auto drop_result = conn_.execute("DROP TABLE test_table");
    ASSERT_TRUE(drop_result.has_value());

    auto step_result = stmt.step();
    ASSERT_FALSE(step_result.has_value()) << "step() should fail after schema change";
    // SQLITE_ERROR or SQLITE_ABORT expected
    EXPECT_NE(step_result.error().code(), SQLITE_ROW);
    EXPECT_NE(step_result.error().code(), SQLITE_DONE);
}

TEST_F(StatementExecuteErrorTest, StepReturnsRow) {
    auto stmt_result = conn_.prepare("SELECT * FROM test_table WHERE id = 1");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    auto step_result = stmt.step();

    ASSERT_TRUE(step_result.has_value()) << "step() should succeed";
    EXPECT_TRUE(step_result.value()) << "step() should return true for available row";

    // Second step should return false (no more rows)
    auto step_result2 = stmt.step();
    ASSERT_TRUE(step_result2.has_value());
    EXPECT_FALSE(step_result2.value()) << "step() should return false when no more rows";
}

// ============================================================================
// Foreign Key Constraint Error Tests
// ============================================================================

class ForeignKeyErrorTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};

    auto SetUp() -> void override {
        // Enable foreign keys
        auto fk_result = conn_.execute("PRAGMA foreign_keys = ON");
        ASSERT_TRUE(fk_result.has_value());

        // Create parent table
        auto parent_result = conn_.execute(
                "CREATE TABLE parent ("
                "id INTEGER PRIMARY KEY"
                ")"
        );
        ASSERT_TRUE(parent_result.has_value());

        // Create child table with foreign key
        auto child_result = conn_.execute(
                "CREATE TABLE child ("
                "id INTEGER PRIMARY KEY, "
                "parent_id INTEGER NOT NULL, "
                "FOREIGN KEY (parent_id) REFERENCES parent(id)"
                ")"
        );
        ASSERT_TRUE(child_result.has_value());

        // Insert parent record
        auto insert_result = conn_.execute("INSERT INTO parent (id) VALUES (1)");
        ASSERT_TRUE(insert_result.has_value());
    }
};

TEST_F(ForeignKeyErrorTest, InsertViolatesForeignKey) {
    auto stmt_result = conn_.prepare("INSERT INTO child (id, parent_id) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());

    auto stmt = std::move(stmt_result.value());

    // Try to insert child with non-existent parent
    auto bind1 = stmt.bind_int(1, 1);
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_int(2, 999); // parent_id=999 doesn't exist
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();

    ASSERT_FALSE(exec_result.has_value()) << "Foreign key violation should fail";
    // Should be SQLITE_CONSTRAINT_FOREIGNKEY or just SQLITE_CONSTRAINT
    EXPECT_TRUE((exec_result.error().code() & 0xFF) == SQLITE_CONSTRAINT);
}

TEST_F(ForeignKeyErrorTest, DeleteViolatesForeignKey) {
    // First insert a valid child
    auto insert_result = conn_.execute("INSERT INTO child (id, parent_id) VALUES (1, 1)");
    ASSERT_TRUE(insert_result.has_value());

    // Now try to delete the parent (should fail due to FK constraint)
    auto delete_result = conn_.execute("DELETE FROM parent WHERE id = 1");

    ASSERT_FALSE(delete_result.has_value()) << "Delete should fail due to foreign key constraint";
    EXPECT_TRUE((delete_result.error().code() & 0xFF) == SQLITE_CONSTRAINT);
}

// ============================================================================
// Statement Cache Tests
// ============================================================================

class StatementCacheTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};
};

TEST_F(StatementCacheTest, CachedStatementReuse) {
    const std::string_view sql = "SELECT 1";

    // First call should create statement
    auto result1 = conn_.prepare_cached(sql);
    ASSERT_TRUE(result1.has_value());
    Statement* stmt1 = *result1;

    // Second call should return same statement (pointer)
    auto result2 = conn_.prepare_cached(sql);
    ASSERT_TRUE(result2.has_value());
    Statement* stmt2 = *result2;

    EXPECT_EQ(stmt1, stmt2) << "Cached statements should be the same pointer";
}

TEST_F(StatementCacheTest, CacheCount) {
    EXPECT_EQ(conn_.cached_statement_count(), 0u) << "Cache should start empty";

    // Add statements to cache
    auto r1 = conn_.prepare_cached("SELECT 1");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(conn_.cached_statement_count(), 1u);

    auto r2 = conn_.prepare_cached("SELECT 2");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(conn_.cached_statement_count(), 2u);

    // Same SQL shouldn't increase count
    auto r3 = conn_.prepare_cached("SELECT 1");
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(conn_.cached_statement_count(), 2u);

    // Clear cache
    conn_.clear_statement_cache();
    EXPECT_EQ(conn_.cached_statement_count(), 0u);
}

TEST_F(StatementCacheTest, PrepareCachedInvalidSQLDoesNotCache) {
    std::size_t initial_count = conn_.cached_statement_count();

    // Try to cache invalid SQL
    auto result = conn_.prepare_cached("INVALID SQL SYNTAX !!!");
    ASSERT_FALSE(result.has_value());

    // Cache count should not increase
    EXPECT_EQ(conn_.cached_statement_count(), initial_count);
}

// ============================================================================
// Error Message Tests
// ============================================================================

class ErrorMessageTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};
};

TEST_F(ErrorMessageTest, ErrorHasMeaningfulMessage) {
    // Create table with unique constraint
    auto create_result = conn_.execute("CREATE TABLE test (id INTEGER PRIMARY KEY, value TEXT UNIQUE)");
    ASSERT_TRUE(create_result.has_value());

    // Insert initial data
    auto insert1 = conn_.execute("INSERT INTO test VALUES (1, 'unique_value')");
    ASSERT_TRUE(insert1.has_value());

    // Try to insert duplicate
    auto insert2 = conn_.execute("INSERT INTO test VALUES (2, 'unique_value')");
    ASSERT_FALSE(insert2.has_value());

    // Error message should be meaningful
    EXPECT_FALSE(insert2.error().message().empty());
    EXPECT_TRUE(
            insert2.error().message().find("UNIQUE") != std::string::npos ||
            insert2.error().message().find("unique") != std::string::npos ||
            insert2.error().message().find("constraint") != std::string::npos
    ) << "Error message should mention constraint: "
      << insert2.error().message();
}

TEST_F(ErrorMessageTest, PrepareErrorHasMessage) {
    auto result = conn_.prepare("SELECT * FROM nonexistent_table_abc123");
    ASSERT_FALSE(result.has_value());

    EXPECT_FALSE(result.error().message().empty());
    EXPECT_TRUE(
            result.error().message().find("no such table") != std::string::npos ||
            result.error().message().find("nonexistent_table_abc123") != std::string::npos
    ) << "Error message should mention the table: "
      << result.error().message();
}

// ============================================================================
// Edge Case Tests
// ============================================================================

class EdgeCaseTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};
};

TEST_F(EdgeCaseTest, LargeTextBinding) {
    auto create_result = conn_.execute("CREATE TABLE large_text (id INTEGER PRIMARY KEY, data TEXT)");
    ASSERT_TRUE(create_result.has_value());

    auto stmt_result = conn_.prepare("INSERT INTO large_text (id, data) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    // Create a large string (1MB)
    std::string large_string(1024 * 1024, 'x');

    auto bind1 = stmt.bind_int(1, 1);
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_text(2, large_string);
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();
    EXPECT_TRUE(exec_result.has_value()) << "Large text binding should succeed";
}

TEST_F(EdgeCaseTest, LargeBlobBinding) {
    auto create_result = conn_.execute("CREATE TABLE large_blob (id INTEGER PRIMARY KEY, data BLOB)");
    ASSERT_TRUE(create_result.has_value());

    auto stmt_result = conn_.prepare("INSERT INTO large_blob (id, data) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    // Create a large blob (1MB)
    std::vector<std::uint8_t> large_blob(1024 * 1024, 0xAB);

    auto bind1 = stmt.bind_int(1, 1);
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_blob(2, large_blob.data(), large_blob.size());
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();
    EXPECT_TRUE(exec_result.has_value()) << "Large blob binding should succeed";
}

TEST_F(EdgeCaseTest, EmptyTextBinding) {
    auto create_result = conn_.execute("CREATE TABLE empty_text (id INTEGER PRIMARY KEY, data TEXT)");
    ASSERT_TRUE(create_result.has_value());

    auto stmt_result = conn_.prepare("INSERT INTO empty_text (id, data) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    auto bind1 = stmt.bind_int(1, 1);
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_text(2, "");
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();
    EXPECT_TRUE(exec_result.has_value()) << "Empty text binding should succeed";
}

TEST_F(EdgeCaseTest, EmptyBlobBinding) {
    auto create_result = conn_.execute("CREATE TABLE empty_blob (id INTEGER PRIMARY KEY, data BLOB)");
    ASSERT_TRUE(create_result.has_value());

    auto stmt_result = conn_.prepare("INSERT INTO empty_blob (id, data) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    auto bind1 = stmt.bind_int(1, 1);
    ASSERT_TRUE(bind1.has_value());
    auto bind2 = stmt.bind_blob(2, nullptr, 0);
    ASSERT_TRUE(bind2.has_value());

    auto exec_result = stmt.execute();
    EXPECT_TRUE(exec_result.has_value()) << "Empty blob binding should succeed";
}

TEST_F(EdgeCaseTest, MultipleResetAndExecute) {
    auto create_result = conn_.execute("CREATE TABLE multi_exec (id INTEGER PRIMARY KEY, value INTEGER)");
    ASSERT_TRUE(create_result.has_value());

    auto stmt_result = conn_.prepare("INSERT INTO multi_exec (id, value) VALUES (?, ?)");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    // Execute multiple times with reset
    for (int i = 1; i <= 10; ++i) {
        stmt.reset();
        auto bind1 = stmt.bind_int(1, i);
        ASSERT_TRUE(bind1.has_value());
        auto bind2 = stmt.bind_int(2, i * 10);
        ASSERT_TRUE(bind2.has_value());
        auto exec_result = stmt.execute();
        ASSERT_TRUE(exec_result.has_value()) << "Execute iteration " << i << " should succeed";
    }

    // Verify all rows inserted
    auto select_result = conn_.prepare("SELECT COUNT(*) FROM multi_exec");
    ASSERT_TRUE(select_result.has_value());
    auto select_stmt = std::move(select_result.value());
    auto step_result = select_stmt.step();
    ASSERT_TRUE(step_result.has_value());
    EXPECT_TRUE(step_result.value());
    EXPECT_EQ(select_stmt.extract_int(0), 10);
}

// ============================================================================
// Transaction Error Tests
// ============================================================================

class TransactionErrorTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};

    auto SetUp() -> void override {
        auto result = conn_.execute("CREATE TABLE tx_test (id INTEGER PRIMARY KEY, value TEXT)");
        ASSERT_TRUE(result.has_value());
    }
};

TEST_F(TransactionErrorTest, CommitWithoutBegin) {
    // Committing without BEGIN should fail or be a no-op
    auto result = conn_.execute("COMMIT");

    // SQLite returns error when COMMIT without BEGIN
    ASSERT_FALSE(result.has_value()) << "COMMIT without BEGIN should fail";
}

TEST_F(TransactionErrorTest, RollbackWithoutBegin) {
    // Rollback without BEGIN should fail or be a no-op
    auto result = conn_.execute("ROLLBACK");

    // SQLite returns error when ROLLBACK without BEGIN
    ASSERT_FALSE(result.has_value()) << "ROLLBACK without BEGIN should fail";
}

TEST_F(TransactionErrorTest, NestedBeginTransactions) {
    auto begin1 = conn_.execute("BEGIN TRANSACTION");
    ASSERT_TRUE(begin1.has_value());

    // Nested BEGIN should fail
    auto begin2 = conn_.execute("BEGIN TRANSACTION");
    ASSERT_FALSE(begin2.has_value()) << "Nested BEGIN should fail";

    // Cleanup
    auto rollback = conn_.execute("ROLLBACK");
    EXPECT_TRUE(rollback.has_value());
}

// ============================================================================
// Column Extraction Tests
// ============================================================================

class ColumnExtractionTest : public ::testing::Test {
  public:
    Connection conn_{Connection::open(":memory:").value()};

    auto SetUp() -> void override {
        auto result = conn_.execute(
                "CREATE TABLE extract_test ("
                "id INTEGER PRIMARY KEY, "
                "int_val INTEGER, "
                "real_val REAL, "
                "text_val TEXT, "
                "blob_val BLOB, "
                "null_val TEXT"
                ")"
        );
        ASSERT_TRUE(result.has_value());

        result = conn_.execute("INSERT INTO extract_test VALUES (1, 42, 3.14, 'hello', X'DEADBEEF', NULL)");
        ASSERT_TRUE(result.has_value());
    }
};

TEST_F(ColumnExtractionTest, ExtractAllTypes) {
    auto stmt_result = conn_.prepare("SELECT * FROM extract_test WHERE id = 1");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    auto step_result = stmt.step();
    ASSERT_TRUE(step_result.has_value());
    ASSERT_TRUE(step_result.value());

    // Extract and verify values
    EXPECT_EQ(stmt.extract_int(0), 1);
    EXPECT_EQ(stmt.extract_int64(1), 42);
    EXPECT_DOUBLE_EQ(stmt.extract_double(2), 3.14);

    auto text = stmt.extract_text_view(3);
    EXPECT_EQ(text, "hello");

    EXPECT_TRUE(stmt.is_null(5));
    EXPECT_FALSE(stmt.is_null(0));
}

TEST_F(ColumnExtractionTest, ExtractBoolFromInt) {
    auto insert = conn_.execute("INSERT INTO extract_test (id, int_val) VALUES (2, 1)");
    ASSERT_TRUE(insert.has_value());

    auto stmt_result = conn_.prepare("SELECT int_val FROM extract_test WHERE id = 2");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    auto step_result = stmt.step();
    ASSERT_TRUE(step_result.has_value());
    ASSERT_TRUE(step_result.value());

    EXPECT_TRUE(stmt.extract_bool(0));
}

TEST_F(ColumnExtractionTest, ExtractNullText) {
    auto stmt_result = conn_.prepare("SELECT null_val FROM extract_test WHERE id = 1");
    ASSERT_TRUE(stmt_result.has_value());
    auto stmt = std::move(stmt_result.value());

    auto step_result = stmt.step();
    ASSERT_TRUE(step_result.has_value());
    ASSERT_TRUE(step_result.value());

    auto text = stmt.extract_text_view(0);
    EXPECT_TRUE(text.empty());
    EXPECT_TRUE(stmt.is_null(0));
}

// ============================================================================
// ORM-Level Error Tests
// ============================================================================
// These tests verify that ORM operations properly handle and propagate
// SQLite errors through the ORM layer (QuerySet, statements, etc.)

import storm;
import std;

#include "../../shared/models/person.h" // NOSONAR
using storm::primary;
using storm::QuerySet;
using storm::orm::schema::SchemaStatement;
using storm::orm::statements::InsertOptions;

// Local struct — UNIQUE constraint tests only
struct UniqueTestPerson {
    [[= primary]] int               id{};
    [[= storm::unique]] std::string email;
    int                             value{};
};

class ORMErrorTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        auto result = DefaultConnection<>::set(":memory:");
        ASSERT_TRUE(result.has_value()) << "Failed to open database";

        auto create_result = SchemaStatement<Person>::create_table_if_not_exists();
        ASSERT_TRUE(create_result.has_value()) << "Failed to create table";

        // Create table with UNIQUE constraint via unique on email field
        auto create_unique = SchemaStatement<UniqueTestPerson>::create_table_if_not_exists();
        ASSERT_TRUE(create_unique.has_value()) << "Failed to create UniqueTestPerson table";
    }

    auto TearDown() -> void override {
        DefaultConnection<>::clear();
    }
};

TEST_F(ORMErrorTest, InsertUniqueConstraintViolation) {
    QuerySet<UniqueTestPerson> qs;

    // Insert first person
    UniqueTestPerson const person1{.id = 0, .email = "test@example.com", .value = 100};
    auto                   result1 = qs.insert(person1).execute();
    ASSERT_TRUE(result1.has_value()) << "First insert should succeed";

    // Try to insert duplicate email (unique constraint violation)
    UniqueTestPerson const person2{.id = 0, .email = "test@example.com", .value = 200};
    auto                   result2 = qs.insert(person2).execute();

    ASSERT_FALSE(result2.has_value()) << "Duplicate email insert should fail";
    // Should be SQLITE_CONSTRAINT
    EXPECT_TRUE((result2.error().code() & 0xFF) == SQLITE_CONSTRAINT)
            << "Error should be constraint violation, got: " << result2.error().code();
}

TEST_F(ORMErrorTest, BatchInsertUniqueConstraintViolation) {
    QuerySet<UniqueTestPerson> qs;

    // Insert first person
    UniqueTestPerson const person1{.id = 0, .email = "first@example.com", .value = 100};
    auto                   result1 = qs.insert(person1).execute();
    ASSERT_TRUE(result1.has_value()) << "First insert should succeed";

    // Try batch insert with one duplicate
    std::vector<UniqueTestPerson> batch =
            {{0, "second@example.com", 200},
             {0, "first@example.com", 300}, // Duplicate!
             {0, "third@example.com", 400}};
    auto result2 = qs.insert(std::span<const UniqueTestPerson>(batch)).execute();

    // Batch insert should fail due to constraint violation
    ASSERT_FALSE(result2.has_value()) << "Batch insert with duplicate should fail";
    EXPECT_TRUE((result2.error().code() & 0xFF) == SQLITE_CONSTRAINT);
}

TEST_F(ORMErrorTest, UpdateUniqueConstraintViolation) {
    QuerySet<UniqueTestPerson> qs;

    // Insert two unique persons
    UniqueTestPerson const person1{.id = 0, .email = "first@example.com", .value = 100};
    UniqueTestPerson const person2{.id = 0, .email = "second@example.com", .value = 200};
    auto                   r1 = qs.insert(person1).execute();
    auto                   r2 = qs.insert(person2).execute();
    ASSERT_TRUE(r1.has_value() && r2.has_value());

    // Try to update person2's email to person1's email
    UniqueTestPerson const updated{.id = static_cast<int>(r2.value()), .email = "first@example.com", .value = 300};
    auto                   update_result = qs.update(updated).execute();

    ASSERT_FALSE(update_result.has_value()) << "Update violating unique constraint should fail";
    EXPECT_TRUE((update_result.error().code() & 0xFF) == SQLITE_CONSTRAINT);
}

TEST_F(ORMErrorTest, SelectEmptyResult) {
    QuerySet<Person> qs;

    // Select from empty table - should succeed with empty result
    auto result = qs.select().execute();
    ASSERT_TRUE(result.has_value()) << "Select from empty table should succeed";
    EXPECT_TRUE(result.value().empty()) << "Result should be empty";
}

TEST_F(ORMErrorTest, SelectWithWhereNoMatch) {
    QuerySet<Person> qs;

    // Insert some data
    Person const person{.id = 0, .name = "Alice", .age = 30};
    auto         insert_result = qs.insert(person).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Select with WHERE that matches nothing
    auto result = qs.where(fields::Person.age > 100).select().execute();
    ASSERT_TRUE(result.has_value()) << "Select with no matches should succeed";
    EXPECT_TRUE(result.value().empty()) << "Result should be empty";
}

TEST_F(ORMErrorTest, RemoveNonExistent) {
    QuerySet<Person> qs;

    // Try to erase non-existent person
    Person const nonexistent{.id = 999, .name = "Ghost", .age = 0};
    auto         result = qs.erase(nonexistent).execute();

    // Erase of non-existent should succeed (SQLite DELETE with no matches is not an error)
    ASSERT_TRUE(result.has_value()) << "Erase of non-existent should succeed";
}

TEST_F(ORMErrorTest, AggregateOnEmptyTable) {
    QuerySet<Person> qs;

    // COUNT on empty table should return 0
    auto count_result = qs.count().execute();
    ASSERT_TRUE(count_result.has_value()) << "COUNT on empty table should succeed";
    EXPECT_EQ(count_result.value(), 0);

    // SUM on empty table should return 0 (NULL coerced to 0)
    auto sum_result = qs.sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum_result.has_value()) << "SUM on empty table should succeed";
    EXPECT_EQ(sum_result.value(), 0);

    // AVG on empty table has no value -> nullopt, not a real 0.0 (#416)
    auto avg_result = qs.avg<fields::Person.age>().execute();
    ASSERT_TRUE(avg_result.has_value()) << "AVG on empty table should succeed";
    EXPECT_FALSE(avg_result.value().has_value()) << "AVG over empty set must be nullopt";
}

TEST_F(ORMErrorTest, AggregateWithWhereNoMatch) {
    QuerySet<Person> qs;

    // Insert data
    Person const person{.id = 0, .name = "Alice", .age = 30};
    auto         insert_result = qs.insert(person).execute();
    ASSERT_TRUE(insert_result.has_value());

    // COUNT with WHERE that matches nothing
    auto count_result = qs.where(fields::Person.age > 100).count().execute();
    ASSERT_TRUE(count_result.has_value()) << "COUNT with no matches should succeed";
    EXPECT_EQ(count_result.value(), 0);
}

TEST_F(ORMErrorTest, DistinctOnEmptyTable) {
    QuerySet<Person> qs;

    // DISTINCT on empty table
    auto result = qs.distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT on empty table should succeed";
    EXPECT_TRUE(result.value().empty());
}

TEST_F(ORMErrorTest, GroupByOnEmptyTable) {
    QuerySet<Person> qs;

    // GROUP BY on empty table
    auto result = qs.group_by<fields::Person.age>().count().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY on empty table should succeed";
    EXPECT_TRUE(result.value().empty());
}

TEST_F(ORMErrorTest, BatchUpdateWithConstraintViolation) {
    QuerySet<UniqueTestPerson> qs;

    // Insert multiple persons with unique emails
    std::vector<UniqueTestPerson> initial =
            {{0, "first@example.com", 100}, {0, "second@example.com", 200}, {0, "third@example.com", 300}};

    for (const auto& p : initial) {
        auto r = qs.insert(p).execute();
        ASSERT_TRUE(r.has_value()) << "Initial insert should succeed";
    }

    // Get the inserted persons
    auto select_result = qs.select().execute();
    ASSERT_TRUE(select_result.has_value());
    ASSERT_EQ(select_result.value().size(), 3);

    // Create batch update where one violates unique constraint
    std::vector<UniqueTestPerson> updates;
    int                           idx = 1;
    for (auto it = select_result.value().begin(); it != select_result.value().end(); ++it, ++idx) {
        if (idx == 2) {
            // Make second person's email same as first - violation
            updates.push_back({it->id, "first@example.com", it->value + 1000});
        } else {
            updates.push_back({it->id, it->email, it->value + 1000});
        }
    }

    auto update_result = qs.update(std::span<const UniqueTestPerson>(updates)).execute();
    ASSERT_FALSE(update_result.has_value()) << "Batch update with constraint violation should fail";
    EXPECT_TRUE((update_result.error().code() & 0xFF) == SQLITE_CONSTRAINT);
}

TEST_F(ORMErrorTest, BatchRemoveFromEmptyTable) {
    QuerySet<Person> qs;

    // Try to batch erase from empty table
    std::vector<Person> to_remove = {{1, "Ghost1", 25}, {2, "Ghost2", 30}, {3, "Ghost3", 35}};

    auto result = qs.erase(std::span<const Person>(to_remove)).execute();
    // Should succeed - SQLite DELETE with no matches is not an error
    ASSERT_TRUE(result.has_value()) << "Batch erase of non-existent should succeed";
}

TEST_F(ORMErrorTest, LargeBatchInsertThenRemove) {
    QuerySet<Person> qs;

    // Insert a larger batch to test chunking logic
    std::vector<Person> batch;
    batch.reserve(100);
    for (int i = 0; i < 100; ++i) {
        batch.emplace_back(0, std::format("Person{}", i), 20 + (i % 50));
    }

    // Batch insert returns void (not IDs) because consecutive ID assumption is unreliable
    auto insert_result = qs.insert(std::span<const Person>(batch)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Large batch insert should succeed";

    // Verify count
    auto count_result = qs.count().execute();
    ASSERT_TRUE(count_result.has_value());
    EXPECT_EQ(count_result.value(), 100);

    // Now batch erase all
    auto select_result = qs.select().execute();
    ASSERT_TRUE(select_result.has_value());
    EXPECT_EQ(select_result.value().size(), 100);

    std::vector<Person> to_remove;
    to_remove.reserve(100);
    for (const auto& p : select_result.value()) {
        to_remove.push_back(p);
    }

    auto remove_result = qs.erase(std::span<const Person>(to_remove)).execute();
    ASSERT_TRUE(remove_result.has_value()) << "Large batch erase should succeed";

    // Verify all removed
    auto final_count = qs.count().execute();
    ASSERT_TRUE(final_count.has_value());
    EXPECT_EQ(final_count.value(), 0);
}

TEST_F(ORMErrorTest, InsertThenSelectWithOrderBy) {
    QuerySet<Person> qs;

    // Insert in random order
    std::vector<Person> batch =
            {{0, "Charlie", 35}, {0, "Alice", 25}, {0, "Bob", 30}, {0, "Diana", 28}, {0, "Eve", 40}};

    for (const auto& p : batch) {
        auto r = qs.insert(p).execute();
        ASSERT_TRUE(r.has_value());
    }

    // Select with ORDER BY age ascending
    auto result = qs.order_by<fields::Person.age>().select().execute();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 5);

    // Verify order
    auto        it       = result.value().begin();
    int         prev_age = 0;
    std::string prev_name;
    while (it != result.value().end()) {
        EXPECT_GE(it->age, prev_age) << "Results should be ordered by age";
        prev_age  = it->age;
        prev_name = it->name;
        ++it;
    }
}

TEST_F(ORMErrorTest, SelectWithLimitOffset) {
    QuerySet<Person> qs;

    // Insert 10 persons
    for (int i = 1; i <= 10; ++i) {
        Person const p{.id = 0, .name = std::format("Person{}", i), .age = 20 + i};
        auto         r = qs.insert(p).execute();
        ASSERT_TRUE(r.has_value());
    }

    // Select with limit 3, offset 2
    auto result = qs.order_by<fields::Person.age>().limit(3).offset(2).select().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 3);

    // Verify we got the right persons (ages 23, 24, 25 with offset 2 and limit 3)
    auto it = result.value().begin();
    EXPECT_EQ(it->age, 23);
    ++it;
    EXPECT_EQ(it->age, 24);
    ++it;
    EXPECT_EQ(it->age, 25);
}

TEST_F(ORMErrorTest, ChunkedInsertRollsBackOnConstraintViolation) {
    QuerySet<UniqueTestPerson> qs;

    // Seed one existing row so the second chunk will hit a unique constraint
    UniqueTestPerson const seed{.id = 0, .email = "duplicate@example.com", .value = 0};
    auto                   seed_result = qs.insert(seed).execute();
    ASSERT_TRUE(seed_result.has_value());

    // Build a batch where the second row (second chunk with batch_size=1) is a duplicate
    std::vector<UniqueTestPerson> batch = {
            {0, "unique@example.com", 100},
            {0, "duplicate@example.com", 200}, // conflicts with seeded row
    };

    auto result = qs.insert(std::span<const UniqueTestPerson>(batch), InsertOptions{.batch_size = 1}).execute();

    ASSERT_FALSE(result.has_value()) << "Chunked insert with constraint violation should fail";
    EXPECT_TRUE((result.error().code() & 0xFF) == SQLITE_CONSTRAINT);

    // Verify rollback: only the seed row exists, the first chunk was rolled back too
    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 1) << "Transaction rollback should leave only the seeded row";
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result) // NOSONAR
// NOLINTEND(readability-identifier-length,cppcoreguidelines-init-variables) // NOSONAR
// NOLINTEND(readability-uppercase-literal-suffix) // NOSONAR
// NOLINTEND(misc-const-correctness,misc-unused-alias-decls,modernize-use-std-numbers) // NOSONAR
// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR
