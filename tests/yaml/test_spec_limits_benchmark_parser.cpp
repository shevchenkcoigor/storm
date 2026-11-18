#include <gtest/gtest.h>

import std;
import storm_benchmark_parser;
import storm_benchmark_schema;

#include "test_spec_limits.h"

// Issue #843: the benchmark parser against the shared spec-limit cases.

namespace {

    constexpr auto parse_case(std::string_view json) -> storm::benchmark::BenchmarkTest {
        std::size_t pos = 0;
        return storm::benchmark::parse_test_object(json, pos);
    }

    TEST(SpecLimitsBenchmarkParser, MalformedAndRepeatedInputIsHandled) {
        storm::test::spec_limits::expect_malformed_and_repeated_input_is_handled<parse_case>();
    }

    TEST(SpecLimitsBenchmarkParser, CountsPastEveryCapAndKeepsParsing) {
        storm::test::spec_limits::expect_counts_past_every_cap<parse_case>();
    }

} // namespace
