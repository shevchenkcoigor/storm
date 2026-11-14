#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
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
