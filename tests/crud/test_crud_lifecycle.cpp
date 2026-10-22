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
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateStatement;

template <typename ConnType> class QuerySetCrudLifecycleTest : public StormTestFixture<Person, ConnType> {
  public:
    static auto countPersons() -> int {
        auto result = (SelectStatement<Person, ConnType>{}).all().count().execute();
        return result.has_value() ? static_cast<int>(result.value()) : -1;
    }

    static auto selectAll() {
        return (SelectStatement<Person, ConnType>{}).all().execute();
    }

    static auto verifyFullPersonInserted(const Person& row) -> void {
        EXPECT_EQ(row.name, "FullPerson");
        EXPECT_EQ(row.age, 42);
        EXPECT_DOUBLE_EQ(row.salary, 99999.99);
        EXPECT_TRUE(row.is_active);
        EXPECT_EQ(row.department, "Engineering");
        ASSERT_TRUE(row.score.has_value());
        EXPECT_EQ(row.score.value(), 95);
        ASSERT_TRUE(row.nickname.has_value());
        EXPECT_EQ(row.nickname.value(), "fp");
        EXPECT_EQ(row.avatar, (std::vector<std::uint8_t>{0x01, 0x02, 0x03}));
    }

    static auto verifyFullPersonUpdated(const Person& row) -> void {
        EXPECT_EQ(row.age, 43);
        EXPECT_DOUBLE_EQ(row.salary, 12345.67);
        EXPECT_FALSE(row.is_active);
        EXPECT_EQ(row.department, "Marketing");
        EXPECT_FALSE(row.score.has_value());
        EXPECT_FALSE(row.nickname.has_value());
        EXPECT_EQ(row.avatar, (std::vector<std::uint8_t>{0xFF}));
    }
};

TYPED_TEST_SUITE(QuerySetCrudLifecycleTest, DatabaseTypes);

TYPED_TEST(QuerySetCrudLifecycleTest, FullLifecycle) {
    // 1. Verify empty state
    EXPECT_EQ(this->countPersons(), 0) << "Should start empty";
    auto empty = this->selectAll();
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty.value().empty()) << "Select should return empty result";

    // 2. Insert data
    Person const alice{.name = "Alice", .age = 30};
    Person const bob{.name = "Bob", .age = 25};
    Person const charlie{.name = "Charlie", .age = 35};

    auto id_alice = (InsertStatement<Person, TypeParam>{}).query(alice).execute();
    ASSERT_TRUE(id_alice.has_value()) << "Insert Alice should succeed";
    EXPECT_GT(id_alice.value(), 0);

    auto id_bob = (InsertStatement<Person, TypeParam>{}).query(bob).execute();
    ASSERT_TRUE(id_bob.has_value()) << "Insert Bob should succeed";

    auto id_charlie = (InsertStatement<Person, TypeParam>{}).query(charlie).execute();
    ASSERT_TRUE(id_charlie.has_value()) << "Insert Charlie should succeed";

    // 3. Verify data exists
    EXPECT_EQ(this->countPersons(), 3) << "Should have 3 persons after insert";
    auto all = this->selectAll();
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(static_cast<int>(all.value().size()), 3) << "Select should return 3 rows";

    // 4. Update data
    const Person updated_alice{.id = static_cast<int>(id_alice.value()), .name = "Alice Updated", .age = 31};
    auto         update_result = (UpdateStatement<Person, TypeParam>{}).query(updated_alice).execute();
    ASSERT_TRUE(update_result.has_value()) << "Update should succeed";

    auto alice_result = (SelectStatement<Person, TypeParam>{})
                                .where(fields::Person.id == static_cast<int>(id_alice.value()))
                                .execute();
    ASSERT_TRUE(alice_result.has_value());
    ASSERT_FALSE(alice_result.value().empty());
    EXPECT_EQ(alice_result.value().begin()->name, "Alice Updated") << "Name should be updated";
    EXPECT_EQ(alice_result.value().begin()->age, 31) << "Age should be updated";

    // 5. Delete all data
    auto del_alice =
            (EraseStatement<Person, TypeParam>{}).query(Person{.id = static_cast<int>(id_alice.value())}).execute();
    ASSERT_TRUE(del_alice.has_value()) << "Erase Alice should succeed";

    auto del_bob =
            (EraseStatement<Person, TypeParam>{}).query(Person{.id = static_cast<int>(id_bob.value())}).execute();
    ASSERT_TRUE(del_bob.has_value()) << "Erase Bob should succeed";

    auto del_charlie =
            (EraseStatement<Person, TypeParam>{}).query(Person{.id = static_cast<int>(id_charlie.value())}).execute();
    ASSERT_TRUE(del_charlie.has_value()) << "Erase Charlie should succeed";

    // 6. Verify empty state again
    EXPECT_EQ(this->countPersons(), 0) << "Should be empty after all erases";
    auto final_select = this->selectAll();
    ASSERT_TRUE(final_select.has_value());
    EXPECT_TRUE(final_select.value().empty()) << "Select should return empty result after all erases";
}

TYPED_TEST(QuerySetCrudLifecycleTest, BatchLifecycle) {
    // 1. Verify empty state
    EXPECT_EQ(this->countPersons(), 0) << "Should start empty";

    // 2. Batch insert
    std::vector<Person> persons;
    for (int i = 1; i <= 10; i++) {
        persons.push_back(Person{.name = std::format("Person{}", i), .age = 20 + i});
    }
    auto insert_result = (InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(persons)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Batch insert should succeed";

    // 3. Verify data exists
    EXPECT_EQ(this->countPersons(), 10) << "Should have 10 persons after batch insert";

    // 4. Batch update
    auto all = this->selectAll();
    ASSERT_TRUE(all.has_value());
    std::vector<Person> updated;
    for (auto person : all.value()) {
        person.age += 1;
        updated.push_back(person);
    }
    auto update_result = (UpdateStatement<Person, TypeParam>{}).query(std::span<const Person>(updated)).execute();
    ASSERT_TRUE(update_result.has_value()) << "Batch update should succeed";

    // 5. Batch erase
    auto erase_result = (EraseStatement<Person, TypeParam>{}).query(std::span<const Person>(updated)).execute();
    ASSERT_TRUE(erase_result.has_value()) << "Batch erase should succeed";

    // 6. Verify empty state again
    EXPECT_EQ(this->countPersons(), 0) << "Should be empty after batch erase";
}

// All supported field types: int, string, double, bool, optional<int>, optional<string>, blob
TYPED_TEST(QuerySetCrudLifecycleTest, AllFieldTypesLifecycle) {
    // 1. Insert with all fields populated
    Person const full{
            .name       = "FullPerson",
            .age        = 42,
            .salary     = 99999.99,
            .is_active  = true,
            .department = "Engineering",
            .score      = 95,
            .nickname   = "fp",
            .avatar     = {0x01, 0x02, 0x03},
    };
    auto id = (InsertStatement<Person, TypeParam>{}).query(full).execute();
    ASSERT_TRUE(id.has_value()) << "Insert with all fields should succeed";

    // 2. Verify all fields round-trip correctly
    auto rows =
            (SelectStatement<Person, TypeParam>{}).where(fields::Person.id == static_cast<int>(id.value())).execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    this->verifyFullPersonInserted(*rows.value().begin());

    // 3. Update all fields
    const Person updated{
            .id         = static_cast<int>(id.value()),
            .name       = "FullPerson",
            .age        = 43,
            .salary     = 12345.67,
            .is_active  = false,
            .department = "Marketing",
            .score      = std::nullopt,
            .nickname   = std::nullopt,
            .avatar     = {0xFF},
    };
    ASSERT_TRUE((UpdateStatement<Person, TypeParam>{}).query(updated).execute().has_value())
            << "Update with all fields should succeed";

    auto updated_rows =
            (SelectStatement<Person, TypeParam>{}).where(fields::Person.id == static_cast<int>(id.value())).execute();
    ASSERT_TRUE(updated_rows.has_value());
    ASSERT_EQ(updated_rows.value().size(), 1U);
    this->verifyFullPersonUpdated(*updated_rows.value().begin());

    // 4. Erase and verify gone
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{})
                        .query(Person{.id = static_cast<int>(id.value())})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->countPersons(), 0);
}

// Batch at the 999-param SQLite boundary: Person has 9 fields → floor(999/9) = 111 rows per chunk
TYPED_TEST(QuerySetCrudLifecycleTest, BoundaryBatchLifecycle) {
    // 1. Insert exactly 111 rows (fills one 999-param chunk exactly)
    std::vector<Person> persons;
    persons.reserve(111);
    for (int i = 1; i <= 111; i++) {
        persons.push_back(Person{.name = std::format("Person{}", i), .age = 20 + (i % 60)});
    }
    auto insert_result = (InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(persons)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Insert at boundary should succeed";
    EXPECT_EQ(this->countPersons(), 111);

    // 2. Insert 112 rows (one past boundary — triggers two chunks)
    std::vector<Person> over;
    over.reserve(112);
    for (int i = 112; i <= 223; i++) {
        over.push_back(Person{.name = std::format("Person{}", i), .age = 20 + (i % 60)});
    }
    auto insert_over = (InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(over)).execute();
    ASSERT_TRUE(insert_over.has_value()) << "Insert over boundary should succeed";
    EXPECT_EQ(this->countPersons(), 223);

    // 3. Batch erase all
    auto all = this->selectAll();
    ASSERT_TRUE(all.has_value());
    std::vector<Person> all_vec(all.value().begin(), all.value().end());
    auto erase_result = (EraseStatement<Person, TypeParam>{}).query(std::span<const Person>(all_vec)).execute();
    ASSERT_TRUE(erase_result.has_value()) << "Batch erase at boundary should succeed";
    EXPECT_EQ(this->countPersons(), 0);
}

// Insert, selectively update and erase via WHERE, verify unaffected rows unchanged
TYPED_TEST(QuerySetCrudLifecycleTest, FilteredLifecycle) {
    // 1. Insert mixed data
    std::vector<Person> persons = {
            {.name = "Alice", .age = 30, .is_active = true},
            {.name = "Bob", .age = 20, .is_active = false},
            {.name = "Charlie", .age = 35, .is_active = true},
            {.name = "Dave", .age = 18, .is_active = false},
    };
    ASSERT_TRUE((InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(persons)).execute().has_value());
    EXPECT_EQ(this->countPersons(), 4);

    // 2. Select only active persons
    // NOLINTNEXTLINE(readability-simplify-boolean-expr) -- `== true` is the WHERE-DSL comparison (generates `is_active = ?`), not a redundant bool op
    auto active = (SelectStatement<Person, TypeParam>{}).where(fields::Person.is_active == true).execute();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(static_cast<int>(active.value().size()), 2) << "Should have 2 active persons";

    // 3. Erase only persons aged < 21
    auto young = (SelectStatement<Person, TypeParam>{}).where(fields::Person.age < 21).execute();
    ASSERT_TRUE(young.has_value());
    std::vector<Person> young_vec(young.value().begin(), young.value().end());
    ASSERT_EQ(young_vec.size(), 2U) << "Bob and Dave should be under 21";
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query(std::span<const Person>(young_vec)).execute().has_value());
    EXPECT_EQ(this->countPersons(), 2) << "Alice and Charlie should remain";

    // 4. Verify only Alice and Charlie remain
    auto remaining = this->selectAll();
    ASSERT_TRUE(remaining.has_value());
    for (const auto& p : remaining.value()) {
        EXPECT_GE(p.age, 21) << std::format("{} should not have been erased", p.name);
    }

    // 5. Erase remaining and confirm empty
    std::vector<Person> rem_vec(remaining.value().begin(), remaining.value().end());
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query(std::span<const Person>(rem_vec)).execute().has_value());
    EXPECT_EQ(this->countPersons(), 0);
}
