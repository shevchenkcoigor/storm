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

    static_assert(order_by_2.within_limits() && !order_by_3.within_limits());
    static_assert(fks_2.within_limits() && !fks_3.within_limits());
    static_assert(group_2.within_limits() && !group_3.within_limits());
    static_assert(distinct_3.within_limits() && !distinct_4.within_limits());
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

} // namespace storm::test::spec_limits
