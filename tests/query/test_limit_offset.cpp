#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/people_25.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"

// Test fixture for LIMIT/OFFSET operations — templated on database backend
template <typename ConnType> class LimitOffsetTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end())
        )));
    }
};

TYPED_TEST_SUITE(LimitOffsetTest, DatabaseTypes);

// ===== Basic OFFSET Tests =====

TYPED_TEST(LimitOffsetTest, OffsetOnly) {
    QuerySet<Person, TypeParam> qs;

    auto result = qs.template order_by<fields::Person.name>().offset(10).select().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT with OFFSET failed: " << result.error().message();

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 15) << "Expected 15 rows (25 total - 10 offset)";

    // Verify first result is Karen (11th alphabetically)
    EXPECT_EQ(people.begin()->name, "Karen");
}

// ===== Combined LIMIT/OFFSET Tests =====

TYPED_TEST(LimitOffsetTest, LimitOffsetPagination) {
    QuerySet<Person, TypeParam> qs;

    // Page 1: names 1-5 (Alice..Eve)
    auto page1 = qs.template order_by<fields::Person.name>().limit(5).offset(0).select().execute();
    ASSERT_TRUE(page1.has_value());
    ASSERT_EQ(page1.value().size(), 5);
    EXPECT_EQ(page1.value().begin()->name, "Alice");
    EXPECT_EQ(std::ranges::next(page1.value().begin(), 4)->name, "Eve");

    // Page 2: names 6-10 (Frank..Jack)
    auto page2 = qs.limit(5).offset(5).select().execute();
    ASSERT_TRUE(page2.has_value());
    ASSERT_EQ(page2.value().size(), 5);
    EXPECT_EQ(page2.value().begin()->name, "Frank");
    EXPECT_EQ(std::ranges::next(page2.value().begin(), 4)->name, "Jack");

    // Page 3: names 11-15 (Karen..Olivia)
    auto page3 = qs.limit(5).offset(10).select().execute();
    ASSERT_TRUE(page3.has_value());
    ASSERT_EQ(page3.value().size(), 5);
    EXPECT_EQ(page3.value().begin()->name, "Karen");
    EXPECT_EQ(std::ranges::next(page3.value().begin(), 4)->name, "Olivia");

    // Page 4: names 16-20 (Paul..Tina)
    auto page4 = qs.limit(5).offset(15).select().execute();
    ASSERT_TRUE(page4.has_value());
    ASSERT_EQ(page4.value().size(), 5);
    EXPECT_EQ(page4.value().begin()->name, "Paul");
    EXPECT_EQ(std::ranges::next(page4.value().begin(), 4)->name, "Tina");

    // Page 5: names 21-25 (Uma..Yara)
    auto page5 = qs.limit(5).offset(20).select().execute();
    ASSERT_TRUE(page5.has_value());
    ASSERT_EQ(page5.value().size(), 5);
    EXPECT_EQ(page5.value().begin()->name, "Uma");
    EXPECT_EQ(std::ranges::next(page5.value().begin(), 4)->name, "Yara");

    // Page 6: no more records
    auto page6 = qs.limit(5).offset(25).select().execute();
    ASSERT_TRUE(page6.has_value());
    EXPECT_EQ(page6.value().size(), 0);
}

// ===== LIMIT/OFFSET with WHERE =====

TYPED_TEST(LimitOffsetTest, WhereWithOffsetNoLimit) {
    QuerySet<Person, TypeParam> qs;

    auto result =
            qs.template order_by<fields::Person.name>().where(fields::Person.age < 30).offset(2).select().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT WHERE with OFFSET failed: " << result.error().message();

    const auto& people = result.value();
    EXPECT_EQ(people.size(), 7) << "Expected 7 rows (9 matching - 2 offset)";
    for (const auto& person : people) {
        EXPECT_LT(person.age, 30);
    }
}

// ===== Reset State Tests =====

TYPED_TEST(LimitOffsetTest, ResetClearsLimitOffset) {
    QuerySet<Person, TypeParam> qs;

    // First query with LIMIT/OFFSET
    auto result1 = qs.limit(5).offset(5).select().execute();
    ASSERT_TRUE(result1.has_value());
    ASSERT_EQ(result1.value().size(), 5);

    // Reset and query again
    qs.reset();
    auto result2 = qs.select().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 25) << "Expected all 25 rows after reset";
}

TYPED_TEST(LimitOffsetTest, ReuseLimitOffset) {
    QuerySet<Person, TypeParam> qs;

    // Chain ORDER BY + LIMIT/OFFSET — returns finalized QS by value
    auto finalized = qs.template order_by<fields::Person.name>().limit(5).offset(10);

    // First query — names 11-15: Karen, Leo, Mia, Nick, Olivia
    auto result1 = finalized.select().execute();
    ASSERT_TRUE(result1.has_value());
    ASSERT_EQ(result1.value().size(), 5);
    EXPECT_EQ(result1.value().begin()->name, "Karen");

    // Second query reuses same finalized QS
    auto result2 = finalized.select().execute();
    ASSERT_TRUE(result2.has_value());
    ASSERT_EQ(result2.value().size(), 5);
    EXPECT_EQ(result2.value().begin()->name, "Karen");
}

TYPED_TEST(LimitOffsetTest, OverwriteLimitOffset) {
    QuerySet<Person, TypeParam> qs;

    // First LIMIT/OFFSET with ORDER BY
    auto result1 = qs.template order_by<fields::Person.name>().limit(5).offset(0).select().execute();
    ASSERT_TRUE(result1.has_value());
    ASSERT_EQ(result1.value().size(), 5);
    EXPECT_EQ(result1.value().begin()->name, "Alice");

    // Change LIMIT/OFFSET
    auto result2 = qs.limit(3).offset(10).select().execute();
    ASSERT_TRUE(result2.has_value());
    ASSERT_EQ(result2.value().size(), 3);
    EXPECT_EQ(result2.value().begin()->name, "Karen");
}

// ===== Boundary Values (issue #362, item C) =====
// limit()/offset() carry an assert(n >= 0) precondition. These guard the valid
// non-negative inputs so the precondition can never reject a legitimate caller.

TYPED_TEST(LimitOffsetTest, LimitZeroReturnsNoRows) {
    QuerySet<Person, TypeParam> qs;

    // LIMIT 0 is valid SQL — it returns no rows (not "unlimited").
    auto result = qs.template order_by<fields::Person.name>().limit(0).select().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT with LIMIT 0 failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 0) << "LIMIT 0 must return zero rows";
}

TYPED_TEST(LimitOffsetTest, OffsetZeroReturnsAllRows) {
    QuerySet<Person, TypeParam> qs;

    // OFFSET 0 is valid — skip nothing.
    auto result = qs.template order_by<fields::Person.name>().offset(0).select().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT with OFFSET 0 failed: " << result.error().message();
    ASSERT_EQ(result.value().size(), 25) << "OFFSET 0 must return all rows";
    EXPECT_EQ(result.value().begin()->name, "Alice");
}

TYPED_TEST(LimitOffsetTest, LimitLargerThanRowCount) {
    QuerySet<Person, TypeParam> qs;

    // A LIMIT far above the row count returns every row, not an error.
    auto result = qs.template order_by<fields::Person.name>().limit(1000).select().execute();
    ASSERT_TRUE(result.has_value()) << "SELECT with large LIMIT failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 25) << "LIMIT > row count must return all rows";
}

// ===== SQL Clause Ordering =====

TYPED_TEST(LimitOffsetTest, MethodChainingOrder) {
    QuerySet<Person, TypeParam> qs;

    // Methods can be chained in any order — add ORDER BY for deterministic results
    auto result1 = qs.template order_by<fields::Person.name>().limit(5).offset(3).select().execute();
    ASSERT_TRUE(result1.has_value());

    qs.reset();
    auto result2 = qs.template order_by<fields::Person.name>().offset(3).limit(5).select().execute();
    ASSERT_TRUE(result2.has_value());

    // Results should be identical
    ASSERT_EQ(result1.value().size(), result2.value().size());
    auto it1 = result1.value().begin();
    auto it2 = result2.value().begin();
    for (; it1 != result1.value().end() && it2 != result2.value().end(); ++it1, ++it2) {
        EXPECT_EQ(it1->name, it2->name);
    }
}
