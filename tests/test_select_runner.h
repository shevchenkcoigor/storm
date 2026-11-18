#pragma once

/**
 * @file test_select_runner.h
 * @brief Field dispatch, WHERE builder, shared checks, and SELECT runners.
 *
 * Provides:
 * - dispatch_field<Model>(name) — via storm::orm::query_builder
 * - build_compare_expr<FieldInfo, op>(value) — YAML operator -> ORM expression (shared)
 * - base_query<Tc, Model, ConnType>() — the case's filtered/joined QuerySet
 * - seed_records<Tc, Model, ConnType>() — insert the case's dataset
 * - expect_agg_value / verify_groups — the one aggregate-value check
 * - SelectRunner, AggregateRunner, ChainAggRunner, DistinctRunner, GroupByRunner:
 *   stateless, each a `static run<Tc>()` (#837)
 *
 * Include AFTER `import storm;` — it pulls the model headers it needs itself.
 */

#include <algorithm>
#include <meta>
#include <string_view>
#include <utility>
#include <vector>

#include "shared/query_builder.hpp" // NOLINT(misc-header-include-cycle)

#include "test_record_helpers.h"
using storm::QuerySet;
using storm::meta::FieldRef;

namespace storm::test {

// Re-use the shared field dispatcher — no duplication with bench side.
using storm::orm::query_builder::build_compare_expr;
using storm::orm::query_builder::call_query_method;
using storm::orm::query_builder::dispatch_field;
using storm::orm::query_builder::field_or_null;
using storm::orm::query_builder::member_function_named;

// One check for an aggregate value (#837), chosen by the result type: MIN/MAX/AVG
// read back as std::optional<double> (#416) — a near match, or nullopt when
// expect_null (empty set / all-NULL group); a floating value is a near match;
// COUNT/SUM integers match exactly.
template <typename Value>
auto expect_agg_value(const Value &actual, double expected, bool expect_null, std::string_view what) -> void {
    if constexpr (std::is_same_v<Value, std::optional<double>>) {
        if (expect_null) {
            EXPECT_FALSE(actual.has_value()) << what << ": MIN/MAX/AVG over an empty set must be nullopt (#416)";
            return;
        }
        ASSERT_TRUE(actual.has_value()) << what << ": aggregate over a non-empty set must hold a value";
        EXPECT_NEAR(actual.value(), expected, 0.01) << what;
    } else if constexpr (std::is_floating_point_v<Value>) {
        EXPECT_NEAR(actual, expected, 0.01) << what;
    } else {
        EXPECT_EQ(static_cast<double>(static_cast<std::int64_t>(expected)), expected)
            << what << ": an integer aggregate needs an integral expectation";
        EXPECT_EQ(actual, static_cast<std::int64_t>(expected)) << what;
    }
}

// The shared QueryBuilder for a case; a JOIN names its FK by member name on Model (#837).
// member_name_resolver<Model> is spelled out only because ConnType follows it in
// QueryBuilder's parameter list.
template <const auto &Tc, typename Model, typename ConnType>
using BuilderFor =
    storm::orm::query_builder::QueryBuilder<Model, Tc.bench, storm::orm::query_builder::member_name_resolver<Model>,
                                            ConnType>;

// The case's filtered/joined QuerySet: the WHERE tree, join, order and limit, all from the
// shared QueryBuilder (#843).
template <const auto &Tc, typename Model, typename ConnType> auto base_query() -> QuerySet<Model, ConnType, true> {
    return BuilderFor<Tc, Model, ConnType>::build_qs();
}

// Insert the case's `dataset_size` records.
template <const auto &Tc, typename Model, typename ConnType> auto seed_records() -> void {
    std::vector<Model> initial;
    initial.reserve(static_cast<std::size_t>(Tc.bench.dataset_size));
    for (int i = 0; i < Tc.bench.dataset_size; ++i)
        initial.push_back(make_record<Model>(i));
    QuerySet<Model, ConnType, true> qs;
    auto ins = qs.insert(std::span<const Model>(initial)).execute();
    ASSERT_TRUE(ins.has_value()) << ins.error().message();
}

// Per-group check: sorted by key, each group's key and aggregate against the case.
// MIN/MAX/AVG groups read back as std::optional<double> (#416).
template <const auto &Tc> auto verify_groups(const auto &rows) -> void {
    static_assert(Tc.bench.group_by.field_count() == 1, "per-group expectations need a single GROUP BY field");
    using Row = std::ranges::range_value_t<std::remove_cvref_t<decltype(rows)>>;
    using Agg = std::tuple_element_t<1, Row>;
    std::vector<std::pair<int, Agg>> groups;
    for (const auto &[key, agg] : rows)
        groups.emplace_back(static_cast<int>(key), agg);
    std::ranges::sort(groups, std::less<>{}, &std::pair<int, Agg>::first);
    ASSERT_EQ(groups.size(), Tc.expected.groups_count);
    // Only MIN/MAX/AVG carry group_agg_dbl; COUNT/SUM (even over a double column) use group_agg_int.
    constexpr bool is_double = std::is_same_v<Agg, std::optional<double>>;
    for (std::size_t g = 0; g < Tc.expected.groups_count; ++g) {
        EXPECT_EQ(groups[g].first, Tc.expected.group_keys[g]) << "Group key mismatch at index " << g;
        const double expected =
            is_double ? Tc.expected.group_agg_dbl[g] : static_cast<double>(Tc.expected.group_agg_int[g]);
        expect_agg_value(groups[g].second, expected, Tc.expected.group_agg_null[g], "group aggregate");
    }
}

// ---------------------------------------------------------------------------
// SelectRunner -- plain SELECT queries (select / first / one)
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class SelectRunner {
    // Compare one row member against its expected value, dispatching on the
    // member's C++ type. A YAML value of the wrong kind fails to compile.
    // Expected is a parser TypedValue; deduced (not named) so this header does not
    // need the benchmark schema module that only the YAML TUs import.
    template <auto Expected, typename Member> static auto expect_member_eq(const Member &actual) -> void {
        using Kind = typename decltype(Expected)::Kind;
        if constexpr (std::is_same_v<Member, std::string>) {
            static_assert(Expected.kind == Kind::String, "expected.first: string member needs a string value");
            EXPECT_EQ(actual, Expected.as_string.view());
        } else if constexpr (std::is_same_v<Member, bool>) {
            static_assert(Expected.kind == Kind::Bool, "expected.first: bool member needs a bool value");
            EXPECT_EQ(actual, Expected.as_bool);
        } else if constexpr (std::is_floating_point_v<Member>) {
            static_assert(Expected.kind == Kind::Double || Expected.kind == Kind::Int,
                          "expected.first: floating member needs a numeric value");
            constexpr double expected =
                Expected.kind == Kind::Double ? Expected.as_double : static_cast<double>(Expected.as_int);
            EXPECT_NEAR(actual, expected, 0.01);
        } else {
            static_assert(std::is_integral_v<Member>, "expected.first: unsupported member type");
            static_assert(Expected.kind == Kind::Int, "expected.first: integer member needs an int value");
            EXPECT_EQ(actual, Expected.as_int);
        }
    }

    template <const auto &Tc, std::size_t I, typename Row> static auto verify_first_field(const Row &row) -> void {
        constexpr auto &field = Tc.expected.first.fields[I];
        static_assert(storm::orm::query_builder::has_field<Row>(field.name.view()),
                      "expected.first names a member the row type does not have");
        constexpr auto member = dispatch_field<Row>(field.name.view());
        expect_member_eq<field.value>(row.[:member:]);
    }

  public:
    // #834: walks the case's `expected.first` keys and splices each named member,
    // replacing one hand-written `has_<field>` branch per model field.
    template <const auto &Tc, typename Row> static auto verify_first_row(const Row &row) -> void {
        constexpr auto &spec = Tc.expected.first;
        static_assert(spec.count <= spec.MAX_FIELDS, "expected.first has more keys than FirstRowSpec::MAX_FIELDS");
        template for (constexpr std::size_t I : std::views::iota(0uz, spec.count)) verify_first_field<Tc, I>(row);
    }

    template <const auto &Tc> static auto run() -> void { // NOSONAR(S3776) -- inherent constexpr dispatch
        auto qs = base_query<Tc, Model, ConnType>();
        if constexpr (Tc.query_type == "first") {
            auto r = qs.first().execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            EXPECT_EQ(r.value().has_value() ? 1 : 0, Tc.expected.count);
            if constexpr (Tc.expected.count == 1) {
                ASSERT_TRUE(r.value().has_value());
                verify_first_row<Tc>(r.value().value());
            }
        } else if constexpr (Tc.query_type == "one") {
            auto r = qs.get().execute();
            if constexpr (Tc.expected.count == 1) {
                ASSERT_TRUE(r.has_value()) << r.error().message();
                verify_first_row<Tc>(r.value());
            } else {
                EXPECT_FALSE(r.has_value());
            }
        } else {
            auto result = qs.select().execute();
            ASSERT_TRUE(result.has_value()) << result.error().message();
            EXPECT_EQ(static_cast<int>(result.value().size()), Tc.expected.count);
            if constexpr (Tc.expected.count > 0) {
                ASSERT_FALSE(result.value().empty());
                verify_first_row<Tc>(*result.value().begin());
            }
        }
    }
};

// ---------------------------------------------------------------------------
// AggregateRunner -- scalar aggregates (count, sum, avg, min, max, count_distinct)
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class AggregateRunner {
  private:
    // #818: pin the statement's SQL text when the case declares expected_sql;
    // a case without it is result-only, as before.
    template <const auto &Tc> static auto expect_sql(auto &query) -> void {
        if constexpr (!Tc.expected_sql.empty())
            EXPECT_EQ(query.sql(), Tc.expected_sql.view())
                << "YAML expected_sql (#818) of case " << Tc.bench.test_name.view();
    }

    // query_type names the QuerySet method, except count_field, which is COUNT(field).
    static consteval auto method_name(std::string_view query_type) -> std::string_view {
        return query_type == "count_field" ? "count" : query_type;
    }

  public:
    template <const auto &Tc> static auto run() -> void {
        static_assert(Tc.query_type != "count" || Tc.bench.aggregate.field.empty(),
                      "query_type count is COUNT(*); use count_field for COUNT(field)");
        auto qs = base_query<Tc, Model, ConnType>();
        constexpr auto fn = member_function_named<decltype(qs)>(method_name(Tc.query_type.view()));
        auto q = call_query_method<fn, field_or_null<Model>(Tc.bench.aggregate.field.view())>(qs);
        expect_sql<Tc>(q);
        auto r = q.execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        using Value = std::remove_cvref_t<decltype(r.value())>;
        constexpr bool exact_int = !std::is_same_v<Value, std::optional<double>> && Tc.expected.int_val != -1;
        constexpr double expected = exact_int ? static_cast<double>(Tc.expected.int_val) : Tc.expected.dbl_val;
        expect_agg_value(r.value(), expected, Tc.dataset.view() == "empty", Tc.query_type.view());
    }
};

// ---------------------------------------------------------------------------
// ChainAggRunner -- multi-aggregate chain queries (query_type == "chain")
//
// The chain is built by recursion over Tc.aggregations rather than one branch
// per func ordering (#834): every step changes the statement's type, so step I
// is applied to the result of step I-1. Only the orderings the YAML uses are
// instantiated.
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class ChainAggRunner {
    // The YAML func name IS the member name (sum/avg/min/max/count), on both
    // QuerySet and the aggregate chain (#834).
    template <const auto &Tc, std::size_t I> static auto apply_agg(auto &&q) {
        constexpr auto &agg = Tc.aggregations[I];
        constexpr auto fn = member_function_named<std::remove_cvref_t<decltype(q)>>(agg.func.view());
        return call_query_method<fn, field_or_null<Model>(agg.field.view())>(std::forward<decltype(q)>(q));
    }

    template <const auto &Tc, std::size_t I = 0> static auto build_chain(auto &&q) {
        auto next = apply_agg<Tc, I>(std::forward<decltype(q)>(q));
        if constexpr (I + 1 == Tc.chain_len)
            return next;
        else
            return build_chain<Tc, I + 1>(std::move(next));
    }

    // I must stay a constant: it indexes Tc.aggregations[I] and the tuple.
    template <const auto &Tc> static auto check_values(const auto &values) -> void {
        template for (constexpr std::size_t I : std::views::iota(0uz, Tc.chain_len))
            expect_agg_value(std::get<I>(values), Tc.aggregations[I].res_value, Tc.dataset.view() == "empty",
                             Tc.aggregations[I].func.view());
    }

  public:
    template <const auto &Tc> static auto run() -> void {
        static_assert(Tc.chain_len >= 2, "a chain case needs at least two aggregations");
        static_assert(Tc.chain_len <= Tc.MAX_CHAIN, "chain case has more aggregations than MAX_CHAIN");
        auto qs = base_query<Tc, Model, ConnType>();
        auto r = build_chain<Tc>(qs).execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        check_values<Tc>(r.value());
    }
};

// ---------------------------------------------------------------------------
// DistinctRunner -- DISTINCT projection (returns plf::hive<FieldType>)
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class DistinctRunner {
  public:
    template <const auto &Tc> static auto run() -> void {
        static_assert(Tc.bench.distinct.field_count() >= 1, "a distinct case needs at least one field");
        auto qs = base_query<Tc, Model, ConnType>();
        // One path for any number of fields: the shared builder expands the field pack.
        auto r = BuilderFor<Tc, Model, ConnType>::build_distinct_stmt(qs).execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        EXPECT_EQ(static_cast<int>(r.value().size()), Tc.expected.count);
    }
};

// ---------------------------------------------------------------------------
// GroupByRunner -- GROUP BY + aggregate (group_count, group_sum, ...)
//
// group_<func> calls func on the grouped query by name (#834), after the
// GROUP BY fields and the optional HAVING.
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class GroupByRunner {
    static constexpr std::string_view GROUP_PREFIX = "group_";

    template <const auto &Tc, std::size_t... Is> static auto grouped(auto &qs, std::index_sequence<Is...>) {
        return qs.template group_by<FieldRef<dispatch_field<Model>(Tc.bench.group_by.fields[Is].view())>{}...>();
    }

    template <const auto &Tc> static auto with_having(auto &&g) {
        constexpr auto &having = Tc.bench.group_by.having;
        if constexpr (having.enabled) {
            constexpr auto fi = dispatch_field<Model>(having.field.view());
            return std::forward<decltype(g)>(g).having(build_compare_expr<fi, having.op>(having.value.as_int));
        } else {
            return std::forward<decltype(g)>(g);
        }
    }

  public:
    template <const auto &Tc> static auto run() -> void {
        constexpr std::string_view query_type = Tc.query_type.view();
        static_assert(query_type.starts_with(GROUP_PREFIX), "GroupByRunner runs group_<func> cases only");
        auto qs = base_query<Tc, Model, ConnType>();
        auto g = with_having<Tc>(grouped<Tc>(qs, std::make_index_sequence<Tc.bench.group_by.field_count()>{}));
        constexpr auto fn = member_function_named<decltype(g)>(query_type.substr(GROUP_PREFIX.size()));
        auto r = call_query_method<fn, field_or_null<Model>(Tc.bench.aggregate.field.view())>(std::move(g)).execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        EXPECT_EQ(static_cast<int>(r.value().size()), Tc.expected.count);
        if constexpr (Tc.expected.groups_count > 0)
            verify_groups<Tc>(r.value());
    }
};

} // namespace storm::test
