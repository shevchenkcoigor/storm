#pragma once

/**
 * @file test_parser.hpp
 * @brief The test-only keys of the YAML-driven unit test corpus (#845).
 *
 * There is ONE spec parser: storm_benchmark_parser (benchmarks/parser.cppm)
 * reads every key a benchmark case can carry. This header adds only what a test
 * case carries beyond that -- `query_type`, `dataset`, `expected`,
 * `expected_sql*`, `aggregations`, `where_expr`, the write counts -- and hands
 * every other key to storm::benchmark::parse_bench_field, so no shared key is
 * parsed in two places.
 *
 * It holds no corpus, so a TU that only needs the parser (the spec-limits TU)
 * includes it alone. The slice TUs select their corpus with
 * STORM_UNIFIED_CASES_FILE, which test_unified_yaml_body.h #embeds (#561/#582).
 */

#include <array>
#include <string_view>

import storm_benchmark_schema;
import storm_benchmark_parser;

namespace storm::test {

    using storm::benchmark::BenchmarkTest;
    using storm::benchmark::ConstexprString;
    using storm::benchmark::JsonKeyRef;
    using storm::benchmark::TypedValue;
    using storm::benchmark::WhereNode;
    using storm::benchmark::WhereSpec;

    using storm::benchmark::jkey_eq;
    using storm::benchmark::parse_array;
    using storm::benchmark::parse_bench_field;
    using storm::benchmark::parse_bool;
    using storm::benchmark::parse_double;
    using storm::benchmark::parse_int;
    using storm::benchmark::parse_object;
    using storm::benchmark::parse_str;
    using storm::benchmark::parse_typed_value;
    using storm::benchmark::skip_braced;
    using storm::benchmark::skip_or_step;
    using storm::benchmark::skip_value;
    using storm::benchmark::skip_ws;

    // --- Test-only sub-specs and structs ---

    // One `expected.first` entry: a row member's name and its expected value (#834).
    struct FirstRowField {
        ConstexprString<32> name;
        TypedValue          value;
    };

    // `expected.first` as a name -> value list, checked member by member against
    // the row via reflection (test_select_runner.h), so a new model field needs no
    // parser change. `count` counts every key seen, even past MAX_FIELDS, so the
    // runner can reject an overflowing case instead of dropping its extra keys.
    struct FirstRowSpec {
        static constexpr std::size_t          MAX_FIELDS = 8;
        std::array<FirstRowField, MAX_FIELDS> fields{};
        std::size_t                           count = 0;
        constexpr FirstRowSpec()                    = default;
    };

    // `expected.rows` (#867): the projected rows of a `values` case, one list of typed cells per row.
    // `row_count` and each `columns` entry count past their caps, so the runner rejects an
    // overflowing case at compile time instead of checking a truncated one.
    struct RowsSpec {
        static constexpr std::size_t                              MAX_ROWS    = 8;
        static constexpr std::size_t                              MAX_COLUMNS = 4;
        std::array<std::array<TypedValue, MAX_COLUMNS>, MAX_ROWS> cells{};
        std::array<std::size_t, MAX_ROWS>                         columns{};
        std::size_t                                               row_count = 0;
        constexpr RowsSpec()                                                = default;
    };

    struct ExpectedSpec {
        int                            count = 0, int_val = -1, unchanged = -1, remaining = -1;
        double                         dbl_val = -1.0;
        FirstRowSpec                   first;
        RowsSpec                       rows;
        static constexpr std::size_t   MAX_GROUPS = 20;
        std::array<int, MAX_GROUPS>    group_keys{};
        std::array<int, MAX_GROUPS>    group_agg_int{};
        std::array<double, MAX_GROUPS> group_agg_dbl{};
        // #416: per-group flag — true when MIN/MAX/AVG over an all-NULL column in
        // that group is SQL NULL (-> std::nullopt). Defaults to all-false.
        std::array<bool, MAX_GROUPS> group_agg_null{};
        std::size_t                  groups_count = 0;
        constexpr ExpectedSpec()                  = default;
    };

    struct ChainAggSpec {
        ConstexprString<16> func;
        ConstexprString<32> field;
        ConstexprString<8>  res_type;
        double              res_value = 0.0;
        constexpr ChainAggSpec()      = default;
    };

    // Longest pinned statement today is 105 chars (a JOIN + WHERE aggregate).
    inline constexpr std::size_t EXPECTED_SQL_CAPACITY = 160;

    struct TestCase { // NOSONAR(cpp:S1820)
        BenchmarkTest       bench;
        ConstexprString<32> query_type;
        ConstexprString<16> dataset;
        // #818: optional. When set, a runner that supports it pins the statement's
        // sql() text against this; empty = result-only check, as before.
        ConstexprString<EXPECTED_SQL_CAPACITY> expected_sql;
        // #844: the text differs per backend once ORDER BY / LIMIT cases pin it (NULLS FIRST/LAST,
        // LIMIT ALL vs LIMIT -1). A case sets expected_sql, or BOTH of these; never both forms.
        ConstexprString<EXPECTED_SQL_CAPACITY> expected_sql_sqlite;
        ConstexprString<EXPECTED_SQL_CAPACITY> expected_sql_pg;
        ExpectedSpec                           expected;
        // `backends: [sqlite]`: the case runs on SQLite only. Required by a feature PostgreSQL lacks (a WHERE
        // collate); the case is not registered for PG (absent from that suite, not a pass). Any other list is rejected.
        bool                                sqlite_only     = false;
        bool                                unknown_backend = false;
        int                                 insert_count = 0, update_count = 0, erase_count = 0;
        std::size_t                         chain_len = 0;
        static constexpr std::size_t        MAX_CHAIN = 5;
        std::array<ChainAggSpec, MAX_CHAIN> aggregations{};
        constexpr TestCase() = default;

        // #843: the spec's caps plus the chain's. A case past one does not compile (QueryBuilder
        // for the spec's, the aggregate runner for MAX_CHAIN).
        [[nodiscard]] constexpr auto within_limits() const -> bool {
            return bench.within_limits() && chain_len <= MAX_CHAIN;
        }

        // The SQL text to pin for a backend: the shared text, or that dialect's own. Empty means
        // the case does not pin SQL.
        [[nodiscard]] constexpr auto expected_sql_for(bool pg_dialect) const -> std::string_view {
            if (!expected_sql.empty())
                return expected_sql.view();
            return pg_dialect ? expected_sql_pg.view() : expected_sql_sqlite.view();
        }

        // A collation (WHERE / HAVING leaf or ORDER BY) exists on SQLite only.
        [[nodiscard]] constexpr auto needs_sqlite() const -> bool {
            bool order_collate = false;
            for (const auto& ob : bench.order_by)
                order_collate = order_collate || (ob.enabled && !ob.collate.empty());
            return bench.where.any_collate() || bench.group_by.having.any_collate() || order_collate;
        }

        // `backends` names only sqlite, and a case that needs SQLite says so.
        [[nodiscard]] constexpr auto backends_well_formed() const -> bool {
            return !unknown_backend && (sqlite_only || !needs_sqlite());
        }

        // Either the shared key or BOTH dialect keys, never both forms: one dialect alone would
        // leave the other backend running with no SQL check at all (#844).
        [[nodiscard]] constexpr auto expected_sql_well_formed() const -> bool {
            const bool has_dialect_key = !expected_sql_sqlite.empty() || !expected_sql_pg.empty();
            if (!expected_sql.empty())
                return !has_dialect_key;
            return expected_sql_sqlite.empty() == expected_sql_pg.empty();
        }
    };

    // --- Test-only parsers ---

    constexpr void parse_first_row_into(std::string_view j, std::size_t& p, FirstRowSpec& f, std::size_t& truncated) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (f.count >= FirstRowSpec::MAX_FIELDS) {
                skip_value(jj, pp);
                ++f.count;
                return;
            }
            FirstRowField& field = f.fields.data()[f.count++];
            for (std::size_t i = 0; i < kr.len && i + 1 < field.name.data.size(); ++i)
                field.name.data.data()[field.name.len++] = ptr[kr.start + i];
            if (field.name.len < kr.len)
                ++truncated; // a member name longer than the buffer (#845)
            field.value = parse_typed_value(jj, pp, truncated);
        });
    }

    // `rows: [[a, b], [c, d]]` — every cell a typed value.
    constexpr void parse_rows_into(std::string_view j, std::size_t& p, RowsSpec& rows, std::size_t& truncated) {
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            const std::size_t row = rows.row_count++;
            parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                if (row >= RowsSpec::MAX_ROWS) {
                    skip_value(jjj, ppp);
                    return;
                }
                const std::size_t column = rows.columns.data()[row]++;
                if (column >= RowsSpec::MAX_COLUMNS)
                    skip_value(jjj, ppp);
                else
                    rows.cells.data()[row].data()[column] = parse_typed_value(jjj, ppp, truncated);
            });
        });
    }

    template <std::size_t MaxLen>
    constexpr auto parse_int_array(std::string_view j, std::size_t& p, std::array<int, MaxLen>& out) -> std::size_t {
        std::size_t n = 0;
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (n < MaxLen)
                out.data()[n++] = parse_int(jj, pp);
            else
                skip_value(jj, pp);
        });
        return n;
    }

    template <std::size_t MaxLen>
    constexpr auto parse_double_array(std::string_view j, std::size_t& p, std::array<double, MaxLen>& out)
            -> std::size_t {
        std::size_t n = 0;
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (n < MaxLen)
                out.data()[n++] = parse_double(jj, pp);
            else
                skip_value(jj, pp);
        });
        return n;
    }

    template <std::size_t MaxLen>
    constexpr auto parse_bool_array(std::string_view j, std::size_t& p, std::array<bool, MaxLen>& out) -> std::size_t {
        std::size_t n = 0;
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (n < MaxLen)
                out.data()[n++] = parse_bool(jj, pp);
            else
                skip_value(jj, pp);
        });
        return n;
    }

    constexpr auto
    parse_expected_group_key(const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp, ExpectedSpec& e)
            -> bool {
        if (jkey_eq(ptr, kr, "groups_count")) {
            e.groups_count = static_cast<std::size_t>(parse_int(jj, pp));
            return true;
        } else if (jkey_eq(ptr, kr, "group_keys")) {
            parse_int_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_keys);
            return true;
        } else if (jkey_eq(ptr, kr, "group_agg_int")) {
            parse_int_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_agg_int);
            return true;
        } else if (jkey_eq(ptr, kr, "group_agg_dbl")) {
            parse_double_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_agg_dbl);
            return true;
        } else if (jkey_eq(ptr, kr, "group_agg_null")) {
            parse_bool_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_agg_null);
            return true;
        }
        return false;
    }

    constexpr void parse_expected_into(std::string_view j, std::size_t& p, ExpectedSpec& e, std::size_t& truncated) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "count"))
                e.count = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr, "int_val"))
                e.int_val = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr, "dbl_val"))
                e.dbl_val = parse_double(jj, pp);
            else if (jkey_eq(ptr, kr, "unchanged"))
                e.unchanged = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr, "remaining"))
                e.remaining = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr, "first"))
                parse_first_row_into(jj, pp, e.first, truncated);
            else if (jkey_eq(ptr, kr, "rows"))
                parse_rows_into(jj, pp, e.rows, truncated);
            else if (!parse_expected_group_key(ptr, kr, jj, pp, e))
                skip_value(jj, pp);
        });
    }

    constexpr auto parse_chain_agg_spec(std::string_view j, std::size_t& p, std::size_t& truncated) -> ChainAggSpec {
        ChainAggSpec spec;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "func"))
                spec.func = parse_str<16>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "field"))
                spec.field = parse_str<32>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "result"))
                parse_object(jj, pp, [&](const char* rptr, JsonKeyRef rk, std::string_view rj, std::size_t& rp) {
                    if (jkey_eq(rptr, rk, "type"))
                        spec.res_type = parse_str<8>(rj, rp, truncated);
                    else if (jkey_eq(rptr, rk, "value"))
                        spec.res_value = parse_double(rj, rp);
                    else
                        skip_value(rj, rp);
                });
            else
                skip_value(jj, pp);
        });
        return spec;
    }

    constexpr auto parse_chain_agg_array(
            std::string_view                               j,
            std::size_t&                                   p,
            std::array<ChainAggSpec, TestCase::MAX_CHAIN>& out,
            std::size_t&                                   truncated
    ) -> std::size_t {
        std::size_t n = 0;
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (n < TestCase::MAX_CHAIN)
                out.data()[n] = parse_chain_agg_spec(jj, pp, truncated);
            else
                skip_or_step(jj, pp); // any value, an object or not
            ++n;                      // counts past MAX_CHAIN too, so AggregateRunner can reject the overflow
        });
        return n;
    }

    constexpr void capture_child(std::string_view jj, std::size_t& pp, bool& has, std::size_t& pos) {
        if (pp < jj.size() && jj.data()[pp] == '{') {
            has = true;
            pos = pp;
            skip_braced(jj, pp, '{', '}');
        } else
            skip_value(jj, pp);
    }

    // One node's own keys: a leaf's field/op and its value, an inner node's op. `value` adds one operand
    // of any type the shared parser reads (null, int64, string, ...); `value_int` / `value_dbl` each add
    // one, typed by WHICH key was written, so 0.0 stays a double.
    constexpr void parse_where_node_leaf(
            const char*      ptr,
            JsonKeyRef       kr,
            std::string_view jj,
            std::size_t&     pp,
            WhereSpec&       w,
            WhereNode&       n,
            std::size_t&     truncated
    ) {
        if (jkey_eq(ptr, kr, "field")) {
            n.field = parse_str<32>(jj, pp, truncated);
        } else if (jkey_eq(ptr, kr, "op")) {
            n.op = parse_str<16>(jj, pp, truncated);
        } else if (jkey_eq(ptr, kr, "collate")) {
            n.collate = parse_str<16>(jj, pp, truncated);
        } else if (jkey_eq(ptr, kr, "value")) {
            w.add_value(parse_typed_value(jj, pp, truncated)); // any typed operand: null, int64, string (#853)
        } else if (jkey_eq(ptr, kr, "values")) {
            // the same operand list as the flat `where:` form, so BETWEEN / IN / LIKE work in the tree
            w.value_count = n.first_value; // a repeated key replaces the first, as in the flat form
            parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                add_where_value(w, jjj, ppp, truncated);
            });
        } else if (jkey_eq(ptr, kr, "value_int")) {
            TypedValue tv;
            tv.kind   = TypedValue::Kind::Int;
            tv.as_int = parse_int(jj, pp);
            w.add_value(tv);
        } else if (jkey_eq(ptr, kr, "value_dbl")) {
            TypedValue tv;
            tv.kind      = TypedValue::Kind::Double;
            tv.as_double = parse_double(jj, pp);
            w.add_value(tv);
        } else {
            skip_value(jj, pp);
        }
    }

    // The `where_expr:` tree, written as the tree itself. Nodes are numbered in pre-order from the
    // root. A node's slot is reserved before its children are read, and every node is counted, so a
    // tree past MAX_NODES is rejected instead of losing nodes (#843).
    constexpr auto parse_where_expr_dfs(std::string_view j, std::size_t& p, WhereSpec& w, std::size_t& truncated)
            -> int { // NOSONAR — recursive
        const std::size_t idx = w.add_node(WhereNode{});
        WhereNode         node;
        node.first_value     = w.value_count;
        std::size_t left_pos = 0, right_pos = 0;
        bool        has_left = false, has_right = false;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            skip_ws(jj, pp);
            if (jkey_eq(ptr, kr, "left"))
                capture_child(jj, pp, has_left, left_pos);
            else if (jkey_eq(ptr, kr, "right"))
                capture_child(jj, pp, has_right, right_pos);
            else
                parse_where_node_leaf(ptr, kr, jj, pp, w, node, truncated);
        });
        node.value_count = w.value_count - node.first_value;
        w.put_node(idx, node);
        int left  = -1;
        int right = -1;
        if (has_left) {
            std::size_t lp = left_pos;
            left           = parse_where_expr_dfs(j, lp, w, truncated);
        }
        if (has_right) {
            std::size_t rp = right_pos;
            right          = parse_where_expr_dfs(j, rp, w, truncated);
        }
        w.set_children(idx, left, right);
        return static_cast<int>(idx);
    }

    // --- Top-level TestCase parser ---

    // `backends: [sqlite]` is the only accepted list; anything else (an unknown name, PG alone, two
    // entries) is recorded and rejected by backends_well_formed().
    constexpr void parse_backends_into(std::string_view j, std::size_t& p, TestCase& tc, std::size_t& truncated) {
        std::size_t listed = 0;
        tc.sqlite_only     = false; // a repeated key replaces the first, as everywhere else
        tc.unknown_backend = false;
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            const auto name    = parse_str<16>(jj, pp, truncated);
            tc.sqlite_only     = name.view() == "sqlite";
            tc.unknown_backend = tc.unknown_backend || name.view() != "sqlite";
            ++listed;
        });
        tc.unknown_backend = tc.unknown_backend || listed != 1;
    }

    // A test-only key, or else a shared one (storm::benchmark::parse_bench_field).
    constexpr void
    parse_test_only_field(TestCase& tc, const char* ptr, JsonKeyRef kr, std::string_view j, std::size_t& p) {
        std::size_t& truncated = tc.bench.truncated_strings;
        if (jkey_eq(ptr, kr, "query_type"))
            tc.query_type = parse_str<32>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "dataset"))
            tc.dataset = parse_str<16>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "expected_sql"))
            tc.expected_sql = parse_str<EXPECTED_SQL_CAPACITY>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "expected_sql_sqlite"))
            tc.expected_sql_sqlite = parse_str<EXPECTED_SQL_CAPACITY>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "backends"))
            parse_backends_into(j, p, tc, truncated);
        else if (jkey_eq(ptr, kr, "expected_sql_pg"))
            tc.expected_sql_pg = parse_str<EXPECTED_SQL_CAPACITY>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "expected"))
            parse_expected_into(j, p, tc.expected, truncated);
        else if (jkey_eq(ptr, kr, "erase_count"))
            tc.erase_count = parse_int(j, p);
        else if (jkey_eq(ptr, kr, "insert_count"))
            tc.insert_count = parse_int(j, p);
        else if (jkey_eq(ptr, kr, "update_count"))
            tc.update_count = parse_int(j, p);
        else if (jkey_eq(ptr, kr, "aggregations"))
            tc.chain_len = parse_chain_agg_array(j, p, tc.aggregations, truncated);
        else if (jkey_eq(ptr, kr, "where_expr")) {
            tc.bench.where.begin_form();
            tc.bench.where.root = parse_where_expr_dfs(j, p, tc.bench.where, truncated);
        } else
            parse_bench_field(tc.bench, ptr, kr, j, p);
    }

    inline constexpr std::string_view GROUP_QUERY_PREFIX = "group_";

    // #841: a scalar aggregate is a chain of one and a group_<func> case is a chain of one after
    // group_by, so both are rewritten into `aggregations` here and ONE runner executes them.
    // A scalar case declares its value as int_val or dbl_val (never both); a chain case already
    // carries `aggregations` and is left alone.
    constexpr void normalise_aggregate_case(TestCase& tc) {
        const std::string_view query_type = tc.query_type.view();
        const bool             is_group   = query_type.starts_with(GROUP_QUERY_PREFIX);
        const bool is_scalar = query_type == "count" || query_type == "count_field" || query_type == "count_distinct" ||
                               query_type == "sum" || query_type == "avg" || query_type == "min" || query_type == "max";
        if (!is_group && !is_scalar)
            return;
        ChainAggSpec spec;
        // count_field is COUNT(field): the QuerySet method is still count.
        std::string_view func = query_type;
        if (is_group)
            func = query_type.substr(GROUP_QUERY_PREFIX.size());
        else if (query_type == "count_field")
            func = "count";
        spec.func.append(func);
        spec.field = tc.bench.aggregate.field;
        // A group case is checked against group_keys / group_agg_*, not res_value.
        if (!is_group)
            spec.res_value = tc.expected.int_val != -1 ? static_cast<double>(tc.expected.int_val) : tc.expected.dbl_val;
        tc.aggregations.data()[0] = spec;
        tc.chain_len              = 1;
    }

    constexpr void parse_test_case_into(std::string_view j, std::size_t& p, TestCase& tc) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            parse_test_only_field(tc, ptr, kr, jj, pp);
        });
        normalise_aggregate_case(tc);
    }

} // namespace storm::test
