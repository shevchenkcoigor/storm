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

    using storm::benchmark::AggregateSpec;
    using storm::benchmark::BenchmarkTest;
    using storm::benchmark::CappedFields;
    using storm::benchmark::ConstexprString;
    using storm::benchmark::JsonKeyRef;
    using storm::benchmark::TypedValue;
    using storm::benchmark::WhereNode;
    using storm::benchmark::WhereSpec;

    using storm::benchmark::at_char;
    using storm::benchmark::jkey_eq;
    using storm::benchmark::parse_aggregations;
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

    // A `{member: value, ...}` list. `count` counts every key seen, even past MAX_FIELDS, so a reader
    // can reject an overflowing case instead of dropping its extra keys.
    template <std::size_t N> struct FieldValueList {
        static constexpr std::size_t MAX_FIELDS = N;
        std::array<FirstRowField, N> fields{};
        std::size_t                  count = 0;
        constexpr FieldValueList()         = default;
    };

    // `expected.first` as a name -> value list, checked member by member against
    // the row via reflection (test_select_runner.h), so a new model field needs no
    // parser change.
    using FirstRowSpec = FieldValueList<8>;

    // `set:` (#871): the SET columns and their values of update_where / update_all, or the row an
    // upsert inserts. The corpus uses all 4; past it the case does not compile.
    using SetSpec = FieldValueList<4>;
    // The rows equal to `set:` are counted through a WHERE of one leaf per entry, AND-joined
    // (test_write_runner.h, set_match_spec): N leaves and N - 1 joins must fit the WHERE caps.
    static_assert(
            2 * SetSpec::MAX_FIELDS - 1 <= storm::benchmark::WhereSpec::MAX_NODES &&
            SetSpec::MAX_FIELDS <= storm::benchmark::WhereSpec::MAX_VALUES
    );

    // `on_conflict: {target: [...], update: [...]}` (#871): the conflict columns and, for
    // upsert_update, the columns DO UPDATE sets. Any other key, or a value that is not a mapping,
    // is counted in `unknown_keys` and rejected, never skipped.
    struct OnConflictSpec {
        bool            enabled = false;
        CappedFields<4> target;
        CappedFields<4> update;
        std::size_t     unknown_keys = 0;
        constexpr OnConflictSpec()   = default;

        [[nodiscard]] constexpr auto within_limits() const -> bool {
            return target.within_limits() && update.within_limits();
        }

        // Whether `name` is one of the conflict columns.
        [[nodiscard]] constexpr auto targets(std::string_view name) const -> bool {
            for (std::size_t i = 0; i < target.declared_count && i < target.fields.size(); ++i)
                if (target.fields[i].view() == name)
                    return true;
            return false;
        }
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
        int count = 0, int_val = -1, unchanged = -1, remaining = -1;
        // #871: rows equal to `set:` after an update_where / update_all / upsert_* write.
        int matching = -1;
        // #871: whether `count` was written (its default 0 is a real value), so a write case that
        // would not read it can reject it.
        bool count_written = false;
        // #871: keys written that no write runner reads (int_val, dbl_val, unchanged, group_*, or an
        // unknown key). Read-side runners ignore it; a write case is rejected when it is non-zero.
        std::size_t                    unread_by_writes = 0;
        double                         dbl_val          = -1.0;
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

    // One aggregate step's expected result (#868), beside bench.aggregations[i]: `value` for a scalar or
    // chain step, `groups` (one value per group, ascending key) for a GROUP BY step. groups_count counts
    // past MAX_GROUPS, so an over-long list is rejected; has_value records that `value` was written, so
    // one on a grouped step is rejected rather than ignored. Any other key (the old `type`) is skipped.
    struct AggregationResult {
        bool                                         has_value = false;
        double                                       value     = 0.0;
        std::array<double, ExpectedSpec::MAX_GROUPS> groups{};
        std::size_t                                  groups_count = 0;
        constexpr AggregationResult()                             = default;
    };

    // Longest pinned statement today is 197 chars (a three-operand set-op chain, #870).
    inline constexpr std::size_t EXPECTED_SQL_CAPACITY = 256;

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
        bool sqlite_only     = false;
        bool unknown_backend = false;
        // normalise_aggregate_case: a scalar / group_<func> case wrote what its one aggregation cannot hold.
        bool aggregate_shape_conflict = false;
        int  insert_count = 0, update_count = 0, erase_count = 0;
        std::array<AggregationResult, BenchmarkTest::MAX_AGGREGATIONS> results{};
        // The conditional writes and upsert (#871).
        SetSpec        set;
        bool           set_not_mapping = false; // `set:` was a list or a scalar
        OnConflictSpec on_conflict;
        constexpr TestCase() = default;

        // #843: the spec's caps (the aggregate chain's among them, #868) plus each step's `groups`,
        // and the write keys' (#871).
        [[nodiscard]] constexpr auto within_limits() const -> bool {
            for (const AggregationResult& r : results)
                if (r.groups_count > ExpectedSpec::MAX_GROUPS)
                    return false;
            return set.count <= SetSpec::MAX_FIELDS && on_conflict.within_limits() && bench.within_limits();
        }

        // #871: every conflict column has a `set:` value; otherwise the conflict would be decided by the
        // member's default, which the case never wrote.
        [[nodiscard]] constexpr auto targets_in_set() const -> bool {
            return listed_in_set(on_conflict.target);
        }

        // #871: every DO UPDATE column has a `set:` value; otherwise DO UPDATE would write the member's
        // default over the stored value.
        [[nodiscard]] constexpr auto updates_in_set() const -> bool {
            return listed_in_set(on_conflict.update);
        }

        [[nodiscard]] constexpr auto listed_in_set(const CappedFields<4>& names) const -> bool {
            for (std::size_t t = 0; t < names.declared_count && t < names.fields.size(); ++t) {
                bool found = false;
                for (std::size_t i = 0; i < set.count && i < set.fields.size(); ++i)
                    found = found || set.fields[i].name.view() == names.fields[t].view();
                if (!found)
                    return false;
            }
            return true;
        }

        // #871: the first rule a write case breaks, or empty. A key no runner of the case's query_type
        // reads, or a shape the write would silently ignore, is an error: the corpus static_asserts this
        // empty with the text as the message. The text is the contract tests/yaml/test_spec_limits_writes.cpp pins.
        [[nodiscard]] constexpr auto write_shape_error() const -> std::string_view { // NOSONAR(cpp:S3776)
            const std::string_view     qt        = query_type.view();
            const bool                 is_upsert = qt == "upsert_update" || qt == "upsert_nothing";
            const bool                 filtered  = qt == "update_where" || qt == "erase_where";
            const bool                 sets      = qt == "update_where" || qt == "update_all" || is_upsert;
            constexpr std::string_view set_only  = "set: is read by update_where / update_all / upsert_* only";
            constexpr std::string_view matching_only =
                    "expected.matching is read by update_where / update_all / upsert_* only";
            constexpr std::string_view on_conflict_only = "on_conflict: is read by upsert_update / upsert_nothing only";
            if (set_not_mapping)
                return "set: is a {member: value} mapping";
            if (!filtered && !sets) {
                if (set.count > 0)
                    return set_only;
                if (on_conflict.enabled)
                    return on_conflict_only;
                if (expected.matching >= 0)
                    return matching_only;
                return {};
            }
            if (filtered != bench.where.enabled)
                return filtered
                               ? "update_where / erase_where need a where: (or where_expr:)"
                               : "update_all / upsert_update / upsert_nothing take no where: the write would ignore it";
            if (bench.join.enabled || bench.order_by_count > 0 || bench.limit.enabled)
                return "a write ignores join: / order_by: / limit:, so a case may not write them";
            if (bench.result_shape_count() > 0)
                return "a write runs no values / distinct / group_by / aggregate / setop / first / get";
            if (expected.remaining < 0)
                return "a write case needs expected.remaining: the rows in the table after the write";
            if (expected.count_written)
                return "a write case checks expected.remaining / matching; expected.count would not be read";
            if (expected.first.count > 0 || expected.rows.row_count > 0)
                return "a write case checks expected.remaining / matching; expected.first / rows would not be read";
            if (expected.unread_by_writes > 0)
                return "a write case checks expected.remaining / matching only; another expected.* key would not be "
                       "read";
            if (sets && set.count == 0)
                return "update_where / update_all / upsert_* need a set: of at least one field";
            if (!sets && set.count > 0)
                return set_only;
            if (sets && expected.matching < 0)
                return "update_where / update_all / upsert_* need expected.matching: the rows equal to set: after the "
                       "write";
            if (!sets && expected.matching >= 0)
                return matching_only;
            if (is_upsert != on_conflict.enabled)
                return is_upsert ? "upsert_update / upsert_nothing need on_conflict: {target: [...]}"
                                 : on_conflict_only;
            if (is_upsert) {
                if (on_conflict.unknown_keys > 0)
                    return "on_conflict: takes target and update only";
                if (on_conflict.target.declared_count == 0)
                    return "on_conflict.target names the conflict columns: at least one";
                if (!targets_in_set())
                    return "every on_conflict.target column needs its value in set:";
                if ((qt == "upsert_update") != (on_conflict.update.declared_count > 0))
                    return "upsert_update needs on_conflict.update, upsert_nothing takes none";
                if (!updates_in_set())
                    return "every on_conflict.update column needs its value in set:";
            }
            if (insert_count != 0 || update_count != 0 || erase_count != 0)
                return "insert_count / update_count / erase_count belong to the batch query types";
            return {};
        }

        // The SQL text to pin for a backend: the shared text, or that dialect's own. Empty means
        // the case does not pin SQL.
        [[nodiscard]] constexpr auto expected_sql_for(bool pg_dialect) const -> std::string_view {
            if (!expected_sql.empty())
                return expected_sql.view();
            return pg_dialect ? expected_sql_pg.view() : expected_sql_sqlite.view();
        }

        // A scalar or group_<func> case is ONE aggregation of the query_type's func (#868): a second step,
        // a contradicting `func` or a step's own `result.value` would be dropped or overwritten silently.
        [[nodiscard]] constexpr auto aggregate_shape_well_formed() const -> bool {
            return !aggregate_shape_conflict;
        }

        // Each `result` key is one the runner reads (#868): `value` on an ungrouped step, `groups` on a
        // grouped one, and `groups` only beside the group keys (expected.groups_count) they line up with.
        // (declared_count, not the consteval field_count(): a constexpr function cannot call that.)
        [[nodiscard]] constexpr auto aggregate_results_well_formed() const -> bool {
            const bool grouped = bench.group_by.declared_count > 0;
            for (const AggregationResult& r : results) {
                if (grouped ? r.has_value : r.groups_count > 0)
                    return false;
                if (r.groups_count > 0 && expected.groups_count == 0)
                    return false;
            }
            return true;
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

    template <std::size_t N>
    constexpr void
    parse_first_row_into(std::string_view j, std::size_t& p, FieldValueList<N>& f, std::size_t& truncated) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (f.count >= FieldValueList<N>::MAX_FIELDS) {
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
            if (jkey_eq(ptr, kr, "count")) {
                e.count         = parse_int(jj, pp);
                e.count_written = true;
            } else if (jkey_eq(ptr, kr, "matching"))
                e.matching = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr, "remaining"))
                e.remaining = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr, "first"))
                parse_first_row_into(jj, pp, e.first, truncated);
            else if (jkey_eq(ptr, kr, "rows"))
                parse_rows_into(jj, pp, e.rows, truncated);
            else {
                ++e.unread_by_writes; // every key below is read-side only, or unknown
                if (jkey_eq(ptr, kr, "int_val"))
                    e.int_val = parse_int(jj, pp);
                else if (jkey_eq(ptr, kr, "dbl_val"))
                    e.dbl_val = parse_double(jj, pp);
                else if (jkey_eq(ptr, kr, "unchanged"))
                    e.unchanged = parse_int(jj, pp);
                else if (!parse_expected_group_key(ptr, kr, jj, pp, e))
                    skip_value(jj, pp);
            }
        });
    }

    // A step's `result: {value, groups}`.
    constexpr void parse_aggregation_result(std::string_view j, std::size_t& p, AggregationResult& r) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "value")) {
                r.value     = parse_double(jj, pp);
                r.has_value = true;
            } else if (jkey_eq(ptr, kr, "groups")) {
                r.groups_count = 0; // a repeated key replaces the first (#843)
                parse_array(jj, pp, [&](std::string_view gj, std::size_t& gp) {
                    if (r.groups_count < ExpectedSpec::MAX_GROUPS)
                        r.groups.data()[r.groups_count] = parse_double(gj, gp);
                    else
                        skip_or_step(gj, gp);
                    ++r.groups_count;
                });
            } else
                skip_value(jj, pp);
        });
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

    // `on_conflict: {target: [...], update: [...]}` (#871). A repeated key replaces the first (#843).
    constexpr void
    parse_on_conflict_into(std::string_view j, std::size_t& p, OnConflictSpec& oc, std::size_t& truncated) {
        oc         = OnConflictSpec{};
        oc.enabled = true;
        if (!at_char(j, p, '{')) {
            ++oc.unknown_keys; // a list or a scalar would be misread as an object
            skip_or_step(j, p);
            return;
        }
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            CappedFields<4>* list = nullptr;
            if (jkey_eq(ptr, kr, "target"))
                list = &oc.target;
            else if (jkey_eq(ptr, kr, "update"))
                list = &oc.update;
            if (list == nullptr) {
                ++oc.unknown_keys;
                skip_value(jj, pp);
                return;
            }
            *list = CappedFields<4>{};
            parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                if (at_char(jjj, ppp, '"'))
                    list->add_field(parse_str<32>(jjj, ppp, truncated));
                else {
                    ++oc.unknown_keys;
                    skip_or_step(jjj, ppp);
                }
            });
        });
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
        else if (jkey_eq(ptr, kr, "set")) {
            tc.set             = SetSpec{}; // a repeated key replaces the first (#843)
            tc.set_not_mapping = !at_char(j, p, '{');
            if (tc.set_not_mapping)
                skip_value(j, p); // a list or a scalar would be misread as an object
            else
                parse_first_row_into(j, p, tc.set, truncated);
        } else if (jkey_eq(ptr, kr, "on_conflict"))
            parse_on_conflict_into(j, p, tc.on_conflict, truncated);
        else if (jkey_eq(ptr, kr, "aggregations"))
            // The shared parser reads each step; `result` is test-only.
            parse_aggregations(
                    tc.bench,
                    j,
                    p,
                    [&](std::size_t index, const char* sp, JsonKeyRef sk, std::string_view sj, std::size_t& spp) {
                        if (index < tc.results.size() && jkey_eq(sp, sk, "result"))
                            parse_aggregation_result(sj, spp, tc.results.data()[index]);
                        else
                            skip_value(sj, spp);
                    }
            );
        else if (jkey_eq(ptr, kr, "where_expr")) {
            tc.bench.where.begin_form();
            tc.bench.where.root = parse_where_expr_dfs(j, p, tc.bench.where, truncated);
        } else
            parse_bench_field(tc.bench, ptr, kr, j, p);
    }

    inline constexpr std::string_view GROUP_QUERY_PREFIX = "group_";

    // #841: a scalar aggregate is a chain of one and a group_<func> case is a chain of one after
    // group_by, so both are rewritten into one aggregation here and ONE runner executes them. The
    // func is the query_type's; the fields are the `aggregate:` shorthand's, or none (#868). A scalar
    // case declares its value as int_val or dbl_val (never both); a chain case is left alone.
    constexpr void normalise_aggregate_case(TestCase& tc) {
        const std::string_view query_type = tc.query_type.view();
        const bool             is_group   = query_type.starts_with(GROUP_QUERY_PREFIX);
        const bool is_scalar = query_type == "count" || query_type == "count_field" || query_type == "count_distinct" ||
                               query_type == "sum" || query_type == "avg" || query_type == "min" || query_type == "max";
        if (!is_group && !is_scalar)
            return;
        // count_field is COUNT(field): the QuerySet method is still count.
        std::string_view func = query_type;
        if (is_group)
            func = query_type.substr(GROUP_QUERY_PREFIX.size());
        else if (query_type == "count_field")
            func = "count";
        if (tc.bench.aggregation_count == 0)
            tc.bench.add_aggregation(AggregateSpec{});
        AggregateSpec&         step    = tc.bench.aggregations.data()[0];
        const std::string_view written = step.func.view();
        tc.aggregate_shape_conflict    = tc.bench.aggregation_count > 1 ||
                                      (!written.empty() && written != func && written != query_type) ||
                                      (!is_group && tc.results[0].has_value);
        step.func = {};
        step.func.append(func);
        // A group case is checked against group_keys / group_agg_*, not the value.
        if (!is_group)
            tc.results.data()[0].value =
                    tc.expected.int_val != -1 ? static_cast<double>(tc.expected.int_val) : tc.expected.dbl_val;
    }

    constexpr void parse_test_case_into(std::string_view j, std::size_t& p, TestCase& tc) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            parse_test_only_field(tc, ptr, kr, jj, pp);
        });
        normalise_aggregate_case(tc);
    }

} // namespace storm::test
