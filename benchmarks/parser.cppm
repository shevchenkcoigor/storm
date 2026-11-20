// storm_benchmark_parser
//
// The ONE compile-time JSON parser for the YAML-driven specs (#845). It fills a
// `BenchmarkTest` from a JSON object; the benchmarks read
// benchmarks/tests/benchmark_tests.json through it (benchmark_tests.hpp), and
// the YAML test corpus reads its shared keys through it too: tests/test_parser.hpp
// adds only the test-only keys (`expected`, `where_expr`, `aggregations`, ...)
// and hands every other key to parse_bench_field here.
//
// JSON layout example:
//   {
//     "test_name": "...",                  (or "name")
//     "model": "Person",
//     "operation": "order_by_where",
//     "iterations": 1000,
//     "init_dataset_size": 10000,          (or "dataset_size")
//     "where":    { "conditions": [ { "field": "age", "op": ">", "values": [30] } ],
//                   "combine": "and" },
//     "order_by": [ { "field": "salary", "direction": "DESC", "collate": "NOCASE" } ],
//     "group_by": { "fields": ["department"],
//                   "having": { "conditions": [{ "field": "age", "op": ">", "values": [30] }] } },
//     "distinct": { "fields": ["name", "age"] },
//     "limit":    { "value": 10, "offset": 20 },
//     "aggregate": { "func": "sum", "field": "salary" },
//     "join":     { "type": "inner", "related": "User", "fk": "sender" },
//     "setop":    { "type": "union" }
//   }
//
// Each value is sniffed from the JSON token type (int, int64, double, bool,
// null or string) and stored in a TypedValue. The `where` conditions become the
// leaves of one WhereSpec tree, joined left to right by `combine` (#843); an
// unknown `combine` makes the spec ill-formed (#847).
//
// Constexpr step budget (#845): the primitives index a raw `const char*` rather
// than `std::string_view::operator[]`, because every call is constexpr steps.
// Measured by bisecting -fconstexpr-steps on the four YAML slices, the former
// string_view parser cost ~2.3x the raw-pointer one; the numbers are in
// tests/CMakeLists.txt. Keep new code on the raw pointer.
//
// Was: benchmarks/parser.hpp (textual header). Issue #221 — Phase 4 of the
// benchmark module conversion.

module;

// Header units consumed by the global module fragment. `#embed` is a
// preprocessor directive so it expands here at preprocess time of this
// module unit; the path is resolved relative to this source file.
#include <array>
#include <cstddef>
#include <string_view>

export module storm_benchmark_parser;

import storm_benchmark_schema; // BenchmarkTest + sub-specs + ConstexprString alias

export namespace storm::benchmark {

    // ========================================================================
    // Primitives
    // ========================================================================

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

    // A 64-bit integer literal. One that overflows int64 is a compile error (signed overflow in a
    // constant expression), never a wrapped value.
    constexpr auto parse_int64(std::string_view j, std::size_t& p) -> std::int64_t {
        skip_ws(j, p);
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        std::int64_t      v   = 0;
        bool              neg = false;
        if (p < sz && ptr[p] == '-') {
            neg = true;
            ++p;
        }
        while (p < sz && ptr[p] >= '0' && ptr[p] <= '9') {
            const int digit = ptr[p++] - '0';
            v               = neg ? (v * 10) - digit : (v * 10) + digit;
        }
        return v;
    }

    constexpr auto parse_null(std::string_view j, std::size_t& p) -> bool {
        if (j.substr(p, 4) != "null")
            return false;
        p += 4;
        return true;
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

    // Skips a JSON string, escapes included; `p` is on its opening quote. `p` never passes the end of
    // the input, even when it ends on a backslash.
    constexpr void skip_string(const char* ptr, std::size_t sz, std::size_t& p) {
        ++p;
        while (p < sz && ptr[p] != '"') {
            if (ptr[p] == '\\' && p + 1 < sz)
                ++p;
            ++p;
        }
        if (p < sz)
            ++p;
    }

    // Skips a `{...}` or `[...]`. A string inside it is skipped whole, so a brace or bracket in
    // its text does not count as nesting.
    constexpr void skip_braced(std::string_view j, std::size_t& p, char open, char close) {
        const char*       ptr   = j.data();
        const std::size_t sz    = j.size();
        int               depth = 0;
        while (p < sz) {
            const char c = ptr[p];
            if (c == '"') {
                skip_string(ptr, sz, p);
                continue;
            }
            ++p;
            if (c == open)
                ++depth;
            else if (c == close) {
                --depth;
                if (depth == 0)
                    return;
            }
        }
    }

    constexpr auto is_value_end(char c) -> bool {
        return c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' || c == '\r' || c == '\t';
    }

    constexpr void skip_scalar(const char* ptr, std::size_t sz, std::size_t& p) {
        while (p < sz && !is_value_end(ptr[p]))
            ++p;
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

    // Skips the next value. A token no parser consumes (a stray `}` in an array) is stepped over,
    // or the loop around it would never end (#843).
    constexpr void skip_or_step(std::string_view j, std::size_t& p) {
        const std::size_t before = p;
        skip_value(j, p);
        if (p == before && p < j.size())
            ++p;
    }

    // A JSON string into a buffer of N - 1 characters. A longer one keeps its first N - 1
    // characters, is still read to its closing quote so the key after it is parsed, and adds one to
    // `truncated` -- which BenchmarkTest::within_limits() rejects, like an over-long array.
    // A value that is not a string is consumed whole and leaves the result empty.
    template <std::size_t N>
    constexpr auto parse_str(std::string_view j, std::size_t& p, std::size_t& truncated) -> ConstexprString<N> {
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
            if (p < sz && ptr[p] != '"') {
                // Past the buffer: count it, and read on to the closing quote so the next key is parsed.
                // The same loop as skip_string's, written out: a call per string is constexpr steps (#845).
                ++truncated;
                while (p < sz && ptr[p] != '"') {
                    if (ptr[p] == '\\' && p + 1 < sz)
                        ++p;
                    ++p;
                }
            }
            if (p < sz)
                ++p;
        } else {
            skip_value(j, p);
        }
        return r;
    }

    // The next token is `c`. Skips whitespace first.
    constexpr auto at_char(std::string_view j, std::size_t& p, char c) -> bool {
        skip_ws(j, p);
        return p < j.size() && j[p] == c;
    }

    // The number of objects in a top-level JSON array.
    constexpr auto count_tests(std::string_view j) -> std::size_t {
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

    // ========================================================================
    // Objects and arrays
    // ========================================================================

    // A key is compared in place, never copied: the key text is [start, start + len) of the input.
    struct JsonKeyRef {
        std::size_t start = 0;
        std::size_t len   = 0;
    };

    // The key equals `lit` exactly. Written out in full rather than forwarding to a helper: it runs
    // for every key against every candidate, and each call is constexpr steps (#845, ~4%).
    template <std::size_t M> constexpr auto jkey_eq(const char* ptr, JsonKeyRef kr, const char (&lit)[M]) -> bool {
        constexpr std::size_t n = M - 1;
        if (kr.len != n)
            return false;
        for (std::size_t i = 0; i < n; ++i)
            if (ptr[kr.start + i] != lit[i])
                return false;
        return true;
    }

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

    // Walk every key-value pair in a JSON object; call fn(ptr, kr, j, p) per key. `fn` consumes
    // exactly one value.
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

    // Walk a JSON array; call fn(j, p) per element. A value that is not an array is skipped.
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
            if (p >= sz || ptr[p] == ']') {
                if (p < sz)
                    ++p;
                break;
            }
            fn(j, p);
            skip_ws(ptr, sz, p);
            if (p < sz && ptr[p] == ',')
                ++p;
        }
    }

    // ========================================================================
    // TypedValue — sniff the next JSON token: " string, t/f bool, n null, a number with a '.'
    // double, any other number an integer (Int, or Int64 past 32 bits: TypedValue::of_integer).
    // ========================================================================
    constexpr auto is_number_with_dot(std::string_view j, std::size_t p) -> bool {
        const char*       ptr = j.data();
        const std::size_t sz  = j.size();
        while (p < sz && !is_value_end(ptr[p])) {
            if (ptr[p] == '.')
                return true;
            ++p;
        }
        return false;
    }

    constexpr auto parse_typed_value(std::string_view j, std::size_t& p, std::size_t& truncated) -> TypedValue {
        skip_ws(j, p);
        TypedValue tv;
        if (p >= j.size())
            return tv;
        const char c = j.data()[p];
        if (c == '"') {
            tv.kind      = TypedValue::Kind::String;
            tv.as_string = parse_str<64>(j, p, truncated);
        } else if (c == 't' || c == 'f') {
            tv.kind    = TypedValue::Kind::Bool;
            tv.as_bool = parse_bool(j, p);
        } else if (c == 'n' && parse_null(j, p)) {
            tv = TypedValue::null_value();
        } else if (is_number_with_dot(j, p)) {
            tv.kind      = TypedValue::Kind::Double;
            tv.as_double = parse_double(j, p);
        } else {
            tv = TypedValue::of_integer(parse_int64(j, p));
        }
        return tv;
    }

    // ========================================================================
    // BenchmarkTest sub-object parsers
    // ========================================================================

    // Adds one value to the WHERE pool, skipping a token that parses to nothing so no loop stalls.
    constexpr void add_where_value(WhereSpec& w, std::string_view j, std::size_t& p, std::size_t& truncated) {
        const std::size_t before = p;
        const TypedValue  value  = parse_typed_value(j, p, truncated);
        if (p == before)
            skip_or_step(j, p);
        else
            w.add_value(value);
    }

    // One condition {field, op, values: [...]} becomes one leaf of the WHERE tree (#843).
    constexpr void parse_where_leaf(WhereSpec& w, std::string_view j, std::size_t& p, std::size_t& truncated) {
        WhereNode leaf;
        leaf.first_value = w.value_count;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "field")) {
                leaf.field = parse_str<32>(jj, pp, truncated);
            } else if (jkey_eq(ptr, kr, "op")) {
                leaf.op = parse_str<16>(jj, pp, truncated);
            } else if (jkey_eq(ptr, kr, "collate")) {
                leaf.collate = parse_str<16>(jj, pp, truncated);
            } else if (jkey_eq(ptr, kr, "values")) {
                w.value_count = leaf.first_value; // a repeated key replaces the first (#843)
                parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                    add_where_value(w, jjj, ppp, truncated);
                });
            } else {
                skip_value(jj, pp);
            }
        });
        leaf.value_count = w.value_count - leaf.first_value;
        w.add_node(leaf);
    }

    // {conditions: [...], combine: "and"|"or"}: the conditions are the leaves, joined left to right.
    // AND when `combine` is absent; it may come before or after `conditions` (#843). Any other
    // `combine` spelling is recorded, and the spec is not well formed (#847, WhereSpec::combine_of).
    constexpr void parse_where(WhereSpec& w, std::string_view j, std::size_t& p, std::size_t& truncated) {
        w.begin_form();
        const std::size_t first_leaf  = w.node_count;
        const std::size_t first_value = w.value_count;
        std::size_t       leaves      = 0;
        WhereCombine      combine     = WhereCombine::And;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "conditions")) {
                // a repeated key replaces the first (#843)
                w.node_count  = first_leaf;
                w.value_count = first_value;
                leaves        = 0;
                parse_array(jj, pp, [&](std::string_view jjj, std::size_t& ppp) {
                    if (at_char(jjj, ppp, '{')) {
                        parse_where_leaf(w, jjj, ppp, truncated);
                        ++leaves;
                    } else {
                        skip_or_step(jjj, ppp); // not a condition: skip it, or parse_array would never leave
                    }
                });
            } else if (jkey_eq(ptr, kr, "combine")) {
                combine = WhereSpec::combine_of(parse_str<8>(jj, pp, truncated).view());
            } else {
                skip_value(jj, pp);
            }
        });
        w.join_leaves(first_leaf, leaves, combine);
    }

    constexpr auto parse_order_by_entry(std::string_view j, std::size_t& p, std::size_t& truncated) -> OrderBySpec {
        OrderBySpec ob;
        ob.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "field"))
                ob.field = parse_str<32>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "direction"))
                ob.direction = parse_str<8>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "collate"))
                ob.collate = parse_str<16>(jj, pp, truncated);
            else
                skip_value(jj, pp);
        });
        return ob;
    }

    constexpr void parse_order_by_array(BenchmarkTest& b, std::string_view j, std::size_t& p) {
        b.order_by_count = 0; // a repeated key replaces the first (#843)
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (at_char(jj, pp, '{'))
                b.add_order_by(parse_order_by_entry(jj, pp, b.truncated_strings));
            else
                skip_or_step(jj, pp); // not an entry: skip it, or parse_array would never leave
        });
    }

    // `add` receives every name; the spec decides what to keep (#843). An element that is not a
    // string is skipped, so it adds no empty name.
    template <typename AddFn>
    constexpr void parse_fields_array(std::string_view j, std::size_t& p, std::size_t& truncated, AddFn add) {
        parse_array(j, p, [&](std::string_view jj, std::size_t& pp) {
            if (at_char(jj, pp, '"'))
                add(parse_str<32>(jj, pp, truncated));
            else
                skip_or_step(jj, pp);
        });
    }

    constexpr auto parse_group_by(std::string_view j, std::size_t& p, std::size_t& truncated) -> GroupBySpec {
        GroupBySpec g;
        g.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "fields")) {
                g.declared_count = 0; // a repeated key replaces the first (#843)
                parse_fields_array(jj, pp, truncated, [&](const ConstexprString<32>& s) { g.add_field(s); });
            } else if (jkey_eq(ptr, kr, "having"))
                parse_where(g.having, jj, pp, truncated);
            else
                skip_value(jj, pp);
        });
        return g;
    }

    // `{fields: [...]}` into a CappedFields spec: DISTINCT and values (#867) share this shape.
    template <typename Spec>
    constexpr auto parse_capped_fields(std::string_view j, std::size_t& p, std::size_t& truncated) -> Spec {
        Spec spec;
        spec.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "fields")) {
                spec.declared_count = 0; // a repeated key replaces the first (#843)
                parse_fields_array(jj, pp, truncated, [&](const ConstexprString<32>& s) { spec.add_field(s); });
            } else
                skip_value(jj, pp);
        });
        return spec;
    }

    constexpr auto parse_limit(std::string_view j, std::size_t& p) -> LimitSpec {
        LimitSpec lim;
        lim.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "value"))
                lim.value = parse_int(jj, pp);
            else if (jkey_eq(ptr, kr, "offset"))
                lim.offset = parse_int(jj, pp);
            else
                skip_value(jj, pp);
        });
        return lim;
    }

    constexpr auto parse_aggregate(std::string_view j, std::size_t& p, std::size_t& truncated) -> AggregateSpec {
        AggregateSpec agg;
        agg.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "func"))
                agg.func = parse_str<16>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "field"))
                agg.field = parse_str<32>(jj, pp, truncated);
            else
                skip_value(jj, pp);
        });
        return agg;
    }

    constexpr auto parse_join(std::string_view j, std::size_t& p, std::size_t& truncated) -> JoinSpec {
        JoinSpec jn;
        jn.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "type"))
                jn.type = parse_str<8>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "related"))
                jn.related = parse_str<32>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "fk"))
                jn.fk = parse_str<32>(jj, pp, truncated);
            else if (jkey_eq(ptr, kr, "fks")) {
                jn.fk_count = 0; // a repeated key replaces the first (#843)
                parse_fields_array(jj, pp, truncated, [&](const ConstexprString<32>& s) { jn.add_fk(s); });
            } else
                skip_value(jj, pp);
        });
        return jn;
    }

    constexpr auto parse_setop(std::string_view j, std::size_t& p, std::size_t& truncated) -> SetOpSpec {
        SetOpSpec s;
        s.enabled = true;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            if (jkey_eq(ptr, kr, "type"))
                s.type = parse_str<16>(jj, pp, truncated);
            else
                skip_value(jj, pp);
        });
        return s;
    }

    // ========================================================================
    // Top-level BenchmarkTest keys
    // ========================================================================

    // The scalar keys. Each accepted alias is listed here, once: `name` (the YAML spelling) for
    // `test_name`, `category` for `test_category`, and `dataset_size` for `init_dataset_size` (#72).
    // Returns false when the key is none of them.
    constexpr auto parse_bench_scalar( // NOSONAR(cpp:S3776)
            BenchmarkTest& b, const char* ptr, JsonKeyRef kr, std::string_view j, std::size_t& p
    ) -> bool {
        std::size_t& truncated = b.truncated_strings;
        if (jkey_eq(ptr, kr, "model"))
            b.model = parse_str<32>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "init_dataset_size") || jkey_eq(ptr, kr, "dataset_size"))
            b.dataset_size = parse_int(j, p);
        else if (jkey_eq(ptr, kr, "test_name") || jkey_eq(ptr, kr, "name"))
            b.test_name = parse_str<64>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "test_category") || jkey_eq(ptr, kr, "category"))
            b.test_category = parse_str<32>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "description"))
            b.description = parse_str<128>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "operation"))
            b.operation = parse_str<32>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "iterations"))
            b.iterations = parse_int(j, p);
        else if (jkey_eq(ptr, kr, "batch_size"))
            b.batch_size = parse_int(j, p);
        else if (jkey_eq(ptr, kr, "size_profile"))
            b.size_profile = parse_str<32>(j, p, truncated);
        else
            return false;
        return true;
    }

    // Every shared key. An unknown key is skipped. The YAML test corpus calls this for every key
    // that is not test-only (tests/test_parser.hpp), so a key added here is read by both.
    constexpr void parse_bench_field( // NOSONAR(cpp:S3776)
            BenchmarkTest& b, const char* ptr, JsonKeyRef kr, std::string_view j, std::size_t& p
    ) {
        if (parse_bench_scalar(b, ptr, kr, j, p))
            return;
        std::size_t& truncated = b.truncated_strings;
        if (jkey_eq(ptr, kr, "where"))
            parse_where(b.where, j, p, truncated);
        else if (jkey_eq(ptr, kr, "order_by"))
            parse_order_by_array(b, j, p);
        else if (jkey_eq(ptr, kr, "group_by"))
            b.group_by = parse_group_by(j, p, truncated);
        else if (jkey_eq(ptr, kr, "distinct"))
            b.distinct = parse_capped_fields<DistinctSpec>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "values"))
            b.values = parse_capped_fields<ValuesSpec>(j, p, truncated);
        else if (jkey_eq(ptr, kr, "limit"))
            b.limit = parse_limit(j, p);
        else if (jkey_eq(ptr, kr, "aggregate"))
            b.aggregate = parse_aggregate(j, p, truncated);
        else if (jkey_eq(ptr, kr, "join"))
            b.join = parse_join(j, p, truncated);
        else if (jkey_eq(ptr, kr, "setop"))
            b.setop = parse_setop(j, p, truncated);
        else
            skip_value(j, p);
    }

    constexpr auto parse_test_object(std::string_view j, std::size_t& p) -> BenchmarkTest {
        BenchmarkTest b;
        parse_object(j, p, [&](const char* ptr, JsonKeyRef kr, std::string_view jj, std::size_t& pp) {
            parse_bench_field(b, ptr, kr, jj, pp);
        });
        return b;
    }

    // ========================================================================
    // Top-level array
    // ========================================================================

    // The first N objects of a top-level JSON array, each read by fn(j, p, element).
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

    template <std::size_t N> constexpr auto parse_tests(std::string_view json) -> std::array<BenchmarkTest, N> {
        return parse_array_into<BenchmarkTest, N>(json, [](std::string_view j, std::size_t& p, BenchmarkTest& b) {
            b = parse_test_object(j, p);
        });
    }

    // Note: `load_benchmark_tests()` and `BENCHMARK_TESTS` are deliberately
    // NOT defined in this module. Putting a `consteval` function whose body
    // contains a 50KB `#embed` literal into the module purview triggers a
    // clang PCM deserialization bug ("declaration ID out-of-range for AST
    // file") when any consumer imports the module and instantiates the call.
    // The glue lives in benchmark_tests.hpp (textual header) instead — it
    // imports this module to use parse_tests<>() / count_tests().

} // namespace storm::benchmark
