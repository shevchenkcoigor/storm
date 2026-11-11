#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_join_data.h"

// Test fixture for VALUES operations — templated on database backend
template <typename ConnType> class ValuesTest : public StormTestFixture<Person, ConnType, Message> {};

TYPED_TEST_SUITE(ValuesTest, DatabaseTypes);

// ============================================================================
// Single-Field Projection Tests
// ============================================================================

// Test: values() on age field
TYPED_TEST(ValuesTest, SingleFieldAgeWithDuplicates) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 25}};

    auto insert_result = queryset.insert(std::span<const Person>(people)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT age — should return 4 rows (duplicates preserved)
    auto result = queryset.template values<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    std::vector<int> ages(result.value().begin(), result.value().end());
    std::ranges::sort(ages);
    EXPECT_EQ(ages, (std::vector<int>{25, 25, 30, 30})) << "Expected all 4 rows (duplicates preserved)";
}

// Test: values() on empty table
TYPED_TEST(ValuesTest, SingleFieldFromEmptyTable) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    EXPECT_TRUE(names.empty()) << "Expected empty result from empty table";
}

// Test: values() with single row
TYPED_TEST(ValuesTest, SingleFieldWithSingleRow) {
    QuerySet<Person, TypeParam> queryset;

    Person const alice{.id = 0, .name = "Alice", .age = 30};
    auto         insert_result = queryset.insert(alice).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 1);
    EXPECT_EQ(*names.begin(), "Alice");
}

// ============================================================================
// Multi-Field Projection Tests
// ============================================================================

// Test: values() on two fields (name, age) — duplicate ages preserved
TYPED_TEST(ValuesTest, TwoFieldsNameAndAge) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {1, "Alice", 30},
            {2, "Bob", 25},
            {3, "Carol", 30},
            {4, "Charlie", 30},
    };

    auto insert_result = queryset.insert(std::span<const Person>(people)).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.template values<fields::Person.name, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT values failed: " << result.error().message();

    const auto& pairs = result.value();
    // Should return ALL 4 rows (duplicate ages preserved, unlike DISTINCT)
    EXPECT_EQ(pairs.size(), 4) << "Expected all 4 rows including duplicate ages";
}

// Test: Return type verification for multi-field values
TYPED_TEST(ValuesTest, VerifyReturnTypes) {
    QuerySet<Person, TypeParam> queryset;

    std::ignore = queryset.insert(Person{.id = 0, .name = "Alice", .age = 30}).execute();

    // Verify return type for values on name field is hive of strings
    auto names_result = queryset.template values<fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(names_result.value()), plf::hive<std::string>&>);

    // Verify return type for values on age field is hive of integers
    auto ages_result = queryset.template values<fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(ages_result.value()), plf::hive<int>&>);

    // Verify return type for multi-field values is hive of tuples
    auto pairs_result = queryset.template values<fields::Person.name, fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(pairs_result.value()), plf::hive<std::tuple<std::string, int>>&>);

    // Reversed order
    auto reversed_result = queryset.template values<fields::Person.age, fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(reversed_result.value()), plf::hive<std::tuple<int, std::string>>&>);
}

// ============================================================================
// Duplicates Preserved (Key difference from DISTINCT)
// ============================================================================

// Test: values() preserves duplicates — core differentiator from distinct()
TYPED_TEST(ValuesTest, DuplicatesPreserved) {
    QuerySet<Person, TypeParam> queryset;

    // Insert 10 people with the same age (unique names due to UNIQUE constraint)
    std::vector<Person> people;
    for (int i = 1; i <= 10; ++i) {
        people.emplace_back(i, std::format("Person{}", i), 42);
    }

    auto insert_result = queryset.insert(std::span<const Person>(people)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // values() should return ALL 10 rows (duplicate ages preserved)
    auto values_result = queryset.template values<fields::Person.age>().execute();
    ASSERT_TRUE(values_result.has_value());
    EXPECT_EQ(values_result.value().size(), 10) << "values() should preserve all duplicates";

    // Verify all values are 42
    for (const auto& age : values_result.value()) {
        EXPECT_EQ(age, 42);
    }
}

// ============================================================================
// WHERE Integration Tests
// ============================================================================

// Test: values() with WHERE clause — single field
TYPED_TEST(ValuesTest, WithWhereSingleField) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Betty", 25},
            {0, "Bob", 30},
            {0, "Charlie", 20},
            {0, "David", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT name WHERE age > 22
    auto result = queryset.where(fields::Person.age > 22).template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value()) << "values with WHERE failed: " << result.error().message();

    const auto& names = result.value();
    // Alice (25), Betty (25), Bob (30), David (35) — 4 rows (Charlie filtered out)
    EXPECT_EQ(names.size(), 4);
}

// Test: values() with multiple WHERE clauses
TYPED_TEST(ValuesTest, WithMultipleWhereClauses) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 20},
            {0, "David", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Chain multiple WHERE clauses (AND)
    auto result = queryset.where(fields::Person.age > 22)
                          .where(fields::Person.age < 32)
                          .template values<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    // Alice (25) and Bob (30)
    EXPECT_EQ(names.size(), 2);
}

// Test: values() with complex WHERE clause
TYPED_TEST(ValuesTest, WithComplexWhere) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 20},
            {0, "David", 40},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT name WHERE age BETWEEN 25 AND 35
    auto result = queryset.where(fields::Person.age.between(25, 35)).template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    EXPECT_EQ(names.size(), 2); // Alice (25), Bob (30)
}

// Test: values() with WHERE — no results
TYPED_TEST(ValuesTest, WithWhereNoResults) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people        = {{0, "Alice", 25}, {0, "Bob", 30}};
    auto                insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.where(fields::Person.age > 100).template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 0);
}

// ============================================================================
// JOIN Integration Tests
// ============================================================================

// Test: values() with JOIN — single field
TYPED_TEST(ValuesTest, WithJoinSingleField) {
    storm::test::populate_join_test_data<TypeParam>();

    QuerySet<Message, TypeParam> msg_qs;

    // SELECT content FROM Message JOIN Person (values replaces field list)
    auto result = msg_qs.template join<fields::Message.sender>().template values<fields::Message.content>().execute();
    ASSERT_TRUE(result.has_value()) << "values with JOIN failed: " << result.error().message();

    const auto& contents = result.value();
    // Should return all 5 messages
    EXPECT_EQ(contents.size(), 5);
}

// Test: values() with JOIN and WHERE
TYPED_TEST(ValuesTest, WithJoinAndWhere) {
    storm::test::populate_join_test_data<TypeParam>();

    QuerySet<Message, TypeParam> msg_qs;

    auto result = msg_qs.template join<fields::Message.sender>()
                          .where(fields::Message.content.like("%o%")) // Hello, World, Goodbye
                          .template values<fields::Message.content>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "values with JOIN and WHERE failed: " << result.error().message();

    std::vector<std::string> contents(result.value().begin(), result.value().end());
    std::ranges::sort(contents);
    EXPECT_EQ(contents, (std::vector<std::string>{"Goodbye", "Hello", "World"})); // "Hi there"/"Test" have no 'o'
}

// ============================================================================
// ORDER BY Tests
// ============================================================================

// Test: values() with ORDER BY ASC
TYPED_TEST(ValuesTest, WithOrderByAsc) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Charlie", 30},
            {0, "Alice", 25},
            {0, "Bob", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.template order_by<fields::Person.name>().template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 3);

    auto it = names.begin();
    EXPECT_EQ(*it++, "Alice");
    EXPECT_EQ(*it++, "Bob");
    EXPECT_EQ(*it++, "Charlie");
}

// Test: values() with ORDER BY DESC
TYPED_TEST(ValuesTest, WithOrderByDesc) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Charlie", 30},
            {0, "Bob", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result =
            queryset.template order_by<fields::Person.name, false>().template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 3);

    auto it = names.begin();
    EXPECT_EQ(*it++, "Charlie");
    EXPECT_EQ(*it++, "Bob");
    EXPECT_EQ(*it++, "Alice");
}

// ============================================================================
// LIMIT / OFFSET Tests
// ============================================================================

// Test: values() with LIMIT
TYPED_TEST(ValuesTest, WithLimit) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 28},
            {0, "Charlie", 30},
            {0, "Dave", 35},
            {0, "Eve", 40},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result =
            queryset.template order_by<fields::Person.name>().limit(3).template values<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 3);

    auto it = names.begin();
    EXPECT_EQ(*it++, "Alice");
    EXPECT_EQ(*it++, "Bob");
    EXPECT_EQ(*it++, "Charlie");
}

// Test: values() with LIMIT and OFFSET
TYPED_TEST(ValuesTest, WithLimitAndOffset) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 28},
            {0, "Charlie", 30},
            {0, "Dave", 35},
            {0, "Eve", 40},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Skip Alice, Bob and return Charlie, Dave
    auto result = queryset.template order_by<fields::Person.name>()
                          .limit(2)
                          .offset(2)
                          .template values<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 2);

    auto it = names.begin();
    EXPECT_EQ(*it++, "Charlie");
    EXPECT_EQ(*it++, "Dave");
}

// Test: values() with WHERE, ORDER BY, LIMIT combined
TYPED_TEST(ValuesTest, WithWhereOrderByLimit) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 35},
            {0, "Charlie", 30},
            {0, "Dave", 40},
            {0, "Eve", 20}, // Filtered out by WHERE
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.where(fields::Person.age > 25)
                          .template order_by<fields::Person.name>()
                          .limit(2)
                          .template values<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 2);

    // Bob, Charlie, Dave pass WHERE; first 2 alphabetically: Bob, Charlie
    auto it = names.begin();
    EXPECT_EQ(*it++, "Bob");
    EXPECT_EQ(*it++, "Charlie");
}

// ============================================================================
// Optional Fields (NULL handling) Tests
// ============================================================================

// Test: values() on optional integer field with NULLs
TYPED_TEST(ValuesTest, OptionalIntFieldWithNulls) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = 30, .nickname = std::nullopt},
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = std::nullopt},
            {.id = 0, .name = "Eve", .score = 25, .nickname = "Evie"}, // Duplicate age
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Insert failed: " << insert_result.error().message();

    auto result = queryset.template values<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value()) << "values on optional field failed: " << result.error().message();

    const auto& ages = result.value();
    // Should return ALL 5 rows (not deduplicated)
    EXPECT_EQ(ages.size(), 5);

    // Count NULLs
    int null_count = 0;
    for (const auto& age : ages) {
        if (!age.has_value()) {
            null_count++;
        }
    }
    EXPECT_EQ(null_count, 2) << "Expected 2 NULL values (Bob and Dave)";
}

// Test: values() on optional string field with NULLs
TYPED_TEST(ValuesTest, OptionalStringFieldWithNulls) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = 30, .nickname = std::nullopt},
            {.id = 0, .name = "Charlie", .score = 35, .nickname = "Ali"},     // Duplicate nickname
            {.id = 0, .name = "Dave", .score = 40, .nickname = std::nullopt}, // Duplicate NULL
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.template values<fields::Person.nickname>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& nicknames = result.value();
    // Should return ALL 4 rows
    EXPECT_EQ(nicknames.size(), 4);

    int null_count = 0;
    int ali_count  = 0;
    for (const auto& nickname : nicknames) {
        if (!nickname.has_value()) {
            null_count++;
        } else if (nickname.value() == "Ali") {
            ali_count++;
        }
    }
    EXPECT_EQ(null_count, 2);
    EXPECT_EQ(ali_count, 2); // Duplicates preserved
}

// ============================================================================
// Statement Reuse and Stability Tests
// ============================================================================

// Test: values() executed multiple times returns consistent results
TYPED_TEST(ValuesTest, StatementReuseStability) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 35},
    };
    std::ignore = queryset.insert(people).execute();

    for (int i = 0; i < 5; ++i) {
        auto result = queryset.template values<fields::Person.name>().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed: " << result.error().message();
        EXPECT_EQ(result.value().size(), 3) << "Iteration " << i << " returned wrong count";
    }
}

// Test: Different WHERE expressions with values()
TYPED_TEST(ValuesTest, DifferentWhereExpressionsWithCaching) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 35},
            {0, "Dave", 40},
    };
    std::ignore = queryset.insert(people).execute();

    // Query 1: age > 25
    auto result1 = queryset.where(fields::Person.age > 25).template values<fields::Person.name>().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value().size(), 3); // Bob, Charlie, Dave

    queryset.reset();

    // Query 2: age > 35
    auto result2 = queryset.where(fields::Person.age > 35).template values<fields::Person.name>().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 1); // Dave only
}

// NOLINTEND(misc-const-correctness)
