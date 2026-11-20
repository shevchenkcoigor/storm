// Issue #843/#845: the test entry point (tests/test_parser.hpp) reads the test-only
// keys and hands every other key to the one shared parser (storm_benchmark_parser).
// It runs the shared cases (test_spec_limits.h), so a key the extension failed to
// pass on would show here, and the shared QueryBuilder must reject a spec that
// overflows a cap. No corpus: test_parser.hpp needs none since #845.

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
import storm_orm_utilities;
import storm_orm_where;
import std;

#include "shared/models/person.h" // NOSONAR cpp:S954
#include "shared/models/extended_types.h"
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
    static_assert(!SpecWithinLimits<C::group_5>);
    static_assert(!SpecWithinLimits<C::distinct_4>);
    // Positive controls: the negatives below must fail BECAUSE of the cap, not for some unrelated reason.
    static_assert(Buildable<C::order_by_2>);
    static_assert(Buildable<C::where_4>);
    static_assert(Buildable<kExpr>);
    static_assert(Buildable<C::where_operands>);
    static_assert(Buildable<C::where_empty_in>);
    static_assert(Buildable<C::fks_2>);
    static_assert(Buildable<C::group_4>);
    static_assert(Buildable<C::distinct_3>);
    // HAVING is built like WHERE and checked like it (#868): positive control, then what it must reject.
    static_assert(Buildable<C::having_leaf>);
    static_assert(Buildable<C::having_or>);
    static_assert(!Buildable<C::having_like_int>);
    static_assert(!Buildable<C::having_in_null>);
    static_assert(!Buildable<C::having_flat_legacy>);
    static_assert(!Buildable<C::having_unknown_combine>);
    static_assert(Buildable<C::having_grouped_second>);
    static_assert(!Buildable<C::having_ungrouped>);
    // needs_sqlite() (TestCase) reads this, so a HAVING collation makes the case SQLite-only.
    static_assert(C::having_collate.group_by.having.any_collate() && !C::having_leaf.group_by.having.any_collate());
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
    static_assert(!Buildable<C::group_5>);
    static_assert(!Buildable<C::distinct_4>);
    // #847: an unknown `combine` does not build; the same tree spelled "and"/"or" does.
    static_assert(Buildable<C::where_or_first>);
    static_assert(Buildable<C::where_no_combine>);
    static_assert(!Buildable<C::where_combine_unknown>);
    static_assert(!Buildable<C::where_combine_upper_case>);
    static_assert(!Buildable<C::where_combine_unknown_one>);

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

    TEST(SpecLimitsTestParser, TypedValuesParseAsNullAndInt64) {
        storm::test::spec_limits::expect_typed_values_parse<parse_case>();
    }

    TEST(SpecLimitsTestParser, WhereCountsPastItsCaps) {
        storm::test::spec_limits::expect_where_counts_past_its_caps<parse_case>();
    }

    TEST(SpecLimitsTestParser, MalformedAndRepeatedInputIsHandled) {
        storm::test::spec_limits::expect_malformed_and_repeated_input_is_handled<parse_case>();
    }

    TEST(SpecLimitsTestParser, ValuesCapsAndOneResultShapePerCase) {
        storm::test::spec_limits::expect_values_limits_and_single_result_shape<parse_case>();
    }

    TEST(SpecLimitsTestParser, CountsPastEveryCapAndKeepsParsing) {
        storm::test::spec_limits::expect_counts_past_every_cap<parse_case>();
    }

    TEST(SpecLimitsTestParser, UnknownCombineIsRejected) {
        storm::test::spec_limits::expect_unknown_combine_is_rejected<parse_case>();
    }

    TEST(SpecLimitsTestParser, HavingIsATree) {
        storm::test::spec_limits::expect_having_is_a_tree<parse_case>();
    }

    TEST(SpecLimitsTestParser, EverySharedKeyIsRead) {
        storm::test::spec_limits::expect_every_shared_key_is_read<parse_case>();
    }

    TEST(SpecLimitsTestParser, BracesInStringsAreText) {
        storm::test::spec_limits::expect_braces_in_strings_are_text<parse_case>();
    }

    TEST(SpecLimitsTestParser, OverLongStringsAreRejected) {
        storm::test::spec_limits::expect_over_long_strings_are_rejected<parse_case>();
    }

    TEST(SpecLimitsTestParser, CountTestsIgnoresBracesInStrings) {
        using storm::test::spec_limits::TWO_OBJECTS_WITH_BRACES;
        constexpr std::size_t n = storm::benchmark::count_tests(TWO_OBJECTS_WITH_BRACES);
        EXPECT_EQ(n, 2U);
        constexpr auto cases = storm::benchmark::parse_array_into<storm::test::TestCase, 2>(
                TWO_OBJECTS_WITH_BRACES, storm::test::parse_test_case_into
        );
        EXPECT_EQ(cases[0].bench.model.view(), "m");
        EXPECT_EQ(cases[1].bench.test_name.view(), "b]");
    }

    // ------------------------------------------------------------------
    // Typed values (#844). The value is read in the FIELD's type, so the model decides what a
    // string or number means: a UUID, a date, an enumerator name, a blob in hex. A value that does
    // not fit its field is a compile error, never a coercion.
    // ------------------------------------------------------------------
    using storm::orm::query_builder::SpecValuesFit;
    using Ext = ExtendedTypes;

    template <typename Model, auto Spec>
    concept BuildableOn = requires { typename QueryBuilder<Model, Spec>; };

    // One WHERE condition `field op value`; `value` is the JSON text of the operand.
    constexpr auto where_case(std::string_view field, std::string_view op, std::string_view value)
            -> storm::benchmark::BenchmarkTest {
        std::string json = R"({"where":{"conditions":[{"field":")";
        json += field;
        json += R"(","op":")";
        json += op;
        json += R"(","values":[)";
        json += value;
        json += R"(]}]},"dataset_size":5})";
        return parse_case(json);
    }

    // null: a nullable column may be compared with it; any other column, operator or list may not.
    constexpr auto kNullOptDoubleEq = where_case("opt_double", "==", "null");
    constexpr auto kNullOptDoubleNe = where_case("opt_double", "!=", "null");
    constexpr auto kNullOptColorEq  = where_case("opt_color", "==", "null");
    constexpr auto kNullOnLabel     = where_case("label", "==", "null");
    constexpr auto kNullOnInt       = where_case("id", "!=", "null");
    constexpr auto kNullOrdered     = where_case("opt_double", ">", "null");
    constexpr auto kNullInList      = where_case("opt_double", "IN", "1.5,null");
    constexpr auto kIsNullSpelling  = where_case("opt_double", "IS NULL", "");
    static_assert(BuildableOn<Ext, kNullOptDoubleEq> && SpecValuesFit<Ext, kNullOptDoubleEq>);
    static_assert(BuildableOn<Ext, kNullOptDoubleNe>);
    static_assert(BuildableOn<Ext, kNullOptColorEq>);
    static_assert(!BuildableOn<Ext, kNullOnLabel>, "null on a NOT NULL column");
    static_assert(!BuildableOn<Ext, kNullOnInt>, "null on a NOT NULL column");
    static_assert(!BuildableOn<Ext, kNullOrdered>, "null only with == / !=");
    static_assert(!BuildableOn<Ext, kNullInList>, "IN (..., NULL) never matches NULL rows (#625)");
    // One spelling: `IS NULL` / `IS NOT NULL` are gone from the YAML vocabulary, `== null` replaces them.
    static_assert(!BuildableOn<Ext, kIsNullSpelling>);

    // int64: a 32-bit literal fits an int64 field; an int64 literal does not fit an int field.
    constexpr auto kBigOnInt64    = where_case("big_num", "==", "5000000000");
    constexpr auto kSmallOnInt64  = where_case("big_num", ">", "5");
    constexpr auto kInInt64       = where_case("big_num", "IN", "1,5000000000,-5000000000");
    constexpr auto kBigBetween    = where_case("big_num", "BETWEEN", "-5000000000,5000000000");
    constexpr auto kBigOnInt      = where_case("id", "==", "5000000000");
    constexpr auto kStringOnInt64 = where_case("big_num", "==", R"("five")");
    constexpr auto kDoubleOnInt64 = where_case("big_num", "==", "1.5");
    static_assert(BuildableOn<Ext, kBigOnInt64>);
    static_assert(BuildableOn<Ext, kSmallOnInt64>);
    static_assert(BuildableOn<Ext, kInInt64>);
    static_assert(BuildableOn<Ext, kBigBetween>);
    static_assert(!BuildableOn<Ext, kBigOnInt>, "5000000000 does not fit an int field");
    static_assert(!BuildableOn<Ext, kStringOnInt64>);
    static_assert(!BuildableOn<Ext, kDoubleOnInt64>, "a fraction is not an integer");

    // The kind a value was written as must match the field (#848): a Double or Bool never reads
    // as an integer, and an optional field reads as its inner type.
    constexpr auto kOptDoubleDouble = where_case("opt_double", "==", "1.5");
    constexpr auto kOptDoubleIn     = where_case("opt_double", "IN", "1.5,2.5");
    constexpr auto kOptDoubleInt    = where_case("opt_double", ">", "2");
    constexpr auto kBoolOnBool      = where_case("is_active", "==", "true");
    constexpr auto kDoubleOnInt     = where_case("age", "==", "1.5");
    constexpr auto kDoubleOnId      = where_case("id", ">", "0.5");
    constexpr auto kBoolOnInt       = where_case("age", "==", "true");
    constexpr auto kBoolOnDouble    = where_case("salary", "==", "true");
    constexpr auto kBoolOnOptDouble = where_case("opt_double", "==", "true");
    constexpr auto kDoubleOnBool    = where_case("is_active", "==", "1.5");
    constexpr auto kDoubleOnOptInt  = where_case("score", "==", "1.5");
    static_assert(BuildableOn<Ext, kOptDoubleDouble>);
    static_assert(BuildableOn<Ext, kOptDoubleIn>);
    static_assert(BuildableOn<Ext, kOptDoubleInt>, "an integer literal on an optional floating column");
    static_assert(BuildableOn<Person, kBoolOnBool>);
    static_assert(!BuildableOn<Person, kDoubleOnInt>, "a Double on an int field");
    static_assert(!BuildableOn<Person, kDoubleOnId>, "a Double on an int field");
    static_assert(!BuildableOn<Person, kBoolOnInt>, "a Bool on an int field");
    static_assert(!BuildableOn<Person, kBoolOnDouble>, "a Bool on a floating field");
    static_assert(!BuildableOn<Ext, kBoolOnOptDouble>, "a Bool on an optional floating field");
    static_assert(!BuildableOn<Person, kDoubleOnBool>, "a Double on a bool field");
    static_assert(!BuildableOn<Person, kDoubleOnOptInt>, "a Double on an optional int field");

    // The same predicate gates typed_value_as itself, so no kind can reach it for a field it cannot fill.
    using storm::orm::query_builder::ValueFits;
    constexpr auto kDoubleValue = kOptDoubleDouble.where.values[0];
    constexpr auto kBoolValue   = kBoolOnBool.where.values[0];
    static_assert(ValueFits<std::optional<double>, kDoubleValue>);
    static_assert(ValueFits<double, kDoubleValue>);
    static_assert(!ValueFits<int, kDoubleValue>);
    static_assert(!ValueFits<std::optional<int>, kDoubleValue>);
    static_assert(ValueFits<bool, kBoolValue>);
    static_assert(ValueFits<std::optional<bool>, kBoolValue>);
    static_assert(!ValueFits<int, kBoolValue>);
    static_assert(!ValueFits<std::optional<double>, kBoolValue>);

    // UUID: a canonical 36-character string.
    constexpr auto kUuidOk        = where_case("uuid_field", "==", R"("550e8400-e29b-41d4-a716-446655440000")");
    constexpr auto kUuidNe        = where_case("uuid_field", "!=", R"("550e8400-e29b-41d4-a716-446655440000")");
    constexpr auto kUuidMalformed = where_case("uuid_field", "==", R"("not-a-uuid")");
    constexpr auto kUuidShort     = where_case("uuid_field", "==", R"("550e8400-e29b-41d4-a716")");
    constexpr auto kUuidNumber    = where_case("uuid_field", "==", "5");
    constexpr auto kUuidOnLabel   = where_case("id", "==", R"("550e8400-e29b-41d4-a716-446655440000")");
    static_assert(BuildableOn<Ext, kUuidOk>);
    static_assert(BuildableOn<Ext, kUuidNe>);
    static_assert(!BuildableOn<Ext, kUuidMalformed>);
    static_assert(!BuildableOn<Ext, kUuidShort>);
    static_assert(!BuildableOn<Ext, kUuidNumber>);
    static_assert(!BuildableOn<Ext, kUuidOnLabel>, "a UUID string on an int field");

    // date / time: ISO 8601, `YYYY-MM-DD` for a date and `YYYY-MM-DDTHH:MM:SS` for a time point.
    constexpr auto kDateOk      = where_case("date_field", "==", R"("2024-01-15")");
    constexpr auto kDateBetween = where_case("date_field", "BETWEEN", R"("2024-01-01","2024-12-31")");
    constexpr auto kDateLeap    = where_case("date_field", "==", R"("2024-02-29")");
    constexpr auto kTimeOk      = where_case("datetime_field", ">", R"("2024-01-15T10:30:00")");
    constexpr auto kDateMonth13 = where_case("date_field", "==", R"("2024-13-01")");
    constexpr auto kDateNotLeap = where_case("date_field", "==", R"("2023-02-29")");
    constexpr auto kDateWords   = where_case("date_field", "==", R"("yesterday")");
    constexpr auto kDateNumber  = where_case("date_field", "==", "20240115");
    constexpr auto kTimeHour25  = where_case("datetime_field", ">", R"("2024-01-15T25:00:00")");
    constexpr auto kTimeNumber  = where_case("datetime_field", ">", "1700000000");
    static_assert(BuildableOn<Ext, kDateOk>);
    static_assert(BuildableOn<Ext, kDateBetween>);
    static_assert(BuildableOn<Ext, kDateLeap>);
    static_assert(BuildableOn<Ext, kTimeOk>);
    static_assert(!BuildableOn<Ext, kDateMonth13>);
    static_assert(!BuildableOn<Ext, kDateNotLeap>, "2023 is not a leap year");
    static_assert(!BuildableOn<Ext, kDateWords>);
    static_assert(!BuildableOn<Ext, kDateNumber>);
    static_assert(!BuildableOn<Ext, kTimeHour25>);
    static_assert(!BuildableOn<Ext, kTimeNumber>);

    // enum: by enumerator name, exact spelling.
    constexpr auto kEnumOk        = where_case("color", "==", R"("Green")");
    constexpr auto kEnumIn        = where_case("color", "IN", R"("Red","Blue")");
    constexpr auto kOptEnumOk     = where_case("opt_color", "==", R"("Blue")");
    constexpr auto kEnumUnknown   = where_case("color", "==", R"("Purple")");
    constexpr auto kEnumWrongCase = where_case("color", "==", R"("green")");
    constexpr auto kEnumNumber    = where_case("color", "==", "1");
    static_assert(BuildableOn<Ext, kEnumOk>);
    static_assert(BuildableOn<Ext, kEnumIn>);
    static_assert(BuildableOn<Ext, kOptEnumOk>);
    static_assert(!BuildableOn<Ext, kEnumUnknown>);
    static_assert(!BuildableOn<Ext, kEnumWrongCase>);
    static_assert(!BuildableOn<Ext, kEnumNumber>, "an enum is written by name, not by number");

    // The fit check never rejects what already worked: plain int, double and string operands on Person.
    constexpr auto kPersonAge    = where_case("age", ">", "30");
    constexpr auto kPersonLike   = where_case("name", "LIKE", R"("A%")");
    constexpr auto kPersonSalary = where_case("salary", "<", "50000");
    static_assert(BuildableOn<Person, kPersonAge>);
    static_assert(BuildableOn<Person, kPersonLike>);
    static_assert(BuildableOn<Person, kPersonSalary>, "an integer literal on a floating column");

    // ------------------------------------------------------------------
    // expected.first: the same typed values, checked against the row member's type.
    // ------------------------------------------------------------------
    constexpr auto expected_value(std::string_view value) -> storm::benchmark::TypedValue {
        std::string json = R"({"expected":{"count":1,"first":{"m":)";
        json += value;
        json += R"(}},"dataset_size":5})";
        return parse_test_case(json).expected.first.fields[0].value;
    }
    using storm::test::ExpectedValueFits;
    using Blob = std::vector<std::byte>;

    constexpr auto kExpNull      = expected_value("null");
    constexpr auto kExpBig       = expected_value("5000000000");
    constexpr auto kExpSmall     = expected_value("7");
    constexpr auto kExpDouble    = expected_value("1.5");
    constexpr auto kExpText      = expected_value(R"("hello")");
    constexpr auto kExpUuid      = expected_value(R"("550e8400-e29b-41d4-a716-446655440000")");
    constexpr auto kExpBadUuid   = expected_value(R"("550e8400")");
    constexpr auto kExpDate      = expected_value(R"("2024-01-15")");
    constexpr auto kExpBadDate   = expected_value(R"("2024-02-30")");
    constexpr auto kExpTime      = expected_value(R"("2024-01-15T10:30:00")");
    constexpr auto kExpBadTime   = expected_value(R"("2024-01-15T10:61:00")");
    constexpr auto kExpBlob      = expected_value(R"("DEADBEEF")");
    constexpr auto kExpBlobLow   = expected_value(R"("deadbeef")");
    constexpr auto kExpOddBlob   = expected_value(R"("DEADBEE")");
    constexpr auto kExpNotHex    = expected_value(R"("ZZ")");
    constexpr auto kExpEmptyBlob = expected_value(R"("")");
    constexpr auto kExpGreen     = expected_value(R"("Green")");
    constexpr auto kExpPurple    = expected_value(R"("Purple")");

    // null: only a nullable member can be NULL.
    static_assert(ExpectedValueFits<std::optional<double>, kExpNull>);
    static_assert(ExpectedValueFits<std::optional<std::int64_t>, kExpNull>);
    static_assert(ExpectedValueFits<std::optional<Color>, kExpNull>);
    static_assert(!ExpectedValueFits<std::string, kExpNull>);
    static_assert(!ExpectedValueFits<int, kExpNull>);
    static_assert(!ExpectedValueFits<double, kExpNull>);
    // int64: 5000000000 does not fit an int member.
    static_assert(ExpectedValueFits<std::int64_t, kExpBig>);
    static_assert(ExpectedValueFits<long long, kExpBig>);
    static_assert(ExpectedValueFits<std::optional<std::int64_t>, kExpBig>);
    static_assert(ExpectedValueFits<std::int64_t, kExpSmall>);
    static_assert(ExpectedValueFits<int, kExpSmall>);
    static_assert(!ExpectedValueFits<int, kExpBig>);
    static_assert(!ExpectedValueFits<std::string, kExpBig>);
    static_assert(ExpectedValueFits<std::optional<double>, kExpDouble>);
    static_assert(!ExpectedValueFits<std::int64_t, kExpDouble>);
    static_assert(ExpectedValueFits<std::string, kExpText>);
    // UUID
    static_assert(ExpectedValueFits<decltype(Ext::uuid_field), kExpUuid>);
    static_assert(!ExpectedValueFits<decltype(Ext::uuid_field), kExpBadUuid>);
    static_assert(!ExpectedValueFits<decltype(Ext::uuid_field), kExpSmall>);
    // date / time
    static_assert(ExpectedValueFits<decltype(Ext::date_field), kExpDate>);
    static_assert(!ExpectedValueFits<decltype(Ext::date_field), kExpBadDate>);
    static_assert(!ExpectedValueFits<decltype(Ext::date_field), kExpSmall>);
    static_assert(ExpectedValueFits<decltype(Ext::datetime_field), kExpTime>);
    static_assert(ExpectedValueFits<std::optional<decltype(Ext::datetime_field)>, kExpTime>);
    static_assert(!ExpectedValueFits<decltype(Ext::datetime_field), kExpBadTime>);
    // blob: upper- or lower-case hex, an even number of digits.
    static_assert(ExpectedValueFits<Blob, kExpBlob>);
    static_assert(ExpectedValueFits<Blob, kExpBlobLow>);
    static_assert(ExpectedValueFits<Blob, kExpEmptyBlob>, "an empty blob is a valid value");
    static_assert(!ExpectedValueFits<Blob, kExpOddBlob>);
    static_assert(!ExpectedValueFits<Blob, kExpNotHex>);
    static_assert(!ExpectedValueFits<Blob, kExpSmall>);
    // enum: by enumerator name
    static_assert(ExpectedValueFits<Color, kExpGreen>);
    static_assert(ExpectedValueFits<std::optional<Color>, kExpGreen>);
    static_assert(!ExpectedValueFits<Color, kExpPurple>);
    static_assert(!ExpectedValueFits<Color, kExpSmall>);
    static_assert(!ExpectedValueFits<Color, kExpNull>);

    TEST(SpecLimitsTestParser, ExpectedFirstKeepsNullAndInt64Apart) {
        using Kind = storm::benchmark::TypedValue::Kind;
        EXPECT_TRUE(kExpNull.kind == Kind::Null);
        EXPECT_TRUE(kExpBig.kind == Kind::Int64);
        EXPECT_EQ(kExpBig.as_int64, 5000000000LL);
        EXPECT_TRUE(kExpSmall.kind == Kind::Int);
        EXPECT_TRUE(kExpUuid.kind == Kind::String);
    }

    // ------------------------------------------------------------------
    // Per-dialect expected SQL (#844): expected_sql_sqlite / expected_sql_pg beside expected_sql.
    // A case sets the shared key, or BOTH dialect keys; never both forms, and never one dialect
    // alone (the other backend would run with no SQL check at all).
    // ------------------------------------------------------------------
    using storm::test::WellFormedExpectedSql;
    constexpr auto kSqlShared = parse_test_case(R"({"expected_sql":"SELECT 1","dataset_size":5})");
    constexpr auto kSqlPair =
            parse_test_case(R"({"expected_sql_sqlite":"LIMIT -1","expected_sql_pg":"LIMIT ALL","dataset_size":5})");
    constexpr auto kSqlNone  = parse_test_case(R"({"dataset_size":5})");
    constexpr auto kSqlMixed = parse_test_case(
            R"({"expected_sql":"SELECT 1","expected_sql_pg":"SELECT 2","expected_sql_sqlite":"SELECT 3","dataset_size":5})"
    );
    constexpr auto kSqlMixedOne =
            parse_test_case(R"({"expected_sql":"SELECT 1","expected_sql_pg":"SELECT 2","dataset_size":5})");
    constexpr auto kSqlOnlySqlite = parse_test_case(R"({"expected_sql_sqlite":"SELECT 3","dataset_size":5})");
    constexpr auto kSqlOnlyPg     = parse_test_case(R"({"expected_sql_pg":"SELECT 2","dataset_size":5})");

    static_assert(WellFormedExpectedSql<kSqlShared>);
    static_assert(WellFormedExpectedSql<kSqlPair>);
    static_assert(WellFormedExpectedSql<kSqlNone>, "result-only stays valid");
    static_assert(!WellFormedExpectedSql<kSqlMixed>, "the shared key and the pair together");
    static_assert(!WellFormedExpectedSql<kSqlMixedOne>);
    static_assert(!WellFormedExpectedSql<kSqlOnlySqlite>, "one dialect alone leaves the other unchecked");
    static_assert(!WellFormedExpectedSql<kSqlOnlyPg>);

    // ------------------------------------------------------------------
    // `backends:` and a WHERE collate (#866): a collation exists on SQLite only, so a case using one
    // must say `backends: [sqlite]`; any other list is rejected rather than read as "all backends".
    // ------------------------------------------------------------------
    constexpr auto kCollateSqlite = parse_test_case(
            R"({"backends":["sqlite"],"where":{"conditions":[{"field":"name","op":"==","collate":"NOCASE","values":["a"]}]},"dataset_size":5})"
    );
    constexpr auto kCollateAllBackends = parse_test_case(
            R"({"where":{"conditions":[{"field":"name","op":"==","collate":"NOCASE","values":["a"]}]},"dataset_size":5})"
    );
    constexpr auto kPlainAllBackends =
            parse_test_case(R"({"where":{"conditions":[{"field":"name","op":"==","values":["a"]}]},"dataset_size":5})");
    constexpr auto kBackendsPg     = parse_test_case(R"({"backends":["pg"],"dataset_size":5})");
    constexpr auto kBackendsTwo    = parse_test_case(R"({"backends":["sqlite","pg"],"dataset_size":5})");
    constexpr auto kBackendsEmpty  = parse_test_case(R"({"backends":[],"dataset_size":5})");
    constexpr auto kBackendsSqlite = parse_test_case(R"({"backends":["sqlite"],"dataset_size":5})");

    static_assert(kCollateSqlite.backends_well_formed() && kCollateSqlite.sqlite_only);
    static_assert(kCollateSqlite.bench.where.nodes[0].collate == "NOCASE");
    static_assert(!kCollateAllBackends.backends_well_formed(), "a collate with no `backends: [sqlite]`");
    static_assert(kPlainAllBackends.backends_well_formed() && !kPlainAllBackends.sqlite_only);
    static_assert(kBackendsSqlite.backends_well_formed());
    static_assert(!kBackendsPg.backends_well_formed(), "an unknown backend name");
    static_assert(!kBackendsTwo.backends_well_formed(), "only the one-entry [sqlite] list is accepted");
    static_assert(!kBackendsEmpty.backends_well_formed());

    constexpr auto kInnerCollate = parse_test_case(
            R"({"backends":["sqlite"],"where_expr":{"op":"AND","collate":"NOCASE","left":{"field":"age","op":">","value_int":1},"right":{"field":"age","op":"<","value_int":9}},"dataset_size":5})"
    );
    static_assert(!kInnerCollate.bench.where.is_well_formed(), "a collate on an AND/OR node is never applied");
    constexpr auto kTreeValues = parse_test_case(
            R"({"where_expr":{"op":"OR","left":{"field":"age","op":"BETWEEN","values":[1,2]},"right":{"field":"age","op":"IN","values":[5,6,7]}},"dataset_size":5})"
    );
    static_assert(kTreeValues.bench.where.is_well_formed());
    static_assert(
            kTreeValues.bench.where.nodes[1].value_count == 2 && kTreeValues.bench.where.nodes[2].value_count == 3
    );
    constexpr auto kOrderCollate =
            parse_test_case(R"({"order_by":[{"field":"name","collate":"NOCASE"}],"dataset_size":5})");
    static_assert(!kOrderCollate.backends_well_formed(), "an ORDER BY collate also needs backends: [sqlite]");

    TEST(SpecLimitsTestParser, ExpectedSqlPicksTheDialectsText) {
        constexpr bool kSqlite = false;
        constexpr bool kPg     = true;
        EXPECT_EQ(kSqlShared.expected_sql_for(kSqlite), "SELECT 1");
        EXPECT_EQ(kSqlShared.expected_sql_for(kPg), "SELECT 1");
        EXPECT_EQ(kSqlPair.expected_sql_for(kSqlite), "LIMIT -1");
        EXPECT_EQ(kSqlPair.expected_sql_for(kPg), "LIMIT ALL");
        EXPECT_TRUE(kSqlNone.expected_sql_for(kSqlite).empty());
        EXPECT_TRUE(kSqlNone.expected_sql_for(kPg).empty());
    }

    TEST(SpecLimitsTestParser, EscapeAtBufferEdgeIsKept) {
        storm::test::spec_limits::expect_escape_at_buffer_edge_is_kept<parse_case>();
    }

    TEST(SpecLimitsTestParser, NonStringValueIsConsumed) {
        storm::test::spec_limits::expect_non_string_value_is_consumed<parse_case>();
    }

    // An element past MAX_CHAIN that is not an object is still one element: it is counted, and the
    // keys after the array are read.
    constexpr std::string_view kChainOverflowScalarJson =
            R"({"aggregations":[{"func":"sum"},{"func":"sum"},{"func":"sum"},{"func":"sum"},{"func":"sum"},7],"query_type":"agg","dataset_size":5})";
    constexpr auto kChainOverflowScalar = parse_test_case(kChainOverflowScalarJson);

    TEST(SpecLimitsTestParser, ChainOverflowScalarIsSkipped) {
        EXPECT_EQ(kChainOverflowScalar.chain_len, 6U);
        EXPECT_FALSE(kChainOverflowScalar.within_limits());
        EXPECT_EQ(kChainOverflowScalar.query_type.view(), "agg");
        EXPECT_EQ(kChainOverflowScalar.bench.dataset_size, 5);
    }

    // `dataset` (a string key) given a number: no phantom key is left for a count to see, so the
    // first-row member count and the keys after it are exact.
    constexpr std::string_view kNonStringDatasetJson =
            R"({"dataset":5,"expected":{"first":{"a":1,"b":"x"}},"query_type":"select"})";
    constexpr auto kNonStringDataset = parse_test_case(kNonStringDatasetJson);

    TEST(SpecLimitsTestParser, NonStringDatasetLeavesNoPhantomKey) {
        EXPECT_TRUE(kNonStringDataset.dataset.view().empty());
        EXPECT_EQ(kNonStringDataset.expected.first.count, 2U);
        EXPECT_EQ(kNonStringDataset.query_type.view(), "select");
        EXPECT_TRUE(kNonStringDataset.within_limits());
    }

    TEST(SpecLimitsTestParser, ExpectedSqlKeysDoNotShadowEachOther) {
        // expected_sql_pg / expected_sql_sqlite share a prefix with expected_sql: the key match is exact.
        EXPECT_TRUE(kSqlPair.expected_sql.empty());
        EXPECT_EQ(kSqlShared.expected_sql_sqlite.view(), "");
        EXPECT_EQ(kSqlShared.expected_sql_pg.view(), "");
        EXPECT_EQ(kSqlPair.expected_sql_sqlite.view(), "LIMIT -1");
        EXPECT_EQ(kSqlPair.expected_sql_pg.view(), "LIMIT ALL");
    }

} // namespace
