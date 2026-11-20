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
 * - ExpectedValue — one actual value against one expected cell (expected.first / expected.rows)
 * - SelectRunner (select / first / one / rows), ValuesRunner (#867), AggregateRunner (scalar, chain
 *   and group, #841), DistinctRunner:
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

// #818/#844: a case sets expected_sql, or BOTH expected_sql_sqlite and expected_sql_pg, never both
// forms; one dialect alone would leave the other backend with no SQL check.
template <const auto &Tc>
concept WellFormedExpectedSql = Tc.expected_sql_well_formed();

// Pin the statement's SQL text when the case declares it (the shared text, or this backend's
// own); a case without any is result-only.
template <const auto &Tc, typename ConnType> auto expect_sql(auto &query) -> void {
    static_assert(WellFormedExpectedSql<Tc>,
                  "a case sets expected_sql, or BOTH expected_sql_sqlite and expected_sql_pg, never both forms");
    constexpr std::string_view text = Tc.expected_sql_for(storm::test::is_postgresql<ConnType>());
    if constexpr (!text.empty())
        EXPECT_EQ(query.sql(), text) << "YAML expected_sql (#818, #844) of case " << Tc.bench.test_name.view();
}

// A value written in `expected.first` can be read as the row member's type (#844): null only for
// an optional member, an integer wider than 32 bits only for a wide enough member, a string only
// for a text, UUID, date, time, enum (by name) or blob (hex) member.
template <typename Member, auto Expected>
concept ExpectedValueFits = storm::orm::query_builder::ValueFits<Member, Expected>;

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
// ExpectedValue -- an actual value against an expected cell (`expected.first`, `expected.rows`)
// ---------------------------------------------------------------------------
struct ExpectedValue {
    // Compare one row member against its expected value, dispatching on the member's C++ type
    // (an optional reads as its inner type, null meaning "no value"). A YAML value that does not
    // fit the member fails to compile (ExpectedValueFits).
    // Expected is a parser TypedValue; deduced (not named) so this header does not
    // need the benchmark schema module that only the YAML TUs import.
    template <auto Expected> static consteval auto expected_as_double() -> double {
        using Kind = typename decltype(Expected)::Kind;
        if constexpr (Expected.kind == Kind::Double)
            return Expected.as_double;
        else if constexpr (Expected.kind == Kind::Int64)
            return static_cast<double>(Expected.as_int64);
        else
            return static_cast<double>(Expected.as_int);
    }

    template <auto Expected, typename T> static auto expect_value_eq(const T &actual) -> void {
        using Kind = typename decltype(Expected)::Kind;
        constexpr std::string_view text = Expected.as_string.view();
        if constexpr (std::is_same_v<T, std::string>) {
            EXPECT_EQ(actual, text);
        } else if constexpr (std::is_same_v<T, bool>) {
            EXPECT_EQ(actual, Expected.kind == Kind::Int ? Expected.as_int != 0 : Expected.as_bool);
        } else if constexpr (std::is_floating_point_v<T>) {
            constexpr double expected = expected_as_double<Expected>();
            EXPECT_NEAR(actual, expected, 0.01);
        } else if constexpr (std::is_enum_v<T>) {
            constexpr auto enumerator = storm::orm::query_builder::find_enumerator<T>(text);
            EXPECT_TRUE(actual == [:enumerator:]) << "expected enumerator " << text;
        } else if constexpr (std::is_same_v<T, storm::orm::utilities::UUID>) {
            EXPECT_EQ(actual.value, text);
        } else if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
            constexpr auto expected = *storm::orm::query_builder::iso_date_of(text);
            EXPECT_TRUE(actual == expected) << "expected date " << text;
        } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
            constexpr auto expected = *storm::orm::query_builder::iso_datetime_of(text);
            EXPECT_TRUE(actual == expected) << "expected date-time " << text;
        } else if constexpr (storm::orm::query_builder::BlobType<T>) {
            EXPECT_TRUE(actual == storm::orm::query_builder::blob_from_hex<T>(text)) << "expected blob " << text;
        } else if constexpr (storm::orm::query_builder::DurationType<T>) {
            expect_value_eq<Expected>(actual.count()); // a duration is its rep (#853)
        } else {
            static_assert(std::is_integral_v<T>, "expected value: unsupported member type");
            if constexpr (Expected.kind == Kind::Int64)
                EXPECT_EQ(static_cast<long long>(actual), Expected.as_int64);
            else
                EXPECT_EQ(actual, Expected.as_int);
        }
    }

    template <auto Expected, typename Member> static auto expect_member_eq(const Member &actual) -> void {
        static_assert(ExpectedValueFits<Member, Expected>, "expected value: the value does not fit the member's type");
        using Kind = typename decltype(Expected)::Kind;
        if constexpr (storm::orm::utilities::is_optional_v<Member>) {
            if constexpr (Expected.kind == Kind::Null) {
                EXPECT_FALSE(actual.has_value());
            } else {
                ASSERT_TRUE(actual.has_value());
                expect_value_eq<Expected>(*actual);
            }
        } else {
            expect_value_eq<Expected>(actual);
        }
    }
};

// ---------------------------------------------------------------------------
// SelectRunner -- plain SELECT queries (select / first / one / rows)
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class SelectRunner {
    template <const auto &Tc, std::size_t I, typename Row> static auto verify_first_field(const Row &row) -> void {
        constexpr auto &field = Tc.expected.first.fields[I];
        static_assert(storm::orm::query_builder::has_field<Row>(field.name.view()),
                      "expected.first names a member the row type does not have");
        constexpr auto member = dispatch_field<Row>(field.name.view());
        ExpectedValue::expect_member_eq<field.value>(row.[:member:]);
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
        // SQL is pinned on `select`; first / one would ignore it and pass without comparing (#844).
        static_assert(Tc.query_type == "select" ||
                          (Tc.expected_sql.empty() && Tc.expected_sql_sqlite.empty() && Tc.expected_sql_pg.empty()),
                      "expected_sql is supported on query_type select only");
        if constexpr (Tc.query_type == "first") {
            auto r = qs.first().execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            EXPECT_EQ(r.value().has_value() ? 1 : 0, Tc.expected.count);
            if constexpr (Tc.expected.count == 1) {
                ASSERT_TRUE(r.value().has_value());
                verify_first_row<Tc>(r.value().value());
            }
        } else if constexpr (Tc.query_type == "rows") {
            int count = 0;
            for (auto &&row : qs.rows()) {
                ASSERT_TRUE(row.has_value()) << row.error().message();
                if (count == 0)
                    verify_first_row<Tc>(row.value());
                ++count;
            }
            EXPECT_EQ(count, Tc.expected.count);
        } else if constexpr (Tc.query_type == "one") {
            auto r = qs.get().execute();
            if constexpr (Tc.expected.count == 1) {
                ASSERT_TRUE(r.has_value()) << r.error().message();
                verify_first_row<Tc>(r.value());
            } else {
                EXPECT_FALSE(r.has_value());
            }
        } else {
            auto query = qs.select();
            expect_sql<Tc, ConnType>(query);
            auto result = query.execute();
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
// ValuesRunner -- column projection values<f1, ...>() (#867): one column yields
// plf::hive<T>, several yield plf::hive<std::tuple<...>>. `expected.rows` is
// checked cell by cell, in result order, so more than one row needs an ORDER BY.
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class ValuesRunner {
    template <const auto &Tc> static consteval auto rows_match_fields() -> bool {
        constexpr auto &rows = Tc.expected.rows;
        for (std::size_t r = 0; r < rows.row_count && r < rows.MAX_ROWS; ++r)
            if (rows.columns[r] != Tc.bench.values.field_count())
                return false;
        return true;
    }

    // One field projects as the bare value, several as a tuple.
    template <std::size_t C, typename Row> static auto cell_of(const Row &row) -> const auto & {
        if constexpr (requires { std::tuple_size<Row>::value; })
            return std::get<C>(row);
        else
            return row;
    }

    template <const auto &Tc, std::size_t R, typename Row> static auto verify_row(const Row &row) -> void {
        template for (constexpr std::size_t C : std::views::iota(0uz, Tc.bench.values.field_count()))
            ExpectedValue::expect_member_eq<Tc.expected.rows.cells[R][C]>(cell_of<C>(row));
    }

  public:
    template <const auto &Tc> static auto run() -> void {
        constexpr auto &rows = Tc.expected.rows;
        static_assert(Tc.bench.values.field_count() >= 1, "a values case needs at least one field");
        static_assert(rows.row_count <= rows.MAX_ROWS, "expected.rows has more rows than RowsSpec::MAX_ROWS");
        static_assert(rows_match_fields<Tc>(),
                      "every expected.rows row needs one cell per values field, and at most MAX_COLUMNS");
        static_assert(rows.row_count == 0 || static_cast<int>(rows.row_count) == Tc.expected.count,
                      "expected.rows lists every result row, so it must agree with expected.count");
        static_assert(rows.row_count <= 1 || Tc.bench.order_by_count > 0,
                      "expected.rows is compared in result order: more than one row needs an order_by");
        auto qs = base_query<Tc, Model, ConnType>();
        auto query = BuilderFor<Tc, Model, ConnType>::build_values_stmt(qs);
        expect_sql<Tc, ConnType>(query);
        auto result = query.execute();
        ASSERT_TRUE(result.has_value()) << result.error().message();
        ASSERT_EQ(static_cast<int>(result.value().size()), Tc.expected.count);
        auto it = result.value().begin();
        template for (constexpr std::size_t R : std::views::iota(0uz, rows.row_count)) {
            verify_row<Tc, R>(*it);
            ++it;
        }
    }
};

// ---------------------------------------------------------------------------
// AggregateRunner -- scalar aggregates, aggregate chains and GROUP BY (#841)
//
// One pipeline: base_query -> [group_by + having] -> aggregates 1..N -> check.
// The parser rewrites every scalar and group_<func> case into `aggregations`
// (normalise_aggregate_case), so a scalar aggregate is a chain of one and a
// group case a chain of one after group_by.
//
// The chain is built by recursion over Tc.aggregations rather than one branch
// per func ordering (#834): every step changes the statement's type, so step I
// is applied to the result of step I-1. Only the orderings the YAML uses are
// instantiated. The YAML func name IS the QuerySet / aggregate-chain member name.
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class AggregateRunner {
    template <const auto &Tc> static constexpr bool is_group = Tc.bench.group_by.field_count() > 0;

    template <const auto &Tc, std::size_t... Is> static auto grouped(auto &qs, std::index_sequence<Is...>) {
        return qs.template group_by<FieldRef<dispatch_field<Model>(Tc.bench.group_by.fields[Is].view())>{}...>();
    }

    template <const auto &Tc> static auto with_having(auto &&g) {
        constexpr auto &having = Tc.bench.group_by.having;
        if constexpr (having.enabled) {
            constexpr auto fi = dispatch_field<Model>(having.field.view());
            static_assert(
                having.value.kind == std::remove_cvref_t<decltype(having.value)>::Kind::Int,
                "a HAVING value is a 32-bit int; null, int64, double and strings are not supported here (#844)");
            return std::forward<decltype(g)>(g).having(build_compare_expr<fi, having.op>(having.value.as_int));
        } else {
            return std::forward<decltype(g)>(g);
        }
    }

    // The statement the aggregates are applied to: the base query, or it grouped (+ HAVING).
    template <const auto &Tc> static auto aggregate_source() {
        auto qs = base_query<Tc, Model, ConnType>();
        if constexpr (is_group<Tc>)
            return with_having<Tc>(grouped<Tc>(qs, std::make_index_sequence<Tc.bench.group_by.field_count()>{}));
        else
            return qs;
    }

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
    template <const auto &Tc> static auto check_chain(const auto &values) -> void {
        template for (constexpr std::size_t I : std::views::iota(0uz, Tc.chain_len))
            expect_agg_value(std::get<I>(values), Tc.aggregations[I].res_value, Tc.dataset.view() == "empty",
                             Tc.aggregations[I].func.view());
    }

    template <const auto &Tc> static auto check(const auto &value) -> void {
        if constexpr (is_group<Tc>) {
            EXPECT_EQ(static_cast<int>(value.size()), Tc.expected.count);
            if constexpr (Tc.expected.groups_count > 0)
                verify_groups<Tc>(value);
        } else if constexpr (Tc.chain_len == 1) {
            expect_agg_value(value, Tc.aggregations[0].res_value, Tc.dataset.view() == "empty",
                             Tc.aggregations[0].func.view());
        } else {
            check_chain<Tc>(value);
        }
    }

  public:
    template <const auto &Tc> static auto run() -> void {
        static_assert(Tc.chain_len >= 1, "an aggregate case needs at least one aggregation");
        static_assert(Tc.query_type != "chain" || Tc.chain_len >= 2, "a chain case needs at least two aggregations");
        static_assert(is_group<Tc> == Tc.query_type.view().starts_with("group_"),
                      "group_by is valid on, and only on, group_<func> cases");
        static_assert(Tc.chain_len <= Tc.MAX_CHAIN, "aggregate case has more aggregations than MAX_CHAIN");
        static_assert(Tc.query_type != "count" || Tc.bench.aggregate.field.empty(),
                      "query_type count is COUNT(*); use count_field for COUNT(field)");
        auto q = build_chain<Tc>(aggregate_source<Tc>());
        expect_sql<Tc, ConnType>(q);
        auto r = q.execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        check<Tc>(r.value());
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

} // namespace storm::test
