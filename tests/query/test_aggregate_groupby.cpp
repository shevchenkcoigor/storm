#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_pool;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_update;
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_seed_helpers.h"
#include "test_aggregate_fixture.h"

TYPED_TEST_SUITE(AggregateTest, DatabaseTypes);

// =============================================================================
// GROUP BY Tests
// =============================================================================

TYPED_TEST(AggregateTest, GroupByWithCount) {
    this->insert_test_data();

    auto result = this->qs->template group_by<fields::Person.years_experience>().count().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + COUNT failed: " << result.error().message();

    auto& results = result.value();
    EXPECT_EQ(results.size(), 3);

    std::int64_t total_count = 0; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& row : results) {
        total_count += std::get<1>(row);
    }
    EXPECT_EQ(total_count, 25);
}

TYPED_TEST(AggregateTest, GroupByWithSum) {
    this->insert_test_data();

    auto result =
            this->qs->template group_by<fields::Person.years_experience>().template sum<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + SUM failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);

    std::int64_t total_sum = 0; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& [years, age_sum] : result.value()) {
        total_sum += age_sum;
    }
    EXPECT_EQ(total_sum, 829);
}

TYPED_TEST(AggregateTest, GroupByWithAvg) {
    this->insert_test_data();

    auto result = this->qs->template group_by<fields::Person.years_experience>()
                          .template avg<fields::Person.salary>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + AVG failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);

    std::optional<double> avg_5; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& [years, salary_avg] : result.value()) {
        if (years == 5) {
            avg_5 = salary_avg;
            break;
        }
    }
    ASSERT_TRUE(avg_5.has_value());
    EXPECT_NEAR(avg_5.value(), 50300.0, 0.01);
}

TYPED_TEST(AggregateTest, GroupByWithMin) {
    this->insert_test_data();

    auto result = this->qs->template group_by<fields::Person.years_experience>()
                          .template min<fields::Person.salary>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + MIN failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);

    std::optional<double> min_5; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& [years, salary_min] : result.value()) {
        if (years == 5) {
            min_5 = salary_min;
            break;
        }
    }
    ASSERT_TRUE(min_5.has_value());
    EXPECT_NEAR(min_5.value(), 32000.0, 0.01);
}

TYPED_TEST(AggregateTest, GroupByWithMax) {
    this->insert_test_data();

    auto result =
            this->qs->template group_by<fields::Person.years_experience>().template max<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + MAX failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);

    std::optional<double> max_5; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& [years, age_max] : result.value()) {
        if (years == 5) {
            max_5 = age_max;
            break;
        }
    }
    ASSERT_TRUE(max_5.has_value());
    EXPECT_NEAR(max_5.value(), 36.0, 0.01);
}

TYPED_TEST(AggregateTest, GroupByWithJoinAndSum) {
    this->insert_join_test_data();

    auto result = this->msg_qs->template join<fields::Message.sender>()
                          .template group_by<fields::Message.content>()
                          .template sum<fields::Message.value>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "JOIN + GROUP BY + SUM failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 6);

    std::int64_t total_sum = 0; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& [content, value_sum] : result.value()) {
        total_sum += value_sum;
    }
    EXPECT_EQ(total_sum, 210);
}

TYPED_TEST(AggregateTest, GroupByRepeatedQueries) {
    this->insert_test_data();

    for (int i = 0; i < 50; ++i) {
        auto result = this->qs->template group_by<fields::Person.years_experience>().count().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed";
        EXPECT_EQ(result.value().size(), 3);
    }
}

TYPED_TEST(AggregateTest, GroupByEmptyTable) {
    auto result = this->qs->template group_by<fields::Person.years_experience>().count().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY on empty table failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 0);
}

TYPED_TEST(AggregateTest, GroupByFullChain_Count) {
    this->insert_full_chain_data();

    auto count_result = this->msg_qs->where(fields::Message.value >= 20)
                                .template join<fields::Message.sender>()
                                .template group_by<fields::Message.value>()
                                .count()
                                .execute();
    ASSERT_TRUE(count_result.has_value()) << "Full chain COUNT failed: " << count_result.error().message();
    EXPECT_EQ(count_result.value().size(), 7);
    for (const auto& [value, count_val] : count_result.value()) {
        EXPECT_EQ(count_val, 1);
    }
}

TYPED_TEST(AggregateTest, GroupByFullChain_SumAndAvg) {
    this->insert_full_chain_data();

    auto sum_result = this->msg_qs->where(fields::Message.value < 50)
                              .template join<fields::Message.sender>()
                              .template group_by<fields::Message.content>()
                              .template sum<fields::Message.value>()
                              .execute();
    ASSERT_TRUE(sum_result.has_value()) << "Full chain SUM failed: " << sum_result.error().message();
    EXPECT_EQ(sum_result.value().size(), 7);

    (*this->msg_qs).reset();

    auto avg_result = this->msg_qs->where(fields::Message.value >= 10 && fields::Message.value <= 70)
                              .template join<fields::Message.sender>()
                              .template group_by<fields::Message.value>()
                              .template avg<fields::Message.value>()
                              .execute();
    ASSERT_TRUE(avg_result.has_value()) << "Full chain AVG failed: " << avg_result.error().message();
    EXPECT_EQ(avg_result.value().size(), 8);
    for (const auto& [value, avg_val] : avg_result.value()) {
        ASSERT_TRUE(avg_val.has_value());
        EXPECT_NEAR(avg_val.value(), static_cast<double>(value), 0.01);
    }
}

// =============================================================================
// COUNT(DISTINCT) Tests
// =============================================================================

TYPED_TEST(AggregateTest, CountDistinctWithDuplicates) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
            {.id = 0, .name = "Alice", .age = 30, .salary = 50000.0, .years_experience = 3},
            {.id = 0, .name = "Bob", .age = 30, .salary = 60000.0, .years_experience = 5},
            {.id = 0, .name = "Charlie", .age = 35, .salary = 70000.0, .years_experience = 7},
            {.id = 0, .name = "Dave", .age = 30, .salary = 80000.0, .years_experience = 10},
            {.id = 0, .name = "Eve", .age = 35, .salary = 90000.0, .years_experience = 15},
    })));

    auto result = this->qs->template count_distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "COUNT(DISTINCT) with duplicates failed";
    EXPECT_EQ(result.value(), 2);
}

TYPED_TEST(AggregateTest, CountDistinctWithJoin) {
    this->insert_join_test_data();

    auto result = this->msg_qs->template join<fields::Message.sender>()
                          .template count_distinct<fields::Message.value>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "COUNT(DISTINCT) with JOIN failed";
    EXPECT_EQ(result.value(), 6);
}

TYPED_TEST(AggregateTest, CountDistinctRepeatedQueries) {
    this->insert_test_data();

    for (int i = 0; i < 50; ++i) {
        auto result = this->qs->template count_distinct<fields::Person.years_experience>().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed";
        EXPECT_EQ(result.value(), 3);
    }
}
