#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length)

import storm_db_postgresql;
import storm_db_sqlite;
import std;
import storm_db_sqlite;

using storm::db::sqlite::Connection;

// ============================================================================
// Issue #271 — Phase 2: L3 statement cache thread safety
//
// These tests pound a single shared Connection from N threads across
// prepare_cached / clear_statement_cache / clear_statement_cache(table) to
// surface data races under TSAN. They are intentionally run on the
// ninja-tsan matrix. Against the pre-Phase-2 (unlocked) cache these races are
// reported by TSAN; after the shared_mutex lands they are clean.
//
// IMPORTANT lifetime note: a returned Statement* is only dereferenced WITHIN
// the iteration that fetched it and never across a concurrent clear() — that
// is the documented exclusive-ownership boundary. These tests stress the map
// synchronization, not pointer lifetime across a clear.
// ============================================================================

namespace {

    constexpr int kThreads        = 8;
    constexpr int kItersPerThread = 500;

    // A small, fixed set of SQL strings every thread cycles through, so cache
    // hits dominate (the shared_lock hot path).
    const std::array<std::string_view, 4> kHotSql = {
            "SELECT 1",
            "SELECT 2",
            "SELECT 3",
            "SELECT 4",
    };

    // Run `worker(thread_index)` on `count` jthreads and join them all. The
    // worker returns the number of failures it observed; the total is returned.
    // Centralizes the spawn/join/aggregate boilerplate shared by every test.
    template <typename Worker> [[nodiscard]] auto run_concurrently(int count, Worker worker) -> int {
        std::atomic<int>         failures{0};
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(count));
        for (int t = 0; t < count; ++t) {
            threads.emplace_back([t, &failures, &worker] { failures.fetch_add(worker(t), std::memory_order_relaxed); });
        }
        for (auto& th : threads) {
            th.join();
        }
        return failures.load();
    }

    // Count of (non-success / null) results from preparing `sql` once.
    [[nodiscard]] auto prepare_failure(Connection& conn, std::string_view sql) -> int {
        auto r = conn.prepare_cached(sql);
        return (r.has_value() && *r != nullptr) ? 0 : 1;
    }

    // Reader/writer stress: the last thread runs `writer` each iteration; the
    // rest prepare a `pick(i)`-selected SQL. Returns total reader failures.
    template <typename Pick, typename Write>
    [[nodiscard]] auto run_reader_writer(Connection& conn, Pick pick, Write writer) -> int {
        const int last = kThreads - 1;
        return run_concurrently(kThreads, [&conn, last, &pick, &writer](int t) {
            int f = 0;
            for (int i = 0; i < kItersPerThread; ++i) {
                if (t == last) {
                    writer();
                } else {
                    f += prepare_failure(conn, pick(i));
                }
            }
            return f;
        });
    }

    // ------------------------------------------------------------------ Test 1
    // N threads hammer the SAME small SQL set on one Connection. Almost all
    // calls are cache hits → exercises the shared_lock read path under
    // contention. TSAN-clean + no null pointers.
    TEST(StatementCacheThreadingTest, ConcurrentHitsAreRaceFree) {
        Connection conn = Connection::open(":memory:").value();
        for (auto sql : kHotSql) { // warm the cache so threads take the hit path
            ASSERT_TRUE(conn.prepare_cached(sql).has_value());
        }

        const int failures = run_concurrently(kThreads, [&conn](int /*t*/) {
            int f = 0;
            for (int i = 0; i < kItersPerThread; ++i) {
                f += prepare_failure(conn, kHotSql[static_cast<std::size_t>(i) % kHotSql.size()]);
            }
            return f;
        });

        EXPECT_EQ(failures, 0);
        EXPECT_EQ(conn.cached_statement_count(), kHotSql.size());
    }

    // ------------------------------------------------------------------ Test 2
    // N threads prepare DISTINCT SQL concurrently → exercises the miss/insert
    // path (release shared_lock, prepare, unique_lock try_emplace). All unique
    // statements must end up cached exactly once; TSAN-clean. Opened unbounded
    // (capacity 0) so this race-freedom check is not perturbed by #273 eviction.
    TEST(StatementCacheThreadingTest, ConcurrentDistinctInsertsAreRaceFree) {
        Connection conn = Connection::open(":memory:", {.statement_cache_capacity = 0}).value();

        const int failures = run_concurrently(kThreads, [&conn](int t) {
            int f = 0;
            for (int i = 0; i < kItersPerThread; ++i) {
                f += prepare_failure(conn, std::format("SELECT {} + {}", t, i));
            }
            return f;
        });

        EXPECT_EQ(failures, 0);
        // Each (t,i) pair is unique → all inserted, none lost.
        EXPECT_EQ(conn.cached_statement_count(), static_cast<std::size_t>(kThreads) * kItersPerThread);
    }

    // ------------------------------------------------------------------ Test 3
    // Readers prepare while one writer clears the WHOLE cache. The torn map
    // access (insert vs clear) is the classic race the mutex must serialize.
    // Pointers are used only within the fetching iteration, before any clear.
    TEST(StatementCacheThreadingTest, ConcurrentPrepareAndClearAllAreRaceFree) {
        Connection conn = Connection::open(":memory:").value();

        const int failures = run_reader_writer(
                conn,
                [](int i) { return kHotSql[static_cast<std::size_t>(i) % kHotSql.size()]; },
                [&conn] { conn.clear_statement_cache(); }
        );

        EXPECT_EQ(failures, 0);
    }

    // ------------------------------------------------------------------ Test 4
    // Readers prepare while one writer does per-table invalidation. Exercises
    // erase_if under the unique_lock against concurrent shared_lock reads.
    TEST(StatementCacheThreadingTest, ConcurrentPrepareAndClearTableAreRaceFree) {
        Connection conn = Connection::open(":memory:").value();
        ASSERT_TRUE(conn.execute("CREATE TABLE persons (id INTEGER PRIMARY KEY, name TEXT)").has_value());
        ASSERT_TRUE(conn.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, body TEXT)").has_value());

        const std::array<std::string_view, 2> table_sql = {
                "SELECT id FROM persons",
                "SELECT id FROM messages",
        };

        const int failures = run_reader_writer(
                conn,
                [&table_sql](int i) { return table_sql[static_cast<std::size_t>(i) % table_sql.size()]; },
                [&conn] { conn.clear_statement_cache(std::string_view{"persons"}); }
        );

        EXPECT_EQ(failures, 0);
    }

    // ------------------------------------------------------------------ Test 5
    // Issue #271 replaced Connection's defaulted move ops with explicit ones
    // (std::shared_mutex is not movable). Verify both the move constructor and
    // move assignment carry the L3 cache over and leave a usable target.
    TEST(StatementCacheThreadingTest, MoveOperationsPreserveCache) {
        Connection src = Connection::open(":memory:").value();
        ASSERT_TRUE(src.prepare_cached("SELECT 1").has_value());
        ASSERT_TRUE(src.prepare_cached("SELECT 2").has_value());
        ASSERT_EQ(src.cached_statement_count(), 2U);

        // Move construction carries the cache.
        Connection moved_ctor = std::move(src);
        EXPECT_EQ(moved_ctor.cached_statement_count(), 2U);
        EXPECT_TRUE(moved_ctor.prepare_cached("SELECT 1").has_value());

        // Move assignment carries the cache into an already-constructed target.
        Connection assign_target = Connection::open(":memory:").value();
        ASSERT_TRUE(assign_target.prepare_cached("SELECT 99").has_value());
        assign_target = std::move(moved_ctor);
        EXPECT_EQ(assign_target.cached_statement_count(), 2U);
        EXPECT_TRUE(assign_target.prepare_cached("SELECT 2").has_value());
    }

} // namespace

// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length)
