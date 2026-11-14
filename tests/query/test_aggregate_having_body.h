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

using AgeCounts = std::map<int, std::int64_t>;

// PEOPLE_25 head-count per age (the seed insert_test_data() writes).
inline const AgeCounts kPeopleAgeCounts{{22, 2}, {25, 3}, {27, 1}, {28, 2}, {29, 1}, {30, 3}, {33, 2},
                                        {35, 2}, {36, 1}, {38, 2}, {40, 2}, {42, 1}, {45, 2}, {48, 1}};

template <typename K, typename V = std::int64_t> auto groups_of(const auto &rows) -> std::map<K, V> {
    std::map<K, V> out;
    for (const auto &[key, value] : rows) {
        out.emplace(key, value);
    }
    return out;
}

template <typename K, typename V = std::int64_t> auto group_list_of(const auto &rows) -> std::vector<std::pair<K, V>> {
    std::vector<std::pair<K, V>> out;
    for (const auto &[key, value] : rows) {
        out.emplace_back(key, value);
    }
    return out;
}

// Ages 20 < age < 40 — every group of PEOPLE_25 except 40/42/45/48.
static auto check_age_between_20_40(const auto &result_set) -> void {
    EXPECT_EQ(groups_of<int>(result_set),
              (AgeCounts{{22, 2}, {25, 3}, {27, 1}, {28, 2}, {29, 1}, {30, 3}, {33, 2}, {35, 2}, {36, 1}, {38, 2}}));
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

    // Same statement again: the cached prepared statement must rebind the HAVING parameter.
    auto again = this->qs->template group_by<fields::Person.years_experience>()
                     .having(fields::Person.years_experience == 5)
                     .template sum<fields::Person.age>()
                     .execute();
    ASSERT_TRUE(again.has_value()) << again.error().message();
    ASSERT_EQ(again.value().size(), 1);
    EXPECT_EQ(std::get<1>(*again.value().begin()), 268);
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
// Each test seeds PEOPLE_25 and asserts the exact surviving groups — an empty table (or a
// predicate that filters nothing / everything) must fail, not pass vacuously.
//
// PEOPLE_25 age -> head-count: 22:2 25:3 27:1 28:2 29:1 30:3 33:2 35:2 36:1 38:2 40:2 42:1 45:2 48:1

TYPED_TEST(AggregateTest, HavingWithNotEqual) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age != 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING != failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()), (AgeCounts{{22, 2},
                                                         {25, 3},
                                                         {27, 1},
                                                         {28, 2},
                                                         {29, 1},
                                                         {33, 2},
                                                         {35, 2},
                                                         {36, 1},
                                                         {38, 2},
                                                         {40, 2},
                                                         {42, 1},
                                                         {45, 2},
                                                         {48, 1}}));
}

TYPED_TEST(AggregateTest, HavingWithLessThan) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age < 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING < failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()), (AgeCounts{{22, 2}, {25, 3}, {27, 1}, {28, 2}, {29, 1}}));
}

TYPED_TEST(AggregateTest, HavingWithLessEqual) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age <= 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING <= failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()), (AgeCounts{{22, 2}, {25, 3}, {27, 1}, {28, 2}, {29, 1}, {30, 3}}));
}

TYPED_TEST(AggregateTest, HavingWithGreaterEqual) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>().having(fields::Person.age >= 30).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING >= failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()),
              (AgeCounts{{30, 3}, {33, 2}, {35, 2}, {36, 1}, {38, 2}, {40, 2}, {42, 1}, {45, 2}, {48, 1}}));
}

TYPED_TEST(AggregateTest, HavingWithIn) {
    this->insert_test_data();
    auto result =
        this->qs->template group_by<fields::Person.age>().having(fields::Person.age.in(25, 30, 35)).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING IN failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()), (AgeCounts{{25, 3}, {30, 3}, {35, 2}}));
}

TYPED_TEST(AggregateTest, HavingWithBetween) {
    this->insert_test_data();
    auto result =
        this->qs->template group_by<fields::Person.age>().having(fields::Person.age.between(25, 35)).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING BETWEEN failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()),
              (AgeCounts{{25, 3}, {27, 1}, {28, 2}, {29, 1}, {30, 3}, {33, 2}, {35, 2}}));
}

TYPED_TEST(AggregateTest, HavingWithLike) {
    this->insert_test_data();
    auto result =
        this->qs->template group_by<fields::Person.name>().having(fields::Person.name.like("A%")).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING LIKE failed: " << result.error().message();
    EXPECT_EQ(groups_of<std::string>(result.value()), (std::map<std::string, std::int64_t>{{"Alice", 1}}));
}

TYPED_TEST(AggregateTest, HavingWithLogicalAnd) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>()
                      .having(fields::Person.age > 20 && fields::Person.age < 40)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING AND failed: " << result.error().message();
    check_age_between_20_40(result.value());
}

TYPED_TEST(AggregateTest, HavingWithLogicalOr) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>()
                      .having(fields::Person.age < 25 || fields::Person.age > 35)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING OR failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()),
              (AgeCounts{{22, 2}, {36, 1}, {38, 2}, {40, 2}, {42, 1}, {45, 2}, {48, 1}}));
}

TYPED_TEST(AggregateTest, HavingWithComplexLogical) {
    this->insert_test_data();
    // The `|| age == 50` arm matches nothing in the seed: the result is exactly the AND arm.
    auto result = this->qs->template group_by<fields::Person.age>()
                      .having((fields::Person.age >= 25 && fields::Person.age <= 35) || fields::Person.age == 50)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING complex logical failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()),
              (AgeCounts{{25, 3}, {27, 1}, {28, 2}, {29, 1}, {30, 3}, {33, 2}, {35, 2}}));
}

TYPED_TEST(AggregateTest, HavingWithWhereAndIn) {
    this->insert_test_data();
    // WHERE salary > 50000 drops Karen (age 25, salary exactly 50000): 25 -> 2, not 3.
    auto result = this->qs->where(fields::Person.salary > 50000.0)
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age.in(25, 30, 35))
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + HAVING IN failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()), (AgeCounts{{25, 2}, {30, 3}, {35, 2}}));
}

TYPED_TEST(AggregateTest, HavingWithWhereAndBetween) {
    this->insert_test_data();
    // Every salary is > 30000; only years_experience 5 falls in 3..8 (10 people).
    auto result = this->qs->where(fields::Person.salary > 30000.0)
                      .template group_by<fields::Person.years_experience>()
                      .having(fields::Person.years_experience.between(3, 8))
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + HAVING BETWEEN failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()), (AgeCounts{{5, 10}}));
}

TYPED_TEST(AggregateTest, HavingWithWhereAndLogicalAnd) {
    this->insert_test_data();
    auto result = this->qs->where(fields::Person.salary > 30000.0)
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age > 20 && fields::Person.age < 40)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + HAVING AND failed: " << result.error().message();
    check_age_between_20_40(result.value());
}

TYPED_TEST(AggregateTest, HavingInOnAggregateStatement) {
    this->insert_test_data();
    auto result =
        this->qs->template group_by<fields::Person.age>().count().having(fields::Person.age.in(25, 30)).execute();
    ASSERT_TRUE(result.has_value()) << "HAVING IN on AggregateStatement failed: " << result.error().message();
    EXPECT_EQ(groups_of<int>(result.value()), (AgeCounts{{25, 3}, {30, 3}}));
}

TYPED_TEST(AggregateTest, HavingBetweenOnAggregateStatement) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>()
                      .template sum<fields::Person.salary>()
                      .having(fields::Person.age.between(25, 35))
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING BETWEEN on AggregateStatement failed: " << result.error().message();
    EXPECT_EQ((groups_of<int, double>(result.value())), (std::map<int, double>{{25, 157000.0},
                                                                               {27, 44000.0},
                                                                               {28, 94000.0},
                                                                               {29, 58000.0},
                                                                               {30, 194000.0},
                                                                               {33, 139000.0},
                                                                               {35, 150000.0}}));
}

TYPED_TEST(AggregateTest, HavingLogicalOnAggregateStatement) {
    this->insert_test_data();
    auto result = this->qs->template group_by<fields::Person.age>()
                      .template avg<fields::Person.salary>()
                      .having(fields::Person.age >= 25 && fields::Person.age <= 40)
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING AND on AggregateStatement failed: " << result.error().message();

    // age -> AVG(salary) = group salary sum / head-count
    const std::map<int, double> expected{{25, 157000.0 / 3}, {27, 44000.0}, {28, 47000.0}, {29, 58000.0},
                                         {30, 194000.0 / 3}, {33, 69500.0}, {35, 75000.0}, {36, 76000.0},
                                         {38, 83500.0},      {40, 91000.0}};
    std::map<int, double> actual;
    for (const auto &[age, avg_salary] : result.value()) {
        ASSERT_TRUE(avg_salary.has_value()) << "age " << age;
        actual.emplace(age, *avg_salary);
    }
    ASSERT_EQ(actual.size(), expected.size());
    for (const auto &[age, avg] : expected) {
        ASSERT_TRUE(actual.contains(age)) << "missing group age " << age;
        EXPECT_NEAR(actual.at(age), avg, 1e-6) << "age " << age;
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
// HAVING age > 25 keeps 12 groups: 27:1 28:2 29:1 30:3 33:2 35:2 36:1 38:2 40:2 42:1 45:2 48:1

TYPED_TEST(AggregateTest, HavingWithOrderByAndLimit) {
    this->insert_test_data();
    auto result = this->qs->template order_by<fields::Person.age>()
                      .limit(3)
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age > 25)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + ORDER BY + LIMIT failed: " << result.error().message();

    // ORDER BY age ASC then LIMIT 3 keeps the three lowest surviving groups, in order.
    const std::vector<std::pair<int, std::int64_t>> expected{{27, 1}, {28, 2}, {29, 1}};
    EXPECT_EQ(group_list_of<int>(result.value()), expected);
}

TYPED_TEST(AggregateTest, HavingWithOrderByOnly) {
    this->insert_test_data();
    auto result = this->qs->template order_by<fields::Person.age, false>()
                      .template group_by<fields::Person.age>()
                      .having(fields::Person.age > 30)
                      .count()
                      .execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + ORDER BY failed: " << result.error().message();

    const std::vector<std::pair<int, std::int64_t>> expected{{48, 1}, {45, 2}, {42, 1}, {40, 2},
                                                             {38, 2}, {36, 1}, {35, 2}, {33, 2}};
    EXPECT_EQ(group_list_of<int>(result.value()), expected);
}

TYPED_TEST(AggregateTest, HavingWithLimitOnly) {
    this->insert_test_data();
    auto result =
        this->qs->limit(2).template group_by<fields::Person.age>().having(fields::Person.age > 25).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + LIMIT failed: " << result.error().message();

    // No ORDER BY, so which two of the 12 groups come back is unspecified — but there are exactly
    // two, each satisfies HAVING, and each carries its true head-count.
    const auto groups = group_list_of<int>(result.value());
    EXPECT_EQ(groups.size(), 2U);
    for (const auto &[age, count] : groups) {
        EXPECT_GT(age, 25);
        EXPECT_EQ(count, kPeopleAgeCounts.at(age)) << "age " << age;
    }
}

TYPED_TEST(AggregateTest, HavingWithOffsetOnly) {
    this->insert_test_data();
    auto result =
        this->qs->offset(1).template group_by<fields::Person.age>().having(fields::Person.age > 25).count().execute();
    ASSERT_TRUE(result.has_value()) << "HAVING + OFFSET failed: " << result.error().message();

    // 12 groups survive HAVING; OFFSET 1 skips one (which one is unspecified without ORDER BY).
    const auto groups = group_list_of<int>(result.value());
    EXPECT_EQ(groups.size(), 11U);
    for (const auto &[age, count] : groups) {
        EXPECT_GT(age, 25);
        EXPECT_EQ(count, kPeopleAgeCounts.at(age)) << "age " << age;
    }
}

// NOLINTEND(misc-use-anonymous-namespace)
