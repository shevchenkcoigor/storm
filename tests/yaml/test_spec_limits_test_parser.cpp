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

    using C = Cases<parse_case>;

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
    static_assert(Buildable<C::fks_2>);
    static_assert(Buildable<C::group_2>);
    static_assert(Buildable<C::distinct_3>);
    static_assert(!Buildable<C::order_by_3>);
    static_assert(!Buildable<C::fks_3>);
    static_assert(!Buildable<C::group_3>);
    static_assert(!Buildable<C::distinct_4>);

    TEST(SpecLimitsTestParser, MalformedAndRepeatedInputIsHandled) {
        storm::test::spec_limits::expect_malformed_and_repeated_input_is_handled<parse_case>();
    }

    TEST(SpecLimitsTestParser, CountsPastEveryCapAndKeepsParsing) {
        storm::test::spec_limits::expect_counts_past_every_cap<parse_case>();
    }

} // namespace
