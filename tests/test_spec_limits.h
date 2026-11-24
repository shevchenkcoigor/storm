#pragma once

/**
 * @file test_spec_limits.h
 * @brief One set of spec-limit cases, run through each entry point of the one spec parser (#843, #845).
 *
 * A spec array holds at most MAX_* entries. A parser must COUNT the entries it was
 * given, even past the cap, so a consumer can reject the overflow at compile time
 * (BenchmarkTest::within_limits) instead of silently checking less than the case
 * says; and an over-long array must leave the parser at the right position, so the
 * key after it is still read. The cases are written once here; each test TU passes
 * its entry point as `Parse` (a constexpr `string_view -> BenchmarkTest`): the
 * benchmark one (storm::benchmark::parse_test_object) and the test one
 * (tests/test_parser.hpp, which forwards every shared key to the same parser).
 *
 * The marker key is `dataset_size`, a shared key (default 10000).
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
// A GROUP BY case carries a count: QueryBuilder rejects one with no aggregate (#868, SpecAggregatesWellFormed).
inline constexpr std::string_view GROUP_4 =
    R"({"group_by":{"fields":["a","b","c","d"]},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view GROUP_5 =
    R"({"group_by":{"fields":["a","b","c","d","e"]},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view DISTINCT_3 = R"({"distinct":{"fields":["a","b","c"]},"dataset_size":5})";
inline constexpr std::string_view VALUES_4 = R"({"values":{"fields":["a","b","c","d"]},"dataset_size":5})";
inline constexpr std::string_view VALUES_5 = R"({"values":{"fields":["a","b","c","d","e"]},"dataset_size":5})";
// Two result shapes in one case: build_terminal would run one and ignore the other (#867).
inline constexpr std::string_view DISTINCT_AND_VALUES =
    R"({"distinct":{"fields":["a"]},"values":{"fields":["b"]},"dataset_size":5})";
inline constexpr std::string_view VALUES_AND_FIRST_OP =
    R"({"operation":"first","values":{"fields":["a"]},"dataset_size":5})";
inline constexpr std::string_view VALUES_AND_GROUP_BY =
    R"({"values":{"fields":["a"]},"group_by":{"fields":["b"]},"dataset_size":5})";
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
// ({conditions:[{field, op, values:[...]}], combine}); each entry point must build the same tree.
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

// HAVING is the same tree of conditions as WHERE (#868): one leaf, an OR of two, an unknown `combine`.
inline constexpr std::string_view HAVING_LEAF =
    R"({"group_by":{"fields":["age"],"having":{"conditions":[{"field":"age","op":">","values":[30]}]}},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view HAVING_OR =
    R"({"group_by":{"fields":["age"],"having":{"conditions":[{"field":"age","op":">","values":[30]},{"field":"age","op":"<","values":[20]}],"combine":"or"}},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view HAVING_UNKNOWN_COMBINE =
    R"({"group_by":{"fields":["age"],"having":{"conditions":[{"field":"age","op":">","values":[30]}],"combine":"xor"}},"aggregate":{"func":"count"},"dataset_size":5})";

// A HAVING tree is checked like WHERE (#868): a pattern is text, IN takes no null, a leaf needs `values`,
// and a collation makes the case SQLite-only.
inline constexpr std::string_view HAVING_LIKE_INT =
    R"({"group_by":{"fields":["age"],"having":{"conditions":[{"field":"age","op":"LIKE","values":[5]}]}},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view HAVING_IN_NULL =
    R"({"group_by":{"fields":["age"],"having":{"conditions":[{"field":"age","op":"IN","values":[null,1]}]}},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view HAVING_FLAT_LEGACY =
    R"({"group_by":{"fields":["age"],"having":{"field":"age","op":">","value":30}},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view HAVING_COLLATE =
    R"({"group_by":{"fields":["name"],"having":{"conditions":[{"field":"name","op":"==","collate":"NOCASE","values":["a"]}]}},"aggregate":{"func":"count"},"dataset_size":5})";

// PostgreSQL rejects a HAVING column that is neither grouped nor aggregated, SQLite accepts it: so a HAVING
// leaf on a field outside GROUP BY is rejected on both (#868).
inline constexpr std::string_view HAVING_UNGROUPED =
    R"({"group_by":{"fields":["age"],"having":{"conditions":[{"field":"name","op":"==","values":["a"]}]}},"aggregate":{"func":"count"},"dataset_size":5})";
inline constexpr std::string_view HAVING_GROUPED_SECOND =
    R"({"group_by":{"fields":["age","name"],"having":{"conditions":[{"field":"name","op":"==","values":["a"]}]}},"aggregate":{"func":"count"},"dataset_size":5})";

// `combine` is exactly "and" or "or" (#847). Any other spelling, a different case included, is
// rejected rather than read as AND -- also when there is only one condition to join.
inline constexpr std::string_view WHERE_COMBINE_UNKNOWN =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]},{"field":"b","op":"==","values":[2]}],"combine":"xor"},"dataset_size":5})";
inline constexpr std::string_view WHERE_COMBINE_UPPER_CASE =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]},{"field":"b","op":"==","values":[2]}],"combine":"OR"},"dataset_size":5})";
// A non-string `combine` (null, a number) is not a spelling either.
inline constexpr std::string_view WHERE_COMBINE_NULL =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]},{"field":"b","op":"==","values":[2]}],"combine":null},"dataset_size":5})";
inline constexpr std::string_view WHERE_COMBINE_UNKNOWN_ONE_CONDITION =
    R"({"where":{"conditions":[{"field":"a","op":"==","values":[1]}],"combine":"xor"},"dataset_size":5})";

// Every shared key is read by one parser, whichever entry point a case comes through (#845).
inline constexpr std::string_view ORDER_BY_COLLATE =
    R"({"order_by":[{"field":"name","direction":"DESC","collate":"NOCASE"}],"dataset_size":5})";
inline constexpr std::string_view NAME_KEY = R"({"name":"by_name","dataset_size":5})";
inline constexpr std::string_view TEST_NAME_KEY = R"({"test_name":"by_test_name","dataset_size":5})";
inline constexpr std::string_view BENCHMARK_KEYS =
    R"({"test_category":"cat","description":"desc","operation":"op","iterations":7,"batch_size":3,"size_profile":"prof","setop":{"operands":[{},{"op":"union"}]},"dataset_size":5})";
// Set-op chains (#870): `setop.operands`, 2..4 entries; the first has no `op`, each later one names it.
inline constexpr std::string_view SETOP_TWO =
    R"({"setop":{"operands":[{"where":{"conditions":[{"field":"age","op":">","values":[30]}]}},{"op":"union","where":{"conditions":[{"field":"age","op":"<","values":[20]}]}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_WHOLE_TABLE =
    R"({"setop":{"operands":[{},{"op":"except","where":{"conditions":[{"field":"name","op":"LIKE","values":["A%"]}]}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_FOUR =
    R"({"setop":{"operands":[{},{"op":"intersect"},{"op":"union_all"},{"op":"except"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_FIVE =
    R"({"setop":{"operands":[{},{"op":"union"},{"op":"union"},{"op":"union"},{"op":"union"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_ONE_OPERAND = R"({"setop":{"operands":[{}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_UNKNOWN_OP = R"({"setop":{"operands":[{},{"op":"minus"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_MISSING_OP =
    R"({"setop":{"operands":[{},{"op":"union"},{"where":{"conditions":[{"field":"age","op":">","values":[1]}]}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_OP_ON_FIRST =
    R"({"setop":{"operands":[{"op":"union"},{"op":"union"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_UNKNOWN_OPERAND_KEY =
    R"({"setop":{"operands":[{},{"op":"union","order_by":[{"field":"age"}]}]},"dataset_size":5})";
// The pre-#870 spelling: `type` beside (or instead of) `operands` is an unknown key, never a silent INTERSECT.
inline constexpr std::string_view SETOP_LEGACY_TYPE =
    R"({"setop":{"type":"union","operands":[{},{"op":"union"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_NON_OBJECT_OPERAND =
    R"({"setop":{"operands":[{},7,{"op":"union"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_REPEATED_OPERANDS =
    R"({"setop":{"operands":[{},{"op":"union"},{"op":"union"},{"op":"union"}],"operands":[{},{"op":"except"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_OPERAND_WHERE_TWICE =
    R"({"setop":{"operands":[{},{"op":"union","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"where":{"conditions":[{"field":"age","op":"<","values":[9]}]}}]},"dataset_size":5})";
// SQLite reads a chain left to right, PostgreSQL binds INTERSECT tighter: only INTERSECT-first is portable.
inline constexpr std::string_view SETOP_INTERSECT_FIRST =
    R"({"setop":{"operands":[{},{"op":"intersect"},{"op":"intersect"},{"op":"union"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_INTERSECT_AFTER_UNION =
    R"({"setop":{"operands":[{},{"op":"union"},{"op":"intersect"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_INTERSECT_AFTER_EXCEPT =
    R"({"setop":{"operands":[{},{"op":"intersect"},{"op":"except"},{"op":"intersect"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_REPEATED_OPERANDS_WITH_WHERE =
    R"({"setop":{"operands":[{"where":{"conditions":[{"field":"age","op":">","values":[1]}]}},{"op":"union"}],"operands":[{},{"op":"except"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_OPERAND_WHERE_EMPTY =
    R"({"setop":{"operands":[{},{"op":"union","where":{"conditions":[]}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_OPERAND_UNKNOWN_COMBINE =
    R"({"setop":{"operands":[{},{"op":"union","where":{"conditions":[{"field":"age","op":">","values":[1]},{"field":"age","op":"<","values":[9]}],"combine":"xor"}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_POOL_OVER_NODE_CAP =
    R"({"setop":{"operands":[{"where":{"conditions":[{"field":"age","op":">","values":[1]},{"field":"age","op":"<","values":[9]}]}},{"op":"union","where":{"conditions":[{"field":"age","op":">","values":[1]},{"field":"age","op":"<","values":[9]}]}},{"op":"union","where":{"conditions":[{"field":"age","op":">","values":[1]},{"field":"age","op":"<","values":[9]}]}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_WHERE_AFTER =
    R"({"setop":{"operands":[{"where":{"conditions":[{"field":"age","op":">","values":[1]}]}},{"op":"union"}]},"where":{"conditions":[{"field":"age","op":"<","values":[9]}]},"dataset_size":5})";
// A top-level `where:` / `join:` would not say which operand it filters.
inline constexpr std::string_view SETOP_WITH_WHERE =
    R"({"where":{"conditions":[{"field":"age","op":">","values":[1]}]},"setop":{"operands":[{},{"op":"union"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_WITH_JOIN =
    R"({"join":{"type":"inner","fk":"sender"},"setop":{"operands":[{},{"op":"union"}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_VALUE_MISFIT =
    R"({"setop":{"operands":[{},{"op":"union","where":{"conditions":[{"field":"age","op":"==","values":["x"]}]}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_OPERAND_WHERE_MALFORMED =
    R"({"setop":{"operands":[{},{"op":"union","where":{"conditions":[{"field":"age","op":"BETWEEN","values":[1]}]}}]},"dataset_size":5})";
inline constexpr std::string_view SETOP_OPERAND_WHERE_OVER_CAP =
    R"({"setop":{"operands":[{},{"op":"union","where":{"conditions":[{"field":"age","op":"IN","values":[1,2,3,4,5,6,7,8,9,10,11]}]}}]},"dataset_size":5})";

// A brace or bracket inside a string is text, not structure: skipping the value it sits in must
// not miscount the nesting, or the parse loses every key after it.
inline constexpr std::string_view BRACE_IN_SKIPPED_STRING =
    R"({"extra":{"note":"a{b]c"},"description":"a{b]c","dataset_size":5})";
inline constexpr std::string_view BRACKET_IN_SKIPPED_ARRAY = R"({"name":"n{1","extra":["x]","y\"]"],"dataset_size":5})";
// Two objects whose strings carry an unbalanced brace and bracket; count_tests must still count 2.
inline constexpr std::string_view TWO_OBJECTS_WITH_BRACES = R"([{"name":"a{","model":"m"},{"name":"b]"}])";

// A string longer than its buffer is kept to the buffer, read to its closing quote (an escaped
// quote included), and counted, so within_limits() rejects the case like any other cap.
inline constexpr std::string_view MODEL_AT_CAPACITY = R"({"model":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","dataset_size":5})";
inline constexpr std::string_view MODEL_OVER_CAPACITY =
    R"({"model":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"b,c","dataset_size":5})";
inline constexpr std::string_view WHERE_FIELD_OVER_CAPACITY =
    R"({"where":{"conditions":[{"field":"ffffffffffffffffffffffffffffffffffff","op":"==","values":[1]}]},"dataset_size":5})";

// An escape whose backslash is the last character that fits the 31-character model buffer: the
// unescaped quote is the 31st character. With nothing after it the string fits; with more text the
// string is over-long, and the key after it is still read.
inline constexpr std::string_view MODEL_ESCAPE_AT_EDGE_FITS =
    R"({"model":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"","dataset_size":5})";
inline constexpr std::string_view MODEL_ESCAPE_AT_EDGE_OVER =
    R"({"model":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"xyz","dataset_size":5})";

// A number where a string belongs: the value is consumed, the field stays empty, the next key is read.
inline constexpr std::string_view NON_STRING_IN_STRING_SLOT = R"({"model":5,"name":"x","dataset_size":5})";

// `\"` and `\\` unescape to the character after the backslash, and the key after the string is read.
inline constexpr std::string_view ESCAPED_STRING = R"({"model":"a\"b\\c","dataset_size":5})";

// Input that ends inside an array, after trailing whitespace, with no terminating NUL to read past:
// the parser must stop at the end of the view rather than read one past it.
template <std::size_t N> constexpr auto without_nul(const char (&text)[N]) -> std::array<char, N - 1> {
    std::array<char, N - 1> out{};
    for (std::size_t i = 0; i + 1 < N; ++i) {
        out[i] = text[i];
    }
    return out;
}
inline constexpr auto UNTERMINATED_ARRAY = without_nul(R"({"order_by":[{"field":"a"}, )");

// The cases parsed by one parser. Exactly at the cap is legal; one more is not.
template <auto Parse> struct Cases {
    static constexpr auto order_by_2 = Parse(ORDER_BY_2);
    static constexpr auto order_by_3 = Parse(ORDER_BY_3);
    static constexpr auto fks_2 = Parse(FKS_2);
    static constexpr auto fks_3 = Parse(FKS_3);
    static constexpr auto group_4 = Parse(GROUP_4);
    static constexpr auto group_5 = Parse(GROUP_5);
    static constexpr auto distinct_3 = Parse(DISTINCT_3);
    static constexpr auto distinct_4 = Parse(DISTINCT_4);
    static constexpr auto values_4 = Parse(VALUES_4);
    static constexpr auto values_5 = Parse(VALUES_5);
    static constexpr auto distinct_and_values = Parse(DISTINCT_AND_VALUES);
    static constexpr auto values_and_group_by = Parse(VALUES_AND_GROUP_BY);
    static constexpr auto values_and_first_op = Parse(VALUES_AND_FIRST_OP);
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

    static constexpr auto having_leaf = Parse(HAVING_LEAF);
    static constexpr auto having_or = Parse(HAVING_OR);
    static constexpr auto having_unknown_combine = Parse(HAVING_UNKNOWN_COMBINE);
    static constexpr auto having_like_int = Parse(HAVING_LIKE_INT);
    static constexpr auto having_in_null = Parse(HAVING_IN_NULL);
    static constexpr auto having_flat_legacy = Parse(HAVING_FLAT_LEGACY);
    static constexpr auto having_collate = Parse(HAVING_COLLATE);
    static constexpr auto having_ungrouped = Parse(HAVING_UNGROUPED);
    static constexpr auto having_grouped_second = Parse(HAVING_GROUPED_SECOND);

    static constexpr auto where_combine_unknown = Parse(WHERE_COMBINE_UNKNOWN);
    static constexpr auto where_combine_upper_case = Parse(WHERE_COMBINE_UPPER_CASE);
    static constexpr auto where_combine_unknown_one = Parse(WHERE_COMBINE_UNKNOWN_ONE_CONDITION);
    static constexpr auto where_combine_null = Parse(WHERE_COMBINE_NULL);

    static constexpr auto brace_in_skipped_string = Parse(BRACE_IN_SKIPPED_STRING);
    static constexpr auto bracket_in_skipped_array = Parse(BRACKET_IN_SKIPPED_ARRAY);
    static constexpr auto model_at_capacity = Parse(MODEL_AT_CAPACITY);
    static constexpr auto model_over_capacity = Parse(MODEL_OVER_CAPACITY);
    static constexpr auto where_field_over_capacity = Parse(WHERE_FIELD_OVER_CAPACITY);
    static constexpr auto unterminated_array =
        Parse(std::string_view(UNTERMINATED_ARRAY.data(), UNTERMINATED_ARRAY.size()));

    static constexpr auto setop_two = Parse(SETOP_TWO);
    static constexpr auto setop_whole_table = Parse(SETOP_WHOLE_TABLE);
    static constexpr auto setop_four = Parse(SETOP_FOUR);
    static constexpr auto setop_five = Parse(SETOP_FIVE);
    static constexpr auto setop_one_operand = Parse(SETOP_ONE_OPERAND);
    static constexpr auto setop_unknown_op = Parse(SETOP_UNKNOWN_OP);
    static constexpr auto setop_missing_op = Parse(SETOP_MISSING_OP);
    static constexpr auto setop_op_on_first = Parse(SETOP_OP_ON_FIRST);
    static constexpr auto setop_unknown_operand_key = Parse(SETOP_UNKNOWN_OPERAND_KEY);
    static constexpr auto setop_legacy_type = Parse(SETOP_LEGACY_TYPE);
    static constexpr auto setop_non_object_operand = Parse(SETOP_NON_OBJECT_OPERAND);
    static constexpr auto setop_repeated_operands = Parse(SETOP_REPEATED_OPERANDS);
    static constexpr auto setop_operand_where_twice = Parse(SETOP_OPERAND_WHERE_TWICE);
    static constexpr auto setop_repeated_operands_with_where = Parse(SETOP_REPEATED_OPERANDS_WITH_WHERE);
    static constexpr auto setop_operand_where_empty = Parse(SETOP_OPERAND_WHERE_EMPTY);
    static constexpr auto setop_operand_unknown_combine = Parse(SETOP_OPERAND_UNKNOWN_COMBINE);
    static constexpr auto setop_pool_over_node_cap = Parse(SETOP_POOL_OVER_NODE_CAP);
    static constexpr auto setop_where_after = Parse(SETOP_WHERE_AFTER);
    static constexpr auto setop_intersect_first = Parse(SETOP_INTERSECT_FIRST);
    static constexpr auto setop_intersect_after_union = Parse(SETOP_INTERSECT_AFTER_UNION);
    static constexpr auto setop_intersect_after_except = Parse(SETOP_INTERSECT_AFTER_EXCEPT);
    static constexpr auto setop_with_where = Parse(SETOP_WITH_WHERE);
    static constexpr auto setop_with_join = Parse(SETOP_WITH_JOIN);
    static constexpr auto setop_value_misfit = Parse(SETOP_VALUE_MISFIT);
    static constexpr auto setop_operand_where_malformed = Parse(SETOP_OPERAND_WHERE_MALFORMED);
    static constexpr auto setop_operand_where_over_cap = Parse(SETOP_OPERAND_WHERE_OVER_CAP);

    static constexpr auto order_by_collate = Parse(ORDER_BY_COLLATE);
    static constexpr auto name_key = Parse(NAME_KEY);
    static constexpr auto test_name_key = Parse(TEST_NAME_KEY);
    static constexpr auto benchmark_keys = Parse(BENCHMARK_KEYS);
    static constexpr auto escaped_string = Parse(ESCAPED_STRING);
    static constexpr auto model_escape_at_edge_fits = Parse(MODEL_ESCAPE_AT_EDGE_FITS);
    static constexpr auto model_escape_at_edge_over = Parse(MODEL_ESCAPE_AT_EDGE_OVER);
    static constexpr auto non_string_in_string_slot = Parse(NON_STRING_IN_STRING_SLOT);

    static_assert(order_by_2.within_limits() && !order_by_3.within_limits());
    static_assert(fks_2.within_limits() && !fks_3.within_limits());
    static_assert(group_4.within_limits() && !group_5.within_limits());
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
    EXPECT_EQ(C::group_5.group_by.declared_count, 5U);
    EXPECT_EQ(C::distinct_4.distinct.declared_count, 4U);

    EXPECT_EQ(C::order_by_3.dataset_size, 5);
    EXPECT_EQ(C::fks_3.dataset_size, 5);
    EXPECT_EQ(C::group_5.dataset_size, 5);
    EXPECT_EQ(C::distinct_4.dataset_size, 5);

    EXPECT_EQ(C::order_by_3.order_by[0].field.view(), "a");
    EXPECT_EQ(C::order_by_3.order_by[1].field.view(), "b");
    EXPECT_EQ(C::fks_3.join.fks[1].view(), "b");
    EXPECT_EQ(C::group_5.group_by.fields[1].view(), "b");
    EXPECT_EQ(C::distinct_4.distinct.fields[2].view(), "c");
}

// values caps at 4 fields, and a case naming two result shapes is out of limits (#867): both make
// BenchmarkTest::within_limits() false, which every consumer turns into a compile error.
template <auto Parse> auto expect_values_limits_and_single_result_shape() -> void {
    using C = Cases<Parse>;
    EXPECT_TRUE(C::values_4.within_limits());
    EXPECT_EQ(C::values_5.values.declared_count, 5U);
    EXPECT_FALSE(C::values_5.within_limits());
    EXPECT_TRUE(C::distinct_3.within_limits());
    EXPECT_EQ(C::distinct_and_values.result_shape_count(), 2U);
    EXPECT_FALSE(C::distinct_and_values.within_limits());
    EXPECT_FALSE(C::values_and_group_by.within_limits());
    EXPECT_FALSE(C::values_and_first_op.within_limits()); // `first` is a result shape too
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
    // No `combine` means AND (the former benchmark parser used to fall through to OR).
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

// Typed values (#844): null and int64 are kinds of their own.
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

// An unknown `combine` is not read as AND (#847): the spec is not well formed, so it cannot build.
template <auto Parse> auto expect_unknown_combine_is_rejected() -> void {
    using C = Cases<Parse>;
    EXPECT_TRUE(C::where_and.where.is_well_formed());
    EXPECT_TRUE(C::where_or_first.where.is_well_formed());
    EXPECT_TRUE(C::where_no_combine.where.is_well_formed());
    EXPECT_FALSE(C::where_combine_unknown.where.is_well_formed());
    EXPECT_FALSE(C::where_combine_upper_case.where.is_well_formed());
    EXPECT_FALSE(C::where_combine_unknown_one.where.is_well_formed());
    EXPECT_FALSE(C::where_combine_null.where.is_well_formed());
    // The rest of the case is still read.
    EXPECT_EQ(C::where_combine_unknown.dataset_size, 5);
}

// HAVING is read as a tree, with the WHERE rules: a leaf keeps its operand, `combine` joins the
// conditions, and an unknown `combine` is not read as AND (#868).
template <auto Parse> auto expect_having_is_a_tree() -> void {
    using C = Cases<Parse>;
    using Kind = storm::benchmark::TypedValue::Kind;

    constexpr auto &leaf = C::having_leaf.group_by.having;
    EXPECT_TRUE(C::having_leaf.within_limits());
    EXPECT_TRUE(leaf.is_well_formed());
    EXPECT_EQ(leaf.node_count, 1U);
    EXPECT_TRUE(leaf.values[0].kind == Kind::Int);
    EXPECT_EQ(leaf.values[0].as_int, 30);

    constexpr auto &either = C::having_or.group_by.having;
    EXPECT_TRUE(either.is_well_formed());
    EXPECT_EQ(either.node_count, 3U);
    EXPECT_EQ(either.nodes[either.root].op.view(), "OR");

    EXPECT_FALSE(C::having_unknown_combine.group_by.having.is_well_formed());
    EXPECT_EQ(C::having_unknown_combine.dataset_size, 5);
}

// One parser for every shared key (#845): each entry point reads all of them.
template <auto Parse> auto expect_every_shared_key_is_read() -> void {
    using C = Cases<Parse>;
    ASSERT_EQ(C::order_by_collate.order_by_count, 1U);
    EXPECT_EQ(C::order_by_collate.order_by[0].field.view(), "name");
    EXPECT_EQ(C::order_by_collate.order_by[0].direction.view(), "DESC");
    EXPECT_EQ(C::order_by_collate.order_by[0].collate.view(), "NOCASE");

    EXPECT_EQ(C::name_key.test_name.view(), "by_name");
    EXPECT_EQ(C::test_name_key.test_name.view(), "by_test_name");

    constexpr auto &b = C::benchmark_keys;
    EXPECT_EQ(b.test_category.view(), "cat");
    EXPECT_EQ(b.description.view(), "desc");
    EXPECT_EQ(b.operation.view(), "op");
    EXPECT_EQ(b.iterations, 7);
    EXPECT_EQ(b.batch_size, 3);
    EXPECT_EQ(b.size_profile.view(), "prof");
    EXPECT_TRUE(b.setop.enabled);
    ASSERT_EQ(b.setop.operand_count, 2U);
    EXPECT_EQ(b.setop.operands[1].op.view(), "union");
    EXPECT_EQ(b.dataset_size, 5);

    EXPECT_EQ(C::escaped_string.model.view(), "a\"b\\c");
    EXPECT_EQ(C::escaped_string.dataset_size, 5);
}

// Braces and brackets inside strings do not end the value they sit in.
template <auto Parse> auto expect_braces_in_strings_are_text() -> void {
    using C = Cases<Parse>;
    EXPECT_EQ(C::brace_in_skipped_string.description.view(), "a{b]c");
    EXPECT_EQ(C::brace_in_skipped_string.dataset_size, 5);
    EXPECT_EQ(C::bracket_in_skipped_array.test_name.view(), "n{1");
    EXPECT_EQ(C::bracket_in_skipped_array.dataset_size, 5);
    // An input cut off inside an array ends the parse at the end of the view.
    EXPECT_EQ(C::unterminated_array.order_by_count, 1U);
}

// An over-long string is truncated to its buffer, read to its end, and rejected.
template <auto Parse> auto expect_over_long_strings_are_rejected() -> void {
    using C = Cases<Parse>;
    EXPECT_TRUE(C::model_at_capacity.within_limits());
    EXPECT_EQ(C::model_at_capacity.model.view().size(), 31U);
    EXPECT_FALSE(C::model_over_capacity.within_limits());
    EXPECT_EQ(C::model_over_capacity.model.view().size(), 31U);
    EXPECT_EQ(C::model_over_capacity.dataset_size, 5);
    EXPECT_FALSE(C::where_field_over_capacity.within_limits());
    EXPECT_EQ(C::where_field_over_capacity.dataset_size, 5);
}

template <auto Parse> auto expect_escape_at_buffer_edge_is_kept() -> void {
    using C = Cases<Parse>;
    EXPECT_TRUE(C::model_escape_at_edge_fits.within_limits());
    EXPECT_EQ(C::model_escape_at_edge_fits.model.view().size(), 31U);
    EXPECT_EQ(C::model_escape_at_edge_fits.model.view().back(), '"');
    EXPECT_EQ(C::model_escape_at_edge_fits.dataset_size, 5);
    EXPECT_FALSE(C::model_escape_at_edge_over.within_limits());
    EXPECT_EQ(C::model_escape_at_edge_over.model.view().size(), 31U);
    EXPECT_EQ(C::model_escape_at_edge_over.model.view().back(), '"');
    EXPECT_EQ(C::model_escape_at_edge_over.dataset_size, 5);
}

template <auto Parse> auto expect_non_string_value_is_consumed() -> void {
    using C = Cases<Parse>;
    EXPECT_TRUE(C::non_string_in_string_slot.model.view().empty());
    EXPECT_EQ(C::non_string_in_string_slot.test_name.view(), "x");
    EXPECT_EQ(C::non_string_in_string_slot.dataset_size, 5);
    EXPECT_TRUE(C::non_string_in_string_slot.within_limits());
}

// Set-op chains (#870): every operand is read, each count goes past its cap, and each malformed
// spelling is recorded for the builder to reject; the key after the setop is still read.
template <auto Parse> auto expect_setop_chain_is_parsed() -> void {
    using C = Cases<Parse>;
    // A chain is well formed against the case's `where`, the pool its operands' trees live in.
    constexpr auto well_formed = [](const auto &c) { return c.setop.is_well_formed(c.where); };
    constexpr auto &two = C::setop_two;
    ASSERT_TRUE(two.setop.enabled);
    ASSERT_EQ(two.setop.operand_count, 2U);
    EXPECT_FALSE(two.where.enabled); // the pool is not the case's own WHERE
    EXPECT_EQ(two.where.forms, 0U);
    EXPECT_TRUE(two.setop.operands[0].op.empty());
    ASSERT_EQ(two.setop.operands[0].root, 0);
    EXPECT_EQ(two.where.nodes[0].field.view(), "age");
    EXPECT_EQ(two.where.values[two.where.nodes[0].first_value].as_int, 30);
    EXPECT_EQ(two.setop.operands[1].op.view(), "union");
    ASSERT_EQ(two.setop.operands[1].root, 1);
    EXPECT_EQ(two.where.nodes[1].op.view(), "<");
    EXPECT_EQ(two.where.values[two.where.nodes[1].first_value].as_int, 20);
    EXPECT_TRUE(well_formed(two) && two.setop.is_portable_chain() && two.within_limits());
    EXPECT_EQ(two.dataset_size, 5);

    EXPECT_EQ(C::setop_whole_table.setop.operands[0].root, -1);
    EXPECT_TRUE(well_formed(C::setop_whole_table));

    EXPECT_TRUE(C::setop_four.within_limits());
    EXPECT_EQ(C::setop_five.setop.operand_count, 5U);
    EXPECT_FALSE(C::setop_five.within_limits());
    EXPECT_EQ(C::setop_five.dataset_size, 5);

    EXPECT_FALSE(well_formed(C::setop_one_operand));
    EXPECT_TRUE(well_formed(C::setop_unknown_op)); // the op NAME is QueryBuilder's ValidSetOp
    EXPECT_FALSE(well_formed(C::setop_missing_op));
    EXPECT_FALSE(well_formed(C::setop_op_on_first));
    EXPECT_EQ(C::setop_unknown_operand_key.setop.operands[1].unknown_keys, 1U);
    EXPECT_FALSE(well_formed(C::setop_unknown_operand_key));
    EXPECT_EQ(C::setop_legacy_type.setop.unknown_keys, 1U);
    EXPECT_FALSE(well_formed(C::setop_legacy_type));
    EXPECT_EQ(C::setop_non_object_operand.setop.operand_count, 2U);
    EXPECT_FALSE(well_formed(C::setop_non_object_operand));
    EXPECT_EQ(C::setop_non_object_operand.dataset_size, 5);
    // A repeated `operands` replaces the list; the first list's trees stay in the pool, unreached.
    EXPECT_EQ(C::setop_repeated_operands.setop.operand_count, 2U);
    EXPECT_EQ(C::setop_repeated_operands.setop.operands[1].op.view(), "except");
    EXPECT_TRUE(well_formed(C::setop_repeated_operands)); // the first list had no WHERE: nothing left behind
    EXPECT_FALSE(well_formed(C::setop_repeated_operands_with_where));
    EXPECT_EQ(C::setop_operand_where_twice.setop.operands[1].where_forms, 2U);
    EXPECT_FALSE(well_formed(C::setop_operand_where_twice));
    EXPECT_FALSE(well_formed(C::setop_operand_where_empty));

    EXPECT_TRUE(C::setop_intersect_first.setop.is_portable_chain());
    EXPECT_FALSE(C::setop_intersect_after_union.setop.is_portable_chain());
    EXPECT_FALSE(C::setop_intersect_after_except.setop.is_portable_chain());

    EXPECT_FALSE(well_formed(C::setop_operand_where_malformed));
    EXPECT_FALSE(well_formed(C::setop_operand_unknown_combine));
    EXPECT_FALSE(C::setop_operand_where_over_cap.within_limits());
    // The operands share the WHERE caps: four 2-leaf ANDs are 12 nodes, past the pool's 7.
    EXPECT_FALSE(C::setop_pool_over_node_cap.within_limits());
    // A top-level `where:` beside `setop:` keeps the pool enabled, before or after the setop key.
    EXPECT_TRUE(C::setop_with_where.where.enabled);
    EXPECT_TRUE(C::setop_where_after.where.enabled);
    EXPECT_TRUE(C::setop_where_after.within_limits()); // one WHERE form: the operands' do not count
}

} // namespace storm::test::spec_limits
