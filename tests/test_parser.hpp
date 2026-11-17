#pragma once

/**
 * @file test_parser.hpp
 * @brief Compile-time JSON parser for YAML-driven unit test cases.
 *
 * Uses raw-pointer access (~2 steps/char) to stay within clang's default
 * constexpr step limit -- which since #582 is all this needs, no
 * -fconstexpr-steps override (see the measurement in tests/CMakeLists.txt).
 * Imports storm_benchmark_schema for struct definitions; does NOT import
 * storm_benchmark_parser.
 *
 * Issue #561: the corpus path is a macro because #embed requires a literal
 * filename -- a TU selects its slice by defining STORM_UNIFIED_CASES_FILE
 * before including this header.
 *
 * Issue #582: there is no default. A full-corpus default let a TU that forgot
 * to select a slice compile anyway, registering all 247 cases under one
 * category with no diagnostic -- so the corpus had to keep being generated in
 * combined form for a branch nothing was meant to take. Failing here instead
 * costs nothing and names the fix.
 */

#ifndef STORM_UNIFIED_CASES_FILE
#error "test_parser.hpp: define STORM_UNIFIED_CASES_FILE to a per-category corpus (see tests/yaml/test_unified_yaml_select.cpp) before including"
#endif

#include <array>
#include <string_view>

import storm_benchmark_schema;

namespace storm::test {

    using storm::benchmark::AggregateSpec;
    using storm::benchmark::BenchmarkTest;
    using storm::benchmark::ConstexprString;
    using storm::benchmark::DistinctSpec;
    using storm::benchmark::GroupBySpec;
    using storm::benchmark::HavingSpec;
    using storm::benchmark::JoinSpec;
    using storm::benchmark::LimitSpec;
    using storm::benchmark::OrderBySpec;
    using storm::benchmark::TypedValue;
    using storm::benchmark::WhereCondition;
    using storm::benchmark::WhereSpec;

    // ============================================================================
    // Raw-pointer JSON primitives
    // ============================================================================

    constexpr void skip_ws(const char* ptr, std::size_t sz, std::size_t& p) { // NOSONAR(S6188)
        while (p < sz) {
            if (char c = ptr[p]; c != ' ' && c != '\n' && c != '\r' && c != '\t')
                break;
            ++p;
        }
    }
    constexpr void skip_ws(std::string_view j, std::size_t& p) {
        skip_ws(j.data(), j.size(), p);
    }

    constexpr void skip_char(std::string_view j, std::size_t& p, char c) {
        skip_ws(j, p);
        if (p < j.size() && j.data()[p] == c)
            ++p;
    }

    constexpr auto parse_int(std::string_view j, std::size_t& p) -> int {
        skip_ws(j, p);
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        int               v   = 0;
        bool              neg = false;
        if (p < sz && ptr[p] == '-') {
            neg = true;
            ++p;
        }
        while (p < sz && ptr[p] >= '0' && ptr[p] <= '9')
            v = v * 10 + (ptr[p++] - '0');
        return neg ? -v : v;
    }

    constexpr auto parse_frac(std::string_view j, std::size_t& p) -> double {
        const char*       ptr  = j.data();
        const std::size_t sz   = j.size();
        double            frac = 0.1;
        double            f    = 0.0;
        while (p < sz && ptr[p] >= '0' && ptr[p] <= '9') {
            f += (ptr[p++] - '0') * frac;
            frac *= 0.1;
        }
        return f;
    }

    constexpr auto parse_double(std::string_view j, std::size_t& p) -> double {
        skip_ws(j, p);
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        double            v   = 0.0;
        bool              neg = false;
        if (p < sz && ptr[p] == '-') {
            neg = true;
            ++p;
        }
        while (p < sz && ptr[p] >= '0' && ptr[p] <= '9')
            v = v * 10.0 + (ptr[p++] - '0');
        if (p < sz && ptr[p] == '.') {
            ++p;
            v += parse_frac(j, p);
        }
        return neg ? -v : v;
    }
    constexpr auto parse_bool_true(const char* ptr, std::size_t sz, std::size_t& p) -> bool {
        if (p + 4 <= sz && ptr[p] == 't' && ptr[p + 1] == 'r' && ptr[p + 2] == 'u' && ptr[p + 3] == 'e') {
            p += 4;
            return true;
        }
        return false;
    }
    constexpr auto parse_bool(std::string_view j, std::size_t& p) -> bool {
        skip_ws(j, p);
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        if (parse_bool_true(ptr, sz, p))
            return true;
        if (p + 5 <= sz && ptr[p] == 'f' && ptr[p + 1] == 'a' && ptr[p + 2] == 'l' && ptr[p + 3] == 's' &&
            ptr[p + 4] == 'e') {
            p += 5;
        }
        return false;
    }

    template <std::size_t N> constexpr auto parse_str(std::string_view j, std::size_t& p) -> ConstexprString<N> {
        skip_ws(j, p);
        const char*        ptr = j.data();
        const std::size_t  sz  = j.size();
        ConstexprString<N> r;
        if (p < sz && ptr[p] == '"') {
            ++p;
            char*       rd = r.data.data();
            std::size_t i  = 0;
            while (p < sz && ptr[p] != '"' && i < N - 1) {
                // #818: `\"` and `\\` unescape to the character after the backslash, so a
                // pinned SQL string can carry the quoted identifiers Storm emits.
                if (ptr[p] == '\\' && p + 1 < sz)
                    ++p;
                rd[i++] = ptr[p++];
            }
            r.len = i;
            rd[i] = '\0';
            if (p < sz && ptr[p] == '"')
                ++p;
        }
        return r;
    }

    constexpr void skip_braced(std::string_view j, std::size_t& p, char open, char close) {
        const char*       ptr   = j.data();
        const std::size_t sz    = j.size();
        int               depth = 0;
        while (p < sz) {
            char c = ptr[p++];
            if (c == open)
                ++depth;
            else if (c == close) {
                --depth;
                if (depth == 0)
                    return;
            }
        }
    }

    constexpr void skip_string(const char* ptr, std::size_t sz, std::size_t& p) {
        ++p;
        while (p < sz && ptr[p] != '"') {
            if (ptr[p] == '\\')
                ++p;
            ++p;
        }
        if (p < sz)
            ++p;
    }

    constexpr void skip_scalar(const char* ptr, std::size_t sz, std::size_t& p) {
        while (p < sz) {
            char c = ptr[p];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
                break;
            ++p;
        }
    }

    constexpr void skip_value(std::string_view j, std::size_t& p) {
        skip_ws(j, p);
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        if (p >= sz)
            return;
        char first = ptr[p];
        if (first == '"')
            skip_string(ptr, sz, p);
        else if (first == '[')
            skip_braced(j, p, '[', ']');
        else if (first == '{')
            skip_braced(j, p, '{', '}');
        else
            skip_scalar(ptr, sz, p);
    }

    constexpr auto count_objects(std::string_view j) -> std::size_t {
        std::size_t n = 0, p = 0;
        skip_char(j, p, '[');
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        while (p < sz) {
            skip_ws(ptr, sz, p);
            if (p >= sz || ptr[p] == ']')
                break;
            if (ptr[p] == '{') {
                ++n;
                skip_braced(j, p, '{', '}');
            }
            skip_ws(ptr, sz, p);
            if (p < sz && ptr[p] == ',')
                ++p;
        }
        return n;
    }

    template <std::size_t M>
    constexpr auto jkey_eq(const char* ptr, std::size_t start, std::size_t len, const char (&lit)[M]) noexcept -> bool {
        constexpr std::size_t n = M - 1;
        if (len != n)
            return false;
        for (std::size_t i = 0; i < n; ++i)
            if (ptr[start + i] != lit[i])
                return false;
        return true;
    }

    struct JsonKeyRef {
        std::size_t start = 0;
        std::size_t len   = 0;
    };

    constexpr auto parse_key_ref(const char* ptr, std::size_t sz, std::size_t& p) -> JsonKeyRef { // NOSONAR(S6188)
        skip_ws(ptr, sz, p);
        JsonKeyRef ref;
        if (p < sz && ptr[p] == '"') {
            ++p;
            ref.start = p;
            while (p < sz && ptr[p] != '"')
                ++p;
            ref.len = p - ref.start;
            if (p < sz)
                ++p;
        }
        return ref;
    }

    constexpr auto at_end(const char* ptr, std::size_t sz, std::size_t& p, char close) -> bool {
        skip_ws(ptr, sz, p);
        if (p < sz && ptr[p] == close) {
            ++p;
            return true;
        }
        return false;
    }

    constexpr void skip_comma(const char* ptr, std::size_t sz, std::size_t& p) {
        skip_ws(ptr, sz, p);
        if (p < sz && ptr[p] == ',')
            ++p;
    }

    // Walk every key-value pair in a JSON object; call fn(ptr, kr, j, p) per key.
    template <typename Fn> constexpr void parse_object(std::string_view j, std::size_t& p, Fn fn) {
        skip_char(j, p, '{');
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        while (p < sz) {
            if (at_end(ptr, sz, p, '}'))
                break;
            skip_ws(ptr, sz, p);
            auto kr = parse_key_ref(ptr, sz, p);
            skip_ws(ptr, sz, p);
            if (p < sz && ptr[p] == ':')
                ++p;
            fn(ptr, kr, j, p);
            skip_comma(ptr, sz, p);
        }
    }

    // Walk a JSON array; call fn(j, p) per element.
    template <typename Fn> constexpr void parse_array(std::string_view j, std::size_t& p, Fn fn) {
        skip_ws(j, p);
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        if (p >= sz || ptr[p] != '[') {
            skip_value(j, p);
            return;
        }
        ++p;
        while (p < sz) {
            skip_ws(ptr, sz, p);
            if (ptr[p] == ']') {
                ++p;
                break;
            }
            fn(j, p);
            skip_ws(ptr, sz, p);
            if (p < sz && ptr[p] == ',')
                ++p;
        }
    }

    constexpr auto is_number_with_dot(std::string_view j, std::size_t p) -> bool {
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        while (p < sz) {
            char c = ptr[p];
            if (c == '.')
                return true;
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n')
                return false;
            ++p;
        }
        return false;
    }

    constexpr auto parse_typed_value(std::string_view j, std::size_t& p) -> TypedValue {
        skip_ws(j, p);
        const char* ptr = j.data();
        TypedValue  tv;
        if (ptr[p] == '"') {
            tv.kind      = TypedValue::Kind::String;
            tv.as_string = parse_str<64>(j, p);
        } else if (ptr[p] == 't' || ptr[p] == 'f') {
            tv.kind    = TypedValue::Kind::Bool;
            tv.as_bool = parse_bool(j, p);
        } else if (is_number_with_dot(j, p)) {
            tv.kind      = TypedValue::Kind::Double;
            tv.as_double = parse_double(j, p);
        } else {
            tv.kind   = TypedValue::Kind::Int;
            tv.as_int = parse_int(j, p);
        }
        return tv;
    }

    // --- BenchmarkTest sub-object parsers ---

    constexpr auto parse_where_condition(std::string_view j, std::size_t& p) -> WhereCondition {
        WhereCondition cond;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "field"))
                cond.field = parse_str<32>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "op"))
                cond.op = parse_str<16>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "values"))
                parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                    if (cond.value_count < WhereCondition::MAX_VALUES)
                        cond.values[cond.value_count++] = parse_typed_value(jjj, ppp);
                    else
                        skip_value(jjj, ppp);
                });
            else
                skip_value(jj, pp);
        });
        return cond;
    }

    constexpr void parse_where_into(std::string_view j, std::size_t& p, WhereSpec& w) {
        w.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "conditions")) {
                parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                    if (w.condition_count < WhereSpec::MAX_CONDITIONS)
                        w.conditions[w.condition_count++] = parse_where_condition(jjj, ppp);
                    else
                        skip_braced(jjj, ppp, '{', '}');
                });
                if (w.condition_count > 1)
                    w.combine_and = true;
            } else
                skip_value(jj, pp);
        });
    }

    constexpr auto parse_order_by_entry(std::string_view j, std::size_t& p) -> OrderBySpec {
        OrderBySpec ob;
        ob.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "field"))
                ob.field = parse_str<32>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "direction"))
                ob.direction = parse_str<8>(jj, pp);
            else
                skip_value(jj, pp);
        });
        return ob;
    }

    constexpr void parse_order_by_array(std::string_view j, std::size_t& p, BenchmarkTest& b) {
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (b.order_by_count < BenchmarkTest::MAX_ORDER_BY)
                b.order_by[b.order_by_count++] = parse_order_by_entry(jj, pp);
            else
                skip_braced(jj, pp, '{', '}');
        });
    }

    constexpr auto parse_having(std::string_view j, std::size_t& p) -> HavingSpec {
        HavingSpec h;
        h.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "field"))
                h.field = parse_str<32>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "op"))
                h.op = parse_str<16>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "value"))
                h.value = parse_typed_value(jj, pp);
            else
                skip_value(jj, pp);
        });
        return h;
    }

    template <std::size_t MaxFields>
    constexpr auto
    parse_fields_array(std::string_view j, std::size_t& p, std::array<ConstexprString<32>, MaxFields>& fields) -> void {
        std::size_t fi = 0;
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (fi < MaxFields)
                fields[fi++] = parse_str<32>(jj, pp);
            else
                skip_value(jj, pp);
        });
    }

    constexpr auto parse_group_by(std::string_view j, std::size_t& p) -> GroupBySpec {
        GroupBySpec g;
        g.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "fields"))
                parse_fields_array(jj, pp, g.fields);
            else if (jkey_eq(ptr, kr.start, kr.len, "having"))
                g.having = parse_having(jj, pp);
            else
                skip_value(jj, pp);
        });
        return g;
    }

    constexpr auto parse_distinct(std::string_view j, std::size_t& p) -> DistinctSpec {
        DistinctSpec d;
        d.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "fields"))
                parse_fields_array(jj, pp, d.fields);
            else
                skip_value(jj, pp);
        });
        return d;
    }

    constexpr auto parse_limit(std::string_view j, std::size_t& p) -> LimitSpec {
        LimitSpec lim;
        lim.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "value"))
                lim.value = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "offset"))
                lim.offset = parse_int(jj, pp);
            else
                skip_value(jj, pp);
        });
        return lim;
    }

    constexpr auto parse_aggregate(std::string_view j, std::size_t& p) -> AggregateSpec {
        AggregateSpec agg;
        agg.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "func"))
                agg.func = parse_str<16>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "field"))
                agg.field = parse_str<32>(jj, pp);
            else
                skip_value(jj, pp);
        });
        return agg;
    }

    constexpr auto parse_join(std::string_view j, std::size_t& p) -> JoinSpec {
        JoinSpec jn;
        jn.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "type"))
                jn.type = parse_str<8>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "related"))
                jn.related = parse_str<32>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "fk"))
                jn.fk = parse_str<32>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "fks"))
                parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                    if (jn.fk_count < JoinSpec::MAX_FKS)
                        jn.fks[jn.fk_count++] = parse_str<32>(jjj, ppp);
                    else
                        skip_value(jjj, ppp);
                });
            else
                skip_value(jj, pp);
        });
        return jn;
    }

    // Issue #72: `init_dataset_size` is the preferred config key; `dataset_size`
    // stays a backward-compatible alias. Kept as a free predicate so
    // parse_bench_field's branch count is unchanged.
    constexpr auto is_init_dataset_size_key(const char* ptr, JsonKeyRef kr) -> bool {
        return jkey_eq(ptr, kr.start, kr.len, "init_dataset_size") || jkey_eq(ptr, kr.start, kr.len, "dataset_size");
    }

    constexpr void
    parse_bench_field(BenchmarkTest& b, const char* ptr, JsonKeyRef kr, std::string_view j, std::size_t& p) {
        if (jkey_eq(ptr, kr.start, kr.len, "model"))
            b.model = parse_str<32>(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "where"))
            parse_where_into(j, p, b.where);
        else if (jkey_eq(ptr, kr.start, kr.len, "order_by"))
            parse_order_by_array(j, p, b);
        else if (jkey_eq(ptr, kr.start, kr.len, "group_by"))
            b.group_by = parse_group_by(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "distinct"))
            b.distinct = parse_distinct(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "limit"))
            b.limit = parse_limit(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "aggregate"))
            b.aggregate = parse_aggregate(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "join"))
            b.join = parse_join(j, p);
        else if (is_init_dataset_size_key(ptr, kr))
            b.dataset_size = parse_int(j, p);
        else
            skip_value(j, p);
    }

    // --- Test-only sub-specs and structs ---

    struct WhereNode {
        ConstexprString<32> field;
        ConstexprString<8>  op;
        int                 value_int = 0;
        double              value_dbl = 0.0;
        int                 left      = -1;
        int                 right     = -1;
    };

    struct FirstRowSpec {
        ConstexprString<64> name;
        bool                has_name             = false;
        int                 age                  = 0;
        bool                has_age              = false;
        double              salary               = 0;
        bool                has_salary           = false;
        bool                is_active            = false;
        bool                has_is_active        = false;
        int                 years_experience     = 0;
        bool                has_years_experience = false;
        ConstexprString<64> department;
        bool                has_department = false;
        ConstexprString<64> content;
        bool                has_content = false;
        int                 value       = 0;
        bool                has_value   = false;
        constexpr FirstRowSpec()        = default;
    };

    struct ExpectedSpec {
        int                            count = 0, int_val = -1, unchanged = -1, remaining = -1;
        double                         dbl_val = -1.0;
        FirstRowSpec                   first;
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
        ExpectedSpec                           expected;
        int                                    insert_count = 0, update_count = 0, erase_count = 0;
        std::size_t                            chain_len = 0;
        static constexpr std::size_t           MAX_CHAIN = 5;
        std::array<ChainAggSpec, MAX_CHAIN>    aggregations{};
        static constexpr int                   MAX_WHERE_NODES = 7;
        std::array<WhereNode, MAX_WHERE_NODES> where_nodes{};
        int                                    where_node_count = 0;
        constexpr TestCase()                                    = default;
    };

    // --- Test-only parsers ---

    constexpr void parse_first_row_into(std::string_view j, std::size_t& p, FirstRowSpec& f) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "name")) {
                f.name     = parse_str<64>(jj, pp);
                f.has_name = true;
            } else if (jkey_eq(ptr, kr.start, kr.len, "age")) {
                f.age     = parse_int(jj, pp);
                f.has_age = true;
            } else if (jkey_eq(ptr, kr.start, kr.len, "salary")) {
                f.salary     = parse_double(jj, pp);
                f.has_salary = true;
            } else if (jkey_eq(ptr, kr.start, kr.len, "is_active")) {
                f.is_active     = parse_bool(jj, pp);
                f.has_is_active = true;
            } else if (jkey_eq(ptr, kr.start, kr.len, "years_experience")) {
                f.years_experience     = parse_int(jj, pp);
                f.has_years_experience = true;
            } else if (jkey_eq(ptr, kr.start, kr.len, "department")) {
                f.department     = parse_str<64>(jj, pp);
                f.has_department = true;
            } else if (jkey_eq(ptr, kr.start, kr.len, "content")) {
                f.content     = parse_str<64>(jj, pp);
                f.has_content = true;
            } else if (jkey_eq(ptr, kr.start, kr.len, "value")) {
                f.value     = parse_int(jj, pp);
                f.has_value = true;
            } else
                skip_value(jj, pp);
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
        if (jkey_eq(ptr, kr.start, kr.len, "groups_count")) {
            e.groups_count = static_cast<std::size_t>(parse_int(jj, pp));
            return true;
        } else if (jkey_eq(ptr, kr.start, kr.len, "group_keys")) {
            parse_int_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_keys);
            return true;
        } else if (jkey_eq(ptr, kr.start, kr.len, "group_agg_int")) {
            parse_int_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_agg_int);
            return true;
        } else if (jkey_eq(ptr, kr.start, kr.len, "group_agg_dbl")) {
            parse_double_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_agg_dbl);
            return true;
        } else if (jkey_eq(ptr, kr.start, kr.len, "group_agg_null")) {
            parse_bool_array<ExpectedSpec::MAX_GROUPS>(jj, pp, e.group_agg_null);
            return true;
        }
        return false;
    }

    constexpr void parse_expected_into(std::string_view j, std::size_t& p, ExpectedSpec& e) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "count"))
                e.count = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "int_val"))
                e.int_val = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "dbl_val"))
                e.dbl_val = parse_double(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "unchanged"))
                e.unchanged = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "remaining"))
                e.remaining = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "first"))
                parse_first_row_into(jj, pp, e.first);
            else if (!parse_expected_group_key(ptr, kr, jj, pp, e))
                skip_value(jj, pp);
        });
    }

    constexpr auto parse_chain_agg_spec(std::string_view j, std::size_t& p) -> ChainAggSpec {
        ChainAggSpec spec;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr.start, kr.len, "func"))
                spec.func = parse_str<16>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "field"))
                spec.field = parse_str<32>(jj, pp);
            else if (jkey_eq(ptr, kr.start, kr.len, "result"))
                parse_object(jj, pp, [&](const char* rptr, JsonKeyRef rk, std::string_view rj, std::size_t& rp) {
                    if (jkey_eq(rptr, rk.start, rk.len, "type"))
                        spec.res_type = parse_str<8>(rj, rp);
                    else if (jkey_eq(rptr, rk.start, rk.len, "value"))
                        spec.res_value = parse_double(rj, rp);
                    else
                        skip_value(rj, rp);
                });
            else
                skip_value(jj, pp);
        });
        return spec;
    }

    constexpr auto
    parse_chain_agg_array(std::string_view j, std::size_t& p, std::array<ChainAggSpec, TestCase::MAX_CHAIN>& out)
            -> std::size_t {
        std::size_t n = 0;
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (n < TestCase::MAX_CHAIN)
                out.data()[n++] = parse_chain_agg_spec(jj, pp);
            else
                skip_braced(jj, pp, '{', '}');
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

    constexpr void
    parse_where_node_leaf(const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp, WhereNode& n) {
        if (jkey_eq(ptr, kr.start, kr.len, "field"))
            n.field = parse_str<32>(jj, pp);
        else if (jkey_eq(ptr, kr.start, kr.len, "op"))
            n.op = parse_str<8>(jj, pp);
        else if (jkey_eq(ptr, kr.start, kr.len, "value_int"))
            n.value_int = parse_int(jj, pp);
        else if (jkey_eq(ptr, kr.start, kr.len, "value_dbl"))
            n.value_dbl = parse_double(jj, pp);
        else
            skip_value(jj, pp);
    }

    template <int MaxNodes>
    constexpr auto parse_where_expr_dfs( // NOSONAR — consteval recursive DFS
            std::string_view j, std::size_t& p,
            std::array<WhereNode, MaxNodes>& nodes, int& count) -> int {
        if (count >= MaxNodes) {
            skip_braced(j, p, '{', '}');
            return -1;
        }
        int idx              = count++;
        nodes[idx]           = WhereNode{};
        std::size_t left_pos = 0, right_pos = 0;
        bool        has_left = false, has_right = false;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            skip_ws(jj, pp);
            if (jkey_eq(ptr, kr.start, kr.len, "left"))
                capture_child(jj, pp, has_left, left_pos);
            else if (jkey_eq(ptr, kr.start, kr.len, "right"))
                capture_child(jj, pp, has_right, right_pos);
            else
                parse_where_node_leaf(ptr, kr, jj, pp, nodes[idx]);
        });
        if (has_left) {
            std::size_t lp  = left_pos;
            nodes[idx].left = parse_where_expr_dfs<MaxNodes>(j, lp, nodes, count);
        }
        if (has_right) {
            std::size_t rp   = right_pos;
            nodes[idx].right = parse_where_expr_dfs<MaxNodes>(j, rp, nodes, count);
        }
        return idx;
    }

    // --- Top-level TestCase parser ---

    constexpr void
    parse_test_only_field(TestCase& tc, const char* ptr, JsonKeyRef kr, std::string_view j, std::size_t& p) {
        if (jkey_eq(ptr, kr.start, kr.len, "name"))
            tc.bench.test_name = parse_str<64>(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "query_type"))
            tc.query_type = parse_str<32>(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "dataset"))
            tc.dataset = parse_str<16>(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "expected_sql"))
            tc.expected_sql = parse_str<EXPECTED_SQL_CAPACITY>(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "expected"))
            parse_expected_into(j, p, tc.expected);
        else if (jkey_eq(ptr, kr.start, kr.len, "erase_count"))
            tc.erase_count = parse_int(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "insert_count"))
            tc.insert_count = parse_int(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "update_count"))
            tc.update_count = parse_int(j, p);
        else if (jkey_eq(ptr, kr.start, kr.len, "aggregations"))
            tc.chain_len = parse_chain_agg_array(j, p, tc.aggregations);
        else if (jkey_eq(ptr, kr.start, kr.len, "where_expr")) {
            int cnt = 0;
            parse_where_expr_dfs<TestCase::MAX_WHERE_NODES>(j, p, tc.where_nodes, cnt);
            tc.where_node_count = cnt;
        } else
            parse_bench_field(tc.bench, ptr, kr, j, p);
    }

    constexpr void parse_test_case_into(std::string_view j, std::size_t& p, TestCase& tc) {
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            parse_test_only_field(tc, ptr, kr, jj, pp);
        });
    }

    template <typename T, std::size_t N, typename ParseIntoFn>
    constexpr auto parse_array_into(std::string_view j, ParseIntoFn fn) -> std::array<T, N> {
        std::array<T, N>  arr{};
        std::size_t       p   = 0;
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        skip_char(j, p, '[');
        for (std::size_t i = 0; i < N; ++i) {
            skip_ws(ptr, sz, p);
            if (p >= sz || ptr[p] == ']')
                break;
            fn(j, p, arr.data()[i]);
            skip_ws(ptr, sz, p);
            if (p < sz && ptr[p] == ',')
                ++p;
        }
        return arr;
    }

    // `static` (internal linkage), NOT inline: STORM_UNIFIED_CASES_FILE differs per
    // TU, so this function's DEFINITION differs per TU. One external-linkage entity
    // with several definitions is [basic.def.odr] IFNDR. It is inert today only
    // because a consteval function is never odr-used and emits no symbol -- one
    // consteval->constexpr edit away from being live. Same discipline as
    // UNIFIED_TESTS in test_unified_yaml_body.h.
    static consteval auto load_unified_tests() {
        static constexpr const char raw[] = {
// #embed is a C23 feature Clang offers as an extension in C++26 mode; this is
// the only portable way to embed the JSON test corpus at compile time. Suppress
// -Wc23-extensions here so the -Werror policy (issue #317) stays clean.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed STORM_UNIFIED_CASES_FILE
#pragma clang diagnostic pop
                , '\0'
        };
        constexpr std::string_view json(raw, sizeof(raw) - 1);
        constexpr std::size_t      n = count_objects(json);
        return parse_array_into<TestCase, n>(json, parse_test_case_into);
    }

} // namespace storm::test
