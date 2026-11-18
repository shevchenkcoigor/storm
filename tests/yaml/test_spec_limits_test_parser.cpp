// Issue #843: the test-side parser (tests/test_parser.hpp) fills the same
// BenchmarkTest spec as the benchmark parser, with its own code. It runs the same
// shared cases (test_spec_limits.h), and the shared QueryBuilder must reject a
// spec that overflows a cap.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - #embed needs a literal filename
#define STORM_UNIFIED_CASES_FILE "test_cases/unified_cases_crud.json"

#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_select;
import storm_orm_where;
import std;

#include "shared/models/person.h" // NOSONAR cpp:S954
#include "test_select_runner.h"
#include "test_parser.hpp"
#include "test_spec_limits.h"

namespace {

    using storm::orm::query_builder::QueryBuilder;
    using storm::orm::query_builder::SpecWithinLimits;
    using storm::test::spec_limits::Cases;

    constexpr auto parse_case(std::string_view json) -> storm::benchmark::BenchmarkTest {
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc.bench;
    }

    constexpr auto parse_test_case(std::string_view json) -> storm::test::TestCase {
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc;
    }

    using C = Cases<parse_case>;

    // `where_expr:` (test only) builds the same tree as `where:`. Nodes are numbered in
    // pre-order from the root: 0 AND(1 leaf, 2 OR(3 leaf, 4 leaf)).
    constexpr std::string_view kExprJson =
            R"({"where_expr":{"op":"AND","left":{"field":"age","op":">","value_int":30},"right":{"op":"OR","left":{"field":"salary","op":"<","value_dbl":0.0},"right":{"field":"age","op":"==","value_int":1}}},"dataset_size":5})";
    // 5 leaves joined by 4 ANDs = 9 nodes, over the 7-node cap.
    constexpr std::string_view kExprOverflowJson =
            R"({"where_expr":{"op":"AND","left":{"op":"AND","left":{"op":"AND","left":{"op":"AND","left":{"field":"a","op":"==","value_int":1},"right":{"field":"b","op":"==","value_int":1}},"right":{"field":"c","op":"==","value_int":1}},"right":{"field":"d","op":"==","value_int":1}},"right":{"field":"e","op":"==","value_int":1}},"dataset_size":5})";
    // Both forms in one case: two trees cannot share one set of nodes.
    constexpr std::string_view kBothJson =
            R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]}]},"where_expr":{"field":"b","op":"==","value_int":2},"dataset_size":5})";
    // An inner node needs both children: with only one it would read as a leaf and drop a subtree.
    constexpr std::string_view kOnlyLeftJson =
            R"({"where_expr":{"op":"AND","left":{"field":"a","op":"==","value_int":1}},"dataset_size":5})";
    constexpr std::string_view kOnlyRightJson =
            R"({"where_expr":{"op":"AND","right":{"field":"a","op":"==","value_int":1}},"dataset_size":5})";
    // A leaf-shaped node (field + value) that also has a right child: the child must not be dropped.
    constexpr std::string_view kLeafWithRightJson =
            R"({"where_expr":{"field":"age","op":">","value_int":30,"right":{"field":"a","op":"==","value_int":1}},"dataset_size":5})";
    // A join that carries a field of its own would ignore it.
    constexpr std::string_view kJoinWithFieldJson =
            R"({"where_expr":{"field":"age","op":"AND","left":{"field":"a","op":"==","value_int":1},"right":{"field":"b","op":"==","value_int":2}},"dataset_size":5})";
    constexpr auto kLeafWithRight = parse_case(kLeafWithRightJson);
    constexpr auto kJoinWithField = parse_case(kJoinWithFieldJson);
    constexpr auto kOnlyLeft      = parse_case(kOnlyLeftJson);
    constexpr auto kOnlyRight     = parse_case(kOnlyRightJson);
    constexpr auto kExpr          = parse_case(kExprJson);
    constexpr auto kExprOverflow  = parse_case(kExprOverflowJson);
    constexpr auto kBoth          = parse_case(kBothJson);

    // The chain limit is part of the same contract (#843): a count past MAX_CHAIN is rejected.
    constexpr std::string_view kChain5Json =
            R"({"aggregations":[{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}}],"dataset_size":5})";
    constexpr std::string_view kChain6Json =
            R"({"aggregations":[{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}},{"func":"sum","field":"age","result":{"type":"int","value":1}}],"dataset_size":5})";
    constexpr auto kChain5 = parse_test_case(kChain5Json);
    constexpr auto kChain6 = parse_test_case(kChain6Json);

    static_assert(kChain5.within_limits() && !kChain6.within_limits());
    static_assert(kExpr.within_limits() && !kExprOverflow.within_limits());
    static_assert(!kBoth.within_limits());
    static_assert(kExpr.where.is_well_formed());
    static_assert(!kOnlyLeft.where.is_well_formed());
    static_assert(!kOnlyRight.where.is_well_formed());
    static_assert(!kLeafWithRight.where.is_well_formed());
    static_assert(!kJoinWithField.where.is_well_formed());

    // The shared builder names its precondition: an over-cap spec does not satisfy
    // SpecWithinLimits, so QueryBuilder<Model, spec> is ill-formed at the use site.
    template <auto Spec>
    concept Buildable = requires { typename QueryBuilder<Person, Spec>; };

    static_assert(SpecWithinLimits<C::order_by_2>);
    static_assert(!SpecWithinLimits<C::order_by_3>);
    static_assert(!SpecWithinLimits<C::fks_3>);
    static_assert(!SpecWithinLimits<C::group_3>);
    static_assert(!SpecWithinLimits<C::distinct_4>);
    // Positive controls: the negatives below must fail BECAUSE of the cap, not for some unrelated reason.
    static_assert(Buildable<C::order_by_2>);
    static_assert(Buildable<C::where_4>);
    static_assert(Buildable<kExpr>);
    static_assert(Buildable<C::where_operands>);
    static_assert(Buildable<C::where_empty_in>);
    static_assert(Buildable<C::fks_2>);
    static_assert(Buildable<C::group_2>);
    static_assert(Buildable<C::distinct_3>);
    static_assert(!Buildable<C::where_5>);
    static_assert(!Buildable<C::where_11_values>);
    static_assert(!Buildable<C::where_twice>);
    static_assert(!Buildable<kExprOverflow>);
    static_assert(!Buildable<kBoth>);
    // Malformed trees do not build: a leaf short of operands would read the next leaf's.
    static_assert(!Buildable<C::where_no_value>);
    static_assert(!Buildable<C::where_between_one>);
    static_assert(!Buildable<C::where_extra_value>);
    static_assert(!Buildable<kOnlyLeft>);
    static_assert(!Buildable<kOnlyRight>);
    static_assert(!Buildable<kLeafWithRight>);
    static_assert(!Buildable<kJoinWithField>);
    static_assert(!Buildable<C::order_by_3>);
    static_assert(!Buildable<C::fks_3>);
    static_assert(!Buildable<C::group_3>);
    static_assert(!Buildable<C::distinct_4>);

    TEST(SpecLimitsTestParser, WhereExprBuildsTheSameTreeInPreOrder) {
        using storm::benchmark::TypedValue;
        constexpr auto& w = kExpr.where;
        EXPECT_EQ(w.node_count, 5U);
        EXPECT_EQ(w.root, 0);
        EXPECT_EQ(w.nodes[0].op.view(), "AND");
        EXPECT_EQ(w.nodes[0].left, 1);
        EXPECT_EQ(w.nodes[0].right, 2);
        EXPECT_TRUE(w.nodes[1].is_leaf());
        EXPECT_EQ(w.nodes[1].field.view(), "age");
        EXPECT_TRUE(w.values[w.nodes[1].first_value].kind == TypedValue::Kind::Int);
        EXPECT_EQ(w.values[w.nodes[1].first_value].as_int, 30);
        EXPECT_EQ(w.nodes[2].op.view(), "OR");
        EXPECT_EQ(w.nodes[2].left, 3);
        EXPECT_EQ(w.nodes[2].right, 4);
    }

    // A double leaf of exactly 0.0 stays a double. The tree used to choose int or double with
    // `value_dbl != 0.0`, so 0.0 was read as the int 0.
    TEST(SpecLimitsTestParser, WhereExprKeepsADoubleLeafOfZeroADouble) {
        using storm::benchmark::TypedValue;
        constexpr auto& w = kExpr.where;
        EXPECT_EQ(w.nodes[3].field.view(), "salary");
        EXPECT_TRUE(w.values[w.nodes[3].first_value].kind == TypedValue::Kind::Double);
        EXPECT_EQ(w.values[w.nodes[3].first_value].as_double, 0.0);
    }

    TEST(SpecLimitsTestParser, WhereExprCountsPastTheNodeCapAndKeepsParsing) {
        EXPECT_EQ(kExprOverflow.where.node_count, 9U);
        EXPECT_EQ(kExprOverflow.dataset_size, 5);
        EXPECT_EQ(kBoth.where.forms, 2U);
    }

    TEST(SpecLimitsTestParser, ChainCountsPastTheCap) {
        EXPECT_EQ(kChain5.chain_len, 5U);
        EXPECT_EQ(kChain6.chain_len, 6U);
        EXPECT_EQ(kChain6.bench.dataset_size, 5);
    }

    TEST(SpecLimitsTestParser, WhereRepeatedKeyAndStrayBraceAreHandled) {
        storm::test::spec_limits::expect_where_repeated_key_and_stray_brace_are_handled<parse_case>();
    }

    TEST(SpecLimitsTestParser, FlatWhereBuildsOneTree) {
        storm::test::spec_limits::expect_flat_where_builds_one_tree<parse_case>();
    }

    TEST(SpecLimitsTestParser, WhereCountsPastItsCaps) {
        storm::test::spec_limits::expect_where_counts_past_its_caps<parse_case>();
    }

    TEST(SpecLimitsTestParser, MalformedAndRepeatedInputIsHandled) {
        storm::test::spec_limits::expect_malformed_and_repeated_input_is_handled<parse_case>();
    }

    TEST(SpecLimitsTestParser, CountsPastEveryCapAndKeepsParsing) {
        storm::test::spec_limits::expect_counts_past_every_cap<parse_case>();
    }

} // namespace
