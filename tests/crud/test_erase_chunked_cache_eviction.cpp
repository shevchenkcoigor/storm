#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
using storm::db::sqlite::Connection;
using storm::orm::schema::SchemaStatement;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// Issue #728: execute_chunked() used to hold a Statement* for the full-chunk SQL
// across a second prepare_cached() for the remainder SQL. With the statement
// cache at capacity, that second call evicts the first entry and the held pointer
// dangles. Capacity 1 makes the eviction deterministic: the remainder insert
// always displaces the full-chunk entry. The use-after-free is reported by ASAN
// in CI (rule 7); on an unsanitized build it surfaces as a bind/step error on the
// finalized handle. On SQLite BEGIN/COMMIT also take a cache slot, so capacity 2
// holds BEGIN plus the full-chunk entry and which one the remainder evicts depends
// on unordered_map iteration order: capacity 1 is the deterministic guard, the
// capacity-2 cases only add coverage on this standard library.
namespace {

    constexpr std::size_t kFullChunk    = 799; // EraseGrammar<Person>::MAX_CHUNK_ROWS (single-column PK)
    constexpr std::size_t kSurvivorRows = 100;

    struct EvictionCase {
        int         rows;
        std::size_t cache_capacity;
    };

    auto case_name(const ::testing::TestParamInfo<EvictionCase>& info) -> std::string {
        return std::format("rows{}_cap{}", info.param.rows, info.param.cache_capacity);
    }

    class ChunkedEraseCacheEvictionTest : public ::testing::TestWithParam<EvictionCase> {
      public:
        std::shared_ptr<Connection> conn_;
        std::vector<Person>         people_;

        auto SetUp() -> void override {
            const auto [rows, capacity] = GetParam();
            conn_                       = std::make_shared<Connection>(
                    Connection::open(":memory:", {.statement_cache_capacity = capacity}).value()
            );
            ASSERT_TRUE(SchemaStatement<Person>::create_table_if_not_exists(conn_).has_value());
            for (int i = 1; i <= rows; ++i) {
                people_.emplace_back(Person{.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 60)});
            }
            auto inserted =
                    InsertStatement<Person, Connection>{conn_}.query(std::span<const Person>(people_)).execute();
            ASSERT_TRUE(inserted.has_value());
            ASSERT_EQ(count_persons(), rows);
            conn_->clear_statement_cache(); // start the erase from an empty, capacity-limited cache
        }

        auto count_persons() const -> std::int64_t {
            return SelectStatement<Person, Connection>{conn_}.all().count().execute().value();
        }
    };

} // namespace

TEST_P(ChunkedEraseCacheEvictionTest, ErasesEveryRowIncludingRemainder) {
    auto result = EraseStatement<Person, Connection>{conn_}.query(std::span<const Person>(people_)).execute();

    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(count_persons(), 0);
}

TEST_P(ChunkedEraseCacheEvictionTest, ErasesOnlyRequestedRows) {
    const auto erased = std::span<const Person>(people_).first(people_.size() - kSurvivorRows);

    auto result = EraseStatement<Person, Connection>{conn_}.query(erased).execute();

    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(count_persons(), static_cast<std::int64_t>(kSurvivorRows));
}

// Row counts cover: one full chunk + remainder, exact multiples (no remainder, so
// the second prepare_cached never runs), several full chunks, and several + remainder.
INSTANTIATE_TEST_SUITE_P(
        ChunkAndCapacityMatrix,
        ChunkedEraseCacheEvictionTest,
        ::testing::Values(
                EvictionCase{.rows = 1000, .cache_capacity = 1},
                EvictionCase{.rows = 1000, .cache_capacity = 2},
                EvictionCase{.rows = 2 * kFullChunk, .cache_capacity = 1},
                EvictionCase{.rows = 1650, .cache_capacity = 1},
                EvictionCase{.rows = 1650, .cache_capacity = 2},
                EvictionCase{.rows = 3 * kFullChunk + 3, .cache_capacity = 1}
        ),
        case_name
);
