#pragma once

// TODO: Convert to storm_orm_query_builder module once clang-p2996 no longer
// crashes when a consumer imports a module that exports templates containing
// ^^QuerySet<Model>::template join<FKs...> splice expressions. Tracked in
// https://github.com/spiritEcosse/storm/issues/256

// Must be included AFTER `import storm;` in each consumer TU.

#include <meta>
import storm_orm_utilities;
import storm_orm_fields;

namespace storm::orm::query_builder {

    using storm::meta::selector_for;
    using storm::orm::utilities::Collate;

    // ========================================================================
    // Field dispatcher — compile-time field lookup by name
    // ========================================================================
    template <typename Model> consteval auto dispatch_field(std::string_view field_name) -> std::meta::info {
        for (std::meta::info member :
             std::meta::nonstatic_data_members_of(^^Model, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(member) == field_name)
                return member;
        }
        std::unreachable();
    }

    template <typename Model> consteval auto has_field(std::string_view field_name) -> bool {
        for (std::meta::info member :
             std::meta::nonstatic_data_members_of(^^Model, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(member) == field_name)
                return true;
        }
        return false;
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

    // ========================================================================
    // Typed values (#844)
    //
    // A YAML value is written as one of null / int / int64 / double / bool / string, and is read
    // in the FIELD's type: a string is a UUID, an ISO date, an ISO date-time, an enumerator name or
    // a hex blob when the field is one. A value that does not fit its field is a compile error
    // (SpecValuesFit), never a coercion.
    // ========================================================================
    inline constexpr std::string_view kIsNullOp    = "IS NULL";
    inline constexpr std::string_view kIsNotNullOp = "IS NOT NULL";

    consteval auto digits_at(std::string_view s, std::size_t pos, std::size_t count) -> int {
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9')
                return -1;
            value = (value * 10) + (c - '0');
        }
        return value;
    }

    consteval auto is_hex_digit(char c) -> bool {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // storm::UUID keeps its text verbatim, so one spelling is accepted: a stored lower-case UUID is never
    // matched by an upper-case literal on SQLite (TEXT compare), and PostgreSQL returns lower case (#853).
    consteval auto is_lower_hex_digit(char c) -> bool {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }

    // 8-4-4-4-12 lower-case hex digits, the form storm::UUID stores.
    consteval auto uuid_text_is_valid(std::string_view s) -> bool {
        if (s.size() != 36)
            return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash ? s[i] != '-' : !is_lower_hex_digit(s[i]))
                return false;
        }
        return true;
    }

    // `YYYY-MM-DD`; a day that does not exist (2023-02-29) is not a date.
    consteval auto iso_date_of(std::string_view s) -> std::optional<std::chrono::year_month_day> {
        if (s.size() != 10 || s[4] != '-' || s[7] != '-')
            return std::nullopt;
        const int year  = digits_at(s, 0, 4);
        const int month = digits_at(s, 5, 2);
        const int day   = digits_at(s, 8, 2);
        if (year < 0 || month < 0 || day < 0)
            return std::nullopt;
        const std::chrono::year_month_day ymd{
                std::chrono::year{year},
                std::chrono::month{static_cast<unsigned>(month)},
                std::chrono::day{static_cast<unsigned>(day)}
        };
        return ymd.ok() ? std::optional{ymd} : std::nullopt;
    }

    // `YYYY-MM-DDTHH:MM:SS`, UTC.
    consteval auto iso_datetime_of(std::string_view s) -> std::optional<std::chrono::system_clock::time_point> {
        if (s.size() != 19 || s[10] != 'T' || s[13] != ':' || s[16] != ':')
            return std::nullopt;
        const auto date   = iso_date_of(s.substr(0, 10));
        const int  hour   = digits_at(s, 11, 2);
        const int  minute = digits_at(s, 14, 2);
        const int  second = digits_at(s, 17, 2);
        if (!date || hour < 0 || minute < 0 || second < 0 || hour > 23 || minute > 59 || second > 59)
            return std::nullopt;
        const auto seconds = std::chrono::sys_days{*date} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
                             std::chrono::seconds{second};
        return std::chrono::time_point_cast<std::chrono::system_clock::duration>(seconds);
    }

    // An even number of hex digits; empty is an empty blob.
    consteval auto blob_hex_is_valid(std::string_view s) -> bool {
        if (s.size() % 2 != 0)
            return false;
        for (const char c : s)
            if (!is_hex_digit(c))
                return false;
        return true;
    }

    constexpr auto hex_digit_value(char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        return (c >= 'a' && c <= 'f' ? c - 'a' : c - 'A') + 10;
    }

    // The bytes of a hex string (blob_hex_is_valid has already vetted it).
    template <typename T>
        requires std::is_same_v<T, std::vector<std::byte>> || std::is_same_v<T, std::vector<std::uint8_t>>
    constexpr auto blob_from_hex(std::string_view text) -> T {
        T bytes;
        for (std::size_t i = 0; i < text.size(); i += 2)
            bytes.push_back(
                    static_cast<typename T::value_type>((hex_digit_value(text[i]) << 4) | hex_digit_value(text[i + 1]))
            );
        return bytes;
    }

    template <typename E> consteval auto find_enumerator(std::string_view name) -> std::meta::info {
        for (std::meta::info e : std::meta::enumerators_of(^^E))
            if (std::meta::identifier_of(e) == name)
                return e;
        return std::meta::info{};
    }

    template <typename T>
    concept BlobType = std::is_same_v<T, std::vector<std::byte>> || std::is_same_v<T, std::vector<std::uint8_t>>;

    // std::chrono::duration only: a time_point also has rep / period, but an integer is not a time.
    template <typename T>
    concept DurationType = requires {
        typename T::rep;
        typename T::period;
    } && std::is_same_v<T, std::chrono::duration<typename T::rep, typename T::period>>;

    // A string the field reads as something other than text: such a value goes through
    // typed_value_as, a text field keeps the string it was given.
    template <typename T>
    concept NonTextString =
            std::is_same_v<T, orm::utilities::UUID> || std::is_same_v<T, std::chrono::year_month_day> ||
            std::is_same_v<T, std::chrono::system_clock::time_point> || std::is_enum_v<T> || BlobType<T>;

    template <typename T> consteval auto integer_in_range(long long v) -> bool {
        if constexpr (std::is_signed_v<T>)
            return v >= static_cast<long long>(std::numeric_limits<T>::min()) &&
                   v <= static_cast<long long>(std::numeric_limits<T>::max());
        else
            return v >= 0 && static_cast<unsigned long long>(v) <= std::numeric_limits<T>::max();
    }

    // An integer a floating type holds exactly, not rounded (16777217 is not a float).
    template <std::floating_point T> consteval auto integer_exact_in_floating(long long v) -> bool {
        // Range-check in the floating domain first: converting 2^63 back to long long is UB.
        const auto f = static_cast<T>(v);
        return f >= -0x1p63 && f < 0x1p63 && static_cast<long long>(f) == v;
    }

    // One rule for an Int and an Int64 literal alike (#853): an INTEGER literal must be held exactly by the field.
    // (A Double literal is not range-checked here: a fraction written for a float field rounds, as in C++.)
    // bool takes 0 / 1 only, a floating field an exactly representable integer, a duration the range of
    // its rep, an integral field its range.
    template <typename T> consteval auto integer_fits(long long v) -> bool {
        if constexpr (std::is_same_v<T, bool>)
            return v == 0 || v == 1;
        else if constexpr (DurationType<T>)
            return integer_fits<typename T::rep>(v);
        else if constexpr (std::is_floating_point_v<T>)
            return integer_exact_in_floating<T>(v);
        else if constexpr (std::is_integral_v<T>)
            return integer_in_range<T>(v);
        else
            return false;
    }

    template <typename T> consteval auto string_fits(std::string_view text) -> bool {
        if constexpr (std::is_same_v<T, orm::utilities::UUID>)
            return uuid_text_is_valid(text);
        else if constexpr (std::is_same_v<T, std::chrono::year_month_day>)
            return iso_date_of(text).has_value();
        else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>)
            return iso_datetime_of(text).has_value();
        else if constexpr (std::is_enum_v<T>)
            return find_enumerator<T>(text) != std::meta::info{};
        else if constexpr (BlobType<T>)
            return blob_hex_is_valid(text);
        else
            return std::is_constructible_v<T, std::string_view> || std::is_same_v<T, std::string>;
    }

    // Can the value `tv`, as written, be read as a FieldType?
    template <typename FieldType> consteval auto value_fits(const auto& tv) -> bool {
        using Kind = std::remove_cvref_t<decltype(tv.kind)>;
        using T    = orm::utilities::optional_inner_type_t<FieldType>;
        switch (tv.kind) {
        case Kind::Null:
            return orm::where::NullableField<FieldType>;
        case Kind::Bool:
            return std::is_same_v<T, bool>;
        case Kind::Double:
            return std::is_floating_point_v<T>;
        case Kind::Int:
            return integer_fits<T>(tv.as_int);
        case Kind::Int64:
            return integer_fits<T>(tv.as_int64);
        case Kind::String:
            return string_fits<T>(tv.as_string.view());
        default:
            return false;
        }
    }

    // value_fits as a constraint: typed_value_as only reads a value its field can hold, so a kind
    // that never set the member it would read is a constraint violation, not a runtime 0 (#848).
    template <typename FieldType, auto tv>
    concept ValueFits = value_fits<FieldType>(tv);

    // A column type the WHERE engine has no comparison for (ExpressionVariant lists no arm for it).
    template <typename T>
    concept NoWhereComparison = BlobType<T> || DurationType<T>;

    // One WHERE leaf (node N) fits its field. A field the model lacks is not this check's to
    // report: the builder's own static_assert names it.
    template <typename Model, const auto& Tree, std::size_t N> consteval auto where_leaf_fits() -> bool {
        constexpr auto& node = Tree.nodes[N];
        if constexpr (!node.is_leaf()) {
            return true;
        } else if constexpr (!has_field<Model>(node.field.view())) {
            return true;
        } else {
            using FieldType = typename[:std::meta::type_of(dispatch_field<Model>(node.field.view())):];
            // ExpressionVariant has no ComparisonExpr arm for a blob or a duration: a WHERE on such a column
            // cannot be built, so the constraint says so here instead of failing deep inside make_shared (#853).
            // Deliberately also rejects `optional<blob> == null` (a NullCheckExpr would build) until #863 lifts it.
            if (NoWhereComparison<orm::utilities::optional_inner_type_t<FieldType>>)
                return false;
            // One spelling for a null check: `== null` / `!= null` (#844). The operator names
            // IS NULL / IS NOT NULL are not part of the YAML.
            if (node.op == kIsNullOp || node.op == kIsNotNullOp)
                return false;
            if (node.op == "LIKE" || node.op == "ILIKE") { // a pattern is text, never a number or null
                if (node.value_count != 1)
                    return false;
                const auto& pattern = Tree.values[node.first_value];
                return pattern.kind == std::remove_cvref_t<decltype(pattern.kind)>::String;
            }
            bool any_null = false;
            for (std::size_t i = 0; i < node.value_count; ++i) {
                const auto& tv = Tree.values[node.first_value + i];
                any_null       = any_null || tv.kind == std::remove_cvref_t<decltype(tv.kind)>::Null;
                if (!value_fits<FieldType>(tv))
                    return false;
            }
            // `x IN (..., NULL)` never matches a NULL row (#625), and a null only means something
            // against one operand of == / !=.
            return !any_null || ((node.op == "==" || node.op == "!=") && node.value_count == 1);
        }
    }

    // Every value in the WHERE tree can be read as the type of the field it is compared with. QueryBuilder
    // applies it to the base model only for a spec without a join: a joined WHERE may target the related
    // model, and apply_where's per-WhereModel static_assert is the check that knows which (#853). That also
    // means a joined WHERE on a blob or duration column is reported by that static_assert's generic message,
    // not by NoWhereComparison's (until #863 gives the engine the arms).
    template <typename Model, auto Spec>
    concept SpecValuesFit = []<std::size_t... Ns>(std::index_sequence<Ns...>) consteval {
        return (where_leaf_fits<Model, Spec.where, Ns>() && ...);
    }(std::make_index_sequence<Spec.where.node_count>{});

    // The same check for the HAVING tree (#868). HAVING is always built on the base model, so unlike
    // SpecValuesFit this one has no join exemption.
    template <typename Model, auto Spec>
    concept SpecHavingValuesFit = []<std::size_t... Ns>(std::index_sequence<Ns...>) consteval {
        return (where_leaf_fits<Model, Spec.group_by.having, Ns>() && ...);
    }(std::make_index_sequence<Spec.group_by.having.node_count>{});

    // A collation applies to a comparison, not to a pattern here. SQLite's docs only say LIKE is ASCII
    // case-insensitive and switched by `case_sensitive_like`; they are silent on COLLATE, and sqlite3 shows
    // `n COLLATE BINARY LIKE 'a%'` still matching `Alice`, so such a case would test nothing. ILIKE has no
    // CollatedField member at all. The names NOCASE/BINARY/RTRIM exist on SQLite only (PostgreSQL has none).
    // (PostgreSQL's own LIKE does honour a COLLATE; that is outside what these SQLite-only cases can say.)
    template <auto op_str, auto collate_str>
    concept CollateApplies = collate_str.view().empty() || (op_str.view() != "LIKE" && op_str.view() != "ILIKE");

    // The Collate enumerator named `name`; the caller has checked ValidCollate.
    consteval auto collate_of(std::string_view name) -> storm::orm::utilities::Collate {
        return std::meta::extract<storm::orm::utilities::Collate>(find_collate(name));
    }

    // The proxy a condition is built on: the field, or the field.collate(...) of a collated leaf.
    template <auto fi, auto collate_str> auto field_proxy() {
        if constexpr (collate_str.view().empty())
            return storm::meta::FieldRef<fi>{};
        else
            return storm::meta::FieldRef<fi>{}.collate(collate_of(collate_str.view()));
    }

    // The WHERE/HAVING condition `FieldRef<fi> <op> vs...` for a YAML operator.
    // Shared by QueryBuilder and the test runners' where-tree builder, so the
    // operator-name mapping exists once (#835).
    template <auto fi, auto op_str, auto collate_str = orm::utilities::ConstexprString<16>{}, typename... Ts>
        requires ValidOperator<op_str, typename storm::meta::FieldRef<fi>::FieldType> &&
                 ValidValueCount<op_str, sizeof...(Ts)> && ValidCollate<collate_str> &&
                 CollateApplies<op_str, collate_str>
    auto build_compare_expr(Ts&&... vs) {
        using Proxy           = decltype(field_proxy<fi, collate_str>());
        constexpr auto method = resolve_operator<Proxy, op_str, std::remove_cvref_t<Ts>...>();
        // The operator templates are instantiated for the decayed type, so their
        // forwarding parameter is an rvalue reference: pass a copy of each value, which
        // also lets a caller hand over a const lvalue.
        return (field_proxy<fi, collate_str>().[:method:](std::remove_cvref_t<Ts>(vs)...));
    }

    // Default FK resolver (#837): a JOIN names its FK by the member name on Model,
    // which dispatch_field already resolves. The benchmark overrides it with its own
    // registry lookup.
    template <typename Model> struct MemberNameResolver {
        consteval auto operator()(std::string_view name) const -> std::meta::info {
            return dispatch_field<Model>(name);
        }
    };
    template <typename Model> inline constexpr MemberNameResolver<Model> member_name_resolver{};

    // ========================================================================
    // QueryBuilder<Model, spec, fk_resolver>
    //
    // Schema-agnostic: `spec` is any NTTP whose nested fields satisfy the
    // spec protocol (spec.where, spec.join, spec.order_by[], spec.group_by,
    // spec.distinct, spec.limit, spec.aggregate, spec.setop (a chain of operands, #870), spec.operation).
    //
    // fk_resolver: consteval callable (string_view) -> std::meta::info for
    // JOIN field dispatch. Defaults to a by-name member lookup on Model.
    //
    // spec.within_limits() (#843): a case that listed more entries than a capped
    // array holds is not silently truncated; it fails the SpecWithinLimits
    // constraint at the QueryBuilder<...> use site.
    // ========================================================================
    template <auto Spec>
    concept SpecWithinLimits = Spec.within_limits();

    // The WHERE tree can be built (#843): children exist, joins are AND/OR, and each leaf has the
    // operands its operator reads. Without it a leaf would index the shared value pool past its own.
    template <auto Spec>
    concept SpecWellFormed = Spec.where.is_well_formed() && Spec.group_by.having.is_well_formed() &&
                             Spec.group_by.having_fields_are_grouped();

    // Every aggregate step can be applied (#868): a GROUP BY has at least one, and only count may name no
    // field (COUNT(*)), and each func is a ValidAggregate name. Whether a field suits sum/avg/min/max is the
    // statement's own NumericAggregateSelectors.
    template <auto Spec>
    concept SpecAggregatesWellFormed = []<std::size_t... Is>(std::index_sequence<Is...>) consteval {
        constexpr auto& steps = Spec.aggregations;
        return !(Spec.group_by.enabled && sizeof...(Is) == 0) &&
               ((ValidAggregate<steps[Is].func> &&
                 (steps[Is].fields.declared_count > 0 || steps[Is].func.view() == "count")) &&
                ...);
    }(std::make_index_sequence<std::min(Spec.aggregation_count, Spec.aggregations.size())>{});

    // The YAML set operation -> the member that applies it (#870), on QuerySet and SetOpBuilder alike;
    // empty for any other spelling. ValidSetOp is the allow-list, as ValidAggregate is for a func.
    consteval auto set_op_member(std::string_view op) -> std::string_view {
        if (op == "union")
            return "union_";
        if (op == "union_all")
            return "union_all";
        if (op == "except")
            return "except_";
        if (op == "intersect")
            return "intersect_";
        return {};
    }

    template <auto op_str>
    concept ValidSetOp = !set_op_member(op_str.view()).empty();

    // The chain's shape (#870): 2..4 operands, an `op` on each but the first, no unknown key, the operands'
    // WHERE trees a well-formed forest of the case's `where` pool. Its values are checked by SpecValuesFit,
    // which walks the whole pool.
    template <auto Spec>
    concept SpecSetOpShape = !Spec.setop.enabled || Spec.setop.is_well_formed(Spec.where);

    // A top-level `where:` / `join:` beside `setop:` would not say which operand, or the combined result, it
    // filters (#870). The operands' trees share the `where` pool but leave it disabled.
    template <auto Spec>
    concept SpecSetOpNoTopLevelFilter = !Spec.setop.enabled || (!Spec.where.enabled && !Spec.join.enabled);

    // Operand I's `op` names a set operation; spelled once per operand so the diagnostic names I and the op.
    template <auto Spec, std::size_t I>
    concept SetOpOperandOpKnown =
            I == 0 || I >= Spec.setop.stored_operand_count() || ValidSetOp<Spec.setop.operands[I].op>;

    template <auto Spec>
    concept SpecSetOpOpsKnown =
            SetOpOperandOpKnown<Spec, 1> && SetOpOperandOpKnown<Spec, 2> && SetOpOperandOpKnown<Spec, 3>;

    // The chain returns the same rows on every backend (#870): no INTERSECT after another operator.
    // Storm emits no parentheses; SQLite reads the chain left to right, PostgreSQL binds INTERSECT tighter.
    template <auto Spec>
    concept SpecSetOpPortable = Spec.setop.is_portable_chain();

    template <
            typename Model,
            auto spec,
            auto fk_resolver  = member_name_resolver<Model>,
            typename ConnType = storm::db::sqlite::Connection>
        requires SpecWithinLimits<spec> && SpecWellFormed<spec> && SpecAggregatesWellFormed<spec> &&
                 (spec.join.enabled || SpecValuesFit<Model, spec>) && SpecHavingValuesFit<Model, spec> &&
                 SpecSetOpShape<spec> && SpecSetOpNoTopLevelFilter<spec> && SpecSetOpOpsKnown<spec> &&
                 SpecSetOpPortable<spec>
    class QueryBuilder {
        // ----------------------------------------------------------------
        // typed_value_as
        // ----------------------------------------------------------------
        // The value in the field's type (an optional field reads as its inner type), chosen by the
        // KIND the value was written as (#843, #844): `1` for a bool field is true, and an integer
        // for a floating field is that number. ValueFits (the check SpecValuesFit runs per leaf) is
        // also this function's constraint, and a null has no member to read, so it never reads a
        // member the value did not set. Not consteval: a UUID or a blob owns heap memory, which
        // cannot leave a constant expression.
        template <typename FieldType, auto tv>
            requires ValueFits<FieldType, tv> && (tv.kind != decltype(tv.kind)::Null)
        static auto typed_value_as() {
            using T               = orm::utilities::optional_inner_type_t<FieldType>;
            constexpr bool is_int = tv.kind == decltype(tv.kind)::Int;
            constexpr bool is_i64 = tv.kind == decltype(tv.kind)::Int64;
            constexpr auto text   = tv.as_string.view();
            if constexpr (std::is_same_v<T, bool>) {
                if constexpr (is_int)
                    return tv.as_int != 0;
                else
                    return bool{tv.as_bool};
            } else if constexpr (std::is_floating_point_v<T>) {
                if constexpr (is_int)
                    return static_cast<T>(tv.as_int);
                else if constexpr (is_i64)
                    return static_cast<T>(tv.as_int64);
                else
                    return T{tv.as_double};
            } else if constexpr (std::is_enum_v<T>) {
                constexpr auto enumerator = find_enumerator<T>(text);
                return [:enumerator:];
            } else if constexpr (std::is_same_v<T, orm::utilities::UUID>) {
                return orm::utilities::UUID{text};
            } else if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
                return *iso_date_of(text);
            } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                return *iso_datetime_of(text);
            } else if constexpr (BlobType<T>) {
                return blob_from_hex<T>(text);
            } else if constexpr (tv.kind == decltype(tv.kind)::String) {
                return std::string{text};
            } else if constexpr (is_i64) {
                return static_cast<T>(tv.as_int64);
            } else {
                return T{tv.as_int};
            }
        }

        // ----------------------------------------------------------------
        // WHERE — one tree of WhereNodes (#843)
        // ----------------------------------------------------------------
        // A leaf: its operands are Tree.values[first_value .. first_value + value_count).
        template <const auto& Tree, typename WhereModel, std::size_t N> static auto build_where_leaf() {
            constexpr auto& cond  = Tree.nodes[N];
            constexpr auto& pool  = Tree.values;
            constexpr auto  first = cond.first_value;
            static_assert(has_field<WhereModel>(cond.field.view()), "condition field not found on model");
            constexpr auto             fi = dispatch_field<WhereModel>(cond.field.view());
            constexpr std::string_view op = cond.op.view();
            using FieldType               = typename[:std::meta::type_of(fi):];

            using Kind  = decltype(pool[first].kind);
            using Inner = orm::utilities::optional_inner_type_t<FieldType>;

            if constexpr (op == "LIKE" || op == "ILIKE") {
                return build_compare_expr<fi, cond.op, cond.collate>(pool[first].as_string.view());
            } else if constexpr (cond.value_count == 1 && pool[first].kind == Kind::Null) {
                // `== null` / `!= null`: the same IS [NOT] NULL as `== std::nullopt` (#844).
                constexpr auto null_op =
                        orm::utilities::ConstexprString<16>(op == "==" ? kIsNullOp.data() : kIsNotNullOp.data());
                return build_compare_expr<fi, null_op, cond.collate>();
            } else if constexpr (op == "BETWEEN") {
                return build_compare_expr<fi, cond.op, cond.collate>(
                        typed_value_as<FieldType, pool[first]>(), typed_value_as<FieldType, pool[first + 1]>()
                );
            } else if constexpr (op == "IN") {
                return []<std::size_t... Is>(std::index_sequence<Is...>) {
                    return build_compare_expr<fi, Tree.nodes[N].op, Tree.nodes[N].collate>(
                            typed_value_as<FieldType, Tree.values[Tree.nodes[N].first_value + Is]>()...
                    );
                }(std::make_index_sequence<cond.value_count>{});
            } else if constexpr (pool[first].kind == Kind::String && !NonTextString<Inner>) {
                return build_compare_expr<fi, cond.op, cond.collate>(pool[first].as_string.view());
            } else {
                return build_compare_expr<fi, cond.op, cond.collate>(typed_value_as<FieldType, pool[first]>());
            }
        }

        // The tree below node N as one expression: a leaf is a comparison, an inner node AND/OR.
        template <const auto& Tree, typename WhereModel, std::size_t N> static auto build_where_node() {
            constexpr auto& node = Tree.nodes[N];
            if constexpr (node.is_leaf()) {
                return build_where_leaf<Tree, WhereModel, N>();
            } else if constexpr (node.op == "AND") {
                return build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.left)>() &&
                       build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.right)>();
            } else {
                static_assert(node.op == "OR", "a condition-tree join node is AND or OR");
                return build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.left)>() ||
                       build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.right)>();
            }
        }

        template <typename WhereModel> static auto apply_where(QuerySet<Model, ConnType>& qs) -> void {
            if constexpr (spec.where.enabled) {
                // The class constraint checked the base model; a WHERE on a related model reads
                // its values as that model's field types.
                static_assert(SpecValuesFit<WhereModel, spec>, "a WHERE value does not fit its field's type");
                qs = qs.where(build_where_node<spec.where, WhereModel, static_cast<std::size_t>(spec.where.root)>());
            }
        }

        // ----------------------------------------------------------------
        // JOIN
        // ----------------------------------------------------------------
        static consteval auto join_fk_name(std::size_t i) -> std::string_view {
            if (spec.join.fk_count >= 2)
                return spec.join.fks[i].view();
            return spec.join.fk.view();
        }

        template <auto... FKs> static consteval auto resolve_join() -> std::meta::info {
            constexpr std::string_view jt = spec.join.type.view();
            if (jt == "left")
                return ^^QuerySet<Model, ConnType>::template left_join<FKs...>;
            return ^^QuerySet<Model, ConnType>::template join<FKs...>;
        }

        template <auto... FKs> static auto dispatch_join_type(auto& qs) -> void {
            constexpr auto method = resolve_join<FKs...>();
            qs                    = qs.[:method:]();
        }

        static auto apply_join(auto& qs) -> void {
            if constexpr (spec.join.enabled) {
                constexpr std::size_t N = spec.join.fk_count >= 2 ? spec.join.fk_count : std::size_t{1};
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    dispatch_join_type<selector_for<fk_resolver(join_fk_name(Is))>()...>(qs);
                }(std::make_index_sequence<N>{});
            }
        }

        // ----------------------------------------------------------------
        // ORDER BY
        // ----------------------------------------------------------------
        static consteval auto parse_collate(std::string_view s) -> Collate {
            return collate_of(s);
        }

        static consteval auto enabled_order_by_count() -> std::size_t {
            std::size_t n = 0;
            for (std::size_t i = 0; i < std::remove_cvref_t<decltype(spec)>::MAX_ORDER_BY; ++i)
                if (spec.order_by[i].enabled)
                    ++n;
            return n;
        }

        static constexpr std::size_t MAX_OB = std::remove_cvref_t<decltype(spec)>::MAX_ORDER_BY;

        template <std::size_t I>
            requires(I >= MAX_OB || !spec.order_by[I].enabled || ValidCollate<spec.order_by[I].collate>)
        static auto apply_order_by_impl(auto qs) {
            if constexpr (I >= MAX_OB) {
                return qs;
            } else if constexpr (!spec.order_by[I].enabled) {
                return apply_order_by_impl<I + 1>(std::move(qs));
            } else {
                constexpr auto& ob = spec.order_by[I];
                static_assert(has_field<Model>(ob.field.view()), "ORDER BY field not found on model");
                constexpr auto fi  = dispatch_field<Model>(ob.field.view());
                constexpr bool asc = (ob.direction.view() != "DESC");
                if constexpr (ob.collate.view() != "") {
                    constexpr auto col = parse_collate(ob.collate.view());
                    return apply_order_by_impl<I + 1>(qs.template order_by<storm::meta::FieldRef<fi>{}, asc, col>());
                } else {
                    return apply_order_by_impl<I + 1>(qs.template order_by<storm::meta::FieldRef<fi>{}, asc>());
                }
            }
        }

        static auto apply_order_by(auto qs) {
            if constexpr (enabled_order_by_count() == 0) {
                return qs;
            } else {
                return apply_order_by_impl<0>(std::move(qs));
            }
        }

        static auto apply_limit(auto qs) {
            if constexpr (!spec.limit.enabled) {
                return qs;
            } else if constexpr (spec.limit.value > 0 && spec.limit.offset > 0) {
                return qs.limit(spec.limit.value).offset(spec.limit.offset);
            } else if constexpr (spec.limit.value > 0) {
                return qs.limit(spec.limit.value);
            } else if constexpr (spec.limit.offset > 0) {
                return qs.offset(spec.limit.offset);
            } else {
                return qs;
            }
        }

        // ----------------------------------------------------------------
        // Aggregate / GROUP BY / DISTINCT / SET OP
        // ----------------------------------------------------------------
        // The aggregate chain (#868): step I is `func` over its field pack, applied to step I-1's
        // statement (every step changes the type, so it is a recursion, not a loop). The YAML `func`
        // IS the member name, resolved by reflection; ValidAggregate stays as the allow-list so a
        // YAML string can only reach those members, not any same-named one.
        template <std::size_t I>
            requires ValidAggregate<spec.aggregations[I].func>
        static auto apply_aggregation(auto&& stmt) {
            constexpr auto& step = spec.aggregations[I];
            return [&]<std::size_t... Fs>(std::index_sequence<Fs...>) {
                static_assert(
                        (has_field<Model>(step.fields.fields[Fs].view()) && ...), "aggregate field not found on model"
                );
                constexpr auto method = member_function_named<std::remove_cvref_t<decltype(stmt)>>(step.func.view());
                return call_query_method<method, dispatch_field<Model>(step.fields.fields[Fs].view())...>(
                        std::forward<decltype(stmt)>(stmt)
                );
            }(std::make_index_sequence<step.fields.declared_count>{});
        }

        template <std::size_t I = 0> static auto apply_aggregations(auto&& stmt) {
            auto next = apply_aggregation<I>(std::forward<decltype(stmt)>(stmt));
            if constexpr (I + 1 >= spec.aggregation_count)
                return next;
            else
                return apply_aggregations<I + 1>(std::move(next));
        }

        static auto build_group_by_query(auto& qs) {
            auto gb = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs
                        .template group_by<selector_for<dispatch_field<Model>(spec.group_by.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.group_by.field_count()>{});

            return apply_aggregations(with_having(std::move(gb)));
        }

        // Operand I of the set-op chain (#870): its WHERE tree over the base model, or the whole table.
        template <std::size_t I> static auto setop_operand() {
            constexpr int             root = spec.setop.operands[I].root;
            QuerySet<Model, ConnType> qs;
            if constexpr (root >= 0)
                return qs.where(build_where_node<spec.where, Model, static_cast<std::size_t>(root)>());
            else
                return qs;
        }

        // The member of Owner (QuerySet for the first pair, SetOpBuilder after) that applies operand I's op.
        template <std::size_t I, typename Owner> static consteval auto setop_method() -> std::meta::info {
            return member_function_named<Owner>(set_op_member(spec.setop.operands[I].op.view()));
        }

        // Operands I.. joined onto the chain, each by its own op. Each step returns the builder by rvalue
        // reference, so it is moved into the next step's parameter rather than assigned to itself.
        template <std::size_t I, typename Chain> static auto append_setop_operands(Chain chain) {
            if constexpr (I >= spec.setop.operand_count) {
                return chain;
            } else {
                constexpr auto method = setop_method<I, Chain>();
                return append_setop_operands<I + 1>(
                        Chain{chain.template[:method:]<Model>(setop_operand<I>().capture_operand())}
                );
            }
        }

        // ORDER BY term I as SetOpBuilder::order_by reads it: the field, its direction, its collation.
        template <std::size_t I>
        static constexpr auto setop_order_field =
                storm::meta::FieldRef<dispatch_field<Model>(spec.order_by[I].field.view())>{};
        template <std::size_t I> static constexpr bool setop_order_asc = spec.order_by[I].direction.view() != "DESC";
        template <std::size_t I>
            requires ValidCollate<spec.order_by[I].collate>
        static constexpr Collate setop_order_collate =
                spec.order_by[I].collate.empty() ? Collate::None : collate_of(spec.order_by[I].collate.view());

        // A SetOpBuilder keeps only its LAST order_by() call (the compound SELECT has one ORDER BY), unlike
        // a QuerySet, which appends; so the terms are collected into one call. The parser stores the
        // enabled terms at [0, enabled_order_by_count()).
        template <std::size_t I, auto... Terms> static auto apply_setop_order_by(auto chain) {
            if constexpr (I < enabled_order_by_count()) {
                static_assert(has_field<Model>(spec.order_by[I].field.view()), "ORDER BY field not found on model");
                return apply_setop_order_by<
                        I + 1,
                        Terms...,
                        setop_order_field<I>,
                        setop_order_asc<I>,
                        setop_order_collate<I>>(std::move(chain));
            } else if constexpr (sizeof...(Terms) == 0) {
                return chain;
            } else {
                return decltype(chain){chain.template order_by<Terms...>()};
            }
        }

        static auto build_setop() {
            static_assert(
                    std::remove_cvref_t<decltype(spec.setop)>::MAX_OPERANDS == 4,
                    "SpecSetOpOpsKnown spells out operands 1..3"
            );
            constexpr auto first = setop_method<1, QuerySet<Model, ConnType>>();
            auto           chain = append_setop_operands<2>(setop_operand<0>().template[:first:]<>(setop_operand<1>()));
            return apply_limit(apply_setop_order_by<0>(std::move(chain)));
        }

        static consteval auto is_setop() -> bool {
            return spec.setop.enabled;
        }
        static consteval auto is_group_by_op() -> bool {
            return spec.group_by.enabled;
        }
        static consteval auto is_aggregate_op() -> bool {
            return spec.aggregation_count > 0 && !spec.group_by.enabled;
        }
        static consteval auto is_distinct_op() -> bool {
            return spec.distinct.enabled;
        }
        static consteval auto is_values_op() -> bool {
            return spec.values.enabled;
        }
        static consteval auto is_first_op() -> bool {
            constexpr auto op = spec.operation.view();
            return op == "first" || op == "first_where";
        }
        static consteval auto is_get_op() -> bool {
            return spec.operation.view() == "get_where";
        }

        // GROUP BY ... HAVING: the HAVING tree is built exactly like the WHERE tree, over the base model.
        static auto with_having(auto&& grouped) {
            if constexpr (spec.group_by.having.enabled) {
                return std::forward<decltype(grouped)>(grouped).having(
                        build_where_node<
                                spec.group_by.having,
                                Model,
                                static_cast<std::size_t>(spec.group_by.having.root)>()
                );
            } else {
                return std::forward<decltype(grouped)>(grouped);
            }
        }

      public:
        static auto build_distinct_stmt(auto& qs) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs
                        .template distinct<selector_for<dispatch_field<Model>(spec.distinct.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.distinct.field_count()>{});
        }

        // `values<f1, ...>()` (#867): the column projection, duplicates kept. Same field-pack
        // expansion as build_distinct_stmt.
        static auto build_values_stmt(auto& qs) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs.template values<selector_for<dispatch_field<Model>(spec.values.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.values.field_count()>{});
        }

        // WhereModel defaults to Model. Pass a related model type for JOIN tests
        // where WHERE conditions live on the joined table rather than Model.
        template <typename WhereModel = Model> static auto build_qs() -> QuerySet<Model, ConnType, true> {
            QuerySet<Model, ConnType> qs;
            apply_join(qs);
            apply_where<WhereModel>(qs);
            return apply_limit(apply_order_by(std::move(qs))).to_finalized();
        }

        static auto build_terminal(auto& qs) {
            if constexpr (is_setop()) {
                return build_setop();
            } else if constexpr (is_aggregate_op()) {
                return apply_aggregations(qs);
            } else if constexpr (is_group_by_op()) {
                return build_group_by_query(qs);
            } else if constexpr (is_distinct_op()) {
                return build_distinct_stmt(qs);
            } else if constexpr (is_values_op()) {
                return build_values_stmt(qs);
            } else if constexpr (is_first_op()) {
                return qs.first();
            } else if constexpr (is_get_op()) {
                return qs.get();
            } else {
                return qs.select();
            }
        }
    };

} // namespace storm::orm::query_builder
