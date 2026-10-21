#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import std;

#include "../../shared/models/simple_record.h"
#include "test_fixture.h"
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// Test fixture for large SELECT operations — templated on database backend
template <typename ConnType> class SelectLargeTest : public StormTestFixture<SimpleRecord, ConnType> {};

TYPED_TEST_SUITE(SelectLargeTest, DatabaseTypes);

// Test: SELECT with result set larger than initial capacity (10K)
// This tests the exponential growth path
TYPED_TEST(SelectLargeTest, SelectMoreThan10KRows) {
    // Insert 25,000 records (will trigger exponential growth: 10K → 20K → 40K)
    constexpr int             RECORD_COUNT = 25000;
    std::vector<SimpleRecord> records;
    records.reserve(RECORD_COUNT);

    for (int i = 1; i <= RECORD_COUNT; ++i) {
        records.emplace_back(SimpleRecord{.id = i, .name = std::format("Record_{}", i), .value = i * 10});
    }

    auto insert_result =
            (InsertStatement<SimpleRecord, TypeParam>{}).query(std::span<const SimpleRecord>(records)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Batch INSERT failed: " << insert_result.error().message();

    // SELECT all rows - this will exercise exponential growth
    auto select_result = (SelectStatement<SimpleRecord, TypeParam>{}).all().execute();
    ASSERT_TRUE(select_result.has_value()) << "SELECT failed: " << select_result.error().message();

    const auto& retrieved = select_result.value();
    ASSERT_EQ(retrieved.size(), RECORD_COUNT) << "Should retrieve all 25K records";

    // Verify data integrity (spot checks)
    auto it = retrieved.begin();
    EXPECT_EQ(it->value, 10);
    EXPECT_EQ(it->name, "Record_1");

    auto it_10k = std::ranges::next(retrieved.begin(), 9999); // 10000th element
    EXPECT_EQ(it_10k->value, 100000);                         // 10K boundary
    EXPECT_EQ(it_10k->name, "Record_10000");

    auto it_20k = std::ranges::next(retrieved.begin(), 19999); // 20000th element
    EXPECT_EQ(it_20k->value, 200000);                          // 20K boundary
    EXPECT_EQ(it_20k->name, "Record_20000");

    auto last_it = std::ranges::next(retrieved.begin(), RECORD_COUNT - 1);
    EXPECT_EQ(last_it->value, RECORD_COUNT * 10);
    EXPECT_EQ(last_it->name, "Record_" + std::to_string(RECORD_COUNT));
}

// Test: SELECT with result set at exactly 10K (no exponential growth needed)
TYPED_TEST(SelectLargeTest, SelectExactly10KRows) {
    constexpr int             RECORD_COUNT = 10000;
    std::vector<SimpleRecord> records;
    records.reserve(RECORD_COUNT);

    for (int i = 1; i <= RECORD_COUNT; ++i) {
        records.emplace_back(SimpleRecord{.id = i, .name = std::format("R{}", i), .value = i});
    }

    auto insert_result =
            (InsertStatement<SimpleRecord, TypeParam>{}).query(std::span<const SimpleRecord>(records)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT all rows - should fit in initial capacity exactly
    auto select_result = (SelectStatement<SimpleRecord, TypeParam>{}).all().execute();
    ASSERT_TRUE(select_result.has_value());

    const auto& retrieved = select_result.value();
    ASSERT_EQ(retrieved.size(), RECORD_COUNT);

    EXPECT_EQ(retrieved.begin()->value, 1);
    auto last_it = std::ranges::next(retrieved.begin(), RECORD_COUNT - 1);
    EXPECT_EQ(last_it->value, RECORD_COUNT);
}

// Test: SELECT with result set slightly over 10K (minimal exponential growth)
TYPED_TEST(SelectLargeTest, SelectSlightlyOver10KRows) {
    constexpr int             RECORD_COUNT = 10001; // Just 1 over capacity
    std::vector<SimpleRecord> records;
    records.reserve(RECORD_COUNT);

    for (int i = 1; i <= RECORD_COUNT; ++i) {
        records.emplace_back(SimpleRecord{.id = i, .name = "Test", .value = i});
    }

    auto insert_result =
            (InsertStatement<SimpleRecord, TypeParam>{}).query(std::span<const SimpleRecord>(records)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT all rows - will grow to 20K
    auto select_result = (SelectStatement<SimpleRecord, TypeParam>{}).all().execute();
    ASSERT_TRUE(select_result.has_value());

    const auto& retrieved = select_result.value();
    ASSERT_EQ(retrieved.size(), RECORD_COUNT);

    auto last_it = std::ranges::next(retrieved.begin(), RECORD_COUNT - 1);
    EXPECT_EQ(last_it->value, 10001); // The one that triggered growth
}

// Test: SELECT with very large result set (100K rows)
// Tests multiple exponential growth cycles
TYPED_TEST(SelectLargeTest, SelectVeryLargeDataset) {
    // 100K records: 10K → 20K → 40K → 80K → 160K (4 growth cycles)
    constexpr int RECORD_COUNT = 100000;

    // Insert in batches to avoid hitting SQLite limits
    constexpr int BATCH_SIZE = 10000;
    for (int batch = 0; batch < RECORD_COUNT / BATCH_SIZE; ++batch) {
        std::vector<SimpleRecord> batch_records;
        batch_records.reserve(BATCH_SIZE);

        for (int i = 1; i <= BATCH_SIZE; ++i) {
            int const record_num = (batch * BATCH_SIZE) + i;
            batch_records.emplace_back(
                    SimpleRecord{.id = record_num, .name = std::format("B{}", batch), .value = record_num}
            );
        }

        auto insert_result = (InsertStatement<SimpleRecord, TypeParam>{})
                                     .query(std::span<const SimpleRecord>(batch_records))
                                     .execute();
        ASSERT_TRUE(insert_result.has_value()) << "Batch " << batch << " INSERT failed";
    }

    // SELECT all 100K rows
    auto select_result = (SelectStatement<SimpleRecord, TypeParam>{}).all().execute();
    ASSERT_TRUE(select_result.has_value()) << "SELECT 100K rows failed: " << select_result.error().message();

    const auto& retrieved = select_result.value();
    ASSERT_EQ(retrieved.size(), RECORD_COUNT) << "Should retrieve all 100K records";

    // Verify data integrity at key boundaries
    EXPECT_EQ(retrieved.begin()->value, 1);
    EXPECT_EQ(std::ranges::next(retrieved.begin(), 10000)->value, 10001); // After initial capacity
    EXPECT_EQ(std::ranges::next(retrieved.begin(), 20000)->value, 20001); // After first growth
    EXPECT_EQ(std::ranges::next(retrieved.begin(), 40000)->value, 40001); // After second growth
    EXPECT_EQ(std::ranges::next(retrieved.begin(), 80000)->value, 80001); // After third growth
    auto last_it = std::ranges::next(retrieved.begin(), RECORD_COUNT - 1);
    EXPECT_EQ(last_it->value, RECORD_COUNT);
}
