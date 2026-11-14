#pragma once

// Shared test body for test_aggregate_optional_sqlite.cpp / test_aggregate_optional_pg.cpp — the two
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
#error "test_aggregate_optional_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"

#include "test_seed_helpers.h"

#include "test_aggregate_fixture.h"

// =============================================================================
// NULL/Optional Tests
// =============================================================================

template <typename ConnType> class OptionalAggregateTest : public StormTestFixture<Person, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        qs = std::make_unique<QuerySet<Person, ConnType>>();
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<Person, ConnType>::TearDown();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes)
    std::unique_ptr<QuerySet<Person, ConnType>> qs; // NOSONAR cpp:S3656
};
TYPED_TEST_SUITE(OptionalAggregateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(OptionalAggregateTest, CountWithNullValues) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .salary = 50000.0, .score = 25},
        {.name = "Bob", .salary = 60000.0, .score = std::nullopt},
        {.name = "Charlie", .salary = 70000.0, .score = 35},
        {.name = "Dave", .salary = 80000.0, .score = std::nullopt},
    })));

    auto count_all = this->qs->count().execute();
    ASSERT_TRUE(count_all.has_value());
    EXPECT_EQ(count_all.value(), 4);

    auto count_age = this->qs->template count<fields::Person.score>().execute();
    ASSERT_TRUE(count_age.has_value());
    EXPECT_EQ(count_age.value(), 2);
}

TYPED_TEST(OptionalAggregateTest, AvgWithNullValues) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .salary = 50000.0, .score = 20},
        {.name = "Bob", .salary = 60000.0, .score = std::nullopt},
        {.name = "Charlie", .salary = 70000.0, .score = 40},
    })));

    auto avg = this->qs->template avg<fields::Person.score>().execute();
    ASSERT_TRUE(avg.has_value());
    ASSERT_TRUE(avg.value().has_value());
    EXPECT_NEAR(avg.value().value(), 30.0, 0.01);
}

TYPED_TEST(OptionalAggregateTest, MinMaxWithNullValues) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .salary = 50000.0, .score = 25},
        {.name = "Bob", .salary = 60000.0, .score = std::nullopt},
        {.name = "Charlie", .salary = 70000.0, .score = 45},
        {.name = "Dave", .salary = 80000.0, .score = std::nullopt},
    })));

    auto min_val = this->qs->template min<fields::Person.score>().execute();
    ASSERT_TRUE(min_val.has_value());
    ASSERT_TRUE(min_val.value().has_value());
    EXPECT_NEAR(min_val.value().value(), 25.0, 0.01);

    auto max_val = this->qs->template max<fields::Person.score>().execute();
    ASSERT_TRUE(max_val.has_value());
    ASSERT_TRUE(max_val.value().has_value());
    EXPECT_NEAR(max_val.value().value(), 45.0, 0.01);
}

TYPED_TEST(OptionalAggregateTest, CountDistinctWithNullValues) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .salary = 50000.0, .score = 30},
        {.name = "Bob", .salary = 60000.0, .score = std::nullopt},
        {.name = "Charlie", .salary = 70000.0, .score = 30},
        {.name = "Dave", .salary = 80000.0, .score = std::nullopt},
        {.name = "Eve", .salary = 90000.0, .score = 40},
    })));

    auto result = this->qs->template count_distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 2);
}

TYPED_TEST(OptionalAggregateTest, GroupByWithAllNullValuesInGroupColumn) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .salary = 50000.0, .score = std::nullopt},
        {.name = "Bob", .salary = 60000.0, .score = std::nullopt},
        {.name = "Charlie", .salary = 70000.0, .score = std::nullopt},
    })));

    auto result = this->qs->template group_by<fields::Person.score>().count().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY with all NULL values failed";
    EXPECT_EQ(result.value().size(), 1);

    auto &groups = result.value();
    auto it = groups.begin();
    ASSERT_NE(it, groups.end());
    auto [score_key, count_val] = *it;
    EXPECT_FALSE(score_key.has_value()) << "Expected NULL group key";
    EXPECT_EQ(count_val, 3) << "Expected count of 3 in NULL group";
}

TYPED_TEST(OptionalAggregateTest, GroupByWithMixedNullAndNonNullValues) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .salary = 50000.0, .score = 25},
        {.name = "Bob", .salary = 60000.0, .score = std::nullopt},
        {.name = "Charlie", .salary = 70000.0, .score = 25},
        {.name = "Dave", .salary = 80000.0, .score = std::nullopt},
        {.name = "Eve", .salary = 90000.0, .score = 30},
    })));

    auto result = this->qs->template group_by<fields::Person.score>().count().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY with mixed NULL values failed";
    EXPECT_EQ(result.value().size(), 3) << "Expected 3 groups (NULL, 25, 30)";

    std::int64_t null_count = 0;   // NOLINT(misc-const-correctness) - modified in loop
    std::int64_t age_25_count = 0; // NOLINT(misc-const-correctness) - modified in loop
    std::int64_t age_30_count = 0; // NOLINT(misc-const-correctness) - modified in loop

    for (const auto &[score_key, count_val] : result.value()) {
        if (!score_key.has_value()) {
            null_count = count_val;
        } else if (score_key.value() == 25) {
            age_25_count = count_val;
        } else if (score_key.value() == 30) {
            age_30_count = count_val;
        }
    }

    EXPECT_EQ(null_count, 2) << "Expected 2 rows in NULL group";
    EXPECT_EQ(age_25_count, 2) << "Expected 2 rows in age=25 group";
    EXPECT_EQ(age_30_count, 1) << "Expected 1 row in age=30 group";
}

// =============================================================================
// Empty / All-Filtered-Out Set Tests (#416)
// MIN/MAX/AVG over no rows have no value -> nullopt (distinguishable from real 0).
// SUM over no rows keeps the SQL-conventional 0. COUNT over no rows is 0.
// =============================================================================

TYPED_TEST(OptionalAggregateTest, MinOverEmptySetIsNullopt) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = 25},
        {.name = "Bob", .age = 35},
    })));

    auto result = this->qs->where(fields::Person.age > 9999).template min<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_FALSE(result.value().has_value()) << "MIN over empty set must be nullopt, not 0";
}

TYPED_TEST(OptionalAggregateTest, MaxOverEmptySetIsNullopt) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = 25},
        {.name = "Bob", .age = 35},
    })));

    auto result = this->qs->where(fields::Person.age > 9999).template max<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_FALSE(result.value().has_value()) << "MAX over empty set must be nullopt, not 0";
}

TYPED_TEST(OptionalAggregateTest, AvgOverEmptySetIsNullopt) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = 25},
        {.name = "Bob", .age = 35},
    })));

    auto result = this->qs->where(fields::Person.age > 9999).template avg<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_FALSE(result.value().has_value()) << "AVG over empty set must be nullopt, not 0.0";
}

TYPED_TEST(OptionalAggregateTest, MinMaxAvgOverEmptySetVsRealZeroAreDistinguishable) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Zero", .age = 0},
    })));

    // Real zero: a row with age == 0 -> the aggregate IS 0, present.
    auto real_min = this->qs->where(fields::Person.age == 0).template min<fields::Person.age>().execute();
    ASSERT_TRUE(real_min.has_value()) << real_min.error().message();
    ASSERT_TRUE(real_min.value().has_value()) << "MIN over a present 0 must hold a value";
    EXPECT_DOUBLE_EQ(real_min.value().value(), 0.0);

    auto real_max = this->qs->where(fields::Person.age == 0).template max<fields::Person.age>().execute();
    ASSERT_TRUE(real_max.has_value()) << real_max.error().message();
    ASSERT_TRUE(real_max.value().has_value()) << "MAX over a present 0 must hold a value";
    EXPECT_DOUBLE_EQ(real_max.value().value(), 0.0);

    auto real_avg = this->qs->where(fields::Person.age == 0).template avg<fields::Person.age>().execute();
    ASSERT_TRUE(real_avg.has_value()) << real_avg.error().message();
    ASSERT_TRUE(real_avg.value().has_value()) << "AVG over a present 0 must hold a value";
    EXPECT_DOUBLE_EQ(real_avg.value().value(), 0.0);

    // Empty set: no rows -> nullopt, NOT a real 0.
    auto empty_min = this->qs->where(fields::Person.age > 9999).template min<fields::Person.age>().execute();
    ASSERT_TRUE(empty_min.has_value()) << empty_min.error().message();
    EXPECT_FALSE(empty_min.value().has_value());

    auto empty_max = this->qs->where(fields::Person.age > 9999).template max<fields::Person.age>().execute();
    ASSERT_TRUE(empty_max.has_value()) << empty_max.error().message();
    EXPECT_FALSE(empty_max.value().has_value());

    auto empty_avg = this->qs->where(fields::Person.age > 9999).template avg<fields::Person.age>().execute();
    ASSERT_TRUE(empty_avg.has_value()) << empty_avg.error().message();
    EXPECT_FALSE(empty_avg.value().has_value());
}

TYPED_TEST(OptionalAggregateTest, SumOverEmptySetKeepsZeroConvention) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = 25},
    })));

    auto result = this->qs->where(fields::Person.age > 9999).template sum<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value(), 0) << "SUM over empty set keeps the 0 convention";
}

// GROUP BY: an aggregate can be NULL within a group whose agg column is all-NULL.
TYPED_TEST(OptionalAggregateTest, GroupByAvgOfAllNullColumnInGroupIsNullopt) {
    // One row whose score is NULL, grouped by name -> AVG(score) is NULL for that group.
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Solo", .salary = 50000.0, .score = std::nullopt},
    })));

    auto result = this->qs->template group_by<fields::Person.name>().template avg<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1U);
    auto it = result.value().begin();
    auto [name_key, avg_sc] = *it;
    EXPECT_FALSE(avg_sc.has_value()) << "AVG of an all-NULL column within a group must be nullopt";
}

// =============================================================================
// Negative Number Tests
// =============================================================================

TYPED_TEST_SUITE(AggregateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(AggregateTest, NegativeNumbersInSum) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = -10, .salary = 50000.0, .years_experience = 3},
        {.name = "Bob", .age = 5, .salary = 60000.0, .years_experience = 5},
        {.name = "Charlie", .age = -3, .salary = 70000.0, .years_experience = 7},
    })));

    auto sum = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum.has_value());
    EXPECT_EQ(sum.value(), -8);
}

TYPED_TEST(AggregateTest, NegativeNumbersInAvg) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = -12, .salary = 50000.0, .years_experience = 3},
        {.name = "Bob", .age = 6, .salary = 60000.0, .years_experience = 5},
        {.name = "Charlie", .age = 0, .salary = 70000.0, .years_experience = 7},
    })));

    auto avg = this->qs->template avg<fields::Person.age>().execute();
    ASSERT_TRUE(avg.has_value());
    ASSERT_TRUE(avg.value().has_value());
    EXPECT_NEAR(avg.value().value(), -2.0, 0.01);
}

TYPED_TEST(AggregateTest, NegativeNumbersInMinMax) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = -10, .salary = 50000.0, .years_experience = 3},
        {.name = "Bob", .age = 5, .salary = 60000.0, .years_experience = 5},
        {.name = "Charlie", .age = -20, .salary = 70000.0, .years_experience = 7},
        {.name = "Dave", .age = 15, .salary = 80000.0, .years_experience = 10},
    })));

    auto min_val = this->qs->template min<fields::Person.age>().execute();
    ASSERT_TRUE(min_val.has_value());
    ASSERT_TRUE(min_val.value().has_value());
    EXPECT_NEAR(min_val.value().value(), -20.0, 0.01);

    auto max_val = this->qs->template max<fields::Person.age>().execute();
    ASSERT_TRUE(max_val.has_value());
    ASSERT_TRUE(max_val.value().has_value());
    EXPECT_NEAR(max_val.value().value(), 15.0, 0.01);
}

TYPED_TEST(AggregateTest, NegativeNumbersInCount) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = -10, .salary = 50000.0, .years_experience = 3},
        {.name = "Bob", .age = -5, .salary = 60000.0, .years_experience = 5},
        {.name = "Charlie", .age = 0, .salary = 70000.0, .years_experience = 7},
    })));

    auto count = this->qs->count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 3);

    auto count_age = this->qs->template count<fields::Person.age>().execute();
    ASSERT_TRUE(count_age.has_value());
    EXPECT_EQ(count_age.value(), 3);
}

TYPED_TEST(AggregateTest, NegativeNumbersInWhere) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = -10, .salary = 50000.0, .years_experience = 3},
        {.name = "Bob", .age = -5, .salary = 60000.0, .years_experience = 5},
        {.name = "Charlie", .age = 0, .salary = 70000.0, .years_experience = 7},
        {.name = "Dave", .age = 5, .salary = 80000.0, .years_experience = 10},
        {.name = "Eve", .age = 10, .salary = 90000.0, .years_experience = 15},
    })));

    auto result = this->qs->where(fields::Person.age < 0).count().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 2);

    auto sum_neg = this->qs->where(fields::Person.age < 0).template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum_neg.has_value());
    EXPECT_EQ(sum_neg.value(), -15);
}
