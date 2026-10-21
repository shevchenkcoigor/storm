// test_order_by.cpp - Comprehensive tests for ORDER BY functionality
#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness,performance-unnecessary-copy-initialization)

import storm_db_default_connection;
import storm_db_pool;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_where;
import std;

#include "../../shared/models/people_25.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
#include "test_select_runner.h"
using storm::db::DefaultConnection;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

template <typename ConnType> auto insert_test_data(const std::vector<Person>& data) -> void {
    auto result = (InsertStatement<Person, ConnType>{}).query(data).execute();
    ASSERT_TRUE(result.has_value()) << "Failed to insert test data";
}

template <typename ConnType> class OrderByTest : public StormTestFixture<Person, ConnType> {
  protected:
    // Helper: verify age and name at each position in result set.
    // entries is a vector of {expected_age, expected_name} pairs.
    static void
    verify_order(const auto& people, const std::vector<std::pair<int, std::string>>& entries) { // NOSONAR(S6024)
        auto it = people.begin();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            EXPECT_EQ(std::ranges::next(people.begin(), static_cast<std::ptrdiff_t>(i))->age, entries[i].first)
                    << "Wrong age at index " << i;
            EXPECT_EQ(std::ranges::next(people.begin(), static_cast<std::ptrdiff_t>(i))->name, entries[i].second)
                    << "Wrong name at index " << i;
        }
    }

    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end())
        )));
    }
};

TYPED_TEST_SUITE(OrderByTest, DatabaseTypes);

// ============================================================================
// Basic ORDER BY Tests (single-field asc/desc migrated to order_by_cases.yaml)
// ============================================================================

// SingleFieldDefaultAsc, SingleFieldExplicitAsc, SingleFieldDesc,
// StringFieldAsc, StringFieldDesc: migrated to order_by_cases.yaml → OrderByYamlTest.

// ============================================================================
// Multiple Fields ORDER BY Tests
// ============================================================================

TYPED_TEST(OrderByTest, MultipleFieldsAllAsc) {
    // Order by age ASC, then name ASC
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.age, fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 25);

    // Verify ordering: age ASC, then name ASC within same age
    // Age 22: Paul, Yara | Age 25: Alice, Grace, Karen | Age 27: Tina
    this->verify_order(people, {{22, "Paul"}, {22, "Yara"}, {25, "Alice"}, {25, "Grace"}, {25, "Karen"}, {27, "Tina"}});
}

TYPED_TEST(OrderByTest, MultipleFieldsMixedDirections) {
    // Order by age ASC, then name DESC
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.age, true, fields::Person.name, false>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 25);

    // Verify ordering: age ASC, then name DESC within same age
    // Age 22: Yara, Paul (DESC) | Age 25: Karen, Grace, Alice (DESC) | Age 27: Tina
    this->verify_order(people, {{22, "Yara"}, {22, "Paul"}, {25, "Karen"}, {25, "Grace"}, {25, "Alice"}, {27, "Tina"}});
}

TYPED_TEST(OrderByTest, MultipleFieldsAllDesc) {
    // Order by age DESC, then name DESC
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.age, false, fields::Person.name, false>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 25);

    // Verify ordering: age DESC, then name DESC
    // Age 48: Olivia | Age 45: Victor, Frank | Age 42: Leo | Age 40: Sam, Eve
    this->verify_order(people, {{48, "Olivia"}, {45, "Victor"}, {45, "Frank"}, {42, "Leo"}, {40, "Sam"}, {40, "Eve"}});
}

// ============================================================================
// Combined with WHERE Tests
// ============================================================================

// WithWhereClause: migrated to order_by_cases.yaml → OrderByYamlTest.

TYPED_TEST(OrderByTest, WhereWithMultipleOrderBy) {
    // Filter age >= 30 and order by age ASC, name DESC
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.age >= 30)
                          .template order_by<fields::Person.age, true, fields::Person.name, false>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 16);

    // Verify age filter
    for (const auto& person : people) {
        EXPECT_GE(person.age, 30);
    }
}

// ============================================================================
// Combined with LIMIT/OFFSET Tests
// ============================================================================

TYPED_TEST(OrderByTest, WithLimit) {
    // Get youngest 3 people
    auto result =
            (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.age>().limit(3).execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 3);

    // First two should be age 22, third should be age 25
    EXPECT_EQ(std::ranges::next(people.begin(), 0)->age, 22);
    EXPECT_EQ(std::ranges::next(people.begin(), 1)->age, 22);
    EXPECT_EQ(std::ranges::next(people.begin(), 2)->age, 25);
}

TYPED_TEST(OrderByTest, WithLimitAndOffset) {
    // Get people ranked 4-6 by name
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name>()
                          .limit(3)
                          .offset(3)
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 3);

    EXPECT_EQ(std::ranges::next(people.begin(), 0)->name, "Diana");
    EXPECT_EQ(std::ranges::next(people.begin(), 1)->name, "Eve");
    EXPECT_EQ(std::ranges::next(people.begin(), 2)->name, "Frank");
}

TYPED_TEST(OrderByTest, OrderByBeforeLimitOffset) {
    // Test that order_by works correctly with limit and offset
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.age, false>()
                          .limit(5)
                          .offset(2)
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 5);

    // Should get ages in descending order, skipping first 2 (48, 45)
    EXPECT_EQ(std::ranges::next(people.begin(), 0)->age, 45);
}

// ============================================================================
// Edge Cases
// ============================================================================

// EmptyResult: migrated to unified_cases.yaml (order_by_empty_result_where_no_match)

TYPED_TEST(OrderByTest, BooleanField) {
    // Order by boolean field
    auto result =
            (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.is_active, false>().execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 25);

    // true comes before false in DESC order (1 > 0)
    EXPECT_TRUE(std::ranges::next(people.begin(), 0)->is_active);
}

TYPED_TEST(OrderByTest, ChainedWithMultipleClauses) {
    // Complex query: WHERE + ORDER BY + LIMIT + OFFSET (23 match age >= 25)
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.age >= 25)
                          .template order_by<fields::Person.age, fields::Person.name>()
                          .limit(5)
                          .offset(1)
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto people = result.value();
    ASSERT_EQ(people.size(), 5);

    // Verify ordering maintained with all clauses
    auto prev_it = people.begin();
    auto it      = std::ranges::next(prev_it);
    for (; it != people.end(); ++prev_it, ++it) {
        if (prev_it->age == it->age) {
            EXPECT_LE(prev_it->name, it->name);
        } else {
            EXPECT_LE(prev_it->age, it->age);
        }
    }
}

// ============================================================================
// Nullable Field ORDER BY Tests (NULL ordering)
// ============================================================================

template <typename ConnType> class OrderByNullableTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        // Insert test data with mix of NULL and non-NULL values
        std::vector<Person> const test_data = {
                {.id = 1, .name = "Alice", .score = std::optional<int>(100)},
                {.id = 2, .name = "Bob", .score = std::nullopt},
                {.id = 3, .name = "Charlie", .score = std::optional<int>(50)},
                {.id = 4, .name = "David", .score = std::nullopt},
                {.id = 5, .name = "Eve", .score = std::optional<int>(75)},
                {.id = 6, .name = "Frank", .score = std::nullopt},
                {.id = 7, .name = "Grace", .score = std::optional<int>(25)},
                {.id = 8, .name = "Henry", .score = std::optional<int>(100)},
        };

        insert_test_data<ConnType>(test_data);
    }
};

TYPED_TEST_SUITE(OrderByNullableTest, DatabaseTypes);

TYPED_TEST(OrderByNullableTest, NullableFieldAsc) {
    // Order by nullable score ASC
    // In SQLite, NULL values sort first in ASC order by default
    auto result = (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);

    // SQLite default: NULL values come first in ASC order
    // First 3 should be NULL (Bob, David, Frank - sorted by insert order within NULLs)
    auto it = items.begin();
    EXPECT_FALSE(it->score.has_value()); // NULL
    ++it;
    EXPECT_FALSE(it->score.has_value()); // NULL
    ++it;
    EXPECT_FALSE(it->score.has_value()); // NULL

    // Then non-NULL values in ascending order: 25, 50, 75, 100, 100
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 25); // Grace
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 50); // Charlie
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 75); // Eve
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 100); // Alice or Henry
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 100); // Alice or Henry
}

TYPED_TEST(OrderByNullableTest, NullableFieldDesc) {
    // Order by nullable score DESC
    // In SQLite, NULL values sort last in DESC order by default
    auto result =
            (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.score, false>().execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);

    // Non-NULL values first in descending order: 100, 100, 75, 50, 25
    auto it = items.begin();
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 100);
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 100);
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 75);
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 50);
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 25);

    // Then NULL values at the end
    ++it;
    EXPECT_FALSE(it->score.has_value());
    ++it;
    EXPECT_FALSE(it->score.has_value());
    ++it;
    EXPECT_FALSE(it->score.has_value());
}

TYPED_TEST(OrderByNullableTest, NullableFieldWithSecondarySort) {
    // Order by score ASC, then name ASC (to have deterministic ordering within same scores)
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.score, true, fields::Person.name, true>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);

    // NULLs first, sorted by name: Bob, David, Frank
    auto it = items.begin();
    EXPECT_FALSE(it->score.has_value());
    EXPECT_EQ(it->name, "Bob");
    ++it;
    EXPECT_FALSE(it->score.has_value());
    EXPECT_EQ(it->name, "David");
    ++it;
    EXPECT_FALSE(it->score.has_value());
    EXPECT_EQ(it->name, "Frank");

    // Non-NULL values sorted by score, then name
    ++it;
    EXPECT_EQ(it->score.value(), 25);
    EXPECT_EQ(it->name, "Grace");
    ++it;
    EXPECT_EQ(it->score.value(), 50);
    EXPECT_EQ(it->name, "Charlie");
    ++it;
    EXPECT_EQ(it->score.value(), 75);
    EXPECT_EQ(it->name, "Eve");
    ++it;
    EXPECT_EQ(it->score.value(), 100);
    EXPECT_EQ(it->name, "Alice"); // Alice before Henry alphabetically
    ++it;
    EXPECT_EQ(it->score.value(), 100);
    EXPECT_EQ(it->name, "Henry");
}

TYPED_TEST(OrderByNullableTest, AllNullValues) {
    // Create a new table with only NULL values
    const auto& conn = DefaultConnection<TypeParam>::get();
    (void)(EraseStatement<Person, TypeParam>{}).query_all().execute();

    std::vector<Person> const all_nulls = {
            {.id = 1, .name = "First", .score = std::nullopt},
            {.id = 2, .name = "Second", .score = std::nullopt},
            {.id = 3, .name = "Third", .score = std::nullopt},
    };
    auto insert_result = (InsertStatement<Person, TypeParam>{}).query(all_nulls).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Order by nullable field when all values are NULL
    auto result = (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 3);

    // All should be NULL
    for (const auto& item : items) {
        EXPECT_FALSE(item.score.has_value());
    }
}

// ============================================================================
// BLOB Field ORDER BY Tests
// ============================================================================

template <typename ConnType> class OrderByBlobTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        // Insert test data with various BLOB values
        // SQLite compares BLOBs byte-by-byte in memcmp order
        insert_test_data<ConnType>({
                {.id = 1, .name = "C", .avatar = {0x03, 0x00}},
                {.id = 2, .name = "A", .avatar = {0x01, 0x00}},
                {.id = 3, .name = "B", .avatar = {0x02, 0x00}},
                {.id = 4, .name = "A2", .avatar = {0x01, 0x01}},
                {.id = 5, .name = "Empty", .avatar = {}},
                {.id = 6, .name = "A_short", .avatar = {0x01}},
        });
    }
};

TYPED_TEST_SUITE(OrderByBlobTest, DatabaseTypes);

TYPED_TEST(OrderByBlobTest, BlobFieldAsc) {
    // Order by BLOB field ASC
    // SQLite sorts BLOBs by memcmp order (byte-by-byte comparison)
    auto result = (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.avatar>().execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 6);

    // Expected order:
    // 1. Empty BLOB (sorts first - zero-length)
    // 2. {0x01} - shortest starting with 0x01
    // 3. {0x01, 0x00} - 0x01 prefix, then 0x00
    // 4. {0x01, 0x01} - 0x01 prefix, then 0x01
    // 5. {0x02, 0x00} - starts with 0x02
    // 6. {0x03, 0x00} - starts with 0x03

    auto it = items.begin();
    EXPECT_EQ(it->name, "Empty");
    EXPECT_TRUE(it->avatar.empty());
    ++it;
    EXPECT_EQ(it->name, "A_short");
    EXPECT_EQ(it->avatar, (std::vector<std::uint8_t>{0x01}));
    ++it;
    EXPECT_EQ(it->name, "A");
    EXPECT_EQ(it->avatar, (std::vector<std::uint8_t>{0x01, 0x00}));
    ++it;
    EXPECT_EQ(it->name, "A2");
    EXPECT_EQ(it->avatar, (std::vector<std::uint8_t>{0x01, 0x01}));
    ++it;
    EXPECT_EQ(it->name, "B");
    EXPECT_EQ(it->avatar, (std::vector<std::uint8_t>{0x02, 0x00}));
    ++it;
    EXPECT_EQ(it->name, "C");
    EXPECT_EQ(it->avatar, (std::vector<std::uint8_t>{0x03, 0x00}));
}

TYPED_TEST(OrderByBlobTest, BlobFieldDesc) {
    // Order by BLOB field DESC
    auto result =
            (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.avatar, false>().execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 6);

    // Expected order (reverse of ASC):
    // 1. {0x03, 0x00}
    // 2. {0x02, 0x00}
    // 3. {0x01, 0x01}
    // 4. {0x01, 0x00}
    // 5. {0x01}
    // 6. Empty

    auto it = items.begin();
    EXPECT_EQ(it->name, "C");
    ++it;
    EXPECT_EQ(it->name, "B");
    ++it;
    EXPECT_EQ(it->name, "A2");
    ++it;
    EXPECT_EQ(it->name, "A");
    ++it;
    EXPECT_EQ(it->name, "A_short");
    ++it;
    EXPECT_EQ(it->name, "Empty");
}

TYPED_TEST(OrderByBlobTest, BlobWithSecondarySort) {
    // Order by BLOB ASC, then name ASC
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.avatar, true, fields::Person.name, true>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 6);

    // Verify ordering is consistent
    EXPECT_EQ(items.begin()->name, "Empty"); // Empty BLOB first
}

TYPED_TEST(OrderByBlobTest, EmptyBlobsOnly) {
    // Test with only empty BLOBs
    const auto& conn = DefaultConnection<TypeParam>::get();
    (void)(EraseStatement<Person, TypeParam>{}).query_all().execute();

    std::vector<Person> const empty_blobs = {
            {.id = 1, .name = "First", .avatar = {}},
            {.id = 2, .name = "Second", .avatar = {}},
            {.id = 3, .name = "Third", .avatar = {}},
    };
    auto insert_result = (InsertStatement<Person, TypeParam>{}).query(empty_blobs).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = (SelectStatement<Person, TypeParam>{}).all().template order_by<fields::Person.avatar>().execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 3);

    // All empty BLOBs should be present
    for (const auto& item : items) {
        EXPECT_TRUE(item.avatar.empty());
    }
}

// ============================================================================
// ORDER BY with Empty Result Set (Additional scenarios)
// ============================================================================

TYPED_TEST(OrderByTest, EmptyResultWithMultipleOrderBy) {
    // Complex ORDER BY with no matching results
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.age > 100)
                          .template order_by<fields::Person.age, false, fields::Person.name, true>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& people = result.value();
    EXPECT_EQ(people.size(), 0);
}

// EmptyTableOrderBy: migrated to unified_cases.yaml (order_by_empty_table)

// NOLINTEND(misc-const-correctness,performance-unnecessary-copy-initialization)
