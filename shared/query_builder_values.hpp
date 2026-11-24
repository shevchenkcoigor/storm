#pragma once

// Typed YAML values (#844) and the WHERE / HAVING comparison they build (build_compare_expr, #835).
//
// Split out of shared/query_builder.hpp (#869), which includes it; include that header, not
// this one. Must be included AFTER `import storm;` in each consumer TU, like query_builder.hpp.

#include <meta>
import storm_orm_utilities;
import storm_orm_fields;

#include "query_builder_resolve.hpp" // NOLINT(misc-header-include-cycle)

namespace storm::orm::query_builder {

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

} // namespace storm::orm::query_builder
