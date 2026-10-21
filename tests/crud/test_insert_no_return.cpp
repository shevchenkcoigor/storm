#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include <sqlite3.h>

import storm;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
#include "test_seed_helpers.h"

// =====================================================
// InsertNoReturn tests — insert<ReturnId::No>
// =====================================================
template <typename ConnType> class InsertNoReturnTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(InsertNoReturnTest, DatabaseTypes);

TYPED_TEST(InsertNoReturnTest, SingleInsertNoReturnSucceeds) {
    using ReturnId = storm::orm::statements::ReturnId;
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30};
    auto         result = qs.template insert<ReturnId::No>(alice).execute();

    static_assert(
            std::is_same_v<decltype(result), std::expected<void, typename TypeParam::Error>>,
            "ReturnId::No should return std::expected<void, Error>"
    );
    ASSERT_TRUE(result.has_value()) << "insert<ReturnId::No> should succeed";

    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 1) << "Should have 1 row after insert";
}

TYPED_TEST(InsertNoReturnTest, SingleInsertYesReturnsId) {
    using ReturnId = storm::orm::statements::ReturnId;
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30};
    auto         result = qs.template insert<ReturnId::Yes>(alice).execute();

    static_assert(
            std::is_same_v<decltype(result), std::expected<std::int64_t, typename TypeParam::Error>>,
            "ReturnId::Yes should return std::expected<int64_t, Error>"
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result.value(), 0) << "Should return a valid ID";
}

TYPED_TEST(InsertNoReturnTest, DefaultInsertReturnsId) {
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30};
    auto         result = qs.insert(alice).execute();

    static_assert(
            std::is_same_v<decltype(result), std::expected<std::int64_t, typename TypeParam::Error>>,
            "Default insert should return std::expected<int64_t, Error>"
    );
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result.value(), 0);
}

TYPED_TEST(InsertNoReturnTest, InsertNoReturnDataIntegrity) {
    using ReturnId = storm::orm::statements::ReturnId;
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30, .salary = 75000.0, .is_active = true, .department = "Engineering"};
    auto         result = qs.template insert<ReturnId::No>(alice).execute();
    ASSERT_TRUE(result.has_value());

    auto rows = qs.select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1);

    const auto& row = *rows.value().begin();
    EXPECT_EQ(row.name, "Alice");
    EXPECT_EQ(row.age, 30);
    EXPECT_DOUBLE_EQ(row.salary, 75000.0);
    EXPECT_TRUE(row.is_active);
    EXPECT_EQ(row.department, "Engineering");
}

TYPED_TEST(InsertNoReturnTest, MultipleInsertNoReturn) {
    using ReturnId = storm::orm::statements::ReturnId;
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30};
    Person const bob{.name = "Bob", .age = 25};
    Person const charlie{.name = "Charlie", .age = 35};

    ASSERT_TRUE(qs.template insert<ReturnId::No>(alice).execute().has_value());
    ASSERT_TRUE(qs.template insert<ReturnId::No>(bob).execute().has_value());
    ASSERT_TRUE(qs.template insert<ReturnId::No>(charlie).execute().has_value());

    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 3);
}

TYPED_TEST(InsertNoReturnTest, InsertNoReturnToSql) {
    using ReturnId = storm::orm::statements::ReturnId;
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30};
    auto         sql = qs.template insert<ReturnId::No>(alice).to_sql();
    ASSERT_TRUE(sql.has_value());

    EXPECT_TRUE(sql.value().contains("INSERT INTO")) << "SQL should contain INSERT INTO";
    EXPECT_FALSE(sql.value().contains("RETURNING")) << "SQL should NOT contain RETURNING";
}

TYPED_TEST(InsertNoReturnTest, InsertYesToSqlHasReturning) {
    using ReturnId = storm::orm::statements::ReturnId;
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30};
    auto         sql = qs.template insert<ReturnId::Yes>(alice).to_sql();
    ASSERT_TRUE(sql.has_value());

    EXPECT_TRUE(sql.value().contains("RETURNING")) << "SQL should contain RETURNING";
}

TYPED_TEST(InsertNoReturnTest, MixedInsertModes) {
    using ReturnId = storm::orm::statements::ReturnId;
    storm::QuerySet<Person, TypeParam> qs;

    Person const alice{.name = "Alice", .age = 30};
    auto         id_result = qs.template insert<ReturnId::Yes>(alice).execute();
    ASSERT_TRUE(id_result.has_value());
    EXPECT_GT(id_result.value(), 0);

    Person const bob{.name = "Bob", .age = 25};
    auto         void_result = qs.template insert<ReturnId::No>(bob).execute();
    ASSERT_TRUE(void_result.has_value());

    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 2);
}

// =====================================================
// Issue #359 — batch_size = 0 must not hang / underflow
// =====================================================

namespace {
    std::vector<Person> three_people() {
        std::vector<Person> people;
        people.reserve(3);
        people.emplace_back(Person{.name = "Alice", .age = 30});
        people.emplace_back(Person{.name = "Bob", .age = 25});
        people.emplace_back(Person{.name = "Charlie", .age = 35});
        return people;
    }
} // namespace

// Bug 1: batch_size = 0 on a non-empty span used to step `offset += 0`
// forever. With a lower clamp it falls back to a single-batch insert.
TYPED_TEST(InsertNoReturnTest, BatchSizeZeroVoidDoesNotHang) {
    using storm::orm::statements::InsertOptions;
    storm::QuerySet<Person, TypeParam> qs;

    std::vector<Person> const people = three_people();
    auto result = qs.insert(std::span<const Person>(people), InsertOptions{.batch_size = 0}).execute();
    ASSERT_TRUE(result.has_value()) << "batch_size = 0 should complete, not hang";

    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 3) << "All rows should be inserted";
}

// Bug 1 (RETURNING path): same lower-clamp protection when R = ReturnId::Yes
// (InsertStatement::execute_batched<R>).
TYPED_TEST(InsertNoReturnTest, BatchSizeZeroReturningDoesNotHang) {
    using ReturnId = storm::orm::statements::ReturnId;
    using storm::orm::statements::InsertOptions;
    storm::QuerySet<Person, TypeParam> qs;

    std::vector<Person> const people = three_people();
    auto result = qs.template insert<ReturnId::Yes>(std::span<const Person>(people), InsertOptions{.batch_size = 0})
                          .execute();
    ASSERT_TRUE(result.has_value()) << "batch_size = 0 (RETURNING) should complete, not hang";
    EXPECT_EQ(result.value().size(), 3) << "All inserted IDs should be returned";
}

// Bug 2: BulkInsertQuery<R>::sql() on an empty span hit (count - 1) → SIZE_MAX in
// build_bulk_insert_body's reserve(). It must return the bare prefix instead.
TYPED_TEST(InsertNoReturnTest, EmptyBulkSqlNoUnderflow) {
    storm::QuerySet<Person, TypeParam> qs;

    std::vector<Person> empty;
    auto                bulk = qs.insert(std::span<const Person>(empty));
    std::string         sql  = bulk.sql();

    EXPECT_TRUE(sql.contains("INSERT INTO")) << "Empty bulk SQL should still be the INSERT prefix";
    EXPECT_FALSE(sql.contains("(?")) << "Empty bulk SQL should carry no value placeholders";
}
