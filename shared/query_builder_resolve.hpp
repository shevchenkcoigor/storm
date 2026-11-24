#pragma once

// By-name resolution of the YAML spec: fields, query methods, WHERE operators, aggregate names and
// collations (#834, #835).
//
// Split out of shared/query_builder.hpp (#869), which includes it; include that header, not
// this one. Must be included AFTER `import storm;` in each consumer TU, like query_builder.hpp.

#include <meta>
import storm_orm_utilities;
import storm_orm_fields;

namespace storm::orm::query_builder {

    // ========================================================================
    // Field dispatcher — compile-time field lookup by name
    // ========================================================================
    // The member `name` of the class reflected by `owner`, or a null info when it has none (#869).
    consteval auto find_member(std::meta::info owner, std::string_view name) -> std::meta::info {
        for (std::meta::info member :
             std::meta::nonstatic_data_members_of(owner, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(member) == name)
                return member;
        }
        return std::meta::info{};
    }

    template <typename Model> consteval auto dispatch_field(std::string_view field_name) -> std::meta::info {
        const std::meta::info member = find_member(^^Model, field_name);
        if (member == std::meta::info{})
            std::unreachable();
        return member;
    }

    template <typename Model> consteval auto has_field(std::string_view field_name) -> bool {
        return find_member(^^Model, field_name) != std::meta::info{};
    }

    // ========================================================================
    // By-name member lookup — shared by the YAML test runners and the benchmark
    // ========================================================================

    // Reflection of the ONE public member function template of class T named
    // `name`, or a null info when T has none or several (#834). Used to call a query
    // method chosen by a YAML string without a branch per method. Overloads are
    // rejected: a splice calls the reflected template directly, so with two
    // candidates overload resolution would be skipped and the first one picked.
    template <typename T> consteval auto member_function_named(std::string_view name) -> std::meta::info {
        std::meta::info found;
        std::size_t     matches = 0;
        for (std::meta::info m : std::meta::members_of(^^T, std::meta::access_context::unchecked()))
            if (std::meta::is_function_template(m) && std::meta::is_public(m) && std::meta::has_identifier(m) &&
                std::meta::identifier_of(m) == name) {
                found = m;
                ++matches;
            }
        return matches == 1 ? found : std::meta::info{};
    }

    // Call q.Fn<FieldRef<Fields>{}...>() (#834). The query methods the YAML names
    // (count/sum/avg/min/max/count_distinct) are all selector-pack templates, so one
    // splice serves every one of them; several fields are one operand, Fn(a + b) (#868).
    template <std::meta::info Fn, std::meta::info... Fields> auto call_query_method(auto&& q) {
        static_assert(
                Fn != std::meta::info{}, "YAML names no single public member function template of the query type"
        );
        // Only COUNT has a no-field form (COUNT(*)). Every other aggregate's
        // constraints accept an empty pack, which would emit e.g. SUM(*) and fail
        // only at runtime, so a missing field is rejected here at compile time.
        static_assert(
                sizeof...(Fields) > 0 || std::meta::identifier_of(Fn) == "count",
                "YAML aggregate needs a field: only count may omit it"
        );
        return std::forward<decltype(q)>(q).template[:Fn:]<storm::meta::FieldRef<Fields>{}...>();
    }

    // ========================================================================
    // WHERE operator resolution by name (#835)
    //
    // A symbolic YAML operator (> >= < <= == !=) is the C++ operator's own symbol,
    // found via operator_of/symbol_of. A word operator maps to a member name by
    // lower-casing and turning spaces into underscores: LIKE -> like, ILIKE -> ilike,
    // IS NOT NULL -> is_not_null. No per-operator branch.
    // ========================================================================
    consteval auto ascii_lower(char c) -> char {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    consteval auto is_symbolic_operator(std::string_view op) -> bool {
        return !op.empty() && !(op.front() >= 'A' && op.front() <= 'Z');
    }

    consteval auto word_operator_matches(std::string_view identifier, std::string_view op) -> bool {
        if (identifier.size() != op.size())
            return false;
        for (std::size_t i = 0; i < op.size(); ++i) {
            const char c        = op[i];
            const char expected = c == ' ' ? '_' : ascii_lower(c);
            if (identifier[i] != expected)
                return false;
        }
        return true;
    }

    // Does member m of a field proxy implement `op`? Symbolic operators are matched
    // on operator templates only: the non-template nullopt_t overloads of ==/!= are
    // a different spelling (`== std::nullopt`) of IS NULL, not a value comparison.
    consteval auto operator_member_matches(std::meta::info m, std::string_view op) -> bool {
        if (!std::meta::is_public(m))
            return false;
        if (is_symbolic_operator(op))
            return std::meta::is_operator_function_template(m) && std::meta::symbol_of(std::meta::operator_of(m)) == op;
        return (std::meta::is_function(m) || std::meta::is_function_template(m)) && std::meta::has_identifier(m) &&
               word_operator_matches(std::meta::identifier_of(m), op);
    }

    // The ONE callable implementing `op` on T (or on a base of T) for argument types
    // Ts, or a null info. IN is the only variadic operator and takes every value
    // type; every other operator template has a single type parameter (BETWEEN's two
    // values share it).
    //
    // A template is judged by its SPECIALIZATION for Ts, because a name can carry
    // several templates: where::Field declares a real `operator==` and a `= delete`d
    // twin that only a composite-FK member satisfies. The reflection of a template
    // does not say it is deleted (is_deleted is false); the specialization does, and
    // an unsatisfied constraint makes can_substitute false. Both are skipped.
    template <typename T, typename... Ts> consteval auto operator_callable(std::string_view op) -> std::meta::info {
        std::vector<std::meta::info> args{^^Ts...};
        args.resize(op == "IN" ? sizeof...(Ts) : std::min<std::size_t>(1, sizeof...(Ts)));
        std::meta::info found;
        std::size_t     matches = 0;
        const auto      scan    = [&](std::meta::info cls) consteval {
            for (std::meta::info m : std::meta::members_of(cls, std::meta::access_context::unchecked())) {
                if (!operator_member_matches(m, op))
                    continue;
                std::meta::info callable = m;
                if (std::meta::is_function_template(m)) {
                    if (!std::meta::can_substitute(m, args))
                        continue;
                    callable = std::meta::substitute(m, args);
                }
                if (std::meta::is_deleted(callable))
                    continue;
                found = callable;
                ++matches;
            }
        };
        scan(^^T);
        for (std::meta::info base : std::meta::bases_of(^^T, std::meta::access_context::unchecked()))
            scan(std::meta::type_of(base));
        return matches == 1 ? found : std::meta::info{};
    }

    template <typename FE, auto op_str, typename... Ts> consteval auto resolve_operator() -> std::meta::info {
        constexpr std::meta::info callable = operator_callable<FE, Ts...>(op_str.view());
        static_assert(
                callable != std::meta::info{},
                "YAML operator names no single callable of the field proxy for these value types"
        );
        return callable;
    }

    // ========================================================================
    // Operator/aggregate validation concepts
    // ========================================================================
    template <auto op_str, typename FieldType>
    concept ValidOperator = []() consteval {
        constexpr std::string_view op = op_str.view();
        // Ordering/BETWEEN on a UUID column is rejected here too (#622), matching
        // where.cppm's OrderableOperand — otherwise rejection lands deep inside
        // resolve_operator's reflection lookup instead of at this named gate.
        constexpr bool is_uuid = std::is_same_v<orm::utilities::optional_inner_type_t<FieldType>, orm::utilities::UUID>;
        if (op == "==" || op == "!=" || op == "LIKE" || op == "ILIKE" || op == "IN") {
            return true;
        }
        if (op == ">" || op == ">=" || op == "<" || op == "<=" || op == "BETWEEN") {
            return !is_uuid;
        }
        if (op == "IS NULL" || op == "IS NOT NULL") {
            return orm::where::NullableField<FieldType>;
        }
        return false;
    }();

    template <auto func_str>
    concept ValidAggregate = []() consteval {
        constexpr std::string_view func = func_str.view();
        return func == "count" || func == "count_distinct" || func == "sum" || func == "avg" || func == "min" ||
               func == "max";
    }();

    // Value-count shape per operator (#835): a comparison and LIKE take exactly one
    // value, BETWEEN two, IN any number (empty is "1 = 0"), IS [NOT] NULL none. Before this check a
    // `values: []` LIKE read values[0] and compiled to LIKE '' (matches nothing).
    template <auto op_str, std::size_t count>
    concept ValidValueCount = []() consteval {
        constexpr std::string_view op = op_str.view();
        if (op == "BETWEEN")
            return count == 2;
        if (op == "IN")
            return true;
        if (op == "IS NULL" || op == "IS NOT NULL")
            return count == 0;
        return count == 1;
    }();

    consteval auto iequals(std::string_view a, std::string_view b) -> bool {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (ascii_lower(a[i]) != ascii_lower(b[i]))
                return false;
        return true;
    }

    // The Collate enumerator named `name` (case-insensitive), or a null info.
    consteval auto find_collate(std::string_view name) -> std::meta::info {
        for (std::meta::info e : std::meta::enumerators_of(^^storm::orm::utilities::Collate))
            if (e != std::meta::enumerators_of(^^storm::orm::utilities::Collate).front() &&
                iequals(std::meta::identifier_of(e), name))
                return e;
        return std::meta::info{};
    }

    // An unknown collation is a compile error rather than a silent NoCase.
    template <auto collate_str>
    concept ValidCollate = collate_str.view().empty() || find_collate(collate_str.view()) != std::meta::info{};

} // namespace storm::orm::query_builder
