#include <gtest/gtest.h>

import std;
import storm_benchmark_parser;
import storm_benchmark_schema;

#include "test_spec_limits.h"

// Issue #843/#845: the benchmark entry point of the one spec parser (parse_test_object)
// against the shared spec-limit cases.

namespace {

    constexpr auto parse_case(std::string_view json) -> storm::benchmark::BenchmarkTest {
        std::size_t pos = 0;
        return storm::benchmark::parse_test_object(json, pos);
    }

    TEST(SpecLimitsBenchmarkParser, WhereRepeatedKeyAndStrayBraceAreHandled) {
        storm::test::spec_limits::expect_where_repeated_key_and_stray_brace_are_handled<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, FlatWhereBuildsOneTree) {
        storm::test::spec_limits::expect_flat_where_builds_one_tree<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, TypedValuesParseAsNullAndInt64) {
        storm::test::spec_limits::expect_typed_values_parse<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, WhereCountsPastItsCaps) {
        storm::test::spec_limits::expect_where_counts_past_its_caps<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, MalformedAndRepeatedInputIsHandled) {
        storm::test::spec_limits::expect_malformed_and_repeated_input_is_handled<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, ValuesCapsAndOneResultShapePerCase) {
        storm::test::spec_limits::expect_values_limits_and_single_result_shape<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, CountsPastEveryCapAndKeepsParsing) {
        storm::test::spec_limits::expect_counts_past_every_cap<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, UnknownCombineIsRejected) {
        storm::test::spec_limits::expect_unknown_combine_is_rejected<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, HavingValueIsScalarAndObjectIsRejected) {
        storm::test::spec_limits::expect_having_value_is_scalar<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, EverySharedKeyIsRead) {
        storm::test::spec_limits::expect_every_shared_key_is_read<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, BracesInStringsAreText) {
        storm::test::spec_limits::expect_braces_in_strings_are_text<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, OverLongStringsAreRejected) {
        storm::test::spec_limits::expect_over_long_strings_are_rejected<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, EscapeAtBufferEdgeIsKept) {
        storm::test::spec_limits::expect_escape_at_buffer_edge_is_kept<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, NonStringValueIsConsumed) {
        storm::test::spec_limits::expect_non_string_value_is_consumed<parse_case>();
    }

    // A string cut off by the end of the input right after a backslash: skipping it stops at the end
    // of the view instead of stepping one past it.
    constexpr auto kLoneBackslash = storm::test::spec_limits::without_nul(R"("abc\)");
    constexpr auto skip_end(std::string_view j) -> std::size_t {
        std::size_t p = 0;
        storm::benchmark::skip_value(j, p);
        return p;
    }
    constexpr auto parse_str_end(std::string_view j) -> std::size_t {
        std::size_t p         = 0;
        std::size_t truncated = 0;
        (void)storm::benchmark::parse_str<8>(j, p, truncated);
        return p;
    }

    TEST(SpecLimitsBenchmarkParser, LoneTrailingBackslashStopsAtEndOfInput) {
        constexpr std::string_view j(kLoneBackslash.data(), kLoneBackslash.size());
        constexpr std::size_t      skipped = skip_end(j);
        constexpr std::size_t      parsed  = parse_str_end(j);
        EXPECT_EQ(skipped, j.size());
        EXPECT_EQ(parsed, j.size());
    }

    // parse_str on a value that is not a string consumes the whole value, so the key after it is
    // found directly rather than through an empty key.
    TEST(SpecLimitsBenchmarkParser, ParseStrConsumesANonStringValue) {
        constexpr std::size_t number = parse_str_end(R"(5, "x")");
        constexpr std::size_t null   = parse_str_end(R"(null, "x")");
        constexpr std::size_t object = parse_str_end(R"({"a":"}"}, "x")");
        EXPECT_EQ(number, 1U);
        EXPECT_EQ(null, 4U);
        EXPECT_EQ(object, 9U);
    }

    TEST(SpecLimitsBenchmarkParser, CountTestsIgnoresBracesInStrings) {
        constexpr std::size_t n = storm::benchmark::count_tests(storm::test::spec_limits::TWO_OBJECTS_WITH_BRACES);
        EXPECT_EQ(n, 2U);
        constexpr auto tests = storm::benchmark::parse_tests<2>(storm::test::spec_limits::TWO_OBJECTS_WITH_BRACES);
        EXPECT_EQ(tests[0].model.view(), "m");
        EXPECT_EQ(tests[1].test_name.view(), "b]");
    }

} // namespace
