#include <gtest/gtest.h>
#include "plf_hive/plf_hive.h"
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness,cppcoreguidelines-rvalue-reference-param-not-moved)

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import std;

#include "../../shared/models/people_25.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::orm::statements::SelectStatement;

template <typename ConnType> class ExpectedTransformTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end())
        )));
    }
};

TYPED_TEST_SUITE(ExpectedTransformTest, DatabaseTypes);

// The recommended idiom pairs `std::expected::transform` with `std::views::as_rvalue`
// so `std::ranges::to` move-constructs elements from the source hive instead of copying.
// Without `as_rvalue`, the hive's iterators yield `Person&` and elements get deep-copied.

// Compile-time proof that `as_rvalue` turns hive iteration into rvalue references.
static_assert(
        std::is_rvalue_reference_v<
                std::ranges::range_reference_t<decltype(std::declval<plf::hive<Person>&&>() | std::views::as_rvalue)>>,
        "as_rvalue over plf::hive must yield rvalue references for move semantics"
);

TYPED_TEST(ExpectedTransformTest, HiveToVectorViaTransformMoves) {
    auto vec_result = (SelectStatement<Person, TypeParam>{}).all().execute().transform([](auto&& hive) {
        return std::forward<decltype(hive)>(hive) | std::views::as_rvalue | std::ranges::to<std::vector<Person>>();
    });

    ASSERT_TRUE(vec_result.has_value()) << "Query failed: " << vec_result.error().message();
    EXPECT_EQ(vec_result.value().size(), 25U);
}

TYPED_TEST(ExpectedTransformTest, EmptyResultTransformYieldsEmptyVector) {
    auto vec_result = (SelectStatement<Person, TypeParam>{})
                              .where(fields::Person.age > 10000)
                              .execute()
                              .transform([](auto&& hive) {
                                  return std::forward<decltype(hive)>(hive) | std::views::as_rvalue |
                                         std::ranges::to<std::vector<Person>>();
                              });

    ASSERT_TRUE(vec_result.has_value()) << "Query failed: " << vec_result.error().message();
    EXPECT_TRUE(vec_result.value().empty());
}

TYPED_TEST(ExpectedTransformTest, ChainViewsInsideTransformMoves) {
    auto names_result = (SelectStatement<Person, TypeParam>{}).all().execute().transform([](auto&& hive) {
        return std::forward<decltype(hive)>(hive) | std::views::as_rvalue |
               std::views::transform([](Person&& p) -> std::string&& { return std::move(p.name); }) |
               std::ranges::to<std::vector<std::string>>();
    });

    ASSERT_TRUE(names_result.has_value()) << "Query failed: " << names_result.error().message();
    EXPECT_EQ(names_result.value().size(), 25U);
    for (const auto& name : names_result.value()) {
        EXPECT_FALSE(name.empty());
    }
}

TYPED_TEST(ExpectedTransformTest, TransformPreservesWhereFilter) {
    auto vec_result =
            (SelectStatement<Person, TypeParam>{}).where(fields::Person.age > 35).execute().transform([](auto&& hive) {
                return std::forward<decltype(hive)>(hive) | std::views::as_rvalue |
                       std::ranges::to<std::vector<Person>>();
            });

    ASSERT_TRUE(vec_result.has_value()) << "Query failed: " << vec_result.error().message();
    EXPECT_GT(vec_result.value().size(), 0U);
    for (const auto& p : vec_result.value()) {
        EXPECT_GT(p.age, 35);
    }
}

// Error propagation: build a pre-failed expected and verify transform() skips the lambda.
TYPED_TEST(ExpectedTransformTest, TransformPropagatesErrorWithoutInvokingLambda) {
    using Error = typename TypeParam::Error;

    std::expected<plf::hive<Person>, Error> failed{std::unexpect, Error{42, "synthetic failure"}};

    bool lambda_invoked = false;
    auto result         = std::move(failed).transform([&lambda_invoked](auto&& hive) {
        lambda_invoked = true;
        return std::forward<decltype(hive)>(hive) | std::views::as_rvalue | std::ranges::to<std::vector<Person>>();
    });

    EXPECT_FALSE(lambda_invoked);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), 42);
    EXPECT_EQ(result.error().message(), "synthetic failure");
}

// NOLINTEND(misc-const-correctness,cppcoreguidelines-rvalue-reference-param-not-moved)
