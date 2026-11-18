// storm_benchmark_parser
//
// Compile-time nested JSON parser for benchmark tests. Reads
// tests/benchmark_tests.json via C++26 `#embed` and converts it into a
// `std::array<BenchmarkTest, N>` of compile-time test descriptors.
//
// JSON layout example:
//   {
//     "test_name": "...",
//     "model": "Person",
//     "operation": "order_by_where",
//     "iterations": 1000,
//     "init_dataset_size": 10000,
//     "where":    { "field": "age", "op": ">", "value": 30 },
//     "order_by": [ { "field": "salary", "direction": "DESC" } ],
//     "group_by": { "fields": ["department"],
//                   "having": { "field": "age", "op": ">", "value": 30 } },
//     "distinct": { "fields": ["name", "age"] },
//     "limit":    { "value": 10, "offset": 20 },
//     "aggregate": { "func": "sum", "field": "salary" },
//     "join":     { "type": "inner", "related": "User", "fk": "sender" }
//   }
//
// `where.value` is sniffed from the JSON token type — int, double, bool,
// or string — and stored in a TypedValue tagged union. Two-value operators
// (BETWEEN) use `value2`; IN uses `in_values`. A second clause combined
// with AND/OR is expressed via `"and": { ... }` or `"or": { ... }` inside
// `where`, encoded as flat fields (`field2`, `op2`, `value2_rhs`) plus
// a `combine_and`/`combine_or` flag.
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
    // Primitive parsers
    // ========================================================================
    constexpr auto skip_whitespace(std::string_view json, std::size_t& pos) -> void {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == '\t')) {
            pos++;
        }
    }

    constexpr auto skip_char(std::string_view json, std::size_t& pos, char c) -> void {
        skip_whitespace(json, pos);
        if (pos < json.size() && json[pos] == c) {
            pos++;
        }
    }

    constexpr auto parse_int(std::string_view json, std::size_t& pos) -> int {
        skip_whitespace(json, pos);
        int  result   = 0;
        bool negative = false;
        if (pos < json.size() && json[pos] == '-') {
            negative = true;
            pos++;
        }
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            result = (result * 10) + (json[pos] - '0');
            pos++;
        }
        return negative ? -result : result;
    }

    constexpr auto parse_double(std::string_view json, std::size_t& pos) -> double {
        skip_whitespace(json, pos);
        double result   = 0.0;
        bool   negative = false;
        if (pos < json.size() && json[pos] == '-') {
            negative = true;
            pos++;
        }
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            result = (result * 10.0) + (json[pos] - '0');
            pos++;
        }
        if (pos < json.size() && json[pos] == '.') {
            pos++;
            double fraction = 0.1;
            while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
                result += (json[pos] - '0') * fraction;
                fraction *= 0.1;
                pos++;
            }
        }
        return negative ? -result : result;
    }

    constexpr auto parse_bool(std::string_view json, std::size_t& pos) -> bool {
        skip_whitespace(json, pos);
        if (pos + 4 <= json.size() && json.substr(pos, 4) == "true") {
            pos += 4;
            return true;
        }
        if (pos + 5 <= json.size() && json.substr(pos, 5) == "false") {
            pos += 5;
            return false;
        }
        return false;
    }

    template <std::size_t N>
    constexpr auto parse_string(std::string_view json, std::size_t& pos) -> ConstexprString<N> {
        skip_whitespace(json, pos);
        ConstexprString<N> result;
        if (pos < json.size() && json[pos] == '"') {
            pos++; // opening quote
            std::size_t i = 0;
            while (pos < json.size() && json[pos] != '"' && i < N - 1) {
                result.data[i] = json[pos];
                i++;
                pos++;
            }
            result.len     = i;
            result.data[i] = '\0';
            if (pos < json.size() && json[pos] == '"') {
                pos++; // closing quote
            }
        }
        return result;
    }

    constexpr auto parse_key(std::string_view json, std::size_t& pos) -> ConstexprString<64> {
        return parse_string<64>(json, pos);
    }

    // ========================================================================
    // Value skipping (for unknown keys and recursive object/array skip)
    // ========================================================================
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    constexpr auto skip_value(std::string_view json, std::size_t& pos) -> void { // NOSONAR(cpp:S3776)
        skip_whitespace(json, pos);
        if (pos >= json.size()) {
            return;
        }

        if (json[pos] == '"') {
            pos++;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\') {
                    pos++;
                }
                pos++;
            }
            if (pos < json.size()) {
                pos++;
            }
            return;
        }

        if (json[pos] == '{' || json[pos] == '[') {
            const char open  = json[pos];
            const char close = (open == '{') ? '}' : ']';
            int        depth = 0;
            while (pos < json.size()) {
                if (json[pos] == '"') {
                    pos++;
                    while (pos < json.size() && json[pos] != '"') {
                        if (json[pos] == '\\') {
                            pos++;
                        }
                        pos++;
                    }
                    if (pos < json.size()) {
                        pos++;
                    }
                    continue;
                }
                if (json[pos] == open) {
                    depth++;
                } else if (json[pos] == close) {
                    depth--;
                    if (depth == 0) {
                        pos++;
                        return;
                    }
                }
                pos++;
            }
            return;
        }

        // number, bool, null
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']' && json[pos] != ' ' &&
               json[pos] != '\n' && json[pos] != '\r' && json[pos] != '\t') {
            pos++;
        }
    }

    // ========================================================================
    // TypedValue — sniff the next JSON token and populate a tagged union.
    //
    // Lookahead: " → string, t/f → bool, digit/-  → scan ahead for '.' → double else int.
    // ========================================================================
    constexpr auto parse_typed_value(std::string_view json, std::size_t& pos) -> TypedValue {
        skip_whitespace(json, pos);
        TypedValue tv;
        if (pos >= json.size()) {
            return tv;
        }

        const char c = json[pos];

        if (c == '"') {
            tv.kind      = TypedValue::Kind::String;
            tv.as_string = parse_string<64>(json, pos);
            return tv;
        }
        if (c == 't' || c == 'f') {
            tv.kind    = TypedValue::Kind::Bool;
            tv.as_bool = parse_bool(json, pos);
            return tv;
        }
        // Number — peek ahead for '.' to decide int vs double
        bool has_dot = false;
        for (std::size_t q = pos; q < json.size(); q++) {
            const char qc = json[q];
            if (qc == '.') {
                has_dot = true;
                break;
            }
            if (qc == ',' || qc == '}' || qc == ']' || qc == ' ' || qc == '\n' || qc == '\r' || qc == '\t') {
                break;
            }
        }
        if (has_dot) {
            tv.kind      = TypedValue::Kind::Double;
            tv.as_double = parse_double(json, pos);
        } else {
            tv.kind   = TypedValue::Kind::Int;
            tv.as_int = parse_int(json, pos);
        }
        return tv;
    }

    // ========================================================================
    // Array of integers — used by where.in_values
    // ========================================================================
    // Parse a "values" array of TypedValues into a WhereCondition
    constexpr auto parse_values_array(WhereCondition& cond, std::string_view json, std::size_t& pos) -> void {
        skip_whitespace(json, pos);
        if (pos >= json.size() || json[pos] != '[') {
            return;
        }
        pos++; // '['
        std::size_t count = 0;
        while (pos < json.size() && count < WhereCondition::MAX_VALUES) {
            skip_whitespace(json, pos);
            if (json[pos] == ']') {
                pos++;
                break;
            }
            cond.values[count] = parse_typed_value(json, pos);
            count++;
            skip_whitespace(json, pos);
            if (json[pos] == ',') {
                pos++;
            }
        }
        cond.value_count = count;
    }

    // ========================================================================
    // Array of short strings — used by group_by.fields, distinct.fields, join.fks
    // ========================================================================
    // `add` receives every entry; the spec decides what to keep (#843).
    template <typename AddFn>
    constexpr auto parse_string_array(std::string_view json, std::size_t& pos, AddFn add) -> void {
        skip_whitespace(json, pos);
        if (pos >= json.size() || json[pos] != '[') {
            return;
        }
        pos++; // '['
        // Reads every entry (#843): parsing on keeps the position right for the key
        // after the array, and the spec counts what does not fit.
        while (pos < json.size()) {
            skip_whitespace(json, pos);
            if (json[pos] == ']') {
                pos++;
                break;
            }
            if (json[pos] == '"') {
                add(parse_string<32>(json, pos));
            } else {
                skip_value(json, pos); // not a name: skip it, or the loop would never leave the array
            }
            skip_whitespace(json, pos);
            if (json[pos] == ',') {
                pos++;
            }
        }
        // Handle possible trailing whitespace + ']'
        skip_whitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') {
            pos++;
        }
    }

    // ========================================================================
    // Nested object parsers
    // ========================================================================

    // Generic object-walker: iterate key/value pairs and dispatch via callback.
    // Callback receives (key_view, json, pos) and consumes exactly one value.
    template <typename Dispatch>
    constexpr auto parse_object_keys(std::string_view json, std::size_t& pos, Dispatch dispatch) -> void {
        skip_char(json, pos, '{');
        while (pos < json.size()) {
            skip_whitespace(json, pos);
            if (pos < json.size() && json[pos] == '}') {
                pos++;
                return;
            }
            auto key = parse_key(json, pos);
            skip_char(json, pos, ':');
            dispatch(key.view(), json, pos);
            skip_whitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') {
                pos++;
            }
        }
    }

    // --- parse_where_condition_into (one condition: {field, op, values: [...]}) ---
    constexpr auto parse_where_condition_into(WhereCondition& cond, std::string_view json, std::size_t& pos) -> void {
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "field") {
                cond.field = parse_string<32>(j, p);
            } else if (key == "op") {
                cond.op = parse_string<16>(j, p);
            } else if (key == "values") {
                parse_values_array(cond, j, p);
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_where_into (conditions array + optional combine) ---
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    constexpr auto parse_where_into(WhereSpec& w, std::string_view json, std::size_t& pos) -> void {
        w.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "conditions") {
                // Parse array of WhereCondition objects
                skip_whitespace(j, p);
                if (p < j.size() && j[p] == '[') {
                    p++; // '['
                    std::size_t idx = 0;
                    while (p < j.size()) {
                        skip_whitespace(j, p);
                        if (j[p] == ']') {
                            p++;
                            break;
                        }
                        if (idx < WhereSpec::MAX_CONDITIONS) {
                            parse_where_condition_into(w.conditions[idx], j, p);
                            idx++;
                            w.condition_count = idx;
                        } else {
                            skip_value(j, p);
                        }
                        skip_whitespace(j, p);
                        if (j[p] == ',') {
                            p++;
                        }
                    }
                }
            } else if (key == "combine") {
                auto s = parse_string<8>(j, p);
                if (s == std::string_view("and")) {
                    w.combine_and = true;
                } else if (s == std::string_view("or")) {
                    w.combine_or = true;
                }
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_order_by_entry_into (one OrderBySpec) ---
    constexpr auto parse_order_by_entry_into(OrderBySpec& o, std::string_view json, std::size_t& pos) -> void {
        o.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "field") {
                o.field = parse_string<32>(j, p);
            } else if (key == "direction") {
                o.direction = parse_string<8>(j, p);
            } else if (key == "collate") {
                o.collate = parse_string<16>(j, p);
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_order_by_into (array of entries) ---
    constexpr auto parse_order_by_array_into(BenchmarkTest& test, std::string_view json, std::size_t& pos) -> void {
        skip_whitespace(json, pos);
        if (pos >= json.size() || json[pos] != '[') {
            return;
        }
        pos++;                   // '['
        test.order_by_count = 0; // a repeated key replaces the first (#843)
        while (pos < json.size()) {
            skip_whitespace(json, pos);
            if (json[pos] == ']') {
                pos++;
                break;
            }
            if (json[pos] == '{') {
                OrderBySpec entry;
                parse_order_by_entry_into(entry, json, pos);
                test.add_order_by(entry);
            } else {
                skip_value(json, pos); // not an entry: skip it, or the loop would never leave the array
            }
            skip_whitespace(json, pos);
            if (json[pos] == ',') {
                pos++;
            }
        }
        skip_whitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') {
            pos++;
        }
    }

    // --- parse_having_into ---
    constexpr auto parse_having_into(HavingSpec& h, std::string_view json, std::size_t& pos) -> void {
        h.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "field") {
                h.field = parse_string<32>(j, p);
            } else if (key == "op") {
                h.op = parse_string<16>(j, p);
            } else if (key == "value") {
                h.value = parse_typed_value(j, p);
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_group_by_into ---
    constexpr auto parse_group_by_into(GroupBySpec& g, std::string_view json, std::size_t& pos) -> void {
        g.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "fields") {
                g.declared_count = 0; // a repeated key replaces the first (#843)
                parse_string_array(j, p, [&](const ConstexprString<32>& s) { g.add_field(s); });
            } else if (key == "having") {
                parse_having_into(g.having, j, p);
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_distinct_into ---
    constexpr auto parse_distinct_into(DistinctSpec& d, std::string_view json, std::size_t& pos) -> void {
        d.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "fields") {
                d.declared_count = 0; // a repeated key replaces the first (#843)
                parse_string_array(j, p, [&](const ConstexprString<32>& s) { d.add_field(s); });
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_limit_into ---
    constexpr auto parse_limit_into(LimitSpec& l, std::string_view json, std::size_t& pos) -> void {
        l.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "value") {
                l.value = parse_int(j, p);
            } else if (key == "offset") {
                l.offset = parse_int(j, p);
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_aggregate_into ---
    constexpr auto parse_aggregate_into(AggregateSpec& a, std::string_view json, std::size_t& pos) -> void {
        a.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "func") {
                a.func = parse_string<16>(j, p);
            } else if (key == "field") {
                a.field = parse_string<32>(j, p);
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_join_into ---
    constexpr auto parse_join_into(JoinSpec& jn, std::string_view json, std::size_t& pos) -> void {
        jn.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "type") {
                jn.type = parse_string<8>(j, p);
            } else if (key == "related") {
                jn.related = parse_string<32>(j, p);
            } else if (key == "fk") {
                jn.fk = parse_string<32>(j, p);
            } else if (key == "fks") {
                jn.fk_count = 0; // a repeated key replaces the first (#843)
                parse_string_array(j, p, [&](const ConstexprString<32>& s) { jn.add_fk(s); });
            } else {
                skip_value(j, p);
            }
        });
    }

    // --- parse_setop_into ---
    constexpr auto parse_setop_into(SetOpSpec& s, std::string_view json, std::size_t& pos) -> void {
        s.enabled = true;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            if (key == "type") {
                s.type = parse_string<16>(j, p);
            } else {
                skip_value(j, p);
            }
        });
    }

    // ========================================================================
    // Top-level BenchmarkTest field dispatcher
    // ========================================================================
    constexpr auto parse_test_field( // NOSONAR(cpp:S3776)
            BenchmarkTest& test, std::string_view key, std::string_view json, std::size_t& pos
    ) -> void {
        if (key == "test_name") {
            test.test_name = parse_string<64>(json, pos);
        } else if (key == "test_category" || key == "category") {
            test.test_category = parse_string<32>(json, pos);
        } else if (key == "description") {
            test.description = parse_string<128>(json, pos);
        } else if (key == "model") {
            test.model = parse_string<32>(json, pos);
        } else if (key == "operation") {
            test.operation = parse_string<32>(json, pos);
        } else if (key == "iterations") {
            test.iterations = parse_int(json, pos);
        } else if (key == "init_dataset_size" || key == "dataset_size") {
            test.dataset_size = parse_int(json, pos);
        } else if (key == "batch_size") {
            test.batch_size = parse_int(json, pos);
        } else if (key == "size_profile") {
            test.size_profile = parse_string<32>(json, pos);
        } else if (key == "where") {
            parse_where_into(test.where, json, pos);
        } else if (key == "order_by") {
            parse_order_by_array_into(test, json, pos);
        } else if (key == "group_by") {
            parse_group_by_into(test.group_by, json, pos);
        } else if (key == "distinct") {
            parse_distinct_into(test.distinct, json, pos);
        } else if (key == "limit") {
            parse_limit_into(test.limit, json, pos);
        } else if (key == "aggregate") {
            parse_aggregate_into(test.aggregate, json, pos);
        } else if (key == "join") {
            parse_join_into(test.join, json, pos);
        } else if (key == "setop") {
            parse_setop_into(test.setop, json, pos);
        } else {
            skip_value(json, pos);
        }
    }

    constexpr auto parse_test_object(std::string_view json, std::size_t& pos) -> BenchmarkTest {
        BenchmarkTest test;
        parse_object_keys(json, pos, [&](std::string_view key, std::string_view j, std::size_t& p) {
            parse_test_field(test, key, j, p);
        });
        return test;
    }

    // ========================================================================
    // Test array parsing
    // ========================================================================
    constexpr auto skip_json_object(std::string_view json, std::size_t& pos) -> void {
        int brace_depth = 0;
        while (pos < json.size()) {
            if (json[pos] == '"') {
                pos++;
                while (pos < json.size() && json[pos] != '"') {
                    if (json[pos] == '\\') {
                        pos++;
                    }
                    pos++;
                }
                if (pos < json.size()) {
                    pos++;
                }
                continue;
            }
            if (json[pos] == '{') {
                brace_depth++;
            }
            if (json[pos] == '}') {
                brace_depth--;
                if (brace_depth == 0) {
                    pos++;
                    break;
                }
            }
            pos++;
        }
    }

    constexpr auto count_tests(std::string_view json) -> std::size_t {
        std::size_t count = 0;
        std::size_t pos   = 0;
        skip_char(json, pos, '[');
        while (pos < json.size()) {
            skip_whitespace(json, pos);
            if (json[pos] == ']') {
                break;
            }
            if (json[pos] == '{') {
                count++;
                skip_json_object(json, pos);
            }
            skip_whitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') {
                pos++;
            }
        }
        return count;
    }

    template <std::size_t N> constexpr auto parse_tests(std::string_view json) -> std::array<BenchmarkTest, N> {
        std::array<BenchmarkTest, N> tests{};
        std::size_t                  pos = 0;
        skip_char(json, pos, '[');
        for (std::size_t i = 0; i < N; i++) {
            skip_whitespace(json, pos);
            if (pos >= json.size() || json[pos] == ']') {
                break;
            }
            tests[i] = parse_test_object(json, pos);
            skip_whitespace(json, pos);
            if (pos < json.size() && json[pos] == ',') {
                pos++;
            }
        }
        return tests;
    }

    // Note: `load_benchmark_tests()` and `BENCHMARK_TESTS` are deliberately
    // NOT defined in this module. Putting a `consteval` function whose body
    // contains a 50KB `#embed` literal into the module purview triggers a
    // clang PCM deserialization bug ("declaration ID out-of-range for AST
    // file") when any consumer imports the module and instantiates the call.
    // The glue lives in benchmark_tests.hpp (textual header) instead — it
    // imports this module to use parse_tests<>() / count_tests().

} // namespace storm::benchmark
