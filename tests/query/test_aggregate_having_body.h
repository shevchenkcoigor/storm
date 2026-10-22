#pragma once

// Shared test body for test_aggregate_having_sqlite.cpp / test_aggregate_having_pg.cpp — the two
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
#error "test_aggregate_having_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

// NOLINTBEGIN(misc-use-anonymous-namespace)

import storm_defaults;
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
import storm_orm_statements_update;
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_person_seed_fixture.h"

#include "test_seed_helpers.h"

#include "test_aggregate_fixture.h"
TYPED_TEST_SUITE(AggregateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// =============================================================================
// HAVING Clause Tests
// =============================================================================

static auto check_ye10_count3(const auto &result) -> void {
    ASSERT_TRUE(result.has_value()) << "HAVING failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 1);
    auto [years_exp, count_val] = *result.value().begin();
    EXPECT_EQ(years_exp, 10);
    EXPECT_EQ(count_val, 3);
}

static auto check_age_between_20_40(const auto &result_set) -> void {
    for (const auto &[age, count] : result_set) {
        EXPECT_GT(age, 20);
        EXPECT_LT(age, 40);
    }
}

template <typename ConnType> auto insert_5_people_ye() -> void {
    ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(std::vector<Person>{
        {.name = "Alice", .age = 25, .salary = 50000.0, .years_experience = 5},
        {.name = "Bob", .age = 30, .salary = 60000.0, .years_experience = 5},
        {.name = "Charlie", .age = 35, .salary = 70000.0, .years_experience = 10},
        {.name = "Dave", .age = 40, .salary = 80000.0, .years_experience = 10},
        {.name = "Eve", .age = 45, .salary = 90000.0, .years_experience = 10},
    })));
}

TYPED_TEST(AggregateTest, HavingOnGroupByBuilder) {
    insert_5_people_ye<TypeParam>();

    auto result = this->qs->template group_by<fields::Person.years_experience>()
                      .having(fields::Person.years_experience > 5)
                      .count()
                      .execute();
    check_ye10_count3(result);
}

TYPED_TEST(AggregateTest, HavingOnAggregateStatement) {
    insert_5_people_ye<TypeParam>();

    auto result = this->qs->template group_by<fields::Person.years_experience>()
                      .count()
                      .having(fields::Person.years_experience > 5)
                      .execute();
    check_ye10_count3(result);
}

TYPED_TEST(AggregateTest, HavingWithJoin) {
    this->insert_join_test_data();

    auto result = this->msg_qs->template join<fields::Message.sender>()
                      .template group_by<fields::Message.value>()
                      .having(fields::Message.value > 30)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + JOIN failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);
}

TYPED_TEST(AggregateTest, HavingRepeatedQueries) {
    this->insert_test_data();

    for (int i = 0; i < 50; ++i) {
        auto result =
            this->qs->template group_by<fields::Person.age>().having(fields::Person.age > 30).count().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed";
        EXPECT_EQ(result.value().size(), 8);
    }
}

TYPED_TEST(AggregateTest, HavingWithSum) {
    this->insert_test_data();

    auto result = this->qs->template group_by<fields::Person.years_experience>()
                      .having(fields::Person.years_experience == 5)
                      .template sum<fields::Person.age>()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + SUM failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 1);

    auto [years_exp, sum_age] = *result.value().begin();
    EXPECT_EQ(years_exp, 5);
    EXPECT_EQ(sum_age, 268);
}

TYPED_TEST(AggregateTest, HavingWithWhereAndJoin) {
    ASSERT_TRUE((storm::test::batch_insert<Person, TypeParam>(std::vector<Person>{
        {.name = "Alice", .age = 25},
        {.name = "Bob", .age = 35},
    })));
    ASSERT_TRUE((storm::test::batch_insert<Message, TypeParam>(std::vector<Message>{
        {.content = "A1", .value = 10, .sender = {.id = 1}},
        {.content = "A2", .value = 20, .sender = {.id = 1}},
        {.content = "A3", .value = 30, .sender = {.id = 1}},
        {.content = "B1", .value = 50, .sender = {.id = 2}},
        {.content = "B2", .value = 70, .sender = {.id = 2}},
    })));

    // WHERE value >= 20 + JOIN + GROUP BY value HAVING value > 25
    // After WHERE: 20,30,50,70 → After HAVING > 25: 30,50,70 → 3 groups
    auto result = this->msg_qs->where(fields::Message.value >= 20)
                      .template join<fields::Message.sender>()
                      .template group_by<fields::Message.value>()
                      .having(fields::Message.value > 25)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + WHERE + JOIN failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 3);
}

// ----- HAVING with all ExpressionVariant types -----

TYPED_TEST(AggregateTest, HavingWithNotEqual) {
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age != 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING != failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_NE(age, 30);
    }
}

TYPED_TEST(AggregateTest, HavingWithLessThan) {
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age < 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING < failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_LT(age, 30);
    }
}

TYPED_TEST(AggregateTest, HavingWithLessEqual) {
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age <= 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING <= failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_LE(age, 30);
    }
}

TYPED_TEST(AggregateTest, HavingWithGreaterEqual) {
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age >= 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING >= failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_GE(age, 30);
    }
}

TYPED_TEST(AggregateTest, HavingWithIn) {
    auto result =
        this->qs->template group_by<fields::Person.age>().having(fields::Person.age.in(25, 30, 35)).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING IN failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_TRUE(age == 25 || age == 30 || age == 35) << "HAVING IN: unexpected age " << age;
    }
}

TYPED_TEST(AggregateTest, HavingWithBetween) {
    auto result =
        this->qs->template group_by<fields::Person.age>().having(fields::Person.age.between(25, 35)).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING BETWEEN failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_GE(age, 25);
        EXPECT_LE(age, 35);
    }
}

TYPED_TEST(AggregateTest, HavingWithLike) {
    auto result =
        this->qs->template group_by<fields::Person.name>().having(fields::Person.name.like("A%")).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING LIKE failed: " << result.error().message();
    for (const auto &[name, count] : result.value()) {
        EXPECT_TRUE(name.starts_with('A')) << "HAVING LIKE 'A%': unexpected name " << name;
    }
}

TYPED_TEST(AggregateTest, HavingWithLogicalAnd) {
    auto result = this->qs->template group_by<fields::Person.age>()
                      .having(fields::Person.age > 20 && fields::Person.age < 40)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING AND failed: " << result.error().message();
    check_age_between_20_40(result.value());
}

TYPED_TEST(AggregateTest, HavingWithLogicalOr) {
    auto result = this->qs->template group_by<fields::Person.age>()
                      .having(fields::Person.age < 25 || fields::Person.age > 35)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING OR failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_TRUE(age < 25 || age > 35) << "HAVING OR: unexpected age " << age;
    }
}

TYPED_TEST(AggregateTest, HavingWithComplexLogical) {
    auto result = this->qs->template group_by<fields::Person.age>()
                      .having((fields::Person.age >= 25 && fields::Person.age <= 35) || fields::Person.age == 50)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING complex logical failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_TRUE((age >= 25 && age <= 35) || age == 50) << "HAVING complex: unexpected age " << age;
    }
}

TYPED_TEST(AggregateTest, HavingWithWhereAndIn) {
    auto result = this->qs->where(fields::Person.salary > 50000.0)
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age.in(25, 30, 35))
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + HAVING IN failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_TRUE(age == 25 || age == 30 || age == 35) << "WHERE + HAVING IN: unexpected age " << age;
    }
}

TYPED_TEST(AggregateTest, HavingWithWhereAndBetween) {
    auto result = this->qs->where(fields::Person.salary > 30000.0)
                      .template group_by<fields::Person.years_experience>()
                      .having(fields::Person.years_experience.between(3, 8))
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + HAVING BETWEEN failed: " << result.error().message();
    for (const auto &[years, count] : result.value()) {
        EXPECT_GE(years, 3);
        EXPECT_LE(years, 8);
    }
}

TYPED_TEST(AggregateTest, HavingWithWhereAndLogicalAnd) {
    auto result = this->qs->where(fields::Person.salary > 30000.0)
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age > 20 && fields::Person.age < 40)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + HAVING AND failed: " << result.error().message();
    check_age_between_20_40(result.value());
}

TYPED_TEST(AggregateTest, HavingInOnAggregateStatement) {
    auto result =
        this->qs->template group_by<fields::Person.age>().count().having(fields::Person.age.in(25, 30)).execute();
    ASSERT_TRUE(result.has_value()) << "HAVING IN on AggregateStatement failed: " << result.error().message();
    for (const auto &[age, count] : result.value()) {
        EXPECT_TRUE(age == 25 || age == 30) << "HAVING IN on AggregateStatement: unexpected age " << age;
    }
}

TYPED_TEST(AggregateTest, HavingBetweenOnAggregateStatement) {
    auto result = this->qs->template group_by<fields::Person.age>()
                      .template sum<fields::Person.salary>()
                      .having(fields::Person.age.between(25, 35))
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING BETWEEN on AggregateStatement failed: " << result.error().message();
    for (const auto &[age, sum_salary] : result.value()) {
        EXPECT_GE(age, 25);
        EXPECT_LE(age, 35);
    }
}

TYPED_TEST(AggregateTest, HavingLogicalOnAggregateStatement) {
    auto result = this->qs->template group_by<fields::Person.age>()
                      .template avg<fields::Person.salary>()
                      .having(fields::Person.age >= 25 && fields::Person.age <= 40)
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING AND on AggregateStatement failed: " << result.error().message();
    for (const auto &[age, avg_salary] : result.value()) {
        EXPECT_GE(age, 25);
        EXPECT_LE(age, 40);
    }
}

// =============================================================================
// GROUP BY + ORDER BY Tests
// =============================================================================

template <typename ConnType> class GroupByOrderByTest : public PersonSeedFixture<ConnType> {};

TYPED_TEST_SUITE(GroupByOrderByTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(GroupByOrderByTest, GroupByWithOrderByAscending) {
    auto result = this->qs->template order_by<fields::Person.department>()
                      .template group_by<fields::Person.department>()
                      .count()
                      .execute();

    ASSERT_TRUE(result.has_value()) << "GROUP BY + ORDER BY should succeed";
    ASSERT_FALSE(result.value().empty());

    auto it = result.value().begin();
    std::string prev;
    while (it != result.value().end()) {
        const auto &[dept, count] = *it;
        EXPECT_GE(dept, prev) << "Results should be ordered ascending by department";
        prev = dept;
        ++it;
    }
}

TYPED_TEST(GroupByOrderByTest, GroupByWithOrderByDescending) {
    auto result = this->qs->template order_by<fields::Person.department, false>()
                      .template group_by<fields::Person.department>()
                      .count()
                      .execute();

    ASSERT_TRUE(result.has_value()) << "GROUP BY + ORDER BY DESC should succeed";
    ASSERT_FALSE(result.value().empty());

    auto it = result.value().begin();
    std::string prev = "ZZZZZZ";
    while (it != result.value().end()) {
        const auto &[dept, count] = *it;
        EXPECT_LE(dept, prev) << "Results should be ordered descending by department";
        prev = dept;
        ++it;
    }
}

TYPED_TEST(GroupByOrderByTest, GroupByRepeatedExecution) {
    for (int i = 0; i < 5; ++i) {
        auto result = this->qs->template order_by<fields::Person.department>()
                          .template group_by<fields::Person.department>()
                          .count()
                          .execute();

        ASSERT_TRUE(result.has_value()) << "Repeated GROUP BY should succeed on iteration " << i;
        EXPECT_EQ(result.value().size(), 5) << "Should have 5 departments";
    }
}

TYPED_TEST(GroupByOrderByTest, GroupByWithDifferentAggregatesSequentially) {
    auto count_result = this->qs->template group_by<fields::Person.department>().count().execute();
    ASSERT_TRUE(count_result.has_value());

    auto sum_result =
        this->qs->template group_by<fields::Person.department>().template sum<fields::Person.salary>().execute();
    ASSERT_TRUE(sum_result.has_value());

    auto avg_result =
        this->qs->template group_by<fields::Person.department>().template avg<fields::Person.age>().execute();
    ASSERT_TRUE(avg_result.has_value());

    EXPECT_EQ(count_result.value().size(), sum_result.value().size());
    EXPECT_EQ(count_result.value().size(), avg_result.value().size());
}

// ============================================================================
// HAVING + ORDER BY/LIMIT combined tests
// ============================================================================

TYPED_TEST(AggregateTest, HavingWithOrderByAndLimit) {
    auto result = this->qs->template order_by<fields::Person.age>()
                      .limit(3)
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age > 25)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + ORDER BY + LIMIT failed: " << result.error().message();

    const auto &groups = result.value();
    EXPECT_LE(groups.size(), 3);

    int prev_age = 0;
    for (const auto &[age, count] : groups) {
        EXPECT_GT(age, 25);
        EXPECT_GE(age, prev_age);
        prev_age = age;
    }
}

TYPED_TEST(AggregateTest, HavingWithOrderByOnly) {
    auto result = this->qs->template order_by<fields::Person.age, false>()
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age > 30)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + ORDER BY failed: " << result.error().message();

    const auto &groups = result.value();
    int prev_age = 100;
    for (const auto &[age, count] : groups) {
        EXPECT_GT(age, 30);
        EXPECT_LE(age, prev_age);
        prev_age = age;
    }
}

TYPED_TEST(AggregateTest, HavingWithLimitOnly) {
    auto result =
        this->qs->limit(2).template group_by<fields::Person.age>().having(fields::Person.age > 25).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + LIMIT failed: " << result.error().message();
    EXPECT_LE(result.value().size(), 2);
}

TYPED_TEST(AggregateTest, HavingWithOffsetOnly) {
    auto result =
        this->qs->offset(1).template group_by<fields::Person.age>().having(fields::Person.age > 25).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + OFFSET failed: " << result.error().message();
}

// NOLINTEND(misc-use-anonymous-namespace)
