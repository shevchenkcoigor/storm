#pragma once

// Shared test body for test_crud_sqlite.cpp / test_crud_pg.cpp — the two
// single-backend TUs of a compile-time TU split (see test_db_helpers.h,
// DatabaseTypesSqliteHalf/DatabaseTypesPgHalf). Splitting a 2-backend TU into
// two lets ninja compile them in parallel instead of serially instantiating
// both backends in one TU; keeping the body here (instead of duplicating it
// into both .cpp files) removes the risk of the two halves silently drifting.
//
// The includer must #define STORM_SPLIT_TYPES / STORM_SPLIT_TYPE_NAMES to one
// backend's ::testing::Types<> alias / NameGenerator before #include-ing this
// file, and #undef both afterward. Never include this file directly.
#if !defined(STORM_SPLIT_TYPES) || !defined(STORM_SPLIT_TYPE_NAMES)
#error "test_crud_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

#include <sqlite3.h>

import storm;
import std;

#include "../../shared/models/people_25.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "../../shared/models/simple_record.h"
#include "test_fixture.h"

#include "test_seed_helpers.h"
using storm::QuerySet;

// Common base fixture for Erase/Update tests — shared setup + helpers.
template <typename ConnType> class PersonCrudTestBase : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        std::vector<Person> const initial = {
            {.name = "Alice", .age = 30},
            {.name = "Bob", .age = 25},
            {.name = "Charlie", .age = 35},
        };
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(initial)));
    }

    static auto countPersons() -> int {
        QuerySet<Person, ConnType> qs;
        auto result = qs.count().execute();
        if (!result.has_value()) {
            return -1;
        }
        return static_cast<int>(result.value());
    }

    static auto personExists(int person_id) -> bool {
        const QuerySet<Person, ConnType> qs;
        auto result = qs.where(fields::Person.id == person_id).select().execute();
        return result.has_value() && !result.value().empty();
    }
};

// Test QuerySet.erase() functionality
template <typename ConnType> class QuerySetEraseTest : public PersonCrudTestBase<ConnType> {};
TYPED_TEST_SUITE(QuerySetEraseTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(QuerySetEraseTest, EraseMultiplePersonsSequentially) {
    QuerySet<Person, TypeParam> queryset;

    // Create person objects to erase
    Person const alice{.id = 1, .name = "Alice", .age = 30};
    Person const bob{.id = 2, .name = "Bob", .age = 25};

    // Verify initial state
    EXPECT_EQ(this->countPersons(), 3) << "Should have 3 persons initially";

    auto result1 = queryset.erase(alice).execute();
    ASSERT_TRUE(result1.has_value()) << "First erase should succeed";
    EXPECT_EQ(this->countPersons(), 2) << "Should have 2 persons after first removal";

    auto result2 = queryset.erase(bob).execute();
    ASSERT_TRUE(result2.has_value()) << "Second erase should succeed";
    EXPECT_EQ(this->countPersons(), 1) << "Should have 1 person after second removal";

    // Verify only Charlie remains
    EXPECT_FALSE(this->personExists(1)) << "Alice should be removed";
    EXPECT_FALSE(this->personExists(2)) << "Bob should be removed";
    EXPECT_TRUE(this->personExists(3)) << "Charlie should still exist";
}

// Test QuerySet.update() functionality
template <typename ConnType> class QuerySetUpdateTest : public PersonCrudTestBase<ConnType> {
  protected:
    // Helper function to get person by ID using the ORM
    static auto getPerson(int person_id) -> std::optional<Person> {
        const QuerySet<Person, ConnType> qs;
        auto result = qs.where(fields::Person.id == person_id).select().execute();
        if (!result.has_value() || result.value().empty()) {
            return std::nullopt;
        }
        return *result.value().begin();
    }
};

TYPED_TEST_SUITE(QuerySetUpdateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(QuerySetUpdateTest, UpdateCachedStatementReuse) {
    QuerySet<Person, TypeParam> queryset;

    // Perform multiple updates to verify statement caching works correctly
    for (int i = 0; i < 10; ++i) {
        Person const updated_alice{.id = 1, .name = storm::test::numbered("Alice V", i), .age = 30 + i};
        auto result = queryset.update(updated_alice).execute();
        ASSERT_TRUE(result.has_value()) << "Update iteration " << i << " should succeed";

        auto alice = this->getPerson(1);
        ASSERT_TRUE(alice.has_value());
        EXPECT_EQ(alice->name, storm::test::numbered("Alice V", i));
        EXPECT_EQ(alice->age, 30 + i);
    }

    // Verify final state
    auto final_alice = this->getPerson(1);
    ASSERT_TRUE(final_alice.has_value());
    EXPECT_EQ(final_alice->name, "Alice V9");
    EXPECT_EQ(final_alice->age, 39);
}

// =============================================================================
// Transaction Edge Cases (from test_coverage_gaps.cpp)
// =============================================================================

template <typename ConnType> class TransactionTest : public StormTestFixture<SimpleRecord, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        qs = std::make_unique<QuerySet<SimpleRecord, ConnType>>();
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<SimpleRecord, ConnType>::TearDown();
    }

    std::unique_ptr<QuerySet<SimpleRecord, ConnType>> qs;
};

TYPED_TEST_SUITE(TransactionTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(TransactionTest, MultiRowUpdateInTransaction) {
    std::vector<SimpleRecord> const people = {{0, "P1", 1}, {0, "P2", 2}, {0, "P3", 3}};

    auto insert_result = this->qs->insert(std::span<const SimpleRecord>(people)).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto select_result = this->qs->select().execute();
    ASSERT_TRUE(select_result.has_value());

    std::vector<SimpleRecord> updates;
    for (const auto &p : select_result.value()) {
        updates.push_back({p.id, p.name, p.value + 100});
    }

    auto update_result = this->qs->update(std::span<const SimpleRecord>(updates)).execute();
    ASSERT_TRUE(update_result.has_value());

    auto verify_result = this->qs->select().execute();
    ASSERT_TRUE(verify_result.has_value());
    std::map<std::string, int> values;
    for (const auto &p : verify_result.value()) {
        values.emplace(p.name, p.value);
    }
    EXPECT_EQ(values, (std::map<std::string, int>{{"P1", 101}, {"P2", 102}, {"P3", 103}}));
}

// =============================================================================
// Query Reset and Reuse Coverage (from test_coverage_gaps.cpp)
// =============================================================================

template <typename ConnType> class QueryResetTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        qs = std::make_unique<QuerySet<Person, ConnType>>();

        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
            std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end()))));
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<Person, ConnType>::TearDown();
    }

  public:
    std::unique_ptr<QuerySet<Person, ConnType>> qs;
};

TYPED_TEST_SUITE(QueryResetTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(QueryResetTest, ResetClearsAllState) {
    auto filtered = this->qs->where(fields::Person.age > 25);

    auto result1 = filtered.template order_by<fields::Person.name>().limit(2).offset(1).select().execute();
    ASSERT_TRUE(result1.has_value());
    // age > 25 by name: Bob, Charlie, Diana, ... — OFFSET 1 / LIMIT 2 leaves Charlie, Diana.
    std::vector<std::string> names;
    for (const auto &p : result1.value()) {
        names.push_back(p.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Charlie", "Diana"}));

    // reset() drops the WHERE: all 25 rows come back, not the 20 with age > 25.
    filtered.reset();
    auto after_reset = filtered.select().execute();
    ASSERT_TRUE(after_reset.has_value());
    EXPECT_EQ(after_reset.value().size(), 25);

    // Original qs is unaffected — no reset needed
    auto result2 = this->qs->select().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 25);
}

TYPED_TEST(QueryResetTest, ReuseSameQuerySetMultipleTimes) {
    auto result1 = this->qs->where(fields::Person.age > 35).select().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value().size(), 9);

    // Original qs is unaffected by where() — still returns all rows
    auto result2 = this->qs->select().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 25);

    // Can create a different filter without reset
    auto result3 = this->qs->where(fields::Person.age < 35).select().execute();
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3.value().size(), 14);
}

TYPED_TEST(QueryResetTest, ResetBetweenDifferentOperations) {
    auto count1 = this->qs->count().execute();
    ASSERT_TRUE(count1.has_value());
    EXPECT_EQ(count1.value(), 25);

    (*this->qs).reset();

    auto sum1 = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum1.has_value());
    EXPECT_EQ(sum1.value(), 829);

    (*this->qs).reset();

    auto avg1 = this->qs->template avg<fields::Person.age>().execute();
    ASSERT_TRUE(avg1.has_value());
    ASSERT_TRUE(avg1.value().has_value());
    EXPECT_NEAR(avg1.value().value(), 33.16, 0.01);

    (*this->qs).reset();

    auto min1 = this->qs->template min<fields::Person.age>().execute();
    ASSERT_TRUE(min1.has_value());
    ASSERT_TRUE(min1.value().has_value());
    EXPECT_EQ(min1.value().value(), 22);

    (*this->qs).reset();

    auto max1 = this->qs->template max<fields::Person.age>().execute();
    ASSERT_TRUE(max1.has_value());
    ASSERT_TRUE(max1.value().has_value());
    EXPECT_EQ(max1.value().value(), 48);
}
