#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-use-anonymous-namespace)

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_seed_helpers.h"
#include "test_aggregate_fixture.h"

TYPED_TEST_SUITE(AggregateTest, DatabaseTypes);

// =============================================================================
// Combined Clause Tests
// =============================================================================

template <typename ConnType> auto insert_full_chain_14_messages() -> void {
    ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(std::vector<Person>{
            {.name = "Alice", .age = 25},
            {.name = "Bob", .age = 35},
            {.name = "Charlie", .age = 45},
            {.name = "Dave", .age = 30},
            {.name = "Eve", .age = 28},
    })));
    ASSERT_TRUE((storm::test::batch_insert<Message, ConnType>(std::vector<Message>{
            {.content = "A1", .value = 10, .sender = {.id = 1}},
            {.content = "A2", .value = 15, .sender = {.id = 1}},
            {.content = "A3", .value = 20, .sender = {.id = 1}},
            {.content = "B1", .value = 25, .sender = {.id = 2}},
            {.content = "B2", .value = 30, .sender = {.id = 2}},
            {.content = "C1", .value = 35, .sender = {.id = 3}},
            {.content = "C2", .value = 40, .sender = {.id = 3}},
            {.content = "C3", .value = 45, .sender = {.id = 3}},
            {.content = "C4", .value = 50, .sender = {.id = 3}},
            {.content = "D1", .value = 55, .sender = {.id = 4}},
            {.content = "D2", .value = 60, .sender = {.id = 4}},
            {.content = "E1", .value = 65, .sender = {.id = 5}},
            {.content = "E2", .value = 70, .sender = {.id = 5}},
            {.content = "E3", .value = 75, .sender = {.id = 5}},
    })));
}

static auto check_message_values(const auto& result_set, const std::vector<int>& expected) -> void {
    std::size_t idx = 0;
    for (const auto& msg : result_set) {
        ASSERT_LT(idx, expected.size());
        EXPECT_EQ(msg.value, expected[idx++]);
    }
}

TYPED_TEST(AggregateTest, FullChain_WhereJoinOrderByLimitOffset) {
    insert_full_chain_14_messages<TypeParam>();

    // WHERE + JOIN + ORDER BY DESC + LIMIT 5 + OFFSET 2
    // Values >= 20 ordered DESC: 75,70,65,60,55,50,45,40,35,30,25,20
    // After OFFSET 2: 65,60,55,50,45,...  After LIMIT 5: 65,60,55,50,45
    auto result = this->msg_qs->where(fields::Message.value >= 20)
                          .template join<fields::Message.sender>()
                          .template order_by<fields::Message.value, false>()
                          .limit(5)
                          .offset(2)
                          .select()
                          .execute();

    ASSERT_TRUE(result.has_value()) << "Full chain DESC query failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 5);
    check_message_values(result.value(), {65, 60, 55, 50, 45});
}

TYPED_TEST(AggregateTest, FullChain_WhereJoinOrderByAscLimit) {
    insert_full_chain_14_messages<TypeParam>();

    auto result = this->msg_qs->where(fields::Message.value >= 20)
                          .template join<fields::Message.sender>()
                          .template order_by<fields::Message.value, true>()
                          .limit(3)
                          .offset(0)
                          .select()
                          .execute();

    ASSERT_TRUE(result.has_value()) << "Full chain ASC query failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);
    check_message_values(result.value(), {20, 25, 30});
}

TYPED_TEST(AggregateTest, FullChain_JoinResolvesCorrectSender) {
    insert_full_chain_14_messages<TypeParam>();

    auto result =
            this->msg_qs->where(fields::Message.value == 75).template join<fields::Message.sender>().select().execute();

    ASSERT_TRUE(result.has_value()) << "JOIN verification query failed";
    ASSERT_EQ(result.value().size(), 1);
    EXPECT_EQ(result.value().begin()->sender.name, "Eve");
}

template <typename ConnType> auto verify_group_by_counts(QuerySet<Person, ConnType>& qs) -> void {
    const std::map<int, std::int64_t> expected_counts = {{5, 10}, {10, 8}, {15, 7}};
    auto result = qs.template group_by<fields::Person.years_experience>().count().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + COUNT failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);
    for (const auto& [ye, count_val] : result.value()) {
        auto it = expected_counts.find(ye);
        ASSERT_NE(it, expected_counts.end()) << "Unexpected years_experience: " << ye;
        EXPECT_EQ(count_val, it->second);
    }
}

// Compares a GROUP BY result (key -> optional<double> aggregate) against expected
// values. MIN/MAX/AVG columns are std::optional<double> (#416); every key present
// in the result and in `expected` must hold a value matching within tolerance.
template <typename Hive>
auto verify_optional_agg_groups(const Hive& rows, const std::map<int, double>& expected) -> void {
    std::map<int, double> actual;
    for (const auto& [ye, v] : rows) {
        ASSERT_TRUE(v.has_value()) << "NULL aggregate for group " << ye;
        actual.emplace(ye, v.value());
    }
    // A missing group must fail: iterate the EXPECTED keys, not whichever keys came back.
    ASSERT_EQ(actual.size(), expected.size());
    for (const auto& [ye, want] : expected) {
        ASSERT_TRUE(actual.contains(ye)) << "missing group " << ye;
        EXPECT_NEAR(actual.at(ye), want, 0.01) << "group " << ye;
    }
}

template <typename ConnType> auto verify_group_by_sum_avg_min_max(QuerySet<Person, ConnType>& qs) -> void {
    const std::map<int, std::int64_t> exp_sum = {{5, 268}, {10, 285}, {15, 276}};
    const std::map<int, double>       exp_avg = {{5, 26.8}, {10, 35.625}, {15, 39.43}};
    const std::map<int, double>       exp_min = {{5, 22.0}, {10, 29.0}, {15, 35.0}};
    const std::map<int, double>       exp_max = {{5, 36.0}, {10, 48.0}, {15, 45.0}};

    qs.reset();
    auto sum = qs.template group_by<fields::Person.years_experience>().template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum.has_value());
    std::map<int, std::int64_t> actual_sum;
    for (const auto& [ye, v] : sum.value()) {
        actual_sum.emplace(ye, v);
    }
    EXPECT_EQ(actual_sum, exp_sum);

    qs.reset();
    auto avg = qs.template group_by<fields::Person.years_experience>().template avg<fields::Person.age>().execute();
    ASSERT_TRUE(avg.has_value());
    verify_optional_agg_groups(avg.value(), exp_avg);

    qs.reset();
    auto mn = qs.template group_by<fields::Person.years_experience>().template min<fields::Person.age>().execute();
    ASSERT_TRUE(mn.has_value());
    verify_optional_agg_groups(mn.value(), exp_min);

    qs.reset();
    auto mx = qs.template group_by<fields::Person.years_experience>().template max<fields::Person.age>().execute();
    ASSERT_TRUE(mx.has_value());
    verify_optional_agg_groups(mx.value(), exp_max);
}

TYPED_TEST(AggregateTest, GroupByWithAllAggregateTypes) {
    this->insert_test_data();
    verify_group_by_counts<TypeParam>(*this->qs);
    verify_group_by_sum_avg_min_max<TypeParam>(*this->qs);

    (*this->qs).reset();
    auto filtered_sum = this->qs->where(fields::Person.years_experience == 5)
                                .template group_by<fields::Person.years_experience>()
                                .template sum<fields::Person.age>()
                                .execute();
    ASSERT_TRUE(filtered_sum.has_value()) << "WHERE + GROUP BY + SUM failed";
    EXPECT_EQ(filtered_sum.value().size(), 1);
    const auto& [years_exp, sum_age] = *filtered_sum.value().begin();
    EXPECT_EQ(years_exp, 5);
    EXPECT_EQ(sum_age, 268);
}

// =============================================================================
// GROUP BY with ORDER BY, LIMIT, OFFSET Tests
// =============================================================================

TYPED_TEST(AggregateTest, GroupByWithOrderByDesc) {
    this->insert_test_data();

    auto result = this->qs->template order_by<fields::Person.years_experience, false>()
                          .template group_by<fields::Person.years_experience>()
                          .count()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + ORDER BY DESC failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);

    std::vector<int> expected_order = {15, 10, 5};
    std::size_t      idx            = 0; // NOLINT(misc-const-correctness) - modified in loop
    for (const auto& [years, count_val] : result.value()) {
        EXPECT_EQ(years, expected_order[idx]) << "Unexpected order at index " << idx;
        idx++;
    }
}

TYPED_TEST(AggregateTest, GroupByMultipleFields) {
    this->insert_test_data();

    auto result = this->qs->template group_by<fields::Person.age, fields::Person.years_experience>().count().execute();
    ASSERT_TRUE(result.has_value()) << "Multi-field GROUP BY failed: " << result.error().message();

    // (age, years_experience) -> head-count over PEOPLE_25: 16 groups summing to 25 people.
    const std::map<std::pair<int, int>, std::int64_t> expected{
            {{22, 5}, 2},
            {{25, 5}, 3},
            {{27, 5}, 1},
            {{28, 5}, 2},
            {{29, 10}, 1},
            {{30, 5}, 1},
            {{30, 10}, 2},
            {{33, 10}, 2},
            {{35, 15}, 2},
            {{36, 5}, 1},
            {{38, 15}, 2},
            {{40, 10}, 1},
            {{40, 15}, 1},
            {{42, 10}, 1},
            {{45, 15}, 2},
            {{48, 10}, 1}
    };
    std::map<std::pair<int, int>, std::int64_t> actual;
    for (const auto& [age, years_exp, count_val] : result.value()) {
        actual.emplace(std::pair{age, years_exp}, count_val);
    }
    EXPECT_EQ(actual, expected);
}

TYPED_TEST(AggregateTest, GroupByMultipleFieldsWithOrderByLimit) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
            {.name = "Alice", .age = 25, .salary = 50000.0, .years_experience = 5},
            {.name = "Bob", .age = 25, .salary = 60000.0, .years_experience = 5},
            {.name = "Charlie", .age = 25, .salary = 70000.0, .years_experience = 10},
            {.name = "Dave", .age = 30, .salary = 80000.0, .years_experience = 5},
            {.name = "Eve", .age = 30, .salary = 90000.0, .years_experience = 10},
    })));

    auto result = this->qs->template order_by<fields::Person.age>()
                          .limit(2)
                          .template group_by<fields::Person.age, fields::Person.years_experience>()
                          .count()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "Multi-field GROUP BY + ORDER BY + LIMIT failed";

    // ORDER BY age + LIMIT 2: age 25 has exactly two groups, so they are the two that survive
    // (ties inside an age are unordered, but the age-30 groups must not displace them).
    std::map<std::pair<int, int>, std::int64_t> actual;
    for (const auto& [age, years_exp, count_val] : result.value()) {
        actual.emplace(std::pair{age, years_exp}, count_val);
    }
    EXPECT_EQ(actual, (std::map<std::pair<int, int>, std::int64_t>{{{25, 5}, 2}, {{25, 10}, 1}}));
}

// NOLINTEND(misc-use-anonymous-namespace)
