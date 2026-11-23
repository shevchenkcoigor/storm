// Issue #868: the aggregate spec limits through the test entry point (tests/test_parser.hpp), the
// per-step `result` keys only the test parser reads, and the shared QueryBuilder rejecting an
// aggregate spec past a cap. The benchmark entry point runs the same shared cases in
// test_spec_limits_benchmark_parser.cpp.

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
#include "test_select_runner.h"
#include "test_parser.hpp"
#include "test_spec_limits_aggregates.h"

namespace {

    using storm::orm::query_builder::QueryBuilder;

    constexpr auto parse_test_case(std::string_view json) -> storm::test::TestCase {
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc;
    }

    constexpr auto parse_case(std::string_view json) -> storm::benchmark::BenchmarkTest {
        return parse_test_case(json).bench;
    }

    template <auto Spec>
    concept Buildable = requires { typename QueryBuilder<Person, Spec>; };

    // 3 fields per step and 5 steps build; one more of either, or both forms at once, does not.
    using AC = storm::test::spec_limits::AggregateCases<parse_case>;
    static_assert(Buildable<AC::fields_3> && !Buildable<AC::fields_4>);
    static_assert(Buildable<AC::aggregations_5> && !Buildable<AC::aggregations_6>);
    static_assert(!Buildable<AC::step_over_fields> && !Buildable<AC::both_forms>);

    // A step the builder cannot apply fails the SpecAggregatesWellFormed constraint at the use site
    // (#868): a GROUP BY with no aggregate, or a step other than count with no field.
    constexpr auto kGroupNoAggregation = parse_case(R"({"group_by":{"fields":["age"]},"dataset_size":5})");
    constexpr auto kGroupCount         = parse_case(R"({"group_by":{"fields":["age"]},"aggregate":{"func":"count"}})");
    constexpr auto kSumNoField         = parse_case(R"({"aggregate":{"func":"sum"},"dataset_size":5})");
    constexpr auto kChainAvgNoField =
            parse_case(R"({"aggregations":[{"func":"count"},{"func":"avg","fields":[]}],"dataset_size":5})");
    constexpr auto kCountStar = parse_case(R"({"aggregate":{"func":"count"},"dataset_size":5})");
    static_assert(!Buildable<kGroupNoAggregation> && Buildable<kGroupCount>);
    static_assert(!Buildable<kSumNoField> && !Buildable<kChainAvgNoField> && Buildable<kCountStar>);
    // A step's func is one of the ValidAggregate names, checked at the same naming site: an unknown func
    // or a step with fields but no func is rejected there, not deep inside apply_aggregation.
    constexpr auto kUnknownFunc = parse_case(R"({"aggregate":{"func":"median","field":"age"},"dataset_size":5})");
    constexpr auto kStepWithoutFunc =
            parse_case(R"({"aggregations":[{"func":"count"},{"fields":["age"]}],"dataset_size":5})");
    constexpr auto kSumAge = parse_case(R"({"aggregate":{"func":"sum","field":"age"},"dataset_size":5})");
    static_assert(!Buildable<kUnknownFunc> && !Buildable<kStepWithoutFunc> && Buildable<kSumAge>);
    // count_field with no field (COUNT(*) under the wrong name) is AggregateRunner::run's static_assert: it is
    // a TestCase/runner rule, not a builder constraint, so it cannot be probed with Buildable here.

    // A step over several fields is one operand: the builder splices `sum<age, name>()`, which the
    // existing NumericAggregateSelectors gate (#475) rejects, so a YAML `fields: [age, name]` on a
    // numeric aggregate stays a compile error rather than a coerced SUM.
    using storm::orm::statements::NumericAggregateSelectors;
    static_assert(NumericAggregateSelectors<
                  storm::meta::selector_for<^^Person::age>(),
                  storm::meta::selector_for<^^Person::salary>()>);
    static_assert(!NumericAggregateSelectors<
                  storm::meta::selector_for<^^Person::age>(),
                  storm::meta::selector_for<^^Person::name>()>);

    TEST(SpecLimitsAggregatesTestParser, AggregatesParseAndCountPastTheirCaps) {
        storm::test::spec_limits::expect_aggregates_parse_and_count_past_their_caps<parse_case>();
    }

    // Each step's `result`: `value` for a scalar or chain step, `groups` (one value per group, in
    // ascending key order) for a GROUP BY step. The step's shared keys still reach the shared parser,
    // and a key `result` does not read (the old `type`) is skipped.
    constexpr auto kResults = parse_test_case(
            R"({"aggregations":[{"func":"count","result":{"type":"int","value":3,"groups":[10,8,7]}},{"func":"avg","fields":["age","salary"],"result":{"groups":[1.5,2.5]}}],"dataset_size":5})"
    );
    // The scalar shorthand: the value comes from expected.int_val / dbl_val, the func from query_type.
    constexpr auto kScalar = parse_test_case(
            R"({"query_type":"sum","aggregate":{"fields":["age","salary"]},"expected":{"dbl_val":2.5}})"
    );
    // A group_<func> case with no `aggregate:` is that func over no field.
    constexpr auto kGroupNoAggregate = parse_test_case(R"({"query_type":"group_count","group_by":{"fields":["age"]}})");
    static_assert(kResults.within_limits());

    TEST(SpecLimitsAggregatesTestParser, AggregationResultsAreReadPerStep) {
        EXPECT_EQ(kResults.bench.aggregation_count, 2U);
        EXPECT_DOUBLE_EQ(kResults.results[0].value, 3.0);
        EXPECT_EQ(kResults.results[0].groups_count, 3U);
        EXPECT_DOUBLE_EQ(kResults.results[0].groups[2], 7.0);
        EXPECT_EQ(kResults.results[1].groups_count, 2U);
        EXPECT_DOUBLE_EQ(kResults.results[1].groups[1], 2.5);
        EXPECT_EQ(kResults.bench.aggregations[1].fields.declared_count, 2U);
        EXPECT_EQ(kResults.bench.dataset_size, 5);
    }

    TEST(SpecLimitsAggregatesTestParser, ScalarAndGroupCasesBecomeAChainOfOne) {
        EXPECT_EQ(kScalar.bench.aggregation_count, 1U);
        EXPECT_EQ(kScalar.bench.aggregations[0].func.view(), "sum");
        EXPECT_EQ(kScalar.bench.aggregations[0].fields.declared_count, 2U);
        EXPECT_DOUBLE_EQ(kScalar.results[0].value, 2.5);

        EXPECT_EQ(kGroupNoAggregate.bench.aggregation_count, 1U);
        EXPECT_EQ(kGroupNoAggregate.bench.aggregations[0].func.view(), "count");
        EXPECT_EQ(kGroupNoAggregate.bench.aggregations[0].fields.declared_count, 0U);
    }

    // A scalar or group_<func> case is ONE aggregation whose func is the query_type's (#868): the parser
    // writes that func into step 0, so a second step, a contradicting func or a step's own `result`
    // value would otherwise be dropped or overwritten without a word. count_field is COUNT(field), so
    // `func: count` and `func: count_field` both agree with it.
    constexpr auto kScalarTwoSteps = parse_test_case(
            R"({"query_type":"sum","aggregations":[{"func":"sum","field":"age"},{"func":"avg","field":"age"}]})"
    );
    constexpr auto kScalarOtherFunc =
            parse_test_case(R"({"query_type":"sum","aggregate":{"func":"avg","field":"age"}})");
    constexpr auto kGroupOtherFunc = parse_test_case(
            R"({"query_type":"group_count","group_by":{"fields":["age"]},"aggregate":{"func":"sum","field":"age"}})"
    );
    constexpr auto kScalarStepValue = parse_test_case(
            R"({"query_type":"sum","aggregations":[{"func":"sum","field":"age","result":{"value":5}}],"expected":{"int_val":5}})"
    );
    constexpr auto kCountFieldAsCount =
            parse_test_case(R"({"query_type":"count_field","aggregate":{"func":"count","field":"id"}})");
    constexpr auto kCountFieldAsItself =
            parse_test_case(R"({"query_type":"count_field","aggregate":{"func":"count_field","field":"id"}})");
    constexpr auto kGroupSameFunc = parse_test_case(
            R"({"query_type":"group_sum","group_by":{"fields":["age"]},"aggregate":{"func":"sum","field":"age"}})"
    );
    constexpr auto kChainTwoSteps =
            parse_test_case(R"({"query_type":"chain","aggregations":[{"func":"count"},{"func":"sum","field":"age"}]})");

    TEST(SpecLimitsAggregatesTestParser, ScalarAndGroupCasesAreOneAggregationOfTheirOwnFunc) {
        EXPECT_FALSE(kScalarTwoSteps.aggregate_shape_well_formed());
        EXPECT_FALSE(kScalarOtherFunc.aggregate_shape_well_formed());
        EXPECT_FALSE(kGroupOtherFunc.aggregate_shape_well_formed());
        EXPECT_FALSE(kScalarStepValue.aggregate_shape_well_formed());
        EXPECT_TRUE(kCountFieldAsCount.aggregate_shape_well_formed());
        EXPECT_TRUE(kCountFieldAsItself.aggregate_shape_well_formed());
        EXPECT_TRUE(kGroupSameFunc.aggregate_shape_well_formed());
        EXPECT_TRUE(kChainTwoSteps.aggregate_shape_well_formed());
        EXPECT_TRUE(kScalar.aggregate_shape_well_formed());
    }

    // Each `result` key is read where it applies (#868): `value` on an ungrouped step, `groups` on a
    // grouped one, and `groups` only with the group keys (expected.groups_count) they line up with.
    constexpr auto kGroupsOnUngrouped = parse_test_case(
            R"({"query_type":"chain","aggregations":[{"func":"count","result":{"value":2,"groups":[1]}},{"func":"sum","field":"age","result":{"value":3}}]})"
    );
    constexpr auto kValueOnGrouped = parse_test_case(
            R"({"query_type":"chain","group_by":{"fields":["age"]},"aggregations":[{"func":"count","result":{"value":2,"groups":[1]}},{"func":"sum","field":"age","result":{"groups":[3]}}],"expected":{"groups_count":1,"group_keys":[30]}})"
    );
    constexpr auto kGroupsWithoutKeys = parse_test_case(
            R"({"query_type":"chain","group_by":{"fields":["age"]},"aggregations":[{"func":"count","result":{"groups":[1]}},{"func":"sum","field":"age","result":{"groups":[3]}}]})"
    );
    constexpr auto kGroupsWithKeys = parse_test_case(
            R"({"query_type":"chain","group_by":{"fields":["age"]},"aggregations":[{"func":"count","result":{"groups":[1]}},{"func":"sum","field":"age","result":{"groups":[3]}}],"expected":{"groups_count":1,"group_keys":[30]}})"
    );

    TEST(SpecLimitsAggregatesTestParser, EachResultKeyIsOneTheRunnerReads) {
        EXPECT_FALSE(kGroupsOnUngrouped.aggregate_results_well_formed());
        EXPECT_FALSE(kValueOnGrouped.aggregate_results_well_formed());
        EXPECT_FALSE(kGroupsWithoutKeys.aggregate_results_well_formed());
        EXPECT_TRUE(kGroupsWithKeys.aggregate_results_well_formed());
        EXPECT_TRUE(kChainTwoSteps.aggregate_results_well_formed());
        EXPECT_TRUE(kScalar.aggregate_results_well_formed());
    }

    // `groups` counts past ExpectedSpec::MAX_GROUPS (20), so an over-long list is rejected.
    constexpr auto kGroups21 = parse_test_case(
            R"({"aggregations":[{"func":"count","result":{"groups":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21]}}],"dataset_size":5})"
    );

    TEST(SpecLimitsAggregatesTestParser, GroupsCountPastTheCap) {
        EXPECT_EQ(kGroups21.results[0].groups_count, 21U);
        EXPECT_FALSE(kGroups21.within_limits());
        EXPECT_EQ(kGroups21.bench.dataset_size, 5);
    }

} // namespace
