#include <gtest/gtest.h>

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes)

import storm_db_sqlite;
import std;
import storm_db_sqlite;
import storm_db_concept;

using storm::db::sqlite::Connection;
using storm::db::sqlite::Statement;

// ============================================================================
// Single-level Connection statement cache (formerly "Level 3")
//
// These tests pin the contract for the Connection statement cache: cached
// Statement* pointers stay stable across map rehash, and the per-table
// clear_statement_cache(table) overload drops only entries that reference the
// named table (word-boundary aware). They also lock down the dangling-pointer
// fix that stores statements as `unordered_map<string, unique_ptr<Statement>>`
// so the pinned pointers survive growth of the map.
// ============================================================================

namespace {

    class CacheInvalidationLevel3Test : public ::testing::Test {
      public:
        Connection conn_{Connection::open(":memory:").value()};
    };

    // ------------------------------------------------------------------ Test 1
    // Pointers returned by prepare_cached must survive arbitrary growth of the
    // underlying cache. With the old `unordered_map<string, Statement>` value
    // storage, the first emplace past the load-factor rehashes and invalidates
    // every Statement* held by upstream callers (Level 2). The fix is
    // `unordered_map<string, unique_ptr<Statement>>` — the unique_ptr stays
    // pinned in place, only the map nodes move.
    TEST_F(CacheInvalidationLevel3Test, PointersStableAcrossRehash) {
        // Cache an initial statement and stash the pointer.
        auto first = conn_.prepare_cached("SELECT 1");
        ASSERT_TRUE(first.has_value());
        Statement* pinned = *first;
        ASSERT_NE(pinned, nullptr);

        // Force well past the default rehash threshold (libstdc++ starts at 1
        // bucket, libc++ at 2; 64 unique inserts guarantees several rehashes).
        constexpr int churn_count = 64;
        for (int i = 2; i < 2 + churn_count; ++i) {
            auto r = conn_.prepare_cached(std::format("SELECT {}", i));
            ASSERT_TRUE(r.has_value()) << "iteration " << i;
        }

        // Re-fetch the original SQL: must match the stashed pointer, must not
        // crash on reset/execute. With the buggy value storage, `pinned` is
        // dangling at this point.
        auto again = conn_.prepare_cached("SELECT 1");
        ASSERT_TRUE(again.has_value());
        EXPECT_EQ(*again, pinned) << "cached Statement* must survive map rehash";

        // Exercise the pointer to surface ASAN failures even if equality holds
        // by coincidence on a release build.
        pinned->reset();
        auto exec = pinned->execute();
        EXPECT_TRUE(exec.has_value() || !exec.has_value()); // touch the object
    }

    // ------------------------------------------------------------------ Test 2
    // clear_statement_cache(table) must drop entries that reference the table
    // and leave entries for other tables alone. Pre-fix there is no overload;
    // the test will not compile until Level 3 is updated.
    TEST_F(CacheInvalidationLevel3Test, PerTableClearKeepsUnrelatedEntries) {
        ASSERT_TRUE(conn_.execute("CREATE TABLE persons (id INTEGER PRIMARY KEY, name TEXT)").has_value());
        ASSERT_TRUE(conn_.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, body TEXT)").has_value());

        ASSERT_TRUE(conn_.prepare_cached("SELECT id FROM persons").has_value());
        ASSERT_TRUE(conn_.prepare_cached("SELECT id FROM messages").has_value());
        ASSERT_EQ(conn_.cached_statement_count(), 2U);

        conn_.clear_statement_cache(std::string_view{"persons"});

        EXPECT_EQ(conn_.cached_statement_count(), 1U) << "per-table clear must keep the unrelated `messages` entry";

        // Re-prepare both: persons should be a miss (count back to 2), messages
        // should be a hit (count unchanged after the miss bumped it to 2).
        auto persons_again = conn_.prepare_cached("SELECT id FROM persons");
        ASSERT_TRUE(persons_again.has_value());
        EXPECT_EQ(conn_.cached_statement_count(), 2U);

        auto messages_again = conn_.prepare_cached("SELECT id FROM messages");
        ASSERT_TRUE(messages_again.has_value());
        EXPECT_EQ(conn_.cached_statement_count(), 2U) << "messages cache hit must not grow the cache";
    }

    // sql_references_table fast-out: empty table name or SQL shorter than the
    // table name must short-circuit to false without scanning. Exercises the
    // early-return branch in concept.cppm.
    TEST(SqlReferencesTableFastOut, EmptyOrTooShortReturnsFalse) {
        EXPECT_FALSE(storm::db::sql_references_table("SELECT 1", ""));
        EXPECT_FALSE(storm::db::sql_references_table("ab", "persons"));
        EXPECT_FALSE(storm::db::sql_references_table("", "persons"));
        // Sanity: a real match still works.
        EXPECT_TRUE(storm::db::sql_references_table("SELECT id FROM persons", "persons"));
    }

    // ------------------------------------------------------------------ Test 3
    // Per-table clear must respect word boundaries: clearing "persons" must
    // NOT drop "person_addresses". A naive substring match would.
    TEST_F(CacheInvalidationLevel3Test, PerTableClearRespectsWordBoundaries) {
        ASSERT_TRUE(conn_.execute("CREATE TABLE persons (id INTEGER PRIMARY KEY)").has_value());
        ASSERT_TRUE(conn_.execute("CREATE TABLE person_addresses (id INTEGER PRIMARY KEY, street TEXT)").has_value());

        ASSERT_TRUE(conn_.prepare_cached("SELECT id FROM persons").has_value());
        ASSERT_TRUE(conn_.prepare_cached("SELECT id FROM person_addresses").has_value());
        ASSERT_EQ(conn_.cached_statement_count(), 2U);

        conn_.clear_statement_cache(std::string_view{"persons"});

        EXPECT_EQ(conn_.cached_statement_count(), 1U) << "person_addresses must survive a clear targeting `persons`";
    }

    // ------------------------------------------------------------------ #273
    // Capacity-bounded eviction: a connection opened with capacity N keeps at
    // most N entries; inserting M > N distinct SQLs evicts M - N of them.
    TEST(CacheEvictionTest, BoundsSizeAtCapacity) {
        auto          conn     = Connection::open(":memory:", {.statement_cache_capacity = 4}).value();
        constexpr int kInserts = 10;
        for (int i = 0; i < kInserts; ++i) {
            ASSERT_TRUE(conn.prepare_cached(std::format("SELECT {}", i)).has_value()) << "insert " << i;
        }
        const auto stats = conn.cache_stats();
        EXPECT_EQ(stats.current_size, 4U);
        EXPECT_EQ(stats.evictions, static_cast<std::uint64_t>(kInserts) - 4U);
    }

    // Second chance: an entry re-hit between cold inserts keeps its ref bit set
    // and survives eviction. With capacity 2, hammering "SELECT 100" while
    // churning distinct cold SQLs must leave the hot entry a cache hit afterwards
    // (its ref bit was re-set every round, so the sweep gives it a second chance).
    TEST(CacheEvictionTest, SecondChanceKeepsHotEntry) {
        auto conn = Connection::open(":memory:", {.statement_cache_capacity = 2}).value();
        ASSERT_TRUE(conn.prepare_cached("SELECT 100").has_value()); // the HOT entry (miss)

        constexpr int kColdRounds = 8;
        for (int i = 0; i < kColdRounds; ++i) {
            ASSERT_TRUE(conn.prepare_cached("SELECT 100").has_value());                // re-hit HOT (sets ref bit)
            ASSERT_TRUE(conn.prepare_cached(std::format("SELECT {}", i)).has_value()); // distinct cold insert
        }
        // Each distinct cold SQL was a miss; sanity-check the churn happened.
        EXPECT_GE(conn.cache_stats().misses, static_cast<std::uint64_t>(kColdRounds));

        // Re-preparing HOT must be a hit: the hits counter increases, misses does not.
        const auto before = conn.cache_stats();
        ASSERT_TRUE(conn.prepare_cached("SELECT 100").has_value());
        const auto after = conn.cache_stats();
        EXPECT_EQ(after.misses, before.misses) << "HOT entry was evicted despite being re-hit each round";
        EXPECT_EQ(after.hits, before.hits + 1);
    }

    // All-referenced sweep: when every entry's ref bit is set, the CLOCK sweep
    // clears them all in one pass (second-chance) then evicts the first entry.
    // Re-hitting all N entries before an over-capacity insert deterministically
    // exercises both the clear-and-continue branch and the all-cleared fallback.
    TEST(CacheEvictionTest, AllReferencedSweepClearsThenEvictsFirst) {
        auto conn = Connection::open(":memory:", {.statement_cache_capacity = 3}).value();
        ASSERT_TRUE(conn.prepare_cached("SELECT 1").has_value());
        ASSERT_TRUE(conn.prepare_cached("SELECT 2").has_value());
        ASSERT_TRUE(conn.prepare_cached("SELECT 3").has_value());
        ASSERT_EQ(conn.cache_stats().current_size, 3U);

        // Re-hit all three: every ref bit is now set.
        ASSERT_TRUE(conn.prepare_cached("SELECT 1").has_value());
        ASSERT_TRUE(conn.prepare_cached("SELECT 2").has_value());
        ASSERT_TRUE(conn.prepare_cached("SELECT 3").has_value());

        // Over-capacity insert: sweep clears all bits (continue branch) then
        // evicts the first entry (all-cleared fallback). One eviction, size holds.
        ASSERT_TRUE(conn.prepare_cached("SELECT 4").has_value());
        const auto stats = conn.cache_stats();
        EXPECT_EQ(stats.current_size, 3U);
        EXPECT_EQ(stats.evictions, 1U);
    }

    // Stats correctness over a known sequence.
    TEST(CacheStatsTest, CountsHitsMissesSize) {
        auto conn = Connection::open(":memory:").value();         // default capacity, no eviction here
        ASSERT_TRUE(conn.prepare_cached("SELECT 1").has_value()); // miss
        ASSERT_TRUE(conn.prepare_cached("SELECT 2").has_value()); // miss
        ASSERT_TRUE(conn.prepare_cached("SELECT 1").has_value()); // hit
        ASSERT_TRUE(conn.prepare_cached("SELECT 1").has_value()); // hit
        const auto stats = conn.cache_stats();
        EXPECT_EQ(stats.misses, 2U);
        EXPECT_EQ(stats.hits, 2U);
        EXPECT_EQ(stats.evictions, 0U);
        EXPECT_EQ(stats.current_size, 2U);
    }

    // Memory-growth bound: 10k unique SQLs at capacity 512 stays at 512.
    TEST(CacheEvictionTest, MemoryGrowthBounded) {
        auto          conn    = Connection::open(":memory:", {.statement_cache_capacity = 512}).value();
        constexpr int kUnique = 10000;
        for (int i = 0; i < kUnique; ++i) {
            ASSERT_TRUE(conn.prepare_cached(std::format("SELECT {}", i)).has_value());
        }
        const auto stats = conn.cache_stats();
        EXPECT_EQ(stats.current_size, 512U);
        EXPECT_EQ(stats.evictions, static_cast<std::uint64_t>(kUnique) - 512U);
    }

    // Unbounded mode (capacity 0): no eviction, every distinct SQL retained.
    TEST(CacheEvictionTest, UnboundedRetainsAll) {
        auto          conn    = Connection::open(":memory:", {.statement_cache_capacity = 0}).value();
        constexpr int kUnique = 2000;
        for (int i = 0; i < kUnique; ++i) {
            ASSERT_TRUE(conn.prepare_cached(std::format("SELECT {}", i)).has_value());
        }
        const auto stats = conn.cache_stats();
        EXPECT_EQ(stats.current_size, static_cast<std::size_t>(kUnique));
        EXPECT_EQ(stats.evictions, 0U);
    }

    // clear_statement_cache empties the map but preserves lifetime counters.
    TEST(CacheStatsTest, ClearPreservesCounters) {
        auto conn = Connection::open(":memory:").value();
        ASSERT_TRUE(conn.prepare_cached("SELECT 1").has_value()); // miss
        ASSERT_TRUE(conn.prepare_cached("SELECT 1").has_value()); // hit
        conn.clear_statement_cache();
        const auto stats = conn.cache_stats();
        EXPECT_EQ(stats.current_size, 0U);
        EXPECT_EQ(stats.hits, 1U) << "counters are lifetime totals, not reset by clear";
        EXPECT_EQ(stats.misses, 1U);
    }

} // namespace

// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes)
