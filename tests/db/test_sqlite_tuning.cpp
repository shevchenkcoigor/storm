#include <gtest/gtest.h>
#include <sqlite3.h> // SQLITE_BUSY macro — import std; cannot deliver C macros

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length)

import storm_db_pool;
import storm_db_sqlite;
import std;
import storm_db_sqlite;
import storm_db_pool;
using storm::db::ConnectionPool;

using storm::db::sqlite::Config;
using storm::db::sqlite::Connection;
using storm::db::sqlite::JournalMode;

// ============================================================================
// Issue #410 — SQLite connection tuning (busy_timeout default + optional WAL)
//
// busy_timeout default: opening a connection now sets a non-zero busy_timeout
// so two pooled connections on the same file wait instead of returning
// SQLITE_BUSY immediately. Optional WAL via journal_mode. Both knobs propagate
// through PoolConfig to each pooled connection.
// ============================================================================

namespace {

    // Read a single-int PRAGMA value (e.g. "busy_timeout", "journal_mode" as text).
    auto pragma_int(Connection& conn, std::string_view sql) -> std::int64_t {
        auto stmt = conn.prepare(sql);
        EXPECT_TRUE(stmt.has_value());
        auto stepped = stmt->step();
        EXPECT_TRUE(stepped.has_value());
        EXPECT_TRUE(*stepped);
        return stmt->extract_int64(0);
    }

    auto pragma_text(Connection& conn, std::string_view sql) -> std::string {
        auto stmt = conn.prepare(sql);
        EXPECT_TRUE(stmt.has_value());
        auto stepped = stmt->step();
        EXPECT_TRUE(stepped.has_value());
        EXPECT_TRUE(*stepped);
        return std::string(stmt->extract_text_view(0));
    }

    // Unique temp file path for tests that need an on-disk database (WAL and
    // cross-connection contention only work on file-backed databases).
    auto temp_db_path(std::string_view tag) -> std::string {
        const auto dir   = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return (dir / std::format("storm_410_{}_{}.db", tag, stamp)).string();
    }

    struct TempDb {
        std::string path;
        explicit TempDb(std::string_view tag) : path(temp_db_path(tag)) {
            remove();
        }
        ~TempDb() {
            remove();
        }
        TempDb(const TempDb&)                    = delete;
        auto operator=(const TempDb&) -> TempDb& = delete;
        TempDb(TempDb&&)                         = delete;
        auto operator=(TempDb&&) -> TempDb&      = delete;

        auto remove() const -> void {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(path + "-wal", ec);
            std::filesystem::remove(path + "-shm", ec);
            std::filesystem::remove(path + "-journal", ec);
        }
    };

    // ----------------------------------------------------------------- defaults
    TEST(SqliteTuningTest, ConfigDefaults) {
        Config cfg;
        EXPECT_EQ(cfg.statement_cache_capacity, 512U);
        EXPECT_EQ(cfg.busy_timeout_ms, 5000);
        EXPECT_EQ(cfg.journal_mode, JournalMode::Default);
    }

    // The default open() must apply the non-zero busy_timeout.
    TEST(SqliteTuningTest, DefaultBusyTimeoutApplied) {
        auto conn = Connection::open(":memory:");
        ASSERT_TRUE(conn.has_value());
        EXPECT_EQ(pragma_int(*conn, "PRAGMA busy_timeout"), 5000);
    }

    // A custom busy_timeout is honoured.
    TEST(SqliteTuningTest, CustomBusyTimeoutApplied) {
        auto conn = Connection::open(":memory:", {.busy_timeout_ms = 1234});
        ASSERT_TRUE(conn.has_value());
        EXPECT_EQ(pragma_int(*conn, "PRAGMA busy_timeout"), 1234);
    }

    // busy_timeout_ms = 0 disables the wait (legacy behaviour, opt-out).
    TEST(SqliteTuningTest, ZeroBusyTimeoutDisablesWait) {
        auto conn = Connection::open(":memory:", {.busy_timeout_ms = 0});
        ASSERT_TRUE(conn.has_value());
        EXPECT_EQ(pragma_int(*conn, "PRAGMA busy_timeout"), 0);
    }

    // ---------------------------------------------------------------------- WAL
    TEST(SqliteTuningTest, WalOptInOnFileDatabase) {
        TempDb db("wal");
        auto   conn = Connection::open(db.path, {.journal_mode = JournalMode::WAL});
        ASSERT_TRUE(conn.has_value());
        EXPECT_EQ(pragma_text(*conn, "PRAGMA journal_mode"), "wal");
    }

    // Default journal_mode leaves the engine default (rollback journal) — "delete"
    // on a fresh file database. The point: we did NOT switch it to WAL.
    TEST(SqliteTuningTest, DefaultJournalModeNotWal) {
        TempDb db("nowal");
        auto   conn = Connection::open(db.path);
        ASSERT_TRUE(conn.has_value());
        EXPECT_NE(pragma_text(*conn, "PRAGMA journal_mode"), "wal");
    }

    // WAL request on :memory: is silently ignored by SQLite (memory DBs can't use
    // WAL) — open() must still succeed, not error.
    TEST(SqliteTuningTest, WalOnMemoryDatabaseSucceeds) {
        auto conn = Connection::open(":memory:", {.journal_mode = JournalMode::WAL});
        ASSERT_TRUE(conn.has_value());
        EXPECT_NE(pragma_text(*conn, "PRAGMA journal_mode"), "wal");
    }

    // -------------------------------------------------------- contention proof
    // With busy_timeout = 0, a second writer on the same file while a write
    // transaction is open returns SQLITE_BUSY immediately. With a non-zero
    // timeout the writer waits (and here, since we hold the lock for the whole
    // test, eventually times out — but does NOT fail instantly). We assert the
    // instant-failure case to prove the knob is wired: timeout=0 => SQLITE_BUSY.
    TEST(SqliteTuningTest, ZeroTimeoutHitsBusyImmediately) {
        TempDb db("busy");

        auto writer = Connection::open(db.path, {.busy_timeout_ms = 0});
        ASSERT_TRUE(writer.has_value());
        auto blocker = Connection::open(db.path, {.busy_timeout_ms = 0});
        ASSERT_TRUE(blocker.has_value());

        ASSERT_TRUE(writer->execute("CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER)").has_value());

        // blocker holds a write lock via an open transaction.
        ASSERT_TRUE(blocker->execute("BEGIN IMMEDIATE").has_value());
        ASSERT_TRUE(blocker->execute("INSERT INTO t(v) VALUES (1)").has_value());

        // writer tries to write — database is locked, timeout=0 => immediate SQLITE_BUSY.
        auto r = writer->execute("INSERT INTO t(v) VALUES (2)");
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code(), SQLITE_BUSY);

        std::ignore = blocker->execute("ROLLBACK");
    }

    // ------------------------------------------------------- pool propagation
    TEST(SqliteTuningTest, PoolPropagatesBusyTimeout) {
        TempDb db("pool");
        auto   pool = ConnectionPool<Connection>::create(
                db.path, {.min_connections = 1, .max_connections = 2, .busy_timeout_ms = 4321}
        );
        ASSERT_TRUE(pool.has_value());

        auto handle = pool->checkout();
        ASSERT_TRUE(handle.has_value());
        EXPECT_EQ(pragma_int(**handle, "PRAGMA busy_timeout"), 4321);
    }

    TEST(SqliteTuningTest, PoolPropagatesWal) {
        TempDb db("poolwal");
        auto   pool = ConnectionPool<Connection>::create(
                db.path, {.min_connections = 1, .max_connections = 2, .journal_mode = JournalMode::WAL}
        );
        ASSERT_TRUE(pool.has_value());

        auto handle = pool->checkout();
        ASSERT_TRUE(handle.has_value());
        EXPECT_EQ(pragma_text(**handle, "PRAGMA journal_mode"), "wal");
    }

} // namespace

// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length)
