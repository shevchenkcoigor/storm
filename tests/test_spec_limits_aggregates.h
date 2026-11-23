#pragma once

/**
 * @file test_spec_limits_aggregates.h
 * @brief The aggregate spec-limit cases (#868), run through each entry point of the one spec parser.
 *
 * The same contract as test_spec_limits.h, for `aggregate:` / `aggregations:`: one AggregateSpec per
 * step, up to 3 fields each (one operand, `func(a + b + c)`) and up to 5 steps. `aggregate:` is the
 * one-step shorthand of `aggregations:`; `field:` is a one-element `fields`, and an empty `field` is no
 * field (COUNT(*)). Writing both forms is rejected, like two WHERE forms. Every count goes past its cap,
 * so within_limits() can reject the case, and the key after an over-long list is still read.
 *
 * The storm_benchmark_schema module and std must already be imported, directly or through test_parser.hpp.
 */

#include <gtest/gtest.h>

namespace storm::test::spec_limits {

inline constexpr std::string_view AGG_SHORTHAND = R"({"aggregate":{"func":"avg","field":"age"},"dataset_size":5})";
inline constexpr std::string_view AGG_COUNT_STAR = R"({"aggregate":{"func":"count","field":""},"dataset_size":5})";
inline constexpr std::string_view AGG_FIELDS_3 =
    R"({"aggregate":{"func":"sum","fields":["a","b","c"]},"dataset_size":5})";
inline constexpr std::string_view AGG_FIELDS_4 =
    R"({"aggregate":{"func":"sum","fields":["a","b","c","d"]},"dataset_size":5})";
inline constexpr std::string_view AGG_FIELD_REPLACES_FIELDS =
    R"({"aggregate":{"func":"sum","fields":["a","b"],"field":"c"},"dataset_size":5})";
// An empty name is no field in both spellings: `field: ''` and an empty slot of `fields:`.
inline constexpr std::string_view AGG_FIELDS_EMPTY_SLOT =
    R"({"aggregate":{"func":"sum","fields":["","a"]},"dataset_size":5})";
inline constexpr std::string_view AGGREGATIONS_5 =
    R"({"aggregations":[{"func":"count"},{"func":"sum","field":"a"},{"func":"avg","fields":["a","b"]},{"func":"min","field":"a"},{"func":"max","field":"a"}],"dataset_size":5})";
inline constexpr std::string_view AGGREGATIONS_6 =
    R"({"aggregations":[{"func":"count"},{"func":"sum","field":"a"},{"func":"avg","field":"a"},{"func":"min","field":"a"},{"func":"max","field":"a"},{"func":"sum","field":"b"}],"dataset_size":5})";
inline constexpr std::string_view AGGREGATIONS_STEP_OVER_FIELDS =
    R"({"aggregations":[{"func":"count"},{"func":"sum","fields":["a","b","c","d"]}],"dataset_size":5})";
inline constexpr std::string_view AGGREGATE_BOTH_FORMS =
    R"({"aggregate":{"func":"count"},"aggregations":[{"func":"sum","field":"a"}],"dataset_size":5})";

template <auto Parse> struct AggregateCases {
    static constexpr auto shorthand = Parse(AGG_SHORTHAND);
    static constexpr auto count_star = Parse(AGG_COUNT_STAR);
    static constexpr auto fields_3 = Parse(AGG_FIELDS_3);
    static constexpr auto fields_4 = Parse(AGG_FIELDS_4);
    static constexpr auto field_replaces_fields = Parse(AGG_FIELD_REPLACES_FIELDS);
    static constexpr auto fields_empty_slot = Parse(AGG_FIELDS_EMPTY_SLOT);
    static constexpr auto aggregations_5 = Parse(AGGREGATIONS_5);
    static constexpr auto aggregations_6 = Parse(AGGREGATIONS_6);
    static constexpr auto step_over_fields = Parse(AGGREGATIONS_STEP_OVER_FIELDS);
    static constexpr auto both_forms = Parse(AGGREGATE_BOTH_FORMS);

    static_assert(fields_3.within_limits() && !fields_4.within_limits());
    static_assert(aggregations_5.within_limits() && !aggregations_6.within_limits());
    static_assert(!step_over_fields.within_limits());
    static_assert(shorthand.within_limits() && !both_forms.within_limits());
};

template <auto Parse> auto expect_aggregates_parse_and_count_past_their_caps() -> void {
    using C = AggregateCases<Parse>;
    constexpr auto &one = C::shorthand;
    EXPECT_EQ(one.aggregation_count, 1U);
    EXPECT_EQ(one.aggregations[0].func.view(), "avg");
    EXPECT_EQ(one.aggregations[0].fields.declared_count, 1U);
    EXPECT_EQ(one.aggregations[0].fields.fields[0].view(), "age");
    EXPECT_EQ(one.result_shape_count(), 1U);

    EXPECT_EQ(C::count_star.aggregation_count, 1U);
    EXPECT_EQ(C::count_star.aggregations[0].fields.declared_count, 0U); // COUNT(*)

    EXPECT_EQ(C::fields_3.aggregations[0].fields.fields[2].view(), "c");
    EXPECT_EQ(C::fields_4.aggregations[0].fields.declared_count, 4U);
    EXPECT_EQ(C::fields_4.dataset_size, 5);
    // A repeated field key replaces the list before it, entries and all.
    EXPECT_EQ(C::field_replaces_fields.aggregations[0].fields.declared_count, 1U);
    EXPECT_EQ(C::field_replaces_fields.aggregations[0].fields.fields[0].view(), "c");
    EXPECT_TRUE(C::field_replaces_fields.aggregations[0].fields.fields[1].empty());

    EXPECT_EQ(C::fields_empty_slot.aggregations[0].fields.declared_count, 1U);
    EXPECT_EQ(C::fields_empty_slot.aggregations[0].fields.fields[0].view(), "a");

    constexpr auto &five = C::aggregations_5;
    EXPECT_EQ(five.aggregation_count, 5U);
    EXPECT_EQ(five.aggregations[0].func.view(), "count");
    EXPECT_EQ(five.aggregations[2].func.view(), "avg");
    EXPECT_EQ(five.aggregations[2].fields.declared_count, 2U);
    EXPECT_EQ(five.aggregations[4].func.view(), "max");
    EXPECT_EQ(C::aggregations_6.aggregation_count, 6U);
    EXPECT_EQ(C::aggregations_6.dataset_size, 5);
    EXPECT_EQ(C::both_forms.aggregate_forms, 2U);
}

} // namespace storm::test::spec_limits
