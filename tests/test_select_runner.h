#pragma once

/**
 * @file test_select_runner.h
 * @brief Field dispatch, WHERE builder, QueryRunnerBase, and SELECT runners.
 *
 * Provides:
 * - dispatch_field<Model>(name) — via storm::orm::query_builder
 * - build_compare_expr<FieldInfo, op>(value) — YAML operator -> ORM expression (shared)
 * - QueryRunnerBase<Model, ConnType> — shared base with seed + WHERE/JOIN apply
 * - SelectRunner, AggregateRunner, ChainAggRunner, DistinctRunner, GroupByRunner
 *
 * Include AFTER `import storm;` — it pulls the model headers it needs itself.
 */

#include <algorithm>
#include <meta>
#include <string_view>
#include <utility>
#include <vector>

#include "shared/query_builder.hpp" // NOLINT(misc-header-include-cycle)

#include "../shared/models/message.h"
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

// MIN/MAX/AVG results are std::optional<double> (#416). Over a non-empty set the
// optional holds the value; over an empty (or all-NULL) set it is nullopt.
inline auto expect_opt_near(const std::optional<double> &actual, double expected) -> void {
    ASSERT_TRUE(actual.has_value()) << "aggregate over non-empty set must hold a value";
    EXPECT_NEAR(actual.value(), expected, 0.01);
}

// Variant for cases that may target an empty set: when expect_null, the
// aggregate must be nullopt; otherwise it must equal `expected`.
inline auto expect_opt_near(const std::optional<double> &actual, double expected, bool expect_null) -> void {
    if (expect_null) {
        EXPECT_FALSE(actual.has_value()) << "MIN/MAX/AVG over empty set must be nullopt (#416)";
    } else {
        expect_opt_near(actual, expected);
    }
}

// FK resolver for test models (Message::sender → Person).
struct TestFkResolver {
    consteval auto operator()(std::string_view name) const -> std::meta::info {
        if (name == "sender")
            return ^^Message::sender;
        std::unreachable();
    }
};
inline constexpr TestFkResolver test_fk{};

// Stateless no-op FK resolver for tests (JOIN is never enabled in test cases).
struct NoOpFkResolver {
    consteval auto operator()(std::string_view) const -> std::meta::info { std::unreachable(); }
};
inline constexpr NoOpFkResolver no_op_fk{};

// Recursively build a WHERE expression from a compile-time WhereNode tree.
template <int N, const auto &Tc, typename Model> auto build_where_node_expr() {
    if constexpr (Tc.where_nodes[N].left < 0) {
        constexpr auto fi = dispatch_field<Model>(Tc.where_nodes[N].field.view());
        if constexpr (Tc.where_nodes[N].value_dbl != 0.0)
            return build_compare_expr<fi, Tc.where_nodes[N].op>(Tc.where_nodes[N].value_dbl);
        else
            return build_compare_expr<fi, Tc.where_nodes[N].op>(Tc.where_nodes[N].value_int);
    } else if constexpr (Tc.where_nodes[N].op == "AND") {
        return build_where_node_expr<Tc.where_nodes[N].left, Tc, Model>() &&
               build_where_node_expr<Tc.where_nodes[N].right, Tc, Model>();
    } else {
        return build_where_node_expr<Tc.where_nodes[N].left, Tc, Model>() ||
               build_where_node_expr<Tc.where_nodes[N].right, Tc, Model>();
    }
}

template <typename Model, typename ConnType> class QueryRunnerBase {
  public:
    QuerySet<Model, ConnType, true> qs_;

    template <const auto &Tc> auto seed_and_insert() -> void {
        std::vector<Model> initial;
        initial.reserve(static_cast<std::size_t>(Tc.bench.dataset_size));
        for (int i = 0; i < Tc.bench.dataset_size; ++i)
            initial.push_back(make_record<Model>(i));
        auto ins = this->qs_.insert(std::span<const Model>(initial)).execute();
        ASSERT_TRUE(ins.has_value()) << ins.error().message();
    }

    template <const auto &Tc> auto apply_where_and_join() -> void { // NOSONAR(S3776)
        if constexpr (Tc.bench.join.enabled) {
            using QB = storm::orm::query_builder::QueryBuilder<Model, Tc.bench, test_fk, ConnType>;
            qs_ = QB::build_qs();
        } else {
            using QB = storm::orm::query_builder::QueryBuilder<Model, Tc.bench, no_op_fk, ConnType>;
            qs_ = QB::build_qs();
        }
        if constexpr (Tc.where_node_count > 0)
            qs_ = qs_.where(build_where_node_expr<0, Tc, Model>());
    }

    template <const auto &Tc> auto qs_with_modifiers() -> decltype(auto) { return (qs_); }
};

template <typename Model, typename ConnType> class SelectQueryRunnerBase : public QueryRunnerBase<Model, ConnType> {};

// ---------------------------------------------------------------------------
// Shared group-by result verification helpers
// ---------------------------------------------------------------------------

template <const auto &Tc>
auto check_group_keys_and_int_agg(const std::vector<std::pair<int, std::int64_t>> &groups) -> void {
    ASSERT_EQ(groups.size(), Tc.expected.groups_count);
    for (std::size_t g = 0; g < Tc.expected.groups_count; ++g) {
        EXPECT_EQ(groups[g].first, Tc.expected.group_keys[g]) << "Group key mismatch at index " << g;
        EXPECT_EQ(groups[g].second, Tc.expected.group_agg_int[g]) << "Group agg mismatch at index " << g;
    }
}

// MIN/MAX/AVG group aggregates are std::optional<double> (#416). A group whose
// aggregate column is all-NULL reads back as nullopt (flagged group_agg_null).
template <const auto &Tc>
auto check_group_keys_and_dbl_agg(const std::vector<std::pair<int, std::optional<double>>> &groups) -> void {
    ASSERT_EQ(groups.size(), Tc.expected.groups_count);
    for (std::size_t g = 0; g < Tc.expected.groups_count; ++g) {
        EXPECT_EQ(groups[g].first, Tc.expected.group_keys[g]) << "Group key mismatch at index " << g;
        if (Tc.expected.group_agg_null[g]) {
            EXPECT_FALSE(groups[g].second.has_value()) << "Group agg should be nullopt at index " << g;
        } else {
            ASSERT_TRUE(groups[g].second.has_value()) << "Group agg should hold a value at index " << g;
            EXPECT_NEAR(groups[g].second.value(), Tc.expected.group_agg_dbl[g], 0.01)
                << "Group agg mismatch at index " << g;
        }
    }
}

template <const auto &Tc> auto verify_group_int_results(const auto &result) -> void {
    std::vector<std::pair<int, std::int64_t>> groups;
    for (const auto &[key, agg] : result)
        groups.emplace_back(static_cast<int>(key), agg);
    std::ranges::sort(groups);
    check_group_keys_and_int_agg<Tc>(groups);
}

template <const auto &Tc> auto verify_group_dbl_results(const auto &result) -> void {
    std::vector<std::pair<int, std::optional<double>>> groups;
    for (const auto &[key, agg] : result)
        groups.emplace_back(static_cast<int>(key), agg);
    std::ranges::sort(groups, [](const auto &a, const auto &b) { return a.first < b.first; });
    check_group_keys_and_dbl_agg<Tc>(groups);
}

// ---------------------------------------------------------------------------
// SelectRunner -- plain SELECT queries (select / first / one)
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class SelectRunner : public SelectQueryRunnerBase<Model, ConnType> {
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

    template <const auto &Tc> auto run() -> void { // NOSONAR(S3776) -- inherent constexpr dispatch
        this->template apply_where_and_join<Tc>();
        if constexpr (Tc.query_type == "first") {
            auto r = this->qs_.first().execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            EXPECT_EQ(r.value().has_value() ? 1 : 0, Tc.expected.count);
            if constexpr (Tc.expected.count == 1) {
                ASSERT_TRUE(r.value().has_value());
                verify_first_row<Tc>(r.value().value());
            }
        } else if constexpr (Tc.query_type == "one") {
            auto r = this->qs_.get().execute();
            if constexpr (Tc.expected.count == 1) {
                ASSERT_TRUE(r.has_value()) << r.error().message();
                verify_first_row<Tc>(r.value());
            } else {
                EXPECT_FALSE(r.has_value());
            }
        } else {
            auto result = this->qs_.select().execute();
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
template <typename Model, typename ConnType> class AggregateRunner : public SelectQueryRunnerBase<Model, ConnType> {
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
    template <const auto &Tc> auto run() -> void {
        static_assert(Tc.query_type != "count" || Tc.bench.aggregate.field.empty(),
                      "query_type count is COUNT(*); use count_field for COUNT(field)");
        this->template apply_where_and_join<Tc>();
        constexpr auto fn = member_function_named<decltype(this->qs_)>(method_name(Tc.query_type.view()));
        auto q = call_query_method<fn, field_or_null<Model>(Tc.bench.aggregate.field.view())>(this->qs_);
        expect_sql<Tc>(q);
        auto r = q.execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        // MIN/MAX/AVG read back as std::optional<double> (#416); COUNT/SUM as numbers.
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(r.value())>, std::optional<double>>)
            expect_opt_near(r.value(), Tc.expected.dbl_val, Tc.dataset.view() == "empty");
        else if constexpr (Tc.expected.int_val != -1)
            EXPECT_EQ(r.value(), Tc.expected.int_val);
        else
            EXPECT_NEAR(r.value(), Tc.expected.dbl_val, 0.01);
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
template <typename Model, typename ConnType> class ChainAggRunner : public SelectQueryRunnerBase<Model, ConnType> {
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

    // COUNT/SUM read back as integers; MIN/MAX/AVG as std::optional<double> (#416),
    // which is nullopt over an empty set.
    template <const auto &Tc, std::size_t I> static auto check_value(const auto &actual) -> void {
        constexpr double expected = Tc.aggregations[I].res_value;
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(actual)>, std::optional<double>>)
            expect_opt_near(actual, expected, Tc.dataset.view() == "empty");
        else
            EXPECT_EQ(actual, static_cast<std::int64_t>(expected)) << "chain aggregate " << I;
    }

    // I must stay a constant: it indexes Tc.aggregations[I] and the tuple.
    template <const auto &Tc> static auto check_values(const auto &values) -> void {
        template for (constexpr std::size_t I : std::views::iota(0uz, Tc.chain_len))
            check_value<Tc, I>(std::get<I>(values));
    }

  public:
    template <const auto &Tc> auto run() -> void {
        static_assert(Tc.chain_len >= 2, "a chain case needs at least two aggregations");
        static_assert(Tc.chain_len <= Tc.aggregations.size(), "chain case has more aggregations than MAX_CHAIN");
        this->template apply_where_and_join<Tc>();
        auto r = build_chain<Tc>(this->qs_).execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        check_values<Tc>(r.value());
    }
};

// ---------------------------------------------------------------------------
// DistinctRunner -- DISTINCT projection (returns plf::hive<FieldType>)
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class DistinctRunner : public SelectQueryRunnerBase<Model, ConnType> {
  public:
    template <const auto &Tc> auto run() -> void {
        static_assert(Tc.bench.distinct.field_count() >= 1, "a distinct case needs at least one field");
        this->template apply_where_and_join<Tc>();
        // One path for any number of fields: the shared builder expands the field pack.
        using QB = storm::orm::query_builder::QueryBuilder<Model, Tc.bench, no_op_fk, ConnType>;
        auto r = QB::build_distinct_stmt(this->qs_).execute();
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
template <typename Model, typename ConnType> class GroupByRunner : public SelectQueryRunnerBase<Model, ConnType> {
    static constexpr std::string_view GROUP_PREFIX = "group_";

    template <const auto &Tc, std::size_t... Is> auto grouped(std::index_sequence<Is...>) {
        return this->qs_.template group_by<FieldRef<dispatch_field<Model>(Tc.bench.group_by.fields[Is].view())>{}...>();
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

    // Per-group check: MIN/MAX/AVG groups read back as std::optional<double> (#416).
    template <const auto &Tc> static auto verify_groups(const auto &rows) -> void {
        static_assert(Tc.bench.group_by.field_count() == 1, "per-group expectations need a single GROUP BY field");
        using Row = std::ranges::range_value_t<std::remove_cvref_t<decltype(rows)>>;
        if constexpr (std::is_same_v<std::tuple_element_t<1, Row>, std::optional<double>>)
            verify_group_dbl_results<Tc>(rows);
        else
            verify_group_int_results<Tc>(rows);
    }

  public:
    template <const auto &Tc> auto run() -> void {
        constexpr std::string_view query_type = Tc.query_type.view();
        static_assert(query_type.starts_with(GROUP_PREFIX), "GroupByRunner runs group_<func> cases only");
        this->template apply_where_and_join<Tc>();
        auto g = with_having<Tc>(grouped<Tc>(std::make_index_sequence<Tc.bench.group_by.field_count()>{}));
        constexpr auto fn = member_function_named<decltype(g)>(query_type.substr(GROUP_PREFIX.size()));
        auto r = call_query_method<fn, field_or_null<Model>(Tc.bench.aggregate.field.view())>(std::move(g)).execute();
        ASSERT_TRUE(r.has_value()) << r.error().message();
        EXPECT_EQ(static_cast<int>(r.value().size()), Tc.expected.count);
        if constexpr (Tc.expected.groups_count > 0)
            verify_groups<Tc>(r.value());
    }
};

} // namespace storm::test
