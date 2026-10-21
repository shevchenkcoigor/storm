#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness,performance-unnecessary-value-param,performance-unnecessary-copy-initialization)

import storm;
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/people_25.h"
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
#include "test_select_runner.h"
using storm::orm::utilities::Collate;

using storm::QuerySet;
using storm::orm::where::ComparisonExpr;
using storm::orm::where::CompOp;
using storm::orm::where::Expr;
using storm::orm::where::ExpressionVariant;

// Test fixture for WHERE operations — templated on database backend
template <typename ConnType> class WhereTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end())
        )));
    }

    static auto check_where_count(auto expr, std::size_t expected_count) -> void {
        QuerySet<Person, ConnType> qs;
        auto                       result = qs.where(expr).select().execute();
        ASSERT_TRUE(result.has_value()) << "WHERE failed: " << result.error().message();
        ASSERT_EQ(result.value().size(), expected_count);
    }
};

TYPED_TEST_SUITE(WhereTest, DatabaseTypes);

// Test: WHERE with three conditions
TYPED_TEST(WhereTest, WhereThreeConditions) {
    QuerySet<Person, TypeParam> queryset;

    auto expr1  = fields::Person.age >= 28;
    auto expr2  = fields::Person.age <= 35;
    auto expr3  = fields::Person.name != "Charlie";
    auto result = queryset.where(expr1 && expr2 && expr3).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE failed: " << result.error().message();

    const auto& people = result.value();
    ASSERT_EQ(people.size(), 9) << "Expected 9 people matching all three conditions";
}

// Test: WHERE with complex expression
TYPED_TEST(WhereTest, WhereComplexExpression) {
    this->check_where_count(
            (fields::Person.age < 30 || fields::Person.age > 35) && fields::Person.name != "Charlie", 18
    );
}

// Test: where() returns a new QuerySet; original is unchanged; returned copy is reusable
TYPED_TEST(WhereTest, WhereReturnsCopyReusable) {
    QuerySet<Person, TypeParam> queryset;

    // where() returns a new QuerySet — original unchanged
    auto filtered = queryset.where(fields::Person.age >= 25);

    auto result1 = filtered.select().execute();
    ASSERT_TRUE(result1.has_value());
    ASSERT_EQ(result1.value().size(), 23) << "Expected 23 people with age >= 25";

    // Returned copy is reusable (state preserved after select)
    auto result2 = filtered.select().execute();
    ASSERT_TRUE(result2.has_value());
    ASSERT_EQ(result2.value().size(), 23) << "State should persist after first select()";

    // Chaining further narrows without affecting the original filtered copy
    auto narrower = filtered.where(fields::Person.age < 40);
    auto result3  = narrower.select().execute();
    ASSERT_TRUE(result3.has_value());
    ASSERT_EQ(result3.value().size(), 17) << "Expected 17 people with age >= 25 AND age < 40";

    // Original queryset is still unfiltered
    auto all = queryset.select().execute();
    ASSERT_TRUE(all.has_value());
    ASSERT_EQ(all.value().size(), 25) << "Original should be unfiltered";
}

// Test fixture for WHERE + JOIN operations — templated on database backend
template <typename ConnType> class WhereJoinTest : public StormTestFixture<Message, ConnType, Person> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        // Insert users (use ID 0 to let the PRIMARY KEY auto-generate IDs)
        QuerySet<Person, ConnType> user_qs;
        auto                       alice_id = user_qs.insert(Person{.id = 0, .name = "alice", .age = 10}).execute();
        ASSERT_TRUE(alice_id.has_value()) << "Failed to insert alice: " << alice_id.error().message();

        auto bob_id = user_qs.insert(Person{.id = 0, .name = "bob", .age = 5}).execute();
        ASSERT_TRUE(bob_id.has_value()) << "Failed to insert bob: " << bob_id.error().message();

        auto charlie_id = user_qs.insert(Person{.id = 0, .name = "charlie", .age = 15}).execute();
        ASSERT_TRUE(charlie_id.has_value()) << "Failed to insert charlie: " << charlie_id.error().message();

        // Insert messages (use generated user IDs)
        QuerySet<Message, ConnType> msg_qs;
        auto                        msg1 = msg_qs.insert(
                                  Message{.id      = 0,
                                                                 .content = "Hello from Alice",
                                                                 .sender = Person{.id = static_cast<int>(alice_id.value())}}
        ).execute();
        ASSERT_TRUE(msg1.has_value()) << "Failed to insert message 1: " << msg1.error().message();

        auto msg2 = msg_qs.insert(
                                  Message{.id      = 0,
                                          .content = "Hi from Bob",
                                          .sender  = Person{.id = static_cast<int>(bob_id.value())}}
        ).execute();
        ASSERT_TRUE(msg2.has_value()) << "Failed to insert message 2: " << msg2.error().message();

        auto msg3 = msg_qs.insert(
                                  Message{.id      = 0,
                                          .content = "Greetings from Alice",
                                          .sender  = Person{.id = static_cast<int>(alice_id.value())}}
        ).execute();
        ASSERT_TRUE(msg3.has_value()) << "Failed to insert message 3: " << msg3.error().message();

        auto msg4 = msg_qs.insert(
                                  Message{.id      = 0,
                                          .content = "Greetings Charlie",
                                          .sender  = Person{.id = static_cast<int>(charlie_id.value())}}
        ).execute();
        ASSERT_TRUE(msg4.has_value()) << "Failed to insert message 4: " << msg4.error().message();
    }
};

TYPED_TEST_SUITE(WhereJoinTest, DatabaseTypes);

// Test: WHERE with JOIN
TYPED_TEST(WhereJoinTest, WhereWithJoin) {
    QuerySet<Message, TypeParam> queryset;

    // SELECT with JOIN and WHERE filtering by Person.age
    auto result = queryset.template join<fields::Message.sender>().where(fields::Person.age >= 10).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + JOIN failed: " << result.error().message();

    const auto& messages = result.value();
    ASSERT_EQ(messages.size(), 3) << "Expected 3 messages from users with age >= 10";

    // Verify sender information is populated
    auto it = messages.begin();
    EXPECT_EQ(it->sender.name, "alice");
    EXPECT_EQ(it->sender.age, 10);
    ++it;
    EXPECT_EQ(it->sender.name, "alice");
    ++it;
    EXPECT_EQ(it->sender.name, "charlie");
    EXPECT_EQ(it->sender.age, 15);
}

// WhereWithJoinContentFilter: migrated to unified_cases.yaml (select_join_where_like_H)

// Test: WHERE with JOIN and multiple conditions
TYPED_TEST(WhereJoinTest, WhereWithJoinMultipleConditions) {
    QuerySet<Message, TypeParam> queryset;

    auto expr1  = fields::Person.age > 5;
    auto expr2  = fields::Message.content.like("%from%");
    auto result = queryset.template join<fields::Message.sender>().where(expr1 && expr2).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + JOIN failed: " << result.error().message();

    const auto& messages = result.value();
    ASSERT_EQ(messages.size(), 2) << "Expected 2 messages matching both conditions";
}

// Test: Natural && and || operators (and/or keywords)
TYPED_TEST(WhereJoinTest, WhereWithNaturalOperators) {
    QuerySet<Message, TypeParam> queryset;

    // Using natural 'and' operator (also works with &&)
    auto result = queryset.template join<fields::Message.sender>()
                          .where(fields::Person.age > 5 and fields::Message.content.like("%from%"))
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + JOIN failed: " << result.error().message();

    const auto& messages = result.value();
    ASSERT_EQ(messages.size(), 2) << "Expected 2 messages matching both conditions";
    auto it = messages.begin();
    EXPECT_EQ(it->sender.name, "alice");
    ++it;
    EXPECT_EQ(it->sender.name, "alice");
}

// Test: Natural operators with complex expressions (same logic as WhereComplexExpression, different syntax)
TYPED_TEST(WhereTest, WhereNaturalOperatorsComplex) {
    this->check_where_count(
            (fields::Person.age < 30 or fields::Person.age > 35) and fields::Person.name != "Charlie", 18
    );
}

// Test: Reusing WHERE conditions across multiple queries
TYPED_TEST(WhereTest, WhereReuseCondition) {
    // Store condition in variable
    auto age_condition = fields::Person.age > 25;

    // First use - should return 20 people with age > 25
    QuerySet<Person, TypeParam> queryset1;
    auto                        result1 = queryset1.where(age_condition).select().execute();
    ASSERT_TRUE(result1.has_value()) << "First WHERE failed: " << result1.error().message();
    EXPECT_EQ(result1.value().size(), 20) << "Expected 20 people with age > 25";

    // Reuse same condition with fresh QuerySet - should return same results
    QuerySet<Person, TypeParam> queryset2;
    auto                        result2 = queryset2.where(age_condition).select().execute();
    ASSERT_TRUE(result2.has_value()) << "Second WHERE failed: " << result2.error().message();
    EXPECT_EQ(result2.value().size(), 20) << "Expected 20 people with age > 25 (reused condition)";

    // Combine reused condition with new one
    QuerySet<Person, TypeParam> queryset3;
    auto result3 = queryset3.where(age_condition and fields::Person.name == "Bob").select().execute();
    ASSERT_TRUE(result3.has_value()) << "Combined WHERE failed: " << result3.error().message();
    ASSERT_EQ(result3.value().size(), 1) << "Expected 1 person matching both conditions";
    auto it = result3.value().begin();
    EXPECT_EQ(it->name, "Bob");
    EXPECT_EQ(it->age, 30);

    // Reuse the original condition again with fresh QuerySet
    QuerySet<Person, TypeParam> queryset4;
    auto                        result4 = queryset4.where(age_condition).select().execute();
    ASSERT_TRUE(result4.has_value()) << "Third WHERE failed: " << result4.error().message();
    EXPECT_EQ(result4.value().size(), 20) << "Expected 20 people with age > 25 (reused after combining)";
}

// Test: Building composable filter library
TYPED_TEST(WhereTest, WhereComposableFilters) {
    // Create reusable filter components
    auto young_filter       = fields::Person.age < 30;
    auto old_filter         = fields::Person.age >= 35;
    auto name_starts_with_a = fields::Person.name.like("A%");

    // Use filters independently with fresh QuerySets
    QuerySet<Person, TypeParam> qs1;
    auto                        young_people = qs1.where(young_filter).select().execute();
    ASSERT_TRUE(young_people.has_value());
    EXPECT_EQ(young_people.value().size(), 9) << "Expected 9 young people (age < 30)";

    QuerySet<Person, TypeParam> qs2;
    auto                        old_people = qs2.where(old_filter).select().execute();
    ASSERT_TRUE(old_people.has_value());
    EXPECT_EQ(old_people.value().size(), 11) << "Expected 11 old people (age >= 35)";

    // Combine filters
    QuerySet<Person, TypeParam> qs3;
    auto                        young_or_old = qs3.where(young_filter || old_filter).select().execute();
    ASSERT_TRUE(young_or_old.has_value());
    EXPECT_EQ(young_or_old.value().size(), 20) << "Expected 20 people (young or old)";

    // Complex composition
    QuerySet<Person, TypeParam> qs4;
    auto                        young_a_names = qs4.where(young_filter && name_starts_with_a).select().execute();
    ASSERT_TRUE(young_a_names.has_value());
    EXPECT_EQ(young_a_names.value().size(), 1) << "Expected 1 person (Alice, age 25, starts with A)";

    // Reuse filters in different combinations
    QuerySet<Person, TypeParam> qs5;
    auto                        old_a_names = qs5.where(old_filter && name_starts_with_a).select().execute();
    ASSERT_TRUE(old_a_names.has_value());
    EXPECT_EQ(old_a_names.value().size(), 0) << "Expected 0 people (no old people with A names)";
}

// Test: Reusable base QuerySet pattern — where() returns independent copies
TYPED_TEST(WhereTest, ReusableBaseQuerySet) {
    QuerySet<Person, TypeParam> queryset;

    // Create a base filtered QuerySet (age >= 25)
    auto base = queryset.where(fields::Person.age >= 25);

    // Reuse base filter multiple times
    auto result1 = base.select().execute();
    ASSERT_TRUE(result1.has_value());
    ASSERT_EQ(result1.value().size(), 23) << "Expected 23 people with age >= 25";

    // Still has base filter on second call
    auto result2 = base.select().execute();
    ASSERT_TRUE(result2.has_value());
    ASSERT_EQ(result2.value().size(), 23) << "Base filter should persist";

    // Refine the base filter — creates new copy, base unchanged
    auto adults = base.where(fields::Person.age >= 30).select().execute();
    ASSERT_TRUE(adults.has_value());
    ASSERT_EQ(adults.value().size(), 16) << "Expected 16 people (age >= 25 AND age >= 30)";

    // Base is still unchanged
    auto result3 = base.select().execute();
    ASSERT_TRUE(result3.has_value());
    ASSERT_EQ(result3.value().size(), 23) << "Base should be unchanged after refinement";
}

// Test: Building query progressively — each where() returns a narrower copy
TYPED_TEST(WhereTest, ProgressiveQueryBuilding) {
    QuerySet<Person, TypeParam> queryset;

    // Start with broad filter
    auto q1      = queryset.where(fields::Person.age >= 25);
    auto result1 = q1.select().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value().size(), 23) << "23 people have age >= 25";

    // Narrow it down
    auto q2      = q1.where(fields::Person.age < 35);
    auto result2 = q2.select().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 12) << "Expected 12 people (25 <= age < 35)";

    // Add another condition (names starting with certain letter)
    auto q3      = q2.where(fields::Person.name.like("D%"));
    auto result3 = q3.select().execute();
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3.value().size(), 1) << "Expected 1 person matching all conditions (Diana)";
    EXPECT_EQ(result3.value().begin()->name, "Diana");

    // All previous stages remain unchanged
    EXPECT_EQ(q1.select().execute().value().size(), 23);
    EXPECT_EQ(q2.select().execute().value().size(), 12);
}

// Test: chaining where() builds complex filters without mutating the original
TYPED_TEST(WhereTest, ChainingBuildsComplexFilter) {
    QuerySet<Person, TypeParam> queryset;

    // Build up complex filter via chaining
    auto filtered = queryset.where(fields::Person.age >= 25)
                            .where(fields::Person.age < 40)
                            .where(fields::Person.name.like("D%"));

    auto result = filtered.select().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 1) << "Complex filter should match Diana only";
    EXPECT_EQ(result.value().begin()->name, "Diana");

    // Original is unchanged
    auto all_people = queryset.select().execute();
    ASSERT_TRUE(all_people.has_value());
    EXPECT_EQ(all_people.value().size(), 25) << "Original should have all rows";

    // Can create a different filter from the same base
    auto age25 = queryset.where(fields::Person.age == 25).select().execute();
    ASSERT_TRUE(age25.has_value());
    EXPECT_EQ(age25.value().size(), 3) << "Different filter from same base (Alice, Grace, Karen)";
}

// Test: where() does not mutate the original QuerySet (returns new copy)
TYPED_TEST(WhereTest, WhereDoesNotMutateOriginal) {
    QuerySet<Person, TypeParam> queryset;

    // Call where() — original should be unchanged
    auto filtered = queryset.where(fields::Person.age > 30);
    auto all      = queryset.select().execute();
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all.value().size(), 25) << "Original QuerySet must not be mutated by where()";

    // The returned copy should have the filter
    auto result = filtered.select().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 13) << "Returned QuerySet should have the WHERE filter";
}

// Test: where() in a loop works without reset()
TYPED_TEST(WhereTest, WhereInLoopWithoutReset) {
    QuerySet<Person, TypeParam> queryset;

    for (int i = 0; i < 10; ++i) {
        auto result = queryset.where(fields::Person.age > 30).count().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed: " << result.error().message();
        EXPECT_EQ(result.value(), 13) << "Each iteration should return 13 (no accumulation)";
    }

    // Original should still be unfiltered
    auto all = queryset.select().execute();
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all.value().size(), 25) << "Original QuerySet must remain unfiltered after loop";
}

// Test: chaining where() returns progressively narrowed copies
TYPED_TEST(WhereTest, WhereChainingReturnsNewCopies) {
    QuerySet<Person, TypeParam> queryset;

    auto q1 = queryset.where(fields::Person.age >= 25);
    auto q2 = q1.where(fields::Person.age < 35);

    // Original, q1, q2 should all be independent
    auto all = queryset.select().execute();
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all.value().size(), 25) << "Original unchanged";

    auto r1 = q1.select().execute();
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value().size(), 23) << "q1: age >= 25";

    auto r2 = q2.select().execute();
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value().size(), 12) << "q2: age >= 25 AND age < 35";
}

// ============================================================================
// WHERE Expression Coverage Tests
// Tests for specific code paths in where.cppm
// ============================================================================

// Test: InExpression with empty values generates "1 = 0" (always false)
TYPED_TEST(WhereTest, WhereInEmptyValuesReturnsFalse) {
    QuerySet<Person, TypeParam> queryset;

    // .in() with no arguments creates InExpression with empty vector → "1 = 0"
    auto result = queryset.where(fields::Person.age.in()).select().execute();
    ASSERT_TRUE(result.has_value()) << "Empty IN should succeed";
    EXPECT_TRUE(result.value().empty()) << "Empty IN (1=0) should match nothing";
}

// Test: Expr::get() accessor
TYPED_TEST(WhereTest, ExprGetReturnsValidPointer) {
    auto expr = fields::Person.age > 25;
    auto ptr  = expr.get();
    EXPECT_NE(ptr, nullptr) << "Expr::get() should return a valid pointer";
}

// Test: Explicit Expr(ExpressionVariant&&) constructor
TYPED_TEST(WhereTest, ExprDirectConstructionFromVariant) {
    // Construct ExpressionVariant directly and wrap in Expr using the explicit rvalue constructor
    ExpressionVariant variant(ComparisonExpr<int>{std::string("age"), CompOp::Greater, 25});
    Expr              direct_expr(std::move(variant));

    // Verify it works by converting to SQL via get()
    auto sql = to_sql(*direct_expr.get());
    EXPECT_EQ(sql, "age > ?");

    // Also verify it works with QuerySet::where()
    QuerySet<Person, TypeParam> queryset;
    auto                        result = queryset.where(direct_expr).select().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 20) << "Should find 20 people with age > 25";
}

// ============================================================================
// IS NULL / IS NOT NULL Tests
// ============================================================================

// Test: field.is_null() — method syntax
TYPED_TEST(WhereTest, IsNull_MethodSyntax) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.where(fields::Person.score.is_null()).select().execute();
    ASSERT_TRUE(result.has_value()) << "IS NULL failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 10) << "Expected 10 people with NULL score";
}

// Test: field.is_not_null() — method syntax
TYPED_TEST(WhereTest, IsNotNull_MethodSyntax) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.where(fields::Person.score.is_not_null()).select().execute();
    ASSERT_TRUE(result.has_value()) << "IS NOT NULL failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 15) << "Expected 15 people with non-NULL score";
}

// Test: field == std::nullopt — operator syntax
TYPED_TEST(WhereTest, IsNull_NulloptSyntax) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.where(fields::Person.score == std::nullopt).select().execute();
    ASSERT_TRUE(result.has_value()) << "== nullopt failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 10) << "Expected 10 people with NULL score (nullopt syntax)";
}

// Test: field != std::nullopt — operator syntax
TYPED_TEST(WhereTest, IsNotNull_NulloptSyntax) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.where(fields::Person.score != std::nullopt).select().execute();
    ASSERT_TRUE(result.has_value()) << "!= nullopt failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 15) << "Expected 15 people with non-NULL score (nullopt syntax)";
}

// Test: IS NULL on optional<string> field
TYPED_TEST(WhereTest, IsNull_StringField) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.where(fields::Person.nickname.is_null()).select().execute();
    ASSERT_TRUE(result.has_value()) << "IS NULL on nickname failed: " << result.error().message();
    // Count people with nickname = std::nullopt in PEOPLE_25
    std::size_t expected_null_nicknames = 0;
    for (const auto& p : storm::test::PEOPLE_25) {
        if (!p.nickname.has_value()) {
            expected_null_nicknames++;
        }
    }
    EXPECT_EQ(result.value().size(), expected_null_nicknames)
            << "Expected " << expected_null_nicknames << " people with NULL nickname";
}

// Test: IS NULL AND value comparison
TYPED_TEST(WhereTest, IsNull_AndCombination) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.where(fields::Person.score.is_null() && fields::Person.age > 30).select().execute();
    ASSERT_TRUE(result.has_value()) << "IS NULL AND failed: " << result.error().message();
    // NULL score AND age > 30: Charlie(35), Eve(40), Henry(33), Jack(38), Olivia(48), Quinn(30→no), Sam(40) = 6
    std::size_t expected = 0;
    for (const auto& p : storm::test::PEOPLE_25) {
        if (!p.score.has_value() && p.age > 30) {
            expected++;
        }
    }
    EXPECT_EQ(result.value().size(), expected) << "Expected " << expected << " people with NULL score AND age > 30";
}

// Test: IS NULL OR value comparison
TYPED_TEST(WhereTest, IsNull_OrCombination) {
    QuerySet<Person, TypeParam> queryset;

    auto result = queryset.where(fields::Person.score.is_null() || fields::Person.age < 25).select().execute();
    ASSERT_TRUE(result.has_value()) << "IS NULL OR failed: " << result.error().message();
    // NULL score (10) OR age < 25 (Paul=22, Yara=22 — but Yara has score=92, Paul has score=40)
    std::size_t expected = 0;
    for (const auto& p : storm::test::PEOPLE_25) {
        if (!p.score.has_value() || p.age < 25) {
            expected++;
        }
    }
    EXPECT_EQ(result.value().size(), expected) << "Expected " << expected << " people with NULL score OR age < 25";
}

// Test: IS NULL on field where no rows match (all non-NULL)
TYPED_TEST(WhereTest, IsNull_EmptyResult) {
    QuerySet<Person, TypeParam> queryset;

    // age is NOT optional — it's always set, so IS NULL on a non-optional field won't make sense
    // Instead, filter to only non-NULL scores first, then check IS NULL on score → 0 results
    auto result =
            queryset.where(fields::Person.score.is_not_null()).where(fields::Person.score.is_null()).select().execute();
    ASSERT_TRUE(result.has_value()) << "IS NULL empty result failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 0) << "IS NOT NULL AND IS NULL should match nothing";
}

// Test: chaining an empty (null) expression keeps the existing filter instead of clearing it
TYPED_TEST(WhereTest, ChainedNullExpressionKeepsFilter) {
    QuerySet<Person, TypeParam> queryset;

    auto filtered = queryset.where(fields::Person.score.is_not_null()).select().to_sql();
    auto chained  = queryset.where(fields::Person.score.is_not_null()).where(nullptr).select().to_sql();
    ASSERT_TRUE(filtered.has_value());
    ASSERT_TRUE(chained.has_value());
    EXPECT_EQ(chained.value(), filtered.value()) << "where(nullptr) must be a no-op";
}

// Test: IS NULL with JOIN
TYPED_TEST(WhereJoinTest, IsNull_WithJoin) {
    QuerySet<Message, TypeParam> queryset;

    // In WhereJoinTest fixture, Person rows are inserted without score → all NULL
    // So IS NULL on score matches all 4 messages
    auto result =
            queryset.template join<fields::Message.sender>().where(fields::Person.score.is_null()).select().execute();
    ASSERT_TRUE(result.has_value()) << "IS NULL with JOIN failed: " << result.error().message();
    EXPECT_EQ(result.value().size(), 4) << "Expected 4 messages (all senders have NULL score in this fixture)";
}

// ============================================================================
// IS NULL + COLLATE (SQLite only — COLLATE NOCASE is SQLite-specific)
// ============================================================================
using SqliteConn = storm::db::sqlite::Connection;

class WhereNullCollateTest : public StormTestFixture<Person, SqliteConn> {
  protected:
    auto on_after_setup(const std::shared_ptr<SqliteConn>&) -> void override {
        ASSERT_TRUE((storm::test::batch_insert<Person, SqliteConn>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end())
        )));
    }
};

TEST_F(WhereNullCollateTest, IsNull_Collated) {
    QuerySet<Person, SqliteConn> queryset;

    auto result = queryset.where(fields::Person.nickname.collate(Collate::NoCase).is_null()).select().execute();
    ASSERT_TRUE(result.has_value()) << "Collated IS NULL failed: " << result.error().message();
    std::size_t expected_null_nicknames = 0;
    for (const auto& p : storm::test::PEOPLE_25) {
        if (!p.nickname.has_value()) {
            expected_null_nicknames++;
        }
    }
    EXPECT_EQ(result.value().size(), expected_null_nicknames) << "Collated IS NULL should work same as plain IS NULL";
}

// NOLINTEND(misc-const-correctness,performance-unnecessary-value-param,performance-unnecessary-copy-initialization)
