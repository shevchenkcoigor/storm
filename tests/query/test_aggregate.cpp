#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm;
import std;

using storm::db::DefaultConnection;

using storm::QuerySet;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
#include "test_aggregate_fixture.h"

TYPED_TEST_SUITE(AggregateTest, DatabaseTypes);

// ============================================================================
// Single Aggregate Function Tests
// ============================================================================

TYPED_TEST(AggregateTest, SumMultipleFields) {
    this->insert_test_data();

    auto result = this->qs->template sum<fields::Person.age, fields::Person.years_experience>().execute();
    ASSERT_TRUE(result.has_value()) << "SUM multi-field failed: " << result.error().message();
    EXPECT_EQ(result.value(), 1064);
}

TYPED_TEST(AggregateTest, AvgMultipleFields) {
    this->insert_test_data();

    auto result = this->qs->template avg<fields::Person.age, fields::Person.years_experience>().execute();
    ASSERT_TRUE(result.has_value()) << "AVG multi-field failed: " << result.error().message();
    ASSERT_TRUE(result.value().has_value());
    EXPECT_NEAR(result.value().value(), 42.56, 0.01);
}

TYPED_TEST(AggregateTest, MinMultipleFields) {
    this->insert_test_data();

    auto result = this->qs->template min<fields::Person.age, fields::Person.years_experience>().execute();
    ASSERT_TRUE(result.has_value()) << "MIN multi-field failed: " << result.error().message();
    ASSERT_TRUE(result.value().has_value());
    EXPECT_DOUBLE_EQ(result.value().value(), 27.0);
}

TYPED_TEST(AggregateTest, MaxMultipleFields) {
    this->insert_test_data();

    auto result = this->qs->template max<fields::Person.age, fields::Person.years_experience>().execute();
    ASSERT_TRUE(result.has_value()) << "MAX multi-field failed: " << result.error().message();
    ASSERT_TRUE(result.value().has_value());
    EXPECT_DOUBLE_EQ(result.value().value(), 60.0);
}

// ============================================================================
// Multiple Aggregate Functions in One Query (direct chaining)
// ============================================================================

TYPED_TEST(AggregateTest, DirectChain_TypeSafety) {
    this->insert_test_data();

    auto result = this->qs->template sum<fields::Person.age>().count().execute();
    ASSERT_TRUE(result.has_value());
    static_assert(
            std::is_same_v<std::remove_reference_t<decltype(result.value())>, std::tuple<std::int64_t, std::int64_t>>,
            "Multiple aggregates should return tuple"
    );
}

TYPED_TEST(AggregateTest, SingleAggregates_WithWhereAndJoin) {
    this->insert_join_test_data();

    auto sum_result = this->msg_qs->where(fields::Message.value > 25)
                              .template join<fields::Message.sender>()
                              .template sum<fields::Message.value>()
                              .execute();
    ASSERT_TRUE(sum_result.has_value()) << "SUM with WHERE+JOIN failed: " << sum_result.error().message();
    EXPECT_EQ(sum_result.value(), 180);

    (*this->msg_qs).reset();

    auto count_result =
            this->msg_qs->where(fields::Message.value > 25).template join<fields::Message.sender>().count().execute();
    ASSERT_TRUE(count_result.has_value()) << "COUNT with WHERE+JOIN failed: " << count_result.error().message();
    EXPECT_EQ(count_result.value(), 4);

    (*this->msg_qs).reset();

    auto min_result = this->msg_qs->where(fields::Message.value > 25)
                              .template join<fields::Message.sender>()
                              .template min<fields::Message.value>()
                              .execute();
    ASSERT_TRUE(min_result.has_value()) << "MIN with WHERE+JOIN failed: " << min_result.error().message();
    ASSERT_TRUE(min_result.value().has_value());
    EXPECT_DOUBLE_EQ(min_result.value().value(), 30.0);
}

TYPED_TEST(AggregateTest, SingleRow_AllAggregates) {
    auto insert_result =
            this->qs->insert(Person{.id = 0, .name = "Alice", .age = 25, .salary = 50000.0, .years_experience = 3})
                    .execute();
    ASSERT_TRUE(insert_result.has_value());

    auto sum = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum.has_value());
    EXPECT_EQ(sum.value(), 25);

    auto count = this->qs->count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 1);

    auto avg = this->qs->template avg<fields::Person.age>().execute();
    ASSERT_TRUE(avg.has_value());
    ASSERT_TRUE(avg.value().has_value());
    EXPECT_DOUBLE_EQ(avg.value().value(), 25.0);

    auto min_val = this->qs->template min<fields::Person.age>().execute();
    ASSERT_TRUE(min_val.has_value());
    ASSERT_TRUE(min_val.value().has_value());
    EXPECT_DOUBLE_EQ(min_val.value().value(), 25.0);

    auto max_val = this->qs->template max<fields::Person.age>().execute();
    ASSERT_TRUE(max_val.has_value());
    ASSERT_TRUE(max_val.value().has_value());
    EXPECT_DOUBLE_EQ(max_val.value().value(), 25.0);
}

// ============================================================================
// Large Dataset Tests
// ============================================================================

TYPED_TEST(AggregateTest, LargeDataset_Sum) {
    std::vector<Person> people;
    people.reserve(1000);
    for (int i = 1; i <= 1000; ++i) {
        people.push_back(
                Person{.id = 0, .name = std::format("Person{}", i), .age = i, .salary = 50000.0, .years_experience = 5}
        );
    }
    auto batch_result = this->qs->insert(std::span<const Person>(people)).execute();
    ASSERT_TRUE(batch_result.has_value()) << "Batch insert failed: " << batch_result.error().message();

    auto result = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << "SUM large dataset failed: " << result.error().message();
    EXPECT_EQ(result.value(), 500500);
}

TYPED_TEST(AggregateTest, LargeDataset_Count) {
    std::vector<Person> people;
    people.reserve(10000);
    for (int i = 1; i <= 10000; ++i) {
        people.push_back(
                Person{.id = 0, .name = std::format("Person{}", i), .age = 25, .salary = 50000.0, .years_experience = 5}
        );
    }
    auto insert_result = this->qs->insert(std::span<const Person>(people)).execute();
    ASSERT_TRUE(insert_result.has_value()) << "Batch insert failed: " << insert_result.error().message();

    auto result = this->qs->count().execute();
    ASSERT_TRUE(result.has_value()) << "COUNT large dataset failed: " << result.error().message();
    EXPECT_EQ(result.value(), 10000);
}

// ============================================================================
// Type Safety Tests
// ============================================================================

TYPED_TEST(AggregateTest, TypeSafety_IntegerResult) {
    this->insert_test_data();

    auto result = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());
    static_assert(
            std::is_same_v<std::remove_reference_t<decltype(result.value())>, std::int64_t>, "SUM should return int64_t"
    );
}

TYPED_TEST(AggregateTest, TypeSafety_OptionalDoubleResult) {
    this->insert_test_data();

    auto result = this->qs->template avg<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value());
    static_assert(
            std::is_same_v<std::remove_reference_t<decltype(result.value())>, std::optional<double>>,
            "AVG should return std::optional<double> (#416)"
    );
}

// ============================================================================
// SUM over floating-point columns — must NOT truncate (#420)
// ============================================================================

TYPED_TEST(AggregateTest, SumDoubleField_ReturnsDouble) {
    auto result = this->qs->template sum<fields::Person.salary>().execute();
    ASSERT_TRUE(result.has_value());
    static_assert(
            std::is_same_v<std::remove_reference_t<decltype(result.value())>, double>,
            "SUM over a double column should return double"
    );
}

TYPED_TEST(AggregateTest, SumDoubleField_PreservesFraction) {
    ASSERT_TRUE(this->qs->insert(Person{.id = 0, .name = "A", .age = 1, .salary = 100.25}).execute().has_value());
    ASSERT_TRUE(this->qs->insert(Person{.id = 0, .name = "B", .age = 1, .salary = 200.50}).execute().has_value());

    auto result = this->qs->template sum<fields::Person.salary>().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result.value(), 300.75);
}

TYPED_TEST(AggregateTest, SumMultiField_PromotesToDoubleIfAnyFloating) {
    ASSERT_TRUE(this->qs->insert(Person{.id = 0, .name = "A", .age = 10, .salary = 100.50}).execute().has_value());
    ASSERT_TRUE(this->qs->insert(Person{.id = 0, .name = "B", .age = 20, .salary = 200.25}).execute().has_value());

    auto result = this->qs->template sum<fields::Person.age, fields::Person.salary>().execute();
    ASSERT_TRUE(result.has_value());
    static_assert(
            std::is_same_v<std::remove_reference_t<decltype(result.value())>, double>,
            "SUM(int + double) should promote to double"
    );
    // 10 + 20 + 100.50 + 200.25 = 330.75
    EXPECT_DOUBLE_EQ(result.value(), 330.75);
}

// ============================================================================
// Statement Caching Tests
// ============================================================================

TYPED_TEST(AggregateTest, StatementCaching_RepeatedQueries) {
    this->insert_test_data();

    for (int i = 0; i < 100; ++i) {
        auto result = this->qs->count().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed";
        EXPECT_EQ(result.value(), 25);
    }
}

TYPED_TEST(AggregateTest, StatementCaching_DifferentAggregates) {
    this->insert_test_data();

    for (int i = 0; i < 10; ++i) {
        auto sum = this->qs->template sum<fields::Person.age>().execute();
        ASSERT_TRUE(sum.has_value());
        EXPECT_EQ(sum.value(), 829);

        auto count = this->qs->count().execute();
        ASSERT_TRUE(count.has_value());
        EXPECT_EQ(count.value(), 25);

        auto avg = this->qs->template avg<fields::Person.salary>().execute();
        ASSERT_TRUE(avg.has_value());
        ASSERT_TRUE(avg.value().has_value());
        EXPECT_NEAR(avg.value().value(), 68080.0, 0.01);
    }
}

// ============================================================================
// Integration with Other ORM Features
// ============================================================================

TYPED_TEST(AggregateTest, Integration_AfterInsert) {
    for (int i = 1; i <= 5; ++i) {
        auto insert_result = this->qs->insert(
                                             Person{.id               = 0,
                                                    .name             = std::format("Person{}", i),
                                                    .age              = i * 10,
                                                    .salary           = 50000.0,
                                                    .years_experience = 5}
        )
                                     .execute();
        ASSERT_TRUE(insert_result.has_value());

        auto count = this->qs->count().execute();
        ASSERT_TRUE(count.has_value());
        EXPECT_EQ(count.value(), i);
    }

    auto sum = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum.has_value());
    EXPECT_EQ(sum.value(), 10 + 20 + 30 + 40 + 50);
}

TYPED_TEST(AggregateTest, Integration_AfterUpdate) {
    this->insert_test_data();

    auto people = this->qs->select().execute();
    ASSERT_TRUE(people.has_value());

    for (auto& person : people.value()) {
        person.age         = 30;
        auto update_result = this->qs->update(person).execute();
        ASSERT_TRUE(update_result.has_value());
    }

    auto sum = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum.has_value());
    EXPECT_EQ(sum.value(), 750);
}

TYPED_TEST(AggregateTest, Integration_AfterDelete) {
    this->insert_test_data();

    auto people = this->qs->select().execute();
    ASSERT_TRUE(people.has_value());

    auto it            = people.value().begin();
    auto delete_result = this->qs->erase(*it).execute();
    ASSERT_TRUE(delete_result.has_value());
    ++it;
    delete_result = this->qs->erase(*it).execute();
    ASSERT_TRUE(delete_result.has_value());

    auto count = this->qs->count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 23);
}

// ============================================================================
// WHERE + Aggregate Tests
// ============================================================================

TYPED_TEST(AggregateTest, WhereWithMultiFieldSum) {
    this->insert_test_data();

    auto result = this->qs->where(fields::Person.age >= 35)
                          .template sum<fields::Person.age, fields::Person.years_experience>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + SUM(multi-field) failed: " << result.error().message();
    EXPECT_EQ(result.value(), 582);
}

TYPED_TEST(AggregateTest, WhereRepeatedQueries) {
    this->insert_test_data();

    for (int i = 0; i < 100; ++i) {
        auto result = this->qs->where(fields::Person.age > 30).count().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed: " << result.error().message();
        EXPECT_EQ(result.value(), 13);
    }
}

TYPED_TEST(AggregateTest, WhereDifferentConditions) {
    this->insert_test_data();

    auto result1 = this->qs->where(fields::Person.age > 30).count().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value(), 13);

    (*this->qs).reset();

    auto result2 = this->qs->where(fields::Person.age < 30).count().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value(), 9);

    (*this->qs).reset();

    auto result3 = this->qs->where(fields::Person.age == 30).count().execute();
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3.value(), 3);
}

// ============================================================================
// JOIN + Aggregate Tests
// ============================================================================

TYPED_TEST(AggregateTest, WhereJoinRepeatedQueries) {
    this->insert_join_test_data();

    for (int i = 0; i < 50; ++i) {
        auto result = this->msg_qs->where(fields::Message.value > 20)
                              .template join<fields::Message.sender>()
                              .count()
                              .execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed: " << result.error().message();
        EXPECT_EQ(result.value(), 4);
    }
}

// Issue #269: regression for the use-after-free of the per-aggregate-type
// thread-local Statement* cache in AggregateStatement::execute_simple().
// Tearing down the default connection invalidated all cached Statement
// pointers; when a fresh connection was allocated at the same address, the
// fast-path identity check passed and reset() was called on freed memory.
// Surfaced as an infinite loop under coverage instrumentation and ASan
// (the freed vector bookkeeping survived just intact enough to enter
// clear() with __end_ < __begin_).
//
// Loops many recycle cycles so the system allocator eventually reuses
// the connection address; on broken code the suite hangs around iteration
// 1-2 instead of completing in <1s.
namespace {
    template <typename ConnType, typename PersonQs, typename MessageQs>
    auto recycle_default_connection(PersonQs& person_qs, MessageQs& message_qs) -> bool {
        person_qs  = nullptr;
        message_qs = nullptr;
        if constexpr (storm::test::is_postgresql<ConnType>()) {
            storm::test::rollback_test_txn<ConnType>(DefaultConnection<ConnType>::get());
        }
        DefaultConnection<ConnType>::clear();
        auto result = DefaultConnection<ConnType>::set(storm::test::get_connection_string<ConnType>());
        if (!result.has_value()) {
            return false;
        }
        const auto& conn = DefaultConnection<ConnType>::get();
        storm::test::pg_schema_init<ConnType>(conn);
        if (!storm::test::ensure_tables<ConnType, Person, Message>(conn)) {
            return false;
        }
        person_qs  = std::make_unique<QuerySet<Person, ConnType>>();
        message_qs = std::make_unique<QuerySet<Message, ConnType>>();
        return true;
    }
} // namespace

TYPED_TEST(AggregateTest, CountSurvivesConnectionRecycling) {
    for (int cycle = 0; cycle < 12; ++cycle) {
        if (cycle > 0) {
            ASSERT_TRUE((recycle_default_connection<TypeParam>(this->qs, this->msg_qs)))
                    << "cycle " << cycle << ": recycle failed";
        }
        this->insert_test_data();
        auto count = this->qs->count().execute();
        ASSERT_TRUE(count.has_value()) << "cycle " << cycle << " count failed: " << count.error().message();
        EXPECT_EQ(count.value(), 25) << "cycle " << cycle;
    }
}
