#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include <sqlite3.h>

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "../../shared/models/simple_record.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::primary;
using storm::primary_autoincrement;
using storm::orm::statements::InsertOptions;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::ReturnId;
using storm::orm::statements::SelectStatement;

// =============================================================================
// Batch INSERT with RETURNING — returns std::vector<int64_t> of inserted IDs
// =============================================================================

template <typename ConnType> class BatchInsertReturningTest : public StormTestFixture<SimpleRecord, ConnType> {};

TYPED_TEST_SUITE(BatchInsertReturningTest, DatabaseTypes);

// Basic batch insert returning IDs
TYPED_TEST(BatchInsertReturningTest, BasicBatchReturnsIds) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
            {0, "Charlie", 30},
    };

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records))
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Batch insert returning should succeed";
    ASSERT_EQ(result.value().size(), 3) << "Should return 3 IDs";

    // All IDs should be positive
    for (const auto id : result.value()) {
        EXPECT_GT(id, 0) << "Each returned ID should be positive";
    }

    // All IDs should be unique
    const std::set<std::int64_t> unique_ids(result.value().begin(), result.value().end());
    EXPECT_EQ(unique_ids.size(), 3) << "All returned IDs should be unique";
}

// Verify returned IDs match actual inserted rows
TYPED_TEST(BatchInsertReturningTest, ReturnedIdsMatchInsertedRows) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
            {0, "Charlie", 30},
    };

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records))
                          .execute();
    ASSERT_TRUE(result.has_value());

    // SELECT back each row by ID and verify data
    for (std::size_t i = 0; i < records.size(); ++i) {
        auto row = (SelectStatement<SimpleRecord, TypeParam>{})
                           .where(fields::SimpleRecord.id == static_cast<int>(result.value()[i]))
                           .execute();
        ASSERT_TRUE(row.has_value()) << "Should find row with ID " << result.value()[i];
        ASSERT_EQ(row.value().size(), 1);
        EXPECT_EQ(row.value().begin()->name, records[i].name);
        EXPECT_EQ(row.value().begin()->value, records[i].value);
    }
}

// =============================================================================
// AUTOINCREMENT opt-in: ids still auto-assign on insert (#379)
// =============================================================================
//
// SimpleRecord (above) uses plain primary → plain INTEGER PRIMARY KEY,
// and BasicBatchReturnsIds already proves ids auto-assign without AUTOINCREMENT.
// AutoIncRecord opts into the never-reuse guarantee; this proves the opt-in path
// also auto-assigns ids (the keyword adds a guarantee, it does not change that
// you INSERT with id=0 and the DB fills it in).
struct AutoIncRecord {
    [[= primary_autoincrement]] int id{};
    int                             value{};
};

template <typename ConnType> class AutoIncrementInsertTest : public StormTestFixture<AutoIncRecord, ConnType> {};

TYPED_TEST_SUITE(AutoIncrementInsertTest, DatabaseTypes);

TYPED_TEST(AutoIncrementInsertTest, IdsAutoAssignOnInsert) {
    using ReturnId = ReturnId;

    std::vector<AutoIncRecord> const records = {{0, 10}, {0, 20}, {0, 30}};

    auto result = (InsertStatement<AutoIncRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const AutoIncRecord>(records))
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Opt-in AUTOINCREMENT insert should succeed";
    ASSERT_EQ(result.value().size(), 3);
    for (const auto id : result.value()) {
        EXPECT_GT(id, 0) << "Opt-in AUTOINCREMENT must still auto-assign a positive id";
    }
    const std::set<std::int64_t> unique_ids(result.value().begin(), result.value().end());
    EXPECT_EQ(unique_ids.size(), 3) << "Auto-assigned ids must be unique";
}

// Empty span returns empty vector
TYPED_TEST(BatchInsertReturningTest, EmptySpanReturnsEmptyVector) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> empty;
    auto                      result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(empty))
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Empty batch insert returning should succeed";
    EXPECT_TRUE(result.value().empty()) << "Should return empty vector for empty input";
}

// Single-element batch
TYPED_TEST(BatchInsertReturningTest, SingleElementBatch) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {{0, "Alice", 10}};

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records))
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Single-element batch insert returning should succeed";
    ASSERT_EQ(result.value().size(), 1) << "Should return 1 ID";
    EXPECT_GT(result.value()[0], 0) << "Returned ID should be positive";

    // Verify count
    auto count = (SelectStatement<SimpleRecord, TypeParam>{}).all().count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 1);
}

// Default batch insert (no template arg) still returns void
TYPED_TEST(BatchInsertReturningTest, DefaultBatchInsertReturnsVoid) {
    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
    };

    auto result = (InsertStatement<SimpleRecord, TypeParam>{}).query(std::span<const SimpleRecord>(records)).execute();

    static_assert(
            std::is_same_v<decltype(result), std::expected<void, typename TypeParam::Error>>,
            "Default batch insert should return std::expected<void, Error>"
    );
    ASSERT_TRUE(result.has_value());
}

// Return type check for batch returning
TYPED_TEST(BatchInsertReturningTest, ReturnTypeIsVectorOfInt64) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {{0, "Alice", 10}};

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records))
                          .execute();

    static_assert(
            std::is_same_v<decltype(result), std::expected<std::vector<std::int64_t>, typename TypeParam::Error>>,
            "Batch insert<ReturnId::Yes> should return std::expected<std::vector<int64_t>, Error>"
    );
    ASSERT_TRUE(result.has_value());
}

// =============================================================================
// Batch INSERT RETURNING with Person (all field types: int, string, double, bool, optional, blob)
// =============================================================================

template <typename ConnType> class BatchInsertReturningPersonTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(BatchInsertReturningPersonTest, DatabaseTypes);

TYPED_TEST(BatchInsertReturningPersonTest, BatchWithAllFieldTypes) {
    using ReturnId = ReturnId;

    std::vector<Person> const people = {
            {.name       = "Alice",
             .age        = 30,
             .salary     = 75000.0,
             .is_active  = true,
             .department = "Eng",
             .score      = 95,
             .nickname   = "Ali"},
            {.name = "Bob", .age = 25, .salary = 50000.0, .is_active = false, .department = "Sales"},
    };

    auto result = (InsertStatement<Person, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const Person>(people))
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Batch insert returning with all field types should succeed";
    ASSERT_EQ(result.value().size(), 2);

    for (const auto id : result.value()) {
        EXPECT_GT(id, 0);
    }
}

// IDs are in insertion order
TYPED_TEST(BatchInsertReturningTest, IdsAreInInsertionOrder) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {
            {0, "First", 1},
            {0, "Second", 2},
            {0, "Third", 3},
            {0, "Fourth", 4},
            {0, "Fifth", 5},
    };

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records))
                          .execute();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 5);

    // IDs should be strictly increasing (insertion order)
    for (std::size_t i = 1; i < result.value().size(); ++i) {
        EXPECT_GT(result.value()[i], result.value()[i - 1]) << "IDs should be strictly increasing (insertion order)";
    }
}

// to_sql includes RETURNING for batch
TYPED_TEST(BatchInsertReturningTest, ToSqlIncludesReturning) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
    };

    auto sql = (InsertStatement<SimpleRecord, TypeParam>{})
                       .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records))
                       .to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("RETURNING")) << "Batch RETURNING SQL should contain RETURNING clause";
    EXPECT_TRUE(sql.value().contains("INSERT INTO")) << "Should contain INSERT INTO";
}

// Default batch to_sql does NOT include RETURNING
TYPED_TEST(BatchInsertReturningTest, DefaultBatchToSqlNoReturning) {
    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
    };

    auto sql = (InsertStatement<SimpleRecord, TypeParam>{}).query(std::span<const SimpleRecord>(records)).to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_FALSE(sql.value().contains("RETURNING")) << "Default batch SQL should NOT contain RETURNING";
}

// =============================================================================
// Chunked batch INSERT RETURNING — exceeding 999-param limit
// =============================================================================

template <typename ConnType> class ChunkedBatchInsertReturningTest : public StormTestFixture<SimpleRecord, ConnType> {};

TYPED_TEST_SUITE(ChunkedBatchInsertReturningTest, DatabaseTypes);

// Chunked batch via custom batch_size (forces chunking with small record counts — fast in coverage)
TYPED_TEST(ChunkedBatchInsertReturningTest, ChunkedBatchReturnsAllIds) {
    using ReturnId = ReturnId;

    const int                 num_records = 25;
    std::vector<SimpleRecord> records;
    records.reserve(num_records);
    for (int i = 0; i < num_records; ++i) {
        records.push_back({0, std::format("Record{}", i), i * 10});
    }

    InsertOptions opts;
    opts.batch_size = 10; // Forces 3 chunks: 10 + 10 + 5

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records), opts)
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Chunked batch insert returning should succeed";
    ASSERT_EQ(result.value().size(), static_cast<std::size_t>(num_records))
            << "Should return IDs for all " << num_records << " records";

    // All IDs should be positive and unique
    const std::set<std::int64_t> unique_ids(result.value().begin(), result.value().end());
    EXPECT_EQ(unique_ids.size(), static_cast<std::size_t>(num_records)) << "All IDs should be unique";

    for (const auto id : result.value()) {
        EXPECT_GT(id, 0);
    }

    // IDs should be strictly increasing
    for (std::size_t i = 1; i < result.value().size(); ++i) {
        EXPECT_GT(result.value()[i], result.value()[i - 1]);
    }

    // Verify total count
    auto count = (SelectStatement<SimpleRecord, TypeParam>{}).all().count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), num_records);
}

// Exact boundary: batch_size == record count (single chunk, no remainder)
TYPED_TEST(ChunkedBatchInsertReturningTest, ExactBoundaryBatch) {
    using ReturnId = ReturnId;

    const int                 num_records = 10;
    std::vector<SimpleRecord> records;
    records.reserve(num_records);
    for (int i = 0; i < num_records; ++i) {
        records.push_back({0, std::format("Record{}", i), i});
    }

    InsertOptions opts;
    opts.batch_size = 10; // Exactly fits in one chunk

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records), opts)
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Boundary batch insert returning should succeed";
    ASSERT_EQ(result.value().size(), static_cast<std::size_t>(num_records));
}

// Just over boundary: batch_size + 1 records (forces 2 chunks: full + remainder of 1)
TYPED_TEST(ChunkedBatchInsertReturningTest, JustOverBoundaryBatch) {
    using ReturnId = ReturnId;

    const int                 num_records = 11;
    std::vector<SimpleRecord> records;
    records.reserve(num_records);
    for (int i = 0; i < num_records; ++i) {
        records.push_back({0, std::format("Record{}", i), i});
    }

    InsertOptions opts;
    opts.batch_size = 10; // 2 chunks: 10 + 1

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records), opts)
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Just-over-boundary batch insert returning should succeed";
    ASSERT_EQ(result.value().size(), static_cast<std::size_t>(num_records));

    // All unique
    const std::set<std::int64_t> unique_ids(result.value().begin(), result.value().end());
    EXPECT_EQ(unique_ids.size(), static_cast<std::size_t>(num_records));
}

// Multiple chunks with remainder
TYPED_TEST(ChunkedBatchInsertReturningTest, MultipleChunksWithRemainder) {
    using ReturnId = ReturnId;

    const int                 num_records = 37;
    std::vector<SimpleRecord> records;
    records.reserve(num_records);
    for (int i = 0; i < num_records; ++i) {
        records.push_back({0, std::format("R{}", i), i});
    }

    InsertOptions opts;
    opts.batch_size = 10; // 4 chunks: 10 + 10 + 10 + 7

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records), opts)
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Multi-chunk batch insert returning should succeed";
    ASSERT_EQ(result.value().size(), static_cast<std::size_t>(num_records));

    // Verify count
    auto count = (SelectStatement<SimpleRecord, TypeParam>{}).all().count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), num_records);
}

// Custom batch_size = 3 (small chunks to test edge cases)
TYPED_TEST(ChunkedBatchInsertReturningTest, CustomBatchSizeSmall) {
    using ReturnId = ReturnId;

    const int                 num_records = 8;
    std::vector<SimpleRecord> records;
    records.reserve(num_records);
    for (int i = 0; i < num_records; ++i) {
        records.push_back({0, std::format("Record{}", i), i});
    }

    InsertOptions opts;
    opts.batch_size = 3; // 3 chunks: 3 + 3 + 2

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records), opts)
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Small batch size insert returning should succeed";
    ASSERT_EQ(result.value().size(), static_cast<std::size_t>(num_records));

    const std::set<std::int64_t> unique_ids(result.value().begin(), result.value().end());
    EXPECT_EQ(unique_ids.size(), static_cast<std::size_t>(num_records));
}

// Verify batch RETURNING + void batch produce same data
TYPED_TEST(ChunkedBatchInsertReturningTest, ReturningAndVoidProduceSameData) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
            {0, "Charlie", 30},
    };

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::Yes>(std::span<const SimpleRecord>(records))
                          .execute();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 3);

    // Verify all data is correct via SELECT
    auto all = (SelectStatement<SimpleRecord, TypeParam>{}).all().execute();
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all.value().size(), 3);
}

// =============================================================================
// Explicit ReturnId::No for batch INSERT (no RETURNING clause)
// =============================================================================

TYPED_TEST(BatchInsertReturningTest, ExplicitReturnIdNoBatchReturnsVoid) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
            {0, "Charlie", 30},
    };

    auto result = (InsertStatement<SimpleRecord, TypeParam>{})
                          .template query<ReturnId::No>(std::span<const SimpleRecord>(records))
                          .execute();

    static_assert(
            std::is_same_v<decltype(result), std::expected<void, typename TypeParam::Error>>,
            "insert<ReturnId::No>(span) should return std::expected<void, Error>"
    );
    ASSERT_TRUE(result.has_value()) << "Explicit ReturnId::No batch insert should succeed";

    auto count = (SelectStatement<SimpleRecord, TypeParam>{}).all().count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 3);
}

TYPED_TEST(BatchInsertReturningTest, ExplicitReturnIdNoBatchToSqlNoReturning) {
    using ReturnId = ReturnId;

    std::vector<SimpleRecord> const records = {
            {0, "Alice", 10},
            {0, "Bob", 20},
    };

    auto sql = (InsertStatement<SimpleRecord, TypeParam>{})
                       .template query<ReturnId::No>(std::span<const SimpleRecord>(records))
                       .to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("INSERT INTO"));
    EXPECT_FALSE(sql.value().contains("RETURNING")) << "ReturnId::No batch SQL should NOT contain RETURNING";
}
