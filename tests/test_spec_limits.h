#pragma once

/**
 * @file test_spec_limits.h
 * @brief One set of spec-limit cases, run against each parser that fills BenchmarkTest (#843).
 *
 * A spec array holds at most MAX_* entries. A parser must COUNT the entries it was
 * given, even past the cap, so a consumer can reject the overflow at compile time
 * (BenchmarkTest::within_limits) instead of silently checking less than the case
 * says; and an over-long array must leave the parser at the right position, so the
 * key after it is still read. The cases are written once here; each test TU passes
 * its parser as `Parse` (a constexpr `string_view -> BenchmarkTest`).
 *
 * The marker key is `dataset_size` because both parsers read it (default 10000).
 *
 * Include AFTER `import storm_benchmark_schema;` and `import std;`.
 */

#include <gtest/gtest.h>

namespace storm::test::spec_limits {

inline constexpr std::string_view ORDER_BY_2 = R"({"order_by":[{"field":"a"},{"field":"b"}],"dataset_size":5})";
inline constexpr std::string_view ORDER_BY_3 =
    R"({"order_by":[{"field":"a"},{"field":"b"},{"field":"c"}],"dataset_size":5})";
inline constexpr std::string_view FKS_2 = R"({"join":{"type":"inner","fks":["a","b"]},"dataset_size":5})";
inline constexpr std::string_view FKS_3 = R"({"join":{"type":"inner","fks":["a","b","c"]},"dataset_size":5})";
inline constexpr std::string_view GROUP_2 = R"({"group_by":{"fields":["a","b"]},"dataset_size":5})";
inline constexpr std::string_view GROUP_3 = R"({"group_by":{"fields":["a","b","c"]},"dataset_size":5})";
inline constexpr std::string_view DISTINCT_3 = R"({"distinct":{"fields":["a","b","c"]},"dataset_size":5})";
inline constexpr std::string_view DISTINCT_4 = R"({"distinct":{"fields":["a","b","c","d"]},"dataset_size":5})";

// A non-string element is not a field name; it is skipped, and parsing goes on (a parser that
// does not advance past it would never leave the array).
inline constexpr std::string_view GROUP_NON_STRING = R"({"group_by":{"fields":["a",1,"b"]},"dataset_size":5})";
inline constexpr std::string_view ORDER_BY_NON_OBJECT =
    R"({"order_by":[{"field":"a"},7,{"field":"b"}],"dataset_size":5})";
// A repeated key replaces the first one rather than adding to it.
inline constexpr std::string_view FKS_REPEATED =
    R"({"join":{"type":"inner","fks":["a"],"fks":["b","c"]},"dataset_size":5})";
inline constexpr std::string_view GROUP_REPEATED =
    R"({"group_by":{"fields":["a"],"fields":["b","c"]},"dataset_size":5})";

// WHERE (#843): `where:` conditions and `combine` arrive as the converters emit them
// ({conditions:[{field, op, values:[...]}], combine}); each parser must build the same tree.
inline constexpr std::string_view WHERE_SINGLE =
    R"({"where":{"conditions":[{"field":"age","op":">","values":[30]}]},"dataset_size":5})";
inline constexpr std::string_view WHERE_AND =
    R"({"where":{"conditions":[{"field":"age","op":">","values":[30]},{"field":"name","op":"LIKE","values":["A%"]}],"combine":"and"},"dataset_size":5})";
inline constexpr std::string_view WHERE_OR_COMBINE_FIRST =
    R"({"where":{"combine":"or","conditions":[{"field":"a","op":"==","values":[1]},{"field":"b","op":"==","values":[2]}]},"dataset_size":5})";
inline constexpr std::string_view WHERE_NO_COMBINE =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]},{"field":"b","op":"==","values":[2]}]},"dataset_size":5})";
inline constexpr std::string_view WHERE_OPERANDS =
    R"({"where":{"conditions":[{"field":"a","op":"IN","values":[1,2,3]},{"field":"b","op":"BETWEEN","values":[1.5,2.5]},{"field":"c","op":"==","values":[null]}],"combine":"and"},"dataset_size":5})";
// 4 leaves + 3 combines = 7 nodes: exactly the cap. 5 leaves = 9 nodes: over it.
inline constexpr std::string_view WHERE_4_CONDITIONS =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]},{"field":"b","op":"==","values":[1]},{"field":"c","op":"==","values":[1]},{"field":"d","op":"==","values":[1]}],"combine":"and"},"dataset_size":5})";
inline constexpr std::string_view WHERE_5_CONDITIONS =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]},{"field":"b","op":"==","values":[1]},{"field":"c","op":"==","values":[1]},{"field":"d","op":"==","values":[1]},{"field":"e","op":"==","values":[1]}],"combine":"and"},"dataset_size":5})";
// The value pool holds WhereSpec::MAX_VALUES (10) across the whole tree.
inline constexpr std::string_view WHERE_10_VALUES =
    R"({"where":{"conditions":[{"field":"a","op":"IN","values":[1,2,3,4,5,6,7,8,9,10]}]},"dataset_size":5})";
inline constexpr std::string_view WHERE_11_VALUES =
    R"({"where":{"conditions":[{"field":"a","op":"IN","values":[1,2,3,4,5,6,7,8,9,10,11]}]},"dataset_size":5})";
// One WHERE per case: two would merge two trees into one set of nodes.
inline constexpr std::string_view WHERE_TWICE =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]}]},"where":{"conditions":[{"field":"b","op":"==","values":[2]}]},"dataset_size":5})";

// A leaf must carry the operands its operator reads. With one shared pool, a leaf short of values
// would read the NEXT leaf's operand, so a wrong count is rejected rather than quietly mis-bound.
inline constexpr std::string_view WHERE_NO_VALUE =
    R"({"where":{"conditions":[{"field":"a","op":">","values":[]},{"field":"b","op":"==","values":[5]}],"combine":"and"},"dataset_size":5})";
inline constexpr std::string_view WHERE_BETWEEN_ONE =
    R"({"where":{"conditions":[{"field":"a","op":"BETWEEN","values":[1]},{"field":"b","op":"==","values":[5]}],"combine":"and"},"dataset_size":5})";
inline constexpr std::string_view WHERE_EXTRA_VALUE =
    R"({"where":{"conditions":[{"field":"a","op":">","values":[1,2]}]},"dataset_size":5})";
inline constexpr std::string_view WHERE_EMPTY_IN =
    R"({"where":{"conditions":[{"field":"a","op":"IN","values":[]}]},"dataset_size":5})";
// An empty WHERE has no root, so it cannot be built.
inline constexpr std::string_view WHERE_EMPTY_CONDITIONS = R"({"where":{"conditions":[]},"dataset_size":5})";
// A repeated `conditions` key replaces the first, like every other repeated key.
inline constexpr std::string_view WHERE_REPEATED_CONDITIONS =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]}],"conditions":[{"field":"b","op":"==","values":[2]},{"field":"c","op":"==","values":[3]}],"combine":"and"},"dataset_size":5})";
// A stray `}` inside an array advances no token parser; it must still end the parse.
inline constexpr std::string_view WHERE_STRAY_BRACE =
    R"({"where":{"conditions":[{"field":"a","op":"IN","values":[1,},2]}]},"dataset_size":5})";

// Typed values (#844). A JSON `null` is its own kind; an integer that does not fit 32 bits is an
// int64. The kind records what was WRITTEN: whether it fits the field is the builder's check.
inline constexpr std::string_view WHERE_NULL_VALUE =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[null]},{"field":"b","op":"!=","values":[null]}],"combine":"and"},"dataset_size":5})";
inline constexpr std::string_view WHERE_INT_RANGE_EDGES =
    R"({"where":{"conditions":[{"field":"a","op":"IN","values":[2147483647,2147483648,-2147483648,-2147483649,5000000000]}]},"dataset_size":5})";
inline constexpr std::string_view WHERE_NEGATIVE_INT64 =
    R"({"where":{"conditions":[{"field":"a","op":"<","values":[-9000000000000000000]}]},"dataset_size":5})";
inline constexpr std::string_view WHERE_INT64_MAX =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[9223372036854775807]}]},"dataset_size":5})";
// null is a value like any other: the key after it is still read.
inline constexpr std::string_view WHERE_NULL_THEN_STRING =
    R"({"where":{"conditions":[{"field":"a","op":"IN","values":[null,"x",7]}]},"dataset_size":5})";

// The cases parsed by one parser. Exactly at the cap is legal; one more is not.
template <auto Parse> struct Cases {
    static constexpr auto order_by_2 = Parse(ORDER_BY_2);
    static constexpr auto order_by_3 = Parse(ORDER_BY_3);
    static constexpr auto fks_2 = Parse(FKS_2);
    static constexpr auto fks_3 = Parse(FKS_3);
    static constexpr auto group_2 = Parse(GROUP_2);
    static constexpr auto group_3 = Parse(GROUP_3);
    static constexpr auto distinct_3 = Parse(DISTINCT_3);
    static constexpr auto distinct_4 = Parse(DISTINCT_4);
    static constexpr auto group_non_string = Parse(GROUP_NON_STRING);
    static constexpr auto order_by_non_object = Parse(ORDER_BY_NON_OBJECT);
    static constexpr auto fks_repeated = Parse(FKS_REPEATED);
    static constexpr auto group_repeated = Parse(GROUP_REPEATED);
    static constexpr auto where_single = Parse(WHERE_SINGLE);
    static constexpr auto where_and = Parse(WHERE_AND);
    static constexpr auto where_or_first = Parse(WHERE_OR_COMBINE_FIRST);
    static constexpr auto where_no_combine = Parse(WHERE_NO_COMBINE);
    static constexpr auto where_operands = Parse(WHERE_OPERANDS);
    static constexpr auto where_4 = Parse(WHERE_4_CONDITIONS);
    static constexpr auto where_5 = Parse(WHERE_5_CONDITIONS);
    static constexpr auto where_10_values = Parse(WHERE_10_VALUES);
    static constexpr auto where_11_values = Parse(WHERE_11_VALUES);
    static constexpr auto where_twice = Parse(WHERE_TWICE);
    static constexpr auto where_empty = Parse(WHERE_EMPTY_CONDITIONS);
    static constexpr auto where_no_value = Parse(WHERE_NO_VALUE);
    static constexpr auto where_between_one = Parse(WHERE_BETWEEN_ONE);
    static constexpr auto where_extra_value = Parse(WHERE_EXTRA_VALUE);
    static constexpr auto where_empty_in = Parse(WHERE_EMPTY_IN);
    static constexpr auto where_repeated = Parse(WHERE_REPEATED_CONDITIONS);
    static constexpr auto where_stray_brace = Parse(WHERE_STRAY_BRACE);

    static constexpr auto where_null = Parse(WHERE_NULL_VALUE);
    static constexpr auto where_int_edges = Parse(WHERE_INT_RANGE_EDGES);
    static constexpr auto where_negative_int64 = Parse(WHERE_NEGATIVE_INT64);
    static constexpr auto where_int64_max = Parse(WHERE_INT64_MAX);
    static constexpr auto where_null_then_string = Parse(WHERE_NULL_THEN_STRING);

    static_assert(order_by_2.within_limits() && !order_by_3.within_limits());
    static_assert(fks_2.within_limits() && !fks_3.within_limits());
    static_assert(group_2.within_limits() && !group_3.within_limits());
    static_assert(distinct_3.within_limits() && !distinct_4.within_limits());
    static_assert(where_4.within_limits() && !where_5.within_limits());
    static_assert(where_10_values.within_limits() && !where_11_values.within_limits());
    static_assert(!where_twice.within_limits());
    static_assert(where_and.where.is_well_formed() && where_operands.where.is_well_formed());
    static_assert(where_empty_in.where.is_well_formed()); // an empty IN is valid (#835)
    static_assert(!where_empty.where.is_well_formed());
    static_assert(!where_no_value.where.is_well_formed());
    static_assert(!where_between_one.where.is_well_formed());
    static_assert(!where_extra_value.where.is_well_formed());
};

// The runtime half of the same contract: counts past the cap, the key after an
// over-long array is still read, and the entries inside the cap are kept in order.
template <auto Parse> auto expect_counts_past_every_cap() -> void {
    using C = Cases<Parse>;
    EXPECT_EQ(C::order_by_3.order_by_count, 3U);
    EXPECT_EQ(C::fks_3.join.fk_count, 3U);
    EXPECT_EQ(C::group_3.group_by.declared_count, 3U);
    EXPECT_EQ(C::distinct_4.distinct.declared_count, 4U);

    EXPECT_EQ(C::order_by_3.dataset_size, 5);
    EXPECT_EQ(C::fks_3.dataset_size, 5);
    EXPECT_EQ(C::group_3.dataset_size, 5);
    EXPECT_EQ(C::distinct_4.dataset_size, 5);

    EXPECT_EQ(C::order_by_3.order_by[0].field.view(), "a");
    EXPECT_EQ(C::order_by_3.order_by[1].field.view(), "b");
    EXPECT_EQ(C::fks_3.join.fks[1].view(), "b");
    EXPECT_EQ(C::group_3.group_by.fields[1].view(), "b");
    EXPECT_EQ(C::distinct_4.distinct.fields[2].view(), "c");
}

// One root, children by index. `combine` joins the leaves left to right; AND when it is absent.
template <auto Parse> auto expect_flat_where_builds_one_tree() -> void {
    using C = Cases<Parse>;
    using storm::benchmark::TypedValue;
    constexpr auto &single = C::where_single.where;
    EXPECT_EQ(single.node_count, 1U);
    EXPECT_EQ(single.root, 0);
    EXPECT_TRUE(single.nodes[0].is_leaf());
    EXPECT_EQ(single.nodes[0].field.view(), "age");
    EXPECT_EQ(single.nodes[0].op.view(), ">");
    EXPECT_EQ(single.nodes[0].value_count, 1U);
    EXPECT_EQ(single.values[single.nodes[0].first_value].as_int, 30);

    constexpr auto &both = C::where_and.where;
    EXPECT_EQ(both.node_count, 3U);
    EXPECT_EQ(both.root, 2);
    EXPECT_EQ(both.nodes[2].op.view(), "AND");
    EXPECT_EQ(both.nodes[2].left, 0);
    EXPECT_EQ(both.nodes[2].right, 1);
    EXPECT_FALSE(both.nodes[2].is_leaf());
    EXPECT_EQ(both.nodes[1].op.view(), "LIKE");
    EXPECT_TRUE(both.values[both.nodes[1].first_value].kind == TypedValue::Kind::String);
    EXPECT_EQ(both.values[both.nodes[1].first_value].as_string.view(), "A%");
    EXPECT_NE(both.nodes[0].first_value, both.nodes[1].first_value);

    // `combine` before `conditions` is the same tree.
    EXPECT_EQ(C::where_or_first.where.nodes[C::where_or_first.where.root].op.view(), "OR");
    // No `combine` means AND, in every parser (the benchmark parser used to fall through to OR).
    EXPECT_EQ(C::where_no_combine.where.nodes[C::where_no_combine.where.root].op.view(), "AND");

    // Operands: an IN list, BETWEEN's two values, one null (IS NULL is spelled `== null`, #844).
    constexpr auto &ops = C::where_operands.where;
    EXPECT_EQ(ops.nodes[0].value_count, 3U);
    EXPECT_EQ(ops.nodes[1].value_count, 2U);
    EXPECT_TRUE(ops.values[ops.nodes[1].first_value].kind == TypedValue::Kind::Double);
    EXPECT_EQ(ops.nodes[2].value_count, 1U);
    EXPECT_TRUE(ops.values[ops.nodes[2].first_value].kind == TypedValue::Kind::Null);
    EXPECT_EQ(ops.value_count, 6U);
}

// Counts past each cap, and the key after the overflow is still read.
template <auto Parse> auto expect_where_counts_past_its_caps() -> void {
    using C = Cases<Parse>;
    EXPECT_EQ(C::where_4.where.node_count, 7U);
    EXPECT_EQ(C::where_5.where.node_count, 9U);
    EXPECT_EQ(C::where_5.dataset_size, 5);
    EXPECT_EQ(C::where_10_values.where.value_count, 10U);
    EXPECT_EQ(C::where_11_values.where.value_count, 11U);
    EXPECT_EQ(C::where_11_values.dataset_size, 5);
}

template <auto Parse> auto expect_where_repeated_key_and_stray_brace_are_handled() -> void {
    using C = Cases<Parse>;
    // The second `conditions` replaces the first: two leaves and one join, starting at "b".
    EXPECT_EQ(C::where_repeated.where.node_count, 3U);
    EXPECT_EQ(C::where_repeated.where.nodes[0].field.view(), "b");
    EXPECT_EQ(C::where_repeated.where.value_count, 2U);
    // The parse ends (compile-time evaluation terminates) and both values around the stray brace are kept.
    EXPECT_EQ(C::where_stray_brace.where.value_count, 2U);
    EXPECT_EQ(C::where_stray_brace.dataset_size, 5);
}

template <auto Parse> auto expect_malformed_and_repeated_input_is_handled() -> void {
    using C = Cases<Parse>;
    // Non-string / non-object elements are skipped, not counted, and do not stall the parser.
    EXPECT_EQ(C::group_non_string.group_by.declared_count, 2U);
    EXPECT_EQ(C::group_non_string.group_by.fields[1].view(), "b");
    EXPECT_EQ(C::group_non_string.dataset_size, 5);
    EXPECT_EQ(C::order_by_non_object.order_by_count, 2U);
    EXPECT_EQ(C::order_by_non_object.order_by[1].field.view(), "b");
    EXPECT_EQ(C::order_by_non_object.dataset_size, 5);
    // A repeated key replaces, so two keys of 1 + 2 entries is 2, not 3.
    EXPECT_EQ(C::fks_repeated.join.fk_count, 2U);
    EXPECT_EQ(C::fks_repeated.join.fks[0].view(), "b");
    EXPECT_EQ(C::group_repeated.group_by.declared_count, 2U);
    EXPECT_EQ(C::group_repeated.group_by.fields[0].view(), "b");
}

// Typed values (#844): null and int64 are kinds of their own, in every parser.
template <auto Parse> auto expect_typed_values_parse() -> void {
    using C = Cases<Parse>;
    using Kind = storm::benchmark::TypedValue::Kind;

    constexpr auto &nulls = C::where_null.where;
    EXPECT_EQ(nulls.value_count, 2U);
    EXPECT_TRUE(nulls.values[nulls.nodes[0].first_value].kind == Kind::Null);
    EXPECT_TRUE(nulls.values[nulls.nodes[1].first_value].kind == Kind::Null);
    EXPECT_TRUE(nulls.values[nulls.nodes[0].first_value].is_set());

    // 32-bit edges stay Int; one past either end is Int64, with the full value kept.
    constexpr auto &edges = C::where_int_edges.where;
    ASSERT_EQ(edges.value_count, 5U);
    EXPECT_TRUE(edges.values[0].kind == Kind::Int);
    EXPECT_EQ(edges.values[0].as_int, 2147483647);
    EXPECT_TRUE(edges.values[1].kind == Kind::Int64);
    EXPECT_EQ(edges.values[1].as_int64, 2147483648LL);
    EXPECT_TRUE(edges.values[2].kind == Kind::Int);
    EXPECT_EQ(edges.values[2].as_int, std::numeric_limits<int>::min());
    EXPECT_TRUE(edges.values[3].kind == Kind::Int64);
    EXPECT_EQ(edges.values[3].as_int64, -2147483649LL);
    EXPECT_TRUE(edges.values[4].kind == Kind::Int64);
    EXPECT_EQ(edges.values[4].as_int64, 5000000000LL);

    constexpr auto &negative = C::where_negative_int64.where;
    EXPECT_TRUE(negative.values[0].kind == Kind::Int64);
    EXPECT_EQ(negative.values[0].as_int64, -9000000000000000000LL);
    constexpr auto &max64 = C::where_int64_max.where;
    EXPECT_TRUE(max64.values[0].kind == Kind::Int64);
    EXPECT_EQ(max64.values[0].as_int64, std::numeric_limits<std::int64_t>::max());

    // null does not stall the parser: the string and the int after it are read.
    constexpr auto &mixed = C::where_null_then_string.where;
    ASSERT_EQ(mixed.value_count, 3U);
    EXPECT_TRUE(mixed.values[0].kind == Kind::Null);
    EXPECT_TRUE(mixed.values[1].kind == Kind::String);
    EXPECT_EQ(mixed.values[1].as_string.view(), "x");
    EXPECT_TRUE(mixed.values[2].kind == Kind::Int);
    EXPECT_EQ(mixed.values[2].as_int, 7);
}

} // namespace storm::test::spec_limits
