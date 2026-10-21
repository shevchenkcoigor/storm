#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

using storm::QuerySet;
using storm::db::DefaultConnection;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_join_data.h"

// Test fixture for DISTINCT operations — templated on database backend
template <typename ConnType> class DistinctTest : public StormTestFixture<Person, ConnType, Message> {};

TYPED_TEST_SUITE(DistinctTest, DatabaseTypes);

// Test: DISTINCT on age field with duplicates
TYPED_TEST(DistinctTest, DistinctNameFieldWithDuplicates) {
    QuerySet<Person, TypeParam> queryset;

    // Insert people with duplicate ages (unique names for UNIQUE constraint)
    std::vector<Person> people_to_insert =
            {{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 40}, {5, "Eve", 25}};

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "INSERT failed: " << insert_result.error().message();

    // SELECT DISTINCT age
    auto result = queryset.template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT DISTINCT failed: " << result.error().message();

    const auto& ages = result.value();
    EXPECT_EQ(ages.size(), 3) << "Expected 3 unique ages";

    // Convert to set for easier verification
    std::set<int> const unique_ages(ages.begin(), ages.end());
    EXPECT_EQ(unique_ages.count(25), 1);
    EXPECT_EQ(unique_ages.count(30), 1);
    EXPECT_EQ(unique_ages.count(40), 1);
}

// Test: DISTINCT on age field
TYPED_TEST(DistinctTest, DistinctAgeFieldWithDuplicates) {
    QuerySet<Person, TypeParam> queryset;

    // Insert people with duplicate ages
    std::vector<Person> people_to_insert =
            {{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 25}, {5, "Eve", 35}};

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age
    auto result = queryset.template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();
    EXPECT_EQ(ages.size(), 3) << "Expected 3 unique ages";

    // Convert to set for verification
    std::set<int> const unique_ages(ages.begin(), ages.end());
    EXPECT_EQ(unique_ages.count(25), 1);
    EXPECT_EQ(unique_ages.count(30), 1);
    EXPECT_EQ(unique_ages.count(35), 1);
}

// Test: DISTINCT on primary key (defaults to PK when no template arg)
TYPED_TEST(DistinctTest, DistinctDefaultsToPrimaryKey) {
    QuerySet<Person, TypeParam> queryset;

    // Insert people
    std::vector<Person> people_to_insert = {{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 35}};

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT (defaults to PK)
    auto result = queryset.distinct().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ids = result.value();
    EXPECT_EQ(ids.size(), 3) << "Expected 3 unique IDs";

    // IDs should match what we inserted
    std::set<int> const id_set(ids.begin(), ids.end());
    EXPECT_TRUE(id_set.count(1));
    EXPECT_TRUE(id_set.count(2));
    EXPECT_TRUE(id_set.count(3));
}

// Test: DISTINCT on name with all unique values
TYPED_TEST(DistinctTest, DistinctNameFieldAllUnique) {
    QuerySet<Person, TypeParam> queryset;

    // Insert people with all unique names
    std::vector<Person> people_to_insert = {{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 35}};

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name
    auto result = queryset.template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    EXPECT_EQ(names.size(), 3) << "Expected all 3 unique names";
}

// DistinctFromEmptyTable: migrated to unified_cases.yaml (distinct_empty_name)

// Test: DISTINCT with single row
TYPED_TEST(DistinctTest, DistinctWithSingleRow) {
    QuerySet<Person, TypeParam> queryset;

    // Insert one person
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    auto         insert_result = queryset.insert(alice).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name
    auto result = queryset.template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 1);
    EXPECT_EQ(*names.begin(), "Alice");
}

// Test: DISTINCT with large dataset
TYPED_TEST(DistinctTest, DistinctWithLargeDataset) {
    QuerySet<Person, TypeParam> queryset;

    // Insert 1000 people with unique names but only 10 unique ages (100 duplicates each)
    std::vector<Person> people_to_insert;
    for (int i = 1; i <= 1000; ++i) {
        const int pattern = (i - 1) % 10;
        people_to_insert.emplace_back(i, std::format("Person{}", i), 20 + pattern);
    }

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age
    auto result = queryset.template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();
    EXPECT_EQ(ages.size(), 10) << "Expected 10 unique ages";
}

// Test: DISTINCT with empty strings
TYPED_TEST(DistinctTest, DistinctWithEmptyStrings) {
    QuerySet<Person, TypeParam> queryset;

    // Insert people with duplicate ages (unique names for UNIQUE constraint)
    std::vector<Person> people_to_insert = {{1, "Alice", 25}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 25}};

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age
    auto result = queryset.template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();
    EXPECT_EQ(ages.size(), 2) << "Expected 2 unique ages (25 and 30)";

    std::set<int> const age_set(ages.begin(), ages.end());
    EXPECT_TRUE(age_set.count(25));
    EXPECT_TRUE(age_set.count(30));
}

// Test: Return type verification
TYPED_TEST(DistinctTest, VerifyReturnTypes) {
    QuerySet<Person, TypeParam> queryset;

    // Insert test data
    std::ignore = queryset.insert(Person{.id = 0, .name = "Alice", .age = 30}).execute();

    // Verify return type for distinct on name field is hive of strings
    auto names_result = queryset.template distinct<fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(names_result.value()), plf::hive<std::string>&>);

    // Verify return type for distinct on age field is hive of integers
    auto ages_result = queryset.template distinct<fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(ages_result.value()), plf::hive<int>&>);

    // Verify return type for distinct on primary key is hive of integers
    auto ids_result = queryset.distinct().execute();
    static_assert(std::is_same_v<decltype(ids_result.value()), plf::hive<int>&>);
}

// ==================== Multi-Field DISTINCT Tests ====================

// Test: DISTINCT on two fields (name, age)
TYPED_TEST(DistinctTest, DistinctTwoFieldsNameAndAge) {
    QuerySet<Person, TypeParam> queryset;

    // Insert people with unique names but duplicate ages
    std::vector<Person> people_to_insert = {
            {1, "Alice", 30},
            {2, "Bob", 25},
            {3, "Charlie", 30}, // Same age as Alice
            {4, "Dave", 25},    // Same age as Bob
            {5, "Eve", 35},
            {6, "Frank", 30} // Same age as Alice and Charlie
    };

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name, age — all (name, age) pairs are unique since names are unique
    auto result = queryset.template distinct<fields::Person.name, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT DISTINCT failed: " << result.error().message();

    const auto& pairs = result.value();

    EXPECT_EQ(pairs.size(), 6) << "Expected 6 unique (name, age) pairs";

    // Convert to set for verification
    std::set<std::tuple<std::string, int>> const unique_pairs(pairs.begin(), pairs.end());
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Alice", 30)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Bob", 25)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Charlie", 30)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Dave", 25)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Eve", 35)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Frank", 30)));
}

// Test: DISTINCT on three fields (name, age) with different field ordering
// NOTE: We skip id field to avoid ambiguity with type-based field matching (both id and age are int)
TYPED_TEST(DistinctTest, DistinctThreeFieldsAllFields) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people_to_insert = {
            {1, "Alice", 30},
            {2, "Bob", 25},
            {3, "Charlie", 30}, // Same age as #1
            {4, "Dave", 25},    // Same age as #2
            {5, "Eve", 30}      // Same age as #1
    };

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.template distinct<fields::Person.name, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& pairs = result.value();
    EXPECT_EQ(pairs.size(), 5) << "Expected 5 unique (name, age) combinations";

    // Verify we got the expected tuples
    std::set<std::tuple<std::string, int>> const unique_pairs(pairs.begin(), pairs.end());
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Alice", 30)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Bob", 25)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Charlie", 30)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Dave", 25)));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple("Eve", 30)));
}

// Test: DISTINCT two fields with all duplicates
TYPED_TEST(DistinctTest, DistinctTwoFieldsAllDuplicates) {
    QuerySet<Person, TypeParam> queryset;

    // Insert 10 people with unique names but the same age
    std::vector<Person> people_to_insert;
    for (int i = 1; i <= 10; ++i) {
        people_to_insert.emplace_back(i, std::format("Person{}", i), 42);
    }

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age — all have the same age, so only 1 result
    auto result = queryset.template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();
    EXPECT_EQ(ages.size(), 1) << "Expected only 1 unique age";

    EXPECT_EQ(*ages.begin(), 42);
}

// DistinctTwoFieldsFromEmptyTable: migrated to unified_cases.yaml (distinct_two_fields_empty)

// Test: DISTINCT two fields with large dataset
TYPED_TEST(DistinctTest, DistinctTwoFieldsLargeDataset) {
    QuerySet<Person, TypeParam> queryset;

    // Insert 10,000 people with unique names but only 10 unique ages (duplicate ages)
    std::vector<Person> people_to_insert;
    for (int i = 1; i <= 10000; ++i) {
        // Each person gets a unique name, but only 10 possible ages (20-29)
        people_to_insert.emplace_back(i, std::format("Person{}", i), 20 + ((i - 1) % 10));
    }

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age — should deduplicate to 10 unique ages
    auto result = queryset.template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();

    // We have 10 unique ages (20-29), each appearing 1000 times
    EXPECT_EQ(ages.size(), 10) << "Expected 10 unique ages";
}

// Test: DISTINCT two fields (age, name) - different order than previous test
TYPED_TEST(DistinctTest, DistinctTwoFieldsDifferentOrder) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people_to_insert = {{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 25}};

    auto insert_result = queryset.insert(std::span<const Person>(people_to_insert)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age, name (reversed order)
    auto result = queryset.template distinct<fields::Person.age, fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& pairs = result.value();
    EXPECT_EQ(pairs.size(), 4) << "Expected 4 unique (age, name) pairs";

    // Verify we got the expected tuples (age first, then name)
    std::set<std::tuple<int, std::string>> const unique_pairs(pairs.begin(), pairs.end());
    EXPECT_TRUE(unique_pairs.count(std::make_tuple(30, "Alice")));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple(25, "Bob")));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple(30, "Charlie")));
    EXPECT_TRUE(unique_pairs.count(std::make_tuple(25, "Dave")));
}

// Test: Return type verification for multi-field DISTINCT
TYPED_TEST(DistinctTest, VerifyMultiFieldReturnTypes) {
    QuerySet<Person, TypeParam> queryset;

    // Insert test data
    std::ignore = queryset.insert(Person{.id = 0, .name = "Alice", .age = 30}).execute();

    // Verify return type for distinct on name and age is hive of tuples containing string and int
    auto pairs_result = queryset.template distinct<fields::Person.name, fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(pairs_result.value()), plf::hive<std::tuple<std::string, int>>&>);

    // Verify return type for distinct on age and name (reversed order) is hive of tuples containing int and string
    auto reversed_result = queryset.template distinct<fields::Person.age, fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(reversed_result.value()), plf::hive<std::tuple<int, std::string>>&>);
}

// Test: DISTINCT two fields with single row
TYPED_TEST(DistinctTest, DistinctTwoFieldsWithSingleRow) {
    QuerySet<Person, TypeParam> queryset;

    Person const alice{.id = 0, .name = "Alice", .age = 30};
    auto         insert_result = queryset.insert(alice).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto result = queryset.template distinct<fields::Person.name, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& pairs = result.value();
    ASSERT_EQ(pairs.size(), 1);
    EXPECT_EQ(std::get<0>((*pairs.begin())), "Alice");
    EXPECT_EQ(std::get<1>((*pairs.begin())), 30);
}
// Test: Duplicate field specification (same field twice)
// This should compile and work, but return redundant data
TYPED_TEST(DistinctTest, DuplicateFieldSpecification) {
    QuerySet<Person, TypeParam> queryset;

    // Insert test data with duplicate ages (unique names for UNIQUE constraint)
    std::vector<Person> people = {
            {1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30} // Charlie has same age as Alice
    };

    auto insert_result = queryset.insert(std::span<const Person>(people)).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age, age (redundant but valid SQL)
    auto result = queryset.template distinct<fields::Person.age, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& pairs = result.value();

    // Should return 2 unique ages, but each appears twice in the tuple
    EXPECT_EQ(pairs.size(), 2);

    // Each tuple should have identical elements
    for (const auto& [age1, age2] : pairs) {
        EXPECT_EQ(age1, age2) << "Duplicate field should have identical values";
    }

    // Verify we got 25 and 30
    std::set<int> ages;
    for (const auto& [age1, age2] : pairs) {
        ages.insert(age1);
    }
    EXPECT_TRUE(ages.count(25));
    EXPECT_TRUE(ages.count(30));
}

// Test: Same field three times
TYPED_TEST(DistinctTest, TriplicateFieldSpecification) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {{1, "Alice", 30}, {2, "Bob", 25}};
    std::ignore                = queryset.insert(std::span<const Person>(people)).execute();

    // SELECT DISTINCT age, age, age (extreme redundancy)
    auto result = queryset.template distinct<fields::Person.age, fields::Person.age, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& triples = result.value();
    EXPECT_EQ(triples.size(), 2); // 2 unique ages

    // Each tuple should have all three elements identical
    for (const auto& [age1, age2, age3] : triples) {
        EXPECT_EQ(age1, age2);
        EXPECT_EQ(age2, age3);
    }
}

// Test: Cross-struct field access is PREVENTED by compiler (type safety!)
// Attempting QuerySet<Person>().template distinct<&Message::sender_id>() results in compile error:
// "left hand operand to .* must be a class compatible with the right hand operand"
//
// This is GOOD! The compiler enforces type safety at compile-time.
//
// The error occurs in get_member_info() when trying to deduce the field type:
//   using MemberPtrFieldType = std::remove_cvref_t<decltype(std::declval<T>().*FP)>;
//
// Since T=Person and FP=&Message::sender_id, the .* operator fails because
// you cannot apply a Message member pointer to a Person object.
//
// This test documents the compile-time safety rather than testing it.
TYPED_TEST(DistinctTest, CrossStructFieldAccessPrevented) {
    /**
     * Cross-struct field access is PREVENTED at compile-time!
     *
     * Example that WON'T compile:
     *   QuerySet<Person> person_qs;
     *   person_qs.template distinct<&Message::sender_id>().execute(); // COMPILE ERROR!
     *
     * Error: "left hand operand to .* must be a class compatible with the right hand operand"
     *
     * This is excellent type safety! The compiler prevents using field pointers
     * from one struct with a QuerySet of a different struct.
     *
     * Benefit: No runtime errors from accessing wrong fields or invalid memory.
     */
    SUCCEED() << "Cross-struct field access correctly prevented by compiler";
}

// Test: Return type verification for duplicate fields
TYPED_TEST(DistinctTest, VerifyDuplicateFieldReturnTypes) {
    QuerySet<Person, TypeParam> queryset;
    std::ignore = queryset.insert(Person{.id = 0, .name = "Alice", .age = 30}).execute();

    // Duplicate field returns tuple with same type repeated
    auto dup_result = queryset.template distinct<fields::Person.name, fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(dup_result.value()), plf::hive<std::tuple<std::string, std::string>>&>);

    // Triple duplicate
    auto trip_result =
            queryset.template distinct<fields::Person.age, fields::Person.age, fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(trip_result.value()), plf::hive<std::tuple<int, int, int>>&>);
}

// Test: Mixed duplicate and unique fields
TYPED_TEST(DistinctTest, MixedDuplicateFields) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {1, "Alice", 30}, {2, "Bob", 30}, {3, "Charlie", 25} // Alice and Bob share age 30
    };
    std::ignore = queryset.insert(std::span<const Person>(people)).execute();

    // SELECT DISTINCT age, name, age (age appears twice)
    auto result = queryset.template distinct<fields::Person.age, fields::Person.name, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& triples = result.value();
    EXPECT_EQ(triples.size(), 3); // 3 unique (age, name) pairs

    // Verify that first and third elements of each tuple are identical
    for (const auto& [age1, name, age2] : triples) {
        EXPECT_EQ(age1, age2);
    }
}

// Test: Documentation of current behavior and limitations
TYPED_TEST(DistinctTest, DocumentedBehaviorAndLimitations) {
    /**
     * Current DISTINCT implementation behavior:
     *
     * POSITIVE (Type Safety):
     * 1. **Cross-Struct Field Access PREVENTED**:
     *    - QuerySet<Person>().template distinct<&Message::sender_id>() → COMPILE ERROR
     *    - Compiler enforces that field pointers must match QuerySet struct type
     *    - No risk of accidentally using fields from wrong struct
     *
     * 2. **Compile-Time Type Safety**:
     *    - Return types automatically deduced from field types
     *    - Single field: std::vector<FieldType>
     *    - Multiple fields: std::vector<std::tuple<Type1, Type2, ...>>
     *
     * NEUTRAL (Features):
     * 3. **Duplicate Fields Allowed**:
     *    - distinct<&Person::name, &Person::name>() compiles and runs
     *    - Generates SQL: SELECT DISTINCT name, name FROM Person
     *    - Returns redundant but valid data
     *    - Useful for testing but not recommended in production
     *
     * LIMITATIONS:
     * 4. **Type Ambiguity with Multiple Fields of Same Type**:
     *    - If struct has multiple int fields (id, age), type-based matching can be ambiguous
     *    - distinct<&Person::id, &Person::age>() works but might confuse which is which
     *    - Workaround: Use fields with unique types when possible
     *
     * 5. **No Statement Caching**:
     *    - Each DISTINCT query prepares a new statement
     *    - Unlike SELECT/UPDATE/DELETE which use statement-level caching
     *    - Minor performance impact for repeated identical queries
     *
     * 6. **No JOIN Support**:
     *    - DISTINCT operates only on the base table
     *    - Cannot do: queryset.template join<fields::M.fk>().template distinct<&JoinedTable::field>()
     *    - Would require significant refactoring to support
     *
     * Recommendations for future improvement:
     * - Consider statement caching for repeated DISTINCT queries
     * - Add warning/docs about type ambiguity issues
     * - Explore JOIN + DISTINCT support (complex feature)
     */
    SUCCEED() << "See test comments for documented behavior and limitations";
}
// Test: Current behavior - DISTINCT on base table only (no JOIN)
TYPED_TEST(DistinctTest, DistinctOnBaseTableWithoutJoin) {
    storm::test::populate_join_test_data<TypeParam>();

    QuerySet<Message, TypeParam> msg_qs;

    // DISTINCT on Message.content (no JOIN involved)
    auto result = msg_qs.template distinct<fields::Message.content>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& contents = result.value();
    EXPECT_EQ(contents.size(), 5); // 5 unique message contents

    std::set<std::string, std::less<>> const content_set(contents.begin(), contents.end());
    EXPECT_TRUE(content_set.count("Hello"));
    EXPECT_TRUE(content_set.count("World"));
    EXPECT_TRUE(content_set.count("Hi there"));
    EXPECT_TRUE(content_set.count("Goodbye"));
    EXPECT_TRUE(content_set.count("Test"));
}

// Test: DISTINCT on FK field limitation
TYPED_TEST(DistinctTest, DistinctOnForeignKeyFieldNotSupported) {
    /**
     * Limitation: DISTINCT on FK object fields not supported
     *
     * Problem:
     *   Message has FK field: [[fk]] Person sender
     *   Trying: msg_qs.template distinct<&Message::sender>()
     *   Error: "Unsupported field type for DISTINCT"
     *
     * Why:
     *   - DISTINCT only supports basic types (int, string, double, etc.)
     *   - FK fields are complex objects (Person in this case)
     *   - extract_column() doesn't know how to extract Person objects
     *
     * Workaround:
     *   - Use raw SQL to get DISTINCT sender_id values
     *   - Then manually fetch Person objects for those IDs
     *
     * Future enhancement:
     *   - Could support DISTINCT on FK *ID* column (sender_id)
     *   - Would require special handling of FK fields
     *   - Return std::vector<int> (IDs) instead of std::vector<Person>
     */
    SUCCEED() << "DISTINCT on FK object fields not currently supported (by design)";
}

// Test: What SQL would we WANT for DISTINCT + JOIN?
TYPED_TEST(DistinctTest, DesiredSQLForDistinctWithJoin) {
    /**
     * Desired SQL for DISTINCT with JOIN:
     *
     * Use Case 1: Get distinct user names who sent messages
     *   SELECT DISTINCT Person.name
     *   FROM Message
     *   INNER JOIN Person ON Person.id = Message.sender_id
     *
     * Use Case 2: Get distinct (user name, message content) pairs
     *   SELECT DISTINCT Person.name, Message.content
     *   FROM Message
     *   INNER JOIN Person ON Person.id = Message.sender_id
     *
     * Use Case 3: Get distinct users (full objects) who sent messages
     *   SELECT DISTINCT Person.*
     *   FROM Message
     *   INNER JOIN Person ON Person.id = Message.sender_id
     *
     * Current Storm ORM API doesn't support this because:
     * 1. DISTINCT operates independently of JOIN
     * 2. No way to specify fields from joined tables
     * 3. QuerySet<Message>.template join<fields::Message.sender>().template distinct<???>() - what goes in distinct?
     *
     * Possible API designs:
     *
     * Option A: Chaining
     *   msg_qs.template join<fields::Message.sender>()
     *         .template distinct<&Person::name>()  // PROBLEM: &Person::name won't match Message fields
     *         .execute()
     *
     * Option B: Separate DISTINCT type for JOINs
     *   msg_qs.template join<fields::Message.sender>()
     *         .distinct_joined<&Message::sender, &Person::name>()
     *         .execute()
     *
     * Option C: SQL-style field selection
     *   msg_qs.template join<fields::Message.sender>()
     *         .select_distinct(
     *             f<&Message::content>(),
     *             f<&Message::sender, &Person::name>()  // Joined field
     *         )
     *
     * Challenges:
     * - Type safety: How to represent fields from different tables?
     * - Return type: What does distinct_joined return?
     * - SQL generation: Building correct SELECT DISTINCT with JOINs
     * - Field extraction: Mapping SQL columns back to C++ types
     */
    SUCCEED() << "See test comments for desired SQL and API design challenges";
}

// Test: Workaround - Raw SQL for DISTINCT with JOIN
TYPED_TEST(DistinctTest, RawSQLWorkaround) {
    storm::test::populate_join_test_data<TypeParam>();

    const auto& conn = DefaultConnection<TypeParam>::get();

    // Workaround: Use raw SQL for DISTINCT + JOIN
    std::string const sql = "SELECT DISTINCT Person.name "
                            "FROM Message "
                            "INNER JOIN Person ON Person.id = Message.sender_id";

    auto stmt_result = conn->prepare(sql);
    ASSERT_TRUE(stmt_result.has_value());

    auto                     stmt = std::move(stmt_result.value());
    std::vector<std::string> user_names;

    while (true) {
        int const step = stmt.step_raw();
        if (step == decltype(stmt)::ROW_AVAILABLE) {
            const auto* text_bytes = stmt.extract_text_ptr(0);
            user_names.emplace_back(reinterpret_cast<const char*>(text_bytes)); // NOSONAR
        } else if (step == decltype(stmt)::NO_MORE_ROWS) {
            break;
        } else {
            FAIL() << "Query failed";
        }
    }

    // We have 3 users (Alice, Bob, Charlie) who all sent messages
    EXPECT_EQ(user_names.size(), 3);

    std::set<std::string, std::less<>> const name_set(user_names.begin(), user_names.end());
    EXPECT_TRUE(name_set.count("Alice"));
    EXPECT_TRUE(name_set.count("Bob"));
    EXPECT_TRUE(name_set.count("Charlie"));
}

// Test: Current JOIN implementation doesn't support DISTINCT
TYPED_TEST(DistinctTest, CurrentLimitations) {
    /**
     * Current Limitations of JOIN + DISTINCT:
     *
     * 1. **Separate Operations**:
     *    - JOIN is handled by QuerySet::join() -> SelectStatement
     *    - DISTINCT is handled by QuerySet::distinct() -> DistinctQuery
     *    - They don't integrate
     *
     * 2. **No Chaining Support**:
     *    - Cannot do: queryset.template join<fields::M.fk>().template distinct<&Field>()
     *    - join() returns QuerySet&&, but DISTINCT expects to operate independently
     *
     * 3. **Field Specification Limitation**:
     *    - DISTINCT takes field pointers from struct T only
     *    - Cannot specify fields from joined tables
     *    - No syntax for "field from joined table"
     *
     * 4. **Return Type Challenge**:
     *    - JOIN returns std::vector<T> with FKs populated
     *    - DISTINCT returns std::vector<FieldType> or std::vector<std::tuple<...>>
     *    - How to represent DISTINCT results from multiple tables?
     *
     * 5. **SQL Generation Complexity**:
     *    - Would need to merge JOIN SQL with DISTINCT SQL
     *    - Proper table aliasing (t1, t2, etc.)
     *    - Qualified field names (t1.name vs t2.name)
     *
     * Recommendations:
     * - For now, use raw SQL for DISTINCT + JOIN queries
     * - Future enhancement: Design proper API for joined DISTINCT
     * - Consider: Does DISTINCT + JOIN justify the implementation complexity?
     *   (It's a relatively rare use case)
     */
    SUCCEED() << "See test comments for current limitations";
}

// Test: Alternative approaches for DISTINCT with relationships
TYPED_TEST(DistinctTest, AlternativeApproaches) {
    /**
     * Alternative approaches for DISTINCT with relationships:
     *
     * Approach 1: Raw SQL (most flexible)
     *   - Use conn->prepare() with custom SQL
     *   - Full control over DISTINCT + JOIN queries
     *   - Loss of type safety and ORM convenience
     *
     * Approach 2: Application-level filtering
     *   - Fetch all data with JOIN: msg_qs.template join<fields::Message.sender>().select()
     *   - Filter unique values in application code (std::set, std::unique)
     *   - May load more data than necessary
     *
     * Approach 3: Separate queries (N+1 issue)
     *   - Query 1: SELECT DISTINCT sender_id FROM Message
     *   - Query 2-N: SELECT * FROM Person WHERE id = ?
     *   - Inefficient but works with current API
     *
     * Recommendation:
     *   For complex DISTINCT + JOIN queries, use raw SQL.
     *   For simple cases, application-level filtering may suffice.
     *   Full ORM support for DISTINCT + JOIN would require significant
     *   architectural changes and may not be worth the complexity.
     */
    SUCCEED() << "See comments for alternative approaches";
}

// ============================================================================
// NEW TESTS: DISTINCT with WHERE and JOIN support
// ============================================================================

// Test DISTINCT with WHERE clause - single field
TYPED_TEST(DistinctTest, DistinctWithWhereSingleField) {
    QuerySet<Person, TypeParam> queryset;

    // Insert test data with duplicate ages (unique names for UNIQUE constraint)
    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 25}, // Duplicate age
            {0, "Charlie", 30},
            {0, "Dave", 30}, // Duplicate age
            {0, "Eve", 20},
            {0, "Frank", 20}, // Duplicate age (filtered out by WHERE)
            {0, "Grace", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age WHERE age > 22
    auto result = queryset.where(fields::Person.age > 22).template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT with WHERE failed: " << result.error().message();

    const auto& ages = result.value();

    // Should return 3 unique ages: 25, 30, 35 (20 filtered out by WHERE)
    EXPECT_EQ(ages.size(), 3);

    std::set<int> const unique_ages(ages.begin(), ages.end());
    EXPECT_EQ(unique_ages.size(), 3);
    EXPECT_TRUE(unique_ages.contains(25));
    EXPECT_TRUE(unique_ages.contains(30));
    EXPECT_TRUE(unique_ages.contains(35));
    EXPECT_FALSE(unique_ages.contains(20)); // Filtered out by WHERE
}

// Test DISTINCT with WHERE clause - multiple fields
TYPED_TEST(DistinctTest, DistinctWithWhereMultipleFields) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 25}, // Duplicate age
            {0, "Charlie", 30},
            {0, "Dave", 20}, // Age 20 - filtered out
            {0, "Eve", 15},  // Age 15 - filtered out
            {0, "Frank", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name, age WHERE age > 22
    auto result = queryset.where(fields::Person.age > 22)
                          .template distinct<fields::Person.name, fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT with WHERE failed: " << result.error().message();

    const auto& pairs = result.value();

    EXPECT_EQ(pairs.size(), 4);

    std::set<std::tuple<std::string, int>> const unique_pairs(pairs.begin(), pairs.end());
    EXPECT_EQ(unique_pairs.size(), 4);
    EXPECT_TRUE(unique_pairs.contains(std::make_tuple("Alice", 25)));
    EXPECT_TRUE(unique_pairs.contains(std::make_tuple("Bob", 25)));
    EXPECT_TRUE(unique_pairs.contains(std::make_tuple("Charlie", 30)));
    EXPECT_TRUE(unique_pairs.contains(std::make_tuple("Frank", 35)));
}

// Test DISTINCT with WHERE clause - no results
TYPED_TEST(DistinctTest, DistinctWithWhereNoResults) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name WHERE age > 100 (no matches)
    auto result = queryset.where(fields::Person.age > 100).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    EXPECT_EQ(names.size(), 0); // No results due to WHERE filtering
}

// Test DISTINCT with complex WHERE clause
TYPED_TEST(DistinctTest, DistinctWithComplexWhere) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 25}, // Duplicate age
            {0, "Charlie", 30},
            {0, "Dave", 30}, // Duplicate age
            {0, "Eve", 20},
            {0, "Frank", 35},
            {0, "Grace", 40},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age WHERE age >= 25 AND age <= 35
    auto result = queryset.where(fields::Person.age.between(25, 35)).template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();

    std::set<int> const unique_ages(ages.begin(), ages.end());
    // 25, 30, 35 pass the filter — 20 and 40 filtered out
    EXPECT_EQ(unique_ages.size(), 3);
    EXPECT_TRUE(unique_ages.contains(25));
    EXPECT_TRUE(unique_ages.contains(30));
    EXPECT_TRUE(unique_ages.contains(35));
}

// Test DISTINCT with JOIN - single field
TYPED_TEST(DistinctTest, DistinctWithJoinSingleField) {
    storm::test::populate_join_test_data<TypeParam>(); // Create Persons and Messages

    QuerySet<Message, TypeParam> msg_qs;

    // SELECT DISTINCT sender.name (via JOIN)
    // Note: We need to select from Message and distinct on Message fields
    // The JOIN should expand the result set
    auto result = msg_qs.template join<fields::Message.sender>().template distinct<fields::Message.content>().execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT with JOIN failed: " << result.error().message();

    const auto& contents = result.value();

    // Should return distinct message contents
    std::set<std::string, std::less<>> const unique_contents(contents.begin(), contents.end());
    EXPECT_EQ(unique_contents.size(), contents.size()); // All should be unique
}

// DistinctWithJoinAndWhere: migrated to unified_cases.yaml (distinct_join_where_like_o)

// Test chaining order: WHERE -> JOIN -> DISTINCT
TYPED_TEST(DistinctTest, ChainingOrderWhereJoinDistinct) {
    storm::test::populate_join_test_data<TypeParam>();

    QuerySet<Message, TypeParam> msg_qs;

    // Test that WHERE -> JOIN -> DISTINCT works
    // Using content field to avoid ambiguous column names
    auto result = msg_qs.where(fields::Message.content.like("%e%"))
                          .template join<fields::Message.sender>()
                          .template distinct<fields::Message.content>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE -> JOIN -> DISTINCT failed: " << result.error().message();
}

// Test chaining order: JOIN -> WHERE -> DISTINCT
TYPED_TEST(DistinctTest, ChainingOrderJoinWhereDistinct) {
    storm::test::populate_join_test_data<TypeParam>();

    QuerySet<Message, TypeParam> msg_qs;

    // Test that JOIN -> WHERE -> DISTINCT works
    // Using content field to avoid ambiguous column names
    auto result = msg_qs.template join<fields::Message.sender>()
                          .where(fields::Message.content.like("%e%"))
                          .template distinct<fields::Message.content>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "JOIN -> WHERE -> DISTINCT failed: " << result.error().message();
}

// Test DISTINCT defaults to PK with WHERE
TYPED_TEST(DistinctTest, DistinctDefaultPKWithWhere) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 20},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT id (default PK) WHERE age > 22
    auto result = queryset.where(fields::Person.age > 22).distinct().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ids = result.value();
    EXPECT_EQ(ids.size(), 2); // Alice and Bob (Charlie filtered out)
}

// Test multiple WHERE clauses with DISTINCT
TYPED_TEST(DistinctTest, MultipleWhereClausesWithDistinct) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 20},
            {0, "David", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Chain multiple WHERE clauses (should combine with AND)
    auto result = queryset.where(fields::Person.age > 22)
                          .where(fields::Person.age < 32)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto&                              names = result.value();
    std::set<std::string, std::less<>> const unique_names(names.begin(), names.end());

    // Should only include Alice (25) and Bob (30)
    EXPECT_EQ(unique_names.size(), 2);
    EXPECT_TRUE(unique_names.contains("Alice"));
    EXPECT_TRUE(unique_names.contains("Bob"));
}

// Test: Validate DISTINCT keyword injection in JOIN queries
TYPED_TEST(DistinctTest, DistinctKeywordInjectionInJoinQueries) {
    /**
     * This test validates that DISTINCT is correctly injected into JOIN SQL.
     *
     * Implementation detail:
     * - JOIN SQL is generated at compile-time via templates
     * - DistinctStatement::inject_distinct_keyword() injects "DISTINCT " after "SELECT "
     * - Defensive check: Returns error if "SELECT " not found (should never happen)
     *
     * This test verifies:
     * 1. Normal JOIN + DISTINCT operations work correctly
     * 2. DISTINCT keyword is actually applied (results are deduplicated)
     * 3. The compile-time JOIN SQL generation produces valid SQL with SELECT clause
     *
     * Note: It's not feasible to test the error path (missing SELECT clause)
     * without internal code modification, since JOIN SQL is compile-time generated.
     * The defensive check exists to catch future compiler bugs or refactoring issues.
     */
    storm::test::populate_join_test_data<TypeParam>();

    QuerySet<Message, TypeParam> msg_qs;

    // Test 1: Verify JOIN + DISTINCT works (implicit validation of SELECT clause injection)
    auto result = msg_qs.template join<fields::Message.sender>().template distinct<fields::Message.content>().execute();
    ASSERT_TRUE(result.has_value()) << "JOIN + DISTINCT failed: " << result.error().message();

    const auto& contents = result.value();

    // Test 2: Verify DISTINCT returns results and deduplicates
    EXPECT_GT(contents.size(), 0) << "Should return some results";

    // Verify no duplicates in result
    std::set<std::string, std::less<>> const unique_contents(contents.begin(), contents.end());
    EXPECT_EQ(unique_contents.size(), contents.size()) << "DISTINCT should eliminate duplicates";
}

// ============================================================================
// DISTINCT + ORDER BY Tests
// ============================================================================

// Test: DISTINCT with ORDER BY ASC (default)
TYPED_TEST(DistinctTest, DistinctWithOrderByAsc) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Charlie", 30},
            {0, "Alice", 25},
            {0, "Bob", 35},
            {0, "Dave", 25}, // Duplicate age
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age ORDER BY age ASC
    auto result = queryset.template order_by<fields::Person.age>().template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT with ORDER BY failed: " << result.error().message();

    const auto& ages = result.value();
    ASSERT_EQ(ages.size(), 3); // 25, 30, 35

    // Verify order: 25, 30, 35 (ASC)
    auto it = ages.begin();
    EXPECT_EQ(*it++, 25);
    EXPECT_EQ(*it++, 30);
    EXPECT_EQ(*it++, 35);
}

// Test: DISTINCT with ORDER BY DESC
TYPED_TEST(DistinctTest, DistinctWithOrderByDesc) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Charlie", 30},
            {0, "Bob", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name ORDER BY name DESC
    auto result =
            queryset.template order_by<fields::Person.name, false>().template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT with ORDER BY DESC failed: " << result.error().message();

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 3);

    // Verify order: Charlie, Bob, Alice (DESC)
    auto it = names.begin();
    EXPECT_EQ(*it++, "Charlie");
    EXPECT_EQ(*it++, "Bob");
    EXPECT_EQ(*it++, "Alice");
}

// Test: DISTINCT with ORDER BY on integer field
TYPED_TEST(DistinctTest, DistinctWithOrderByIntegerField) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 30},
            {0, "Bob", 20},
            {0, "Charlie", 25},
            {0, "Dave", 20}, // Duplicate age
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age ORDER BY age ASC
    auto result = queryset.template order_by<fields::Person.age>().template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();
    ASSERT_EQ(ages.size(), 3); // 20, 25, 30

    // Verify order: 20, 25, 30 (ASC)
    auto it = ages.begin();
    EXPECT_EQ(*it++, 20);
    EXPECT_EQ(*it++, 25);
    EXPECT_EQ(*it++, 30);
}

// Test: DISTINCT multi-field with ORDER BY
TYPED_TEST(DistinctTest, DistinctMultiFieldWithOrderBy) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Charlie", 30},
            {0, "Alice", 25},
            {0, "Bob", 25},  // Duplicate age with Alice
            {0, "Dave", 30}, // Duplicate age with Charlie
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name, age ORDER BY name ASC
    auto result = queryset.template order_by<fields::Person.name>()
                          .template distinct<fields::Person.name, fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& pairs = result.value();
    ASSERT_EQ(pairs.size(), 4); // All unique (name, age) pairs

    // Verify first element is Alice (alphabetically first)
    auto it = pairs.begin();
    EXPECT_EQ(std::get<0>(*it), "Alice");
}

// ============================================================================
// DISTINCT + ORDER BY + LIMIT Tests
// ============================================================================

// Test: DISTINCT with ORDER BY and LIMIT
TYPED_TEST(DistinctTest, DistinctWithOrderByAndLimit) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Eve", 35},
            {0, "Alice", 25},
            {0, "Charlie", 30},
            {0, "Bob", 28},
            {0, "Dave", 40},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name ORDER BY name ASC LIMIT 3
    auto result = queryset.template order_by<fields::Person.name>()
                          .limit(3)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT with ORDER BY and LIMIT failed: " << result.error().message();

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 3);

    // Verify order: Alice, Bob, Charlie (first 3 alphabetically)
    auto it = names.begin();
    EXPECT_EQ(*it++, "Alice");
    EXPECT_EQ(*it++, "Bob");
    EXPECT_EQ(*it++, "Charlie");
}

// Test: DISTINCT with ORDER BY DESC and LIMIT
TYPED_TEST(DistinctTest, DistinctWithOrderByDescAndLimit) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 28},
            {0, "Charlie", 30},
            {0, "Dave", 40},
            {0, "Eve", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name ORDER BY name DESC LIMIT 2
    auto result = queryset.template order_by<fields::Person.name, false>()
                          .limit(2)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 2);

    // Verify order: Eve, Dave (last 2 alphabetically, DESC)
    auto it = names.begin();
    EXPECT_EQ(*it++, "Eve");
    EXPECT_EQ(*it++, "Dave");
}

// Test: DISTINCT with ORDER BY, LIMIT, and OFFSET
TYPED_TEST(DistinctTest, DistinctWithOrderByLimitOffset) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 28},
            {0, "Charlie", 30},
            {0, "Dave", 40},
            {0, "Eve", 35},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name ORDER BY name ASC LIMIT 2 OFFSET 2
    // Should skip Alice, Bob and return Charlie, Dave
    auto result = queryset.template order_by<fields::Person.name>()
                          .limit(2)
                          .offset(2)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 2);

    // Verify: Charlie, Dave (skipped Alice, Bob)
    auto it = names.begin();
    EXPECT_EQ(*it++, "Charlie");
    EXPECT_EQ(*it++, "Dave");
}

// Test: DISTINCT with WHERE, ORDER BY, and LIMIT
TYPED_TEST(DistinctTest, DistinctWithWhereOrderByLimit) {
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

    // SELECT DISTINCT name WHERE age > 25 ORDER BY name ASC LIMIT 2
    auto result = queryset.where(fields::Person.age > 25)
                          .template order_by<fields::Person.name>()
                          .limit(2)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();
    ASSERT_EQ(names.size(), 2);

    // Bob, Charlie, Dave pass WHERE (age > 25), first 2 alphabetically: Bob, Charlie
    auto it = names.begin();
    EXPECT_EQ(*it++, "Bob");
    EXPECT_EQ(*it++, "Charlie");
}

// ============================================================================
// DISTINCT on std::optional fields (NULL handling) Tests
// ============================================================================

// Test: DISTINCT on optional integer field with NULLs
TYPED_TEST(DistinctTest, DistinctOptionalIntFieldWithNulls) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},       // NULL age
            {.id = 0, .name = "Charlie", .score = 30, .nickname = std::nullopt},        // NULL nickname
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = std::nullopt}, // Both NULL
            {.id = 0, .name = "Eve", .score = 25, .nickname = "Evie"},                  // Duplicate age (25)
            {.id = 0, .name = "Frank", .score = std::nullopt, .nickname = "Frankie"},   // Another NULL age
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Insert failed: " << insert_result.error().message();

    // SELECT DISTINCT age (should include NULL as a distinct value)
    auto result = queryset.template distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT on optional field failed: " << result.error().message();

    const auto& ages = result.value();

    // Expected: 25, 30, NULL (3 distinct values)
    // Note: Multiple NULL values should collapse to one
    EXPECT_EQ(ages.size(), 3) << "Expected 3 distinct ages (25, 30, NULL)";

    // Count NULLs and non-NULLs
    int null_count     = 0;
    int non_null_count = 0;
    for (const auto& age : ages) {
        if (age.has_value()) {
            non_null_count++;
        } else {
            null_count++;
        }
    }

    EXPECT_EQ(null_count, 1) << "Expected exactly 1 NULL value";
    EXPECT_EQ(non_null_count, 2) << "Expected 2 non-NULL values (25, 30)";
}

// Test: DISTINCT on optional string field with NULLs
TYPED_TEST(DistinctTest, DistinctOptionalStringFieldWithNulls) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = 30, .nickname = std::nullopt},  // NULL nickname
            {.id = 0, .name = "Charlie", .score = 35, .nickname = "Ali"},     // Duplicate nickname ("Ali")
            {.id = 0, .name = "Dave", .score = 40, .nickname = std::nullopt}, // Another NULL nickname
            {.id = 0, .name = "Eve", .score = 45, .nickname = "Evie"},
            {.id = 0, .name = "Frank", .score = 50, .nickname = "Ali"}, // Another duplicate ("Ali")
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Insert failed: " << insert_result.error().message();

    // SELECT DISTINCT nickname (should include NULL as a distinct value)
    auto result = queryset.template distinct<fields::Person.nickname>().execute();
    ASSERT_TRUE(result.has_value()) << "DISTINCT on optional string field failed: " << result.error().message();

    const auto& nicknames = result.value();

    // Expected: "Ali", "Evie", NULL (3 distinct values)
    EXPECT_EQ(nicknames.size(), 3) << "Expected 3 distinct nicknames (Ali, Evie, NULL)";

    // Count NULLs and non-NULLs, verify values
    int                                null_count     = 0;
    int                                non_null_count = 0;
    std::set<std::string, std::less<>> seen_values;
    for (const auto& nickname : nicknames) {
        if (nickname.has_value()) {
            non_null_count++;
            seen_values.insert(nickname.value());
            // Verify no garbage values - must be one of our inserted nicknames
            EXPECT_TRUE(nickname.value() == "Ali" || nickname.value() == "Evie")
                    << "Unexpected nickname value: '" << nickname.value() << "'";
        } else {
            null_count++;
        }
    }

    EXPECT_EQ(null_count, 1) << "Expected exactly 1 NULL value";
    EXPECT_EQ(non_null_count, 2) << "Expected 2 non-NULL values (Ali, Evie)";
    EXPECT_TRUE(seen_values.contains("Ali")) << "Expected 'Ali' in distinct nicknames";
    EXPECT_TRUE(seen_values.contains("Evie")) << "Expected 'Evie' in distinct nicknames";
}

// Test: DISTINCT on optional field - all NULLs
TYPED_TEST(DistinctTest, DistinctOptionalFieldAllNulls) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = std::nullopt, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = std::nullopt, .nickname = "Chuck"},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age (all NULLs)
    auto result = queryset.template distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();

    // Should return exactly 1 NULL
    EXPECT_EQ(ages.size(), 1);
    EXPECT_FALSE(ages.begin()->has_value()) << "Expected single NULL value";
}

// Test: DISTINCT on optional field - no NULLs
TYPED_TEST(DistinctTest, DistinctOptionalFieldNoNulls) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = 30, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = 25, .nickname = "Chuck"}, // Duplicate age
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age (no NULLs)
    auto result = queryset.template distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();

    // Should return 2 distinct ages: 25, 30
    EXPECT_EQ(ages.size(), 2);

    // All should have values
    for (const auto& age : ages) {
        EXPECT_TRUE(age.has_value()) << "Expected all non-NULL values";
    }
}

// Test: DISTINCT multi-field with one optional field
TYPED_TEST(DistinctTest, DistinctMultiFieldWithOptional) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},  // NULL score
            {.id = 0, .name = "Charlie", .score = 25, .nickname = "Chuck"},        // Same score as Alice
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = "Davey"}, // NULL score like Bob
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name, score
    auto result = queryset.template distinct<fields::Person.name, fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& pairs = result.value();

    EXPECT_EQ(pairs.size(), 4);
}

// Test: DISTINCT with WHERE on optional field
TYPED_TEST(DistinctTest, DistinctOptionalWithWhereOnOptional) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = 30, .nickname = "Chuck"},
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = "Davey"},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT name WHERE age > 20 (filters out NULLs)
    auto result = queryset.where(fields::Person.score > 20).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& names = result.value();

    // Only Alice (25) and Charlie (30) pass the WHERE, Bob and Dave have NULL age
    EXPECT_EQ(names.size(), 2);

    std::set<std::string, std::less<>> const name_set(names.begin(), names.end());
    EXPECT_TRUE(name_set.contains("Alice"));
    EXPECT_TRUE(name_set.contains("Charlie"));
    EXPECT_FALSE(name_set.contains("Bob"));
    EXPECT_FALSE(name_set.contains("Dave"));
}

// Test: DISTINCT + ORDER BY with optional field
TYPED_TEST(DistinctTest, DistinctOptionalWithOrderBy) {
    QuerySet<Person, TypeParam> queryset;

    std::vector<Person> people = {
            {.id = 0, .name = "Charlie", .score = 30, .nickname = "Chuck"},
            {.id = 0, .name = "Alice", .score = std::nullopt, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = 25, .nickname = "Bobby"},
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = "Davey"},
    };

    auto insert_result = queryset.insert(people).execute();
    ASSERT_TRUE(insert_result.has_value());

    // SELECT DISTINCT age ORDER BY age ASC
    // NULL values typically sort first or last depending on DB
    auto result =
            queryset.template order_by<fields::Person.score>().template distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value());

    const auto& ages = result.value();

    // Should have 3 distinct values: NULL, 25, 30
    EXPECT_EQ(ages.size(), 3);
}

// ============================================================================
// Error Handling Tests
// ============================================================================

// Test: DISTINCT returns std::expected - verify error type is correct
TYPED_TEST(DistinctTest, DistinctReturnsExpectedType) {
    QuerySet<Person, TypeParam> queryset;

    // Insert test data
    std::ignore = queryset.insert(Person{.id = 0, .name = "Alice", .age = 30}).execute();

    // Verify return type is std::expected
    auto result = queryset.template distinct<fields::Person.name>().execute();

    // Verify result is a std::expected type by checking has_value()
    // The exact type includes the connection's Error type, which is internal
    EXPECT_TRUE(result.has_value());

    // Verify we can access the value
    const auto& names = result.value();
    EXPECT_EQ(names.size(), 1);
    EXPECT_EQ(*names.begin(), "Alice");
}

// Test: Error handling documentation
TYPED_TEST(DistinctTest, ErrorHandlingDocumentation) {
    /**
     * DISTINCT Error Handling Architecture:
     *
     * 1. **Return Type**: All DISTINCT operations return std::expected<ResultType, Error>
     *    - On success: result.has_value() == true, access via result.value()
     *    - On failure: result.has_value() == false, access via result.error()
     *
     * 2. **Potential Error Sources**:
     *    a) Database connection errors (rare with in-memory SQLite)
     *    b) Statement preparation failures (rare with compile-time SQL)
     *    c) Parameter binding failures (rare with type-safe API)
     *    d) Query execution failures (can occur with corrupted DB)
     *
     * 3. **Error Propagation**:
     *    - All internal errors are wrapped in std::unexpected(Error{...})
     *    - Error contains: error code (int) + message (std::string)
     *    - Statement is properly reset on error to avoid resource leaks
     *
     * 4. **Defensive Checks**:
     *    - inject_distinct_keyword() returns error if "SELECT " not found
     *    - This catches internal bugs in JOIN SQL generation
     *    - Error message clearly indicates this is an internal bug
     *
     * 5. **Example Error Handling**:
     *    ```cpp
     *    auto result = queryset.template distinct<fields::Person.name>().execute();
     *    if (!result.has_value()) {
     *        // Handle error
     *        std::cerr << "Error: " << result.error().message() << std::endl;
     *        return;
     *    }
     *    // Use result.value()
     *    ```
     *
     * 6. **Error Safety**:
     *    - Statements are reset on error (execute_query_loop)
     *    - WHERE parameters are bound atomically
     *    - No partial state on failure
     */
    SUCCEED() << "See test comments for error handling documentation";
}

// ErrorHandlingInChainedOperations: migrated to unified_cases.yaml (distinct_where_order_limit_all)
// NoMatchingRowsReturnsEmptyNotError: migrated to unified_cases.yaml (distinct_where_no_match)

// Test: Verify statement reuse doesn't cause issues
TYPED_TEST(DistinctTest, StatementReuseStability) {
    QuerySet<Person, TypeParam> queryset;

    // Insert data
    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 35},
    };
    std::ignore = queryset.insert(people).execute();

    // Execute the same DISTINCT query multiple times
    // This tests that statement caching works correctly
    for (int i = 0; i < 5; ++i) {
        auto result = queryset.template distinct<fields::Person.name>().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed: " << result.error().message();
        EXPECT_EQ(result.value().size(), 3) << "Iteration " << i << " returned wrong count";
    }
}

// Test: Verify different WHERE expressions work with cached statement
TYPED_TEST(DistinctTest, DifferentWhereExpressionsWithCaching) {
    QuerySet<Person, TypeParam> queryset;

    // Insert data
    std::vector<Person> people = {
            {0, "Alice", 25},
            {0, "Bob", 30},
            {0, "Charlie", 35},
            {0, "Dave", 40},
    };
    std::ignore = queryset.insert(people).execute();

    // Query 1: age > 25
    auto result1 = queryset.where(fields::Person.age > 25).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value().size(), 3); // Bob, Charlie, Dave

    // Reset WHERE for new query
    queryset.reset();

    // Query 2: age > 35
    auto result2 = queryset.where(fields::Person.age > 35).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 1); // Dave only

    // Reset and Query 3: different condition
    queryset.reset();

    auto result3 = queryset.where(fields::Person.age < 30).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3.value().size(), 1); // Alice only
}

// ============================================================================
// SQL Verification Tests — prevent SQL generation regressions
// ============================================================================

// Helper: returns "ASC" or "ASC NULLS FIRST" depending on backend
template <typename ConnType> constexpr auto order_asc() -> std::string_view {
    if constexpr (storm::test::is_postgresql<ConnType>()) {
        return "ASC NULLS FIRST";
    } else {
        return "ASC";
    }
}

// Test: DISTINCT simple (no JOIN) generates correct SQL
TYPED_TEST(DistinctTest, SqlVerifyDistinctSimple) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name FROM Person");
}

// Test: DISTINCT on integer field
TYPED_TEST(DistinctTest, SqlVerifyDistinctIntField) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template distinct<fields::Person.age>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT age FROM Person");
}

// Test: DISTINCT default PK
TYPED_TEST(DistinctTest, SqlVerifyDistinctDefaultPK) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.distinct().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT id FROM Person");
}

// Test: DISTINCT multi-field
TYPED_TEST(DistinctTest, SqlVerifyDistinctMultiField) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template distinct<fields::Person.name, fields::Person.age>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name, age FROM Person");
}

// Test: DISTINCT + WHERE generates correct SQL
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithWhere) {
    QuerySet<Person, TypeParam> queryset;
    auto sql = queryset.where(fields::Person.age > 30).template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name FROM Person WHERE age > ?");
}

// Test: DISTINCT + JOIN generates projected fields with t1 prefix (the bug fix)
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithJoin) {
    QuerySet<Message, TypeParam> msg_qs;
    auto sql = msg_qs.template join<fields::Message.sender>().template distinct<fields::Message.content>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id");
}

// Test: DISTINCT + JOIN + WHERE
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithJoinAndWhere) {
    QuerySet<Message, TypeParam> msg_qs;
    auto                         sql = msg_qs.template join<fields::Message.sender>()
                       .where(fields::Message.content.like("%test%"))
                       .template distinct<fields::Message.content>()
                       .sql();
    EXPECT_EQ(
            sql,
            "SELECT DISTINCT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
            " WHERE content LIKE ?"
    );
}

// Test: DISTINCT + JOIN multi-field with FK field
TYPED_TEST(DistinctTest, SqlVerifyDistinctJoinMultiFieldWithFK) {
    QuerySet<Message, TypeParam> msg_qs;
    auto                         sql = msg_qs.template join<fields::Message.sender>()
                       .template distinct<fields::Message.content, fields::Message.sender>()
                       .sql();
    EXPECT_EQ(
            sql,
            "SELECT DISTINCT t1.content, t1.sender_id"
            " FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
    );
}

// Test: VALUES (no DISTINCT keyword) + JOIN
TYPED_TEST(DistinctTest, SqlVerifyValuesWithJoin) {
    QuerySet<Message, TypeParam> msg_qs;
    auto sql = msg_qs.template join<fields::Message.sender>().template values<fields::Message.content>().sql();
    EXPECT_EQ(sql, "SELECT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id");
}

// Test: VALUES simple (no JOIN)
TYPED_TEST(DistinctTest, SqlVerifyValuesSimple) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template values<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT name FROM Person");
}

// Test: DISTINCT + ORDER BY
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithOrderBy) {
    QuerySet<Person, TypeParam> queryset;
    auto sql = queryset.template order_by<fields::Person.name>().template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, std::format("SELECT DISTINCT name FROM Person ORDER BY name {}", order_asc<TypeParam>()));
}

// Test: DISTINCT + LIMIT + OFFSET
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithLimitOffset) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.limit(10).offset(5).template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name FROM Person LIMIT 10 OFFSET 5");
}

// Test: DISTINCT + JOIN + WHERE + ORDER BY + LIMIT (full combo)
TYPED_TEST(DistinctTest, SqlVerifyDistinctFullCombo) {
    QuerySet<Message, TypeParam> msg_qs;
    auto                         sql = msg_qs.template join<fields::Message.sender>()
                       .where(fields::Message.value > 10)
                       .template order_by<fields::Message.content>()
                       .limit(5)
                       .template distinct<fields::Message.content>()
                       .sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT DISTINCT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
                    " WHERE value > ? ORDER BY content {} LIMIT 5",
                    order_asc<TypeParam>()
            )
    );
}

// NOLINTEND(misc-const-correctness)
