#include <gtest/gtest.h>
#include "test_db_helpers.h"

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
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// Test fixture for SELECT operations — templated on database backend
template <typename ConnType> class SelectTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(SelectTest, DatabaseTypes);

// SelectFromEmptyTable: migrated to unified_cases.yaml (select_empty_table)

// Test: SELECT single row
TYPED_TEST(SelectTest, SelectSingleRow) {
    // Insert one person
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    auto         insert_result = (InsertStatement<Person, TypeParam>{}).query(alice).execute();
    ASSERT_TRUE(insert_result.has_value()) << "INSERT failed: " << insert_result.error().message();

    std::int64_t const inserted_id = insert_result.value();

    // Select all rows
    auto result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT failed: " << result.error().message();

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 1) << "Expected exactly one row";

    // Verify row contents
    EXPECT_EQ(people.begin()->id, inserted_id);
    EXPECT_EQ(people.begin()->name, "Alice");
    EXPECT_EQ(people.begin()->age, 30);
}

// Test: SELECT multiple rows
TYPED_TEST(SelectTest, SelectMultipleRows) {
    // Insert multiple people
    std::vector<Person> people_to_insert = {{0, "Alice", 30}, {0, "Bob", 25}, {0, "Charlie", 35}};

    auto insert_result =
            (InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value())
            << "INSERT failed: code=" << insert_result.error().code() << " message=" << insert_result.error().message();

    // Select all rows
    auto result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT failed: " << result.error().message();

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 3) << "Expected exactly 3 rows";

    // Verify row contents (IDs are auto-generated, so just check they exist)
    auto it = people.begin();
    EXPECT_GT(it->id, 0);
    EXPECT_EQ(it->name, "Alice");
    EXPECT_EQ(it->age, 30);

    ++it;
    EXPECT_GT(it->id, 0);
    EXPECT_EQ(it->name, "Bob");
    EXPECT_EQ(it->age, 25);

    ++it;
    EXPECT_GT(it->id, 0);
    EXPECT_EQ(it->name, "Charlie");
    EXPECT_EQ(it->age, 35);
}

// Test: SELECT with different field types (int and std::string)
TYPED_TEST(SelectTest, SelectDifferentFieldTypes) {
    // Insert person with specific values
    Person const dave{.id = 0, .name = "Dave", .age = 40};
    auto         insert_result = (InsertStatement<Person, TypeParam>{}).query(dave).execute();
    ASSERT_TRUE(insert_result.has_value()) << "INSERT failed: " << insert_result.error().message();

    // Select and verify field types are correctly handled
    auto result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT failed: " << result.error().message();

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 1);

    // Verify int field
    EXPECT_EQ(people.begin()->age, 40);
    EXPECT_TRUE((std::is_same_v<decltype(people.begin()->age), int>));

    // Verify string field
    EXPECT_EQ(people.begin()->name, "Dave");
    EXPECT_TRUE((std::is_same_v<decltype(people.begin()->name), std::string>));
}

// Test: SELECT after INSERT and DELETE
TYPED_TEST(SelectTest, SelectAfterInsertAndDelete) {
    // Insert multiple people
    std::vector<Person> people_to_insert = {{0, "Alice", 30}, {0, "Bob", 25}, {0, "Charlie", 35}};

    auto insert_result =
            (InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "INSERT failed: " << insert_result.error().message();

    // Select to get the auto-generated IDs
    auto select_result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(select_result.has_value()) << "SELECT failed: " << select_result.error().message();

    // Find Bob's ID
    int bob_id = 0; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& person : select_result.value()) {
        if (person.name == "Bob") {
            bob_id = person.id;
            break;
        }
    }
    ASSERT_GT(bob_id, 0) << "Bob not found";

    // Delete Bob
    Person const bob_to_delete{.id = bob_id, .name = "Bob", .age = 25};
    auto         delete_result = (EraseStatement<Person, TypeParam>{}).query(bob_to_delete).execute();
    ASSERT_TRUE(delete_result.has_value()) << "DELETE failed: " << delete_result.error().message();

    // Select all rows again
    auto result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT failed: " << result.error().message();

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 2) << "Expected 2 rows after deletion";

    // Verify remaining rows
    auto it = people.begin();
    EXPECT_EQ(it->name, "Alice");
    ++it;
    EXPECT_EQ(it->name, "Charlie");
}

// Test: SELECT with large dataset
TYPED_TEST(SelectTest, SelectLargeDataset) {
    // Insert 100 people (use explicit IDs for batch insert)
    std::vector<Person> people_to_insert;
    for (int i = 1; i <= 100; ++i) {
        people_to_insert.emplace_back(i, std::format("SelectPerson{}", i), 20 + i);
    }

    auto insert_result =
            (InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "INSERT failed: " << insert_result.error().message();

    // Select all rows
    auto result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT failed: " << result.error().message();

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 100) << "Expected 100 rows";

    // Verify a few rows
    auto it = people.begin();
    EXPECT_EQ(it->name, "SelectPerson1");
    EXPECT_EQ(it->age, 21);

    auto it_50 = std::ranges::next(people.begin(), 50); // 51st element (index 50)
    EXPECT_EQ(it_50->name, "SelectPerson51");
    EXPECT_EQ(it_50->age, 71);

    auto it_99 = std::ranges::next(people.begin(), 99); // 100th element (index 99)
    EXPECT_EQ(it_99->name, "SelectPerson100");
    EXPECT_EQ(it_99->age, 120);
}

// Test: Multiple SELECT calls reuse cached statement
TYPED_TEST(SelectTest, MultipleSelectCallsUseCaching) {
    // Insert test data
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    auto         insert_result = (InsertStatement<Person, TypeParam>{}).query(alice).execute();
    ASSERT_TRUE(insert_result.has_value());

    // First SELECT
    auto result1 = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value().size(), 1);

    // Second SELECT (should use cached statement)
    auto result2 = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 1);

    // Third SELECT
    auto result3 = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3.value().size(), 1);
}

// Test: SELECT with empty strings
TYPED_TEST(SelectTest, SelectWithEmptyString) {
    // Insert person with empty name
    Person const anonymous{.id = 0, .name = "", .age = 25};
    auto         insert_result = (InsertStatement<Person, TypeParam>{}).query(anonymous).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Select and verify
    auto result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result.has_value());

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 1);
    EXPECT_EQ(people.begin()->name, "");
    EXPECT_EQ(people.begin()->age, 25);
}

// Test: SELECT preserves row order
TYPED_TEST(SelectTest, SelectPreservesRowOrder) {
    // Insert in specific order (use explicit IDs for batch insert)
    std::vector<Person> people_to_insert =
            {{1, "First", 1}, {2, "Second", 2}, {3, "Third", 3}, {4, "Fourth", 4}, {5, "Fifth", 5}};

    auto insert_result =
            (InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Select and verify order is preserved
    auto result = (SelectStatement<Person, TypeParam>{}).all().execute();
    ASSERT_TRUE(result.has_value());

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 5);

    std::size_t i = 0;
    for (auto it = people.begin(); it != people.end(); ++it, ++i) {
        EXPECT_EQ(it->age, static_cast<int>(i + 1)) << "Row order not preserved at index " << i;
    }
}
