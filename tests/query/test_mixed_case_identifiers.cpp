#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

// #822: table and column names that are not all lowercase. Storm emits every identifier
// quoted and lowercased (#828), so a mixed-case name is the same lowercase name on both
// backends. DDL and every statement that names the table afterwards must agree with each
// other — which only executing the statements shows, hence no SQL-text assertions here.
// Names that are SQL reserved words (`Union`, `desc`): test_reserved_word_identifiers.cpp.
//
// WorstCase1/WorstCase2 both have a `MixedCase` column, so the JOIN test also covers the same
// mixed-case name on both sides of a join.

import storm;
import std;

using storm::QuerySet;

#include "test_fixture.h" // NOSONAR cpp:S954

struct WorstCase1 {
    [[= storm::primary_assigned]] int lowercase{};
    int                               UPPERCASE{};
    int                               MixedCase{};
};

struct WorstCase2 {
    [[= storm::primary_assigned]] int RowKey{};
    int                               MixedCase{};
    [[= storm::fk<>]] WorstCase1      Parent;
};

namespace fields {
    struct WorstCase1T;
    struct WorstCase2T;
    consteval {
        std::meta::define_aggregate(^^WorstCase1T, storm::field_specs_for(^^WorstCase1));
        std::meta::define_aggregate(^^WorstCase2T, storm::field_specs_for(^^WorstCase2));
    }
    inline constexpr WorstCase1T WorstCase1{};
    inline constexpr WorstCase2T WorstCase2{};
} // namespace fields

namespace {

    using Row1 = std::tuple<int, int, int>;

    auto as_tuple(const WorstCase1& row) -> Row1 {
        return {row.lowercase, row.UPPERCASE, row.MixedCase};
    }

} // namespace

// Seeds three WorstCase1 rows and one WorstCase2 row per parent.
template <typename ConnType> class MixedCaseIdentifierTest : public StormTestFixture<WorstCase1, ConnType, WorstCase2> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
        const std::vector<WorstCase1> parents{
                {.lowercase = 1, .UPPERCASE = 2, .MixedCase = 3},
                {.lowercase = 2, .UPPERCASE = 2, .MixedCase = 3},
                {.lowercase = 4, .UPPERCASE = 3, .MixedCase = 2},
        };
        auto inserted_parents = QuerySet<WorstCase1, ConnType>().insert(parents).execute();
        ASSERT_TRUE(inserted_parents.has_value()) << inserted_parents.error().message();

        const std::vector<WorstCase2> children{
                {.RowKey = 10, .MixedCase = 30, .Parent = {.lowercase = 1}},
                {.RowKey = 20, .MixedCase = 30, .Parent = {.lowercase = 2}},
                {.RowKey = 40, .MixedCase = 20, .Parent = {.lowercase = 4}},
        };
        auto inserted_children = QuerySet<WorstCase2, ConnType>().insert(children).execute();
        ASSERT_TRUE(inserted_children.has_value()) << inserted_children.error().message();
    }
};

TYPED_TEST_SUITE(MixedCaseIdentifierTest, DatabaseTypes);

TYPED_TEST(MixedCaseIdentifierTest, BatchInsertedRowsSelectBack) {
    auto result =
            QuerySet<WorstCase1, TypeParam>().template order_by<fields::WorstCase1.lowercase>().select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::vector<Row1> rows;
    for (const auto& row : result.value()) {
        rows.emplace_back(as_tuple(row));
    }
    EXPECT_EQ(rows, (std::vector<Row1>{{1, 2, 3}, {2, 2, 3}, {4, 3, 2}}));
}

TYPED_TEST(MixedCaseIdentifierTest, SingleRowInsertSelectsBack) {
    QuerySet<WorstCase1, TypeParam> qs;
    auto inserted = qs.insert(WorstCase1{.lowercase = 7, .UPPERCASE = 8, .MixedCase = 9}).execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    auto result = qs.where(fields::WorstCase1.lowercase == 7).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1);
    EXPECT_EQ(as_tuple(*result.value().begin()), (Row1{7, 8, 9}));
}

// A projection names each column itself instead of going through the model's full column list.
TYPED_TEST(MixedCaseIdentifierTest, ProjectionOfMixedCaseColumns) {
    auto result = QuerySet<WorstCase1, TypeParam>()
                          .template order_by<fields::WorstCase1.lowercase>()
                          .template values<fields::WorstCase1.UPPERCASE, fields::WorstCase1.MixedCase>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    const std::vector<std::tuple<int, int>> rows(result.value().begin(), result.value().end());
    EXPECT_EQ(rows, (std::vector<std::tuple<int, int>>{{2, 3}, {2, 3}, {3, 2}}));
}

TYPED_TEST(MixedCaseIdentifierTest, WhereAndOrderByOnMixedCaseColumns) {
    // Matches on MixedCase but not on UPPERCASE, so each half of the condition excludes a row.
    auto inserted = QuerySet<WorstCase1, TypeParam>()
                            .insert(WorstCase1{.lowercase = 5, .UPPERCASE = 1, .MixedCase = 3})
                            .execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    auto result = QuerySet<WorstCase1, TypeParam>()
                          .where(fields::WorstCase1.MixedCase == 3 && fields::WorstCase1.UPPERCASE >= 2)
                          .template order_by<fields::WorstCase1.lowercase, false>()
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::vector<int> keys;
    for (const auto& row : result.value()) {
        keys.emplace_back(row.lowercase);
    }
    EXPECT_EQ(keys, (std::vector<int>{2, 1}));
}

TYPED_TEST(MixedCaseIdentifierTest, UpdateAndEraseByMixedCaseColumn) {
    QuerySet<WorstCase1, TypeParam> qs;

    auto updated = qs.where(fields::WorstCase1.UPPERCASE == 3)
                           .template update<fields::WorstCase1.MixedCase>(WorstCase1{.MixedCase = 99})
                           .execute();
    ASSERT_TRUE(updated.has_value()) << updated.error().message();

    auto changed = qs.where(fields::WorstCase1.MixedCase == 99).select().execute();
    ASSERT_TRUE(changed.has_value()) << changed.error().message();
    ASSERT_EQ(changed.value().size(), 1);
    EXPECT_EQ(as_tuple(*changed.value().begin()), (Row1{4, 3, 99}));

    // WorstCase2 rows reference their parents, so erase the children first. The filter matches
    // every child on purpose: it runs a DELETE that names a mixed-case column of that table.
    auto erased_children = QuerySet<WorstCase2, TypeParam>().where(fields::WorstCase2.MixedCase > 0).erase().execute();
    ASSERT_TRUE(erased_children.has_value()) << erased_children.error().message();

    auto erased = qs.where(fields::WorstCase1.MixedCase == 3).erase().execute();
    ASSERT_TRUE(erased.has_value()) << erased.error().message();

    auto remaining = qs.select().execute();
    ASSERT_TRUE(remaining.has_value()) << remaining.error().message();
    ASSERT_EQ(remaining.value().size(), 1);
    EXPECT_EQ(remaining.value().begin()->lowercase, 4);
}

// Both tables have a `MixedCase` column: the WHERE names the child's, and each side of the
// joined row must come back with its own value.
TYPED_TEST(MixedCaseIdentifierTest, JoinWithSameMixedCaseColumnOnBothSides) {
    auto result = QuerySet<WorstCase2, TypeParam>()
                          .template join<fields::WorstCase2.Parent>()
                          .where(fields::WorstCase2.MixedCase == 30)
                          .template order_by<fields::WorstCase2.RowKey>()
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::vector<std::tuple<int, int, Row1>> rows;
    for (const auto& child : result.value()) {
        rows.emplace_back(child.RowKey, child.MixedCase, as_tuple(child.Parent));
    }
    EXPECT_EQ(rows, (std::vector<std::tuple<int, int, Row1>>{{10, 30, {1, 2, 3}}, {20, 30, {2, 2, 3}}}));
}

// NOLINTEND(misc-const-correctness)
