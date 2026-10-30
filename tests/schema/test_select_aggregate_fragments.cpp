#include <gtest/gtest.h>
#include <meta>
#include <plf_hive/plf_hive.h>

import std;
import storm_db_sqlite;
import storm_orm_field_attr;
import storm_orm_fields;
import storm_orm_statements_aggregate;
import storm_orm_statements_base;
import storm_orm_statements_select;
import storm_orm_utilities;
// The aggregate SQL-text grammar and the m2m stitch-key helpers are #include fragments of
// storm_orm_statements_aggregate / _select (#671): a file split, not a module split. This TU
// pins what they emit and compute — SQL text, result types and stitch keys — plus the
// selector gates that select.cppm defines, through the two parent modules, without
// `import storm;`.

namespace {
    using namespace storm::orm::statements;
    using storm::orm::utilities::StitchKey;

    struct AgOwner {
        [[= storm::meta::primary]] int id{};
        std::string                    label;
    };

    struct AgRow {
        [[= storm::meta::primary]] int  id{};
        std::string                     name;
        int                             qty{};
        double                          price{};
        [[= storm::meta::fk<>]] AgOwner owner;
    };

    // Composite key mixing an integer part, a text part and an FK part — one per
    // append_obj_dispatched_part arm, plus the FK indirection to the referenced key.
    struct AgPart {
        [[= storm::meta::primary_part]] int                            a{};
        [[= storm::meta::primary_part]] std::string                    code;
        [[= storm::meta::primary_part]][[= storm::meta::fk<>]] AgOwner owner;
        int                                                            qty{};
    };

    using SumQtyPrice   = AggregateOp<AggregateType::SUM, ^^AgRow::qty, ^^AgRow::price>;
    using SumQty        = AggregateOp<AggregateType::SUM, ^^AgRow::qty>;
    using CountAll      = AggregateOp<AggregateType::COUNT>;
    using CountDistinct = AggregateOp<AggregateType::COUNT_DISTINCT, ^^AgRow::owner>;
    using AvgPrice      = AggregateOp<AggregateType::AVG, ^^AgRow::price>;

    using Conn       = storm::db::sqlite::Connection;
    using RowStitch  = SelectStitch<AgRow, Conn>;
    using PartStitch = SelectStitch<AgPart, Conn>;
} // namespace

TEST(AggregateGrammar, FunctionNameFoldsCountDistinctIntoCount) {
    EXPECT_EQ(get_agg_function_name(AggregateType::SUM), "SUM");
    EXPECT_EQ(get_agg_function_name(AggregateType::COUNT_DISTINCT), "COUNT");
    EXPECT_EQ(get_agg_function_name(AggregateType::MAX), "MAX");
}

TEST(AggregateGrammar, SingleOpSqlNamesColumns) {
    constexpr auto sum_sql      = build_single_op_sql<SumQtyPrice>();
    constexpr auto count_sql    = build_single_op_sql<CountAll>();
    constexpr auto distinct_sql = build_single_op_sql<CountDistinct>();
    EXPECT_EQ(sum_sql.view(), "SUM(qty + price)");
    EXPECT_EQ(count_sql.view(), "COUNT(*)");
    EXPECT_EQ(distinct_sql.view(), "COUNT(DISTINCT owner_id)");
}

TEST(AggregateGrammar, OpsAndGroupFieldListsAreCommaJoined) {
    constexpr auto ops = build_ops_list<CountAll, AvgPrice>(std::make_index_sequence<2>{});
    constexpr auto groups =
            build_group_fields_sql<GroupByFields<^^AgRow::name, ^^AgRow::owner>>(std::make_index_sequence<2>{});
    EXPECT_EQ(ops.view(), "COUNT(*), AVG(price)");
    EXPECT_EQ(groups.view(), "name, owner_id");
    EXPECT_EQ(NoGroupBy::size, 0U);
}

TEST(AggregateGrammar, OpResultFollowsAggregateAndOperandType) {
    EXPECT_TRUE((std::is_same_v<OpResult<CountAll>, std::int64_t>));
    EXPECT_TRUE((std::is_same_v<OpResult<SumQty>, std::int64_t>));
    EXPECT_TRUE((std::is_same_v<OpResult<SumQtyPrice>, double>));
    EXPECT_TRUE((std::is_same_v<OpResult<AvgPrice>, std::optional<double>>));
}

TEST(AggregateGrammar, NumericAggregateableUnwrapsOptionalAndRejectsBool) {
    EXPECT_TRUE(NumericAggregateable<int>);
    EXPECT_TRUE(NumericAggregateable<std::optional<double>>);
    EXPECT_FALSE(NumericAggregateable<bool>);
    EXPECT_FALSE(NumericAggregateable<std::string>);
}

TEST(SelectStitchKey, SingleColumnKeyNarrowsToOneWord) {
    EXPECT_TRUE(RowStitch::narrow_stitch_key_);
    EXPECT_TRUE((std::is_same_v<RowStitch::StitchMapKey, std::uint64_t>));

    AgRow const row{.id = 7, .name = "x", .qty = 1, .price = 2.0, .owner = {}};
    EXPECT_EQ(RowStitch::narrow_stitch_key(RowStitch::build_owner_stitch_key(row)), 7U);
}

// Pins the Q1-side key shape only. That it still matches the Q2-side key built by
// TwoQueryJoinBase::extract_q2_owner_pk is proven end to end on a real database by
// tests/query/test_composite_m2m_through_body.h (the extractor needs a Statement).
TEST(SelectStitchKey, CompositeKeyKeepsEveryPart) {
    EXPECT_FALSE(PartStitch::narrow_stitch_key_);
    EXPECT_TRUE((std::is_same_v<PartStitch::StitchMapKey, StitchKey>));

    AgPart const part{.a = 3, .code = "sku", .owner = {.id = 9, .label = "o"}, .qty = 0};
    StitchKey    expected;
    expected.append_int64(3);
    expected.append_string("sku");
    expected.append_int64(9); // the FK part keys on the referenced row's own id
    EXPECT_EQ(PartStitch::narrow_stitch_key(PartStitch::build_owner_stitch_key(part)), expected);
}

TEST(SelectStitchKey, OneFromHiveReducesToFirstOrGet) {
    plf::hive<AgRow> one;
    one.insert(AgRow{.id = 1, .name = "a", .qty = 0, .price = 0.0, .owner = {}});
    auto first = RowStitch::m2m_one_from_hive<false>(plf::hive<AgRow>{one});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ((*first)->id, 1);

    auto got = RowStitch::m2m_one_from_hive<true>(plf::hive<AgRow>{one});
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, 1);

    auto none = RowStitch::m2m_one_from_hive<false>(plf::hive<AgRow>{});
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());

    auto missing = RowStitch::m2m_one_from_hive<true>(plf::hive<AgRow>{});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().message(), "No row found matching query");

    plf::hive<AgRow> two{one};
    two.insert(AgRow{.id = 2, .name = "b", .qty = 0, .price = 0.0, .owner = {}});
    auto multiple = RowStitch::m2m_one_from_hive<true>(std::move(two));
    ASSERT_FALSE(multiple.has_value());
    EXPECT_EQ(multiple.error().message(), "Multiple rows found matching query");
}

TEST(SelectSelectorGates, SelectorGatesSeparateColumnsFromNumericTargets) {
    constexpr auto name  = storm::meta::selector_for<^^AgRow::name>();
    constexpr auto qty   = storm::meta::selector_for<^^AgRow::qty>();
    constexpr auto price = storm::meta::selector_for<^^AgRow::price>();
    EXPECT_TRUE((ProjectionSelectors<name, qty>));
    EXPECT_TRUE((NumericAggregateSelectors<qty, price>));
    EXPECT_FALSE((NumericAggregateSelectors<name>));
}
