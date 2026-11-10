module;
#ifdef STORM_CLANG_TIDY
#include <meta> // clang-tidy segfaults on a TU without a textual <meta> (#756); the real build never defines this
#endif

export module storm_orm_statements_orderby;

import std;
import storm_std_meta;

import storm_orm_utilities;
import storm_orm_fields;     // selector_info — #518 proxy args
import storm_orm_field_attr; // column_name_view — #422/#570 canonical column name

namespace detail {

    using storm::orm::utilities::Collate;

    struct OrderByField {
        std::meta::info field = std::meta::info{};
        bool            asc   = true;
        Collate         col   = Collate::None;
    };

    // Free consteval helper — avoids generic-lambda-in-consteval compiler bug
    //
    // INVARIANT: the "is this a field?" test below (the else arm) must stay in
    // lockstep with is_field_arg() beneath — one sizes the array, the other
    // advances the write index. If they ever disagree, idx desynchronises from
    // field position and a DESC/COLLATE modifier silently attaches to the wrong
    // column: valid SQL, wrong result, no diagnostic. Edit both or neither.
    template <std::size_t N, auto Arg>
    consteval void process_order_by_arg(std::array<OrderByField, N>& result, std::size_t& idx) {
        if constexpr (std::same_as<decltype(Arg), bool>) {
            result[idx - 1].asc = Arg;
        } else if constexpr (std::same_as<decltype(Arg), Collate>) {
            result[idx - 1].col = Arg;
        } else {
            // Field args arrive in either public spelling (#518): a raw
            // std::meta::info or a generated FieldRef proxy. Both normalise to
            // info here, so the emitted ORDER BY text is byte-identical.
            result[idx] = {.field = storm::meta::selector_info<Arg>(), .asc = true, .col = Collate::None};
            ++idx;
        }
    }

    // Count field args (skip bool and Collate modifiers).
    // Mirrors process_order_by_arg's dispatch above — see the INVARIANT there.
    template <auto Arg> consteval auto is_field_arg() -> bool {
        return !std::same_as<decltype(Arg), bool> && !std::same_as<decltype(Arg), Collate>;
    }

} // namespace detail

export namespace storm::orm::statements {

    namespace buffer_size = storm::orm::utilities::buffer_size;
    using storm::orm::utilities::collate_to_sql;
    using storm::orm::utilities::ConstexprString;

    // The alias prefix the JOIN SQL gives the BASE table — "FROM <Base> t1 …" for an FK,
    // reverse-FK or m2m join (#785). Over such a join a base-table column in WHERE /
    // ORDER BY / an aggregate / GROUP BY / HAVING must be written "t1.<col>": a bare name
    // shared with a joined table (every model's `id`) is rejected as ambiguous. A column
    // of a JOINED model keeps its bare spelling.
    inline constexpr std::string_view join_base_alias_prefix = "t1.";

    // True when `member` is a column of the model `Owner`. Owner = void matches nothing,
    // which is how the non-join writers opt out. A type, not a std::meta::info NTTP: a
    // reflection template argument forwarded into a lambda-expanded member template
    // crashes clang-p2996 (TransformCXXReflectExpr).
    template <typename Owner> consteval auto is_column_of(std::meta::info member) -> bool {
        if constexpr (std::is_void_v<Owner>) {
            return false;
        } else {
            return std::meta::parent_of(member) == std::meta::dealias(^^Owner);
        }
    }

    // storm::meta::append_column_name, prefixed with join_base_alias_prefix when `member`
    // belongs to `Owner` (#785). With Owner = void it is exactly append_column_name.
    template <typename Owner> consteval void append_owned_column_name(auto& buf, std::meta::info member) {
        if (is_column_of<Owner>(member)) {
            buf.append(join_base_alias_prefix);
        }
        storm::meta::append_column_name(buf, member);
    }

    // ============================================================================
    // ORDER BY Clause - variadic pack processing (no recursive specializations)
    // ============================================================================

    // Every field arg in the pack names exactly ONE column (#570). False only for an FK
    // whose target has a composite primary key: that member spreads over
    // "<member>_<part>" columns, so there is no single name to sort by — and the
    // multi-column form has no correct shape here either, since the ASC/DESC/COLLATE
    // suffix attaches once, after one column name. Expanding to a comma list would
    // silently apply the direction to the LAST part only: valid SQL, wrong order, no
    // diagnostic — the failure mode this file's INVARIANT comment already warns about.
    // Rejected rather than mis-emitted, matching #500/#511/#537.
    // Per-arg, so the modifier args (bool / Collate) short-circuit before the single-column
    // test runs at all: selector_info<Arg>() is constrained by AnySelector, and a bad Arg
    // there maps to `false` rather than hard-erroring (verified — see ValidSelector's
    // comment, fields.cppm) — but skipping it for a non-field Arg is still clearer than
    // relying on that. The single-column test itself is the shared
    // storm::meta::SingleColumnSelector (fields.cppm), consolidated with COUNT(DISTINCT)'s
    // gate (#613); this concept just ORs in the modifier-arg escape on top.
    template <auto Arg>
    concept OrderByArgIsSingleColumn = !detail::is_field_arg<Arg>() || storm::meta::SingleColumnSelector<Arg>;

    template <auto... Args>
    concept OrderBySelectorsAreSingleColumn = (OrderByArgIsSingleColumn<Args> && ...);

    template <auto... Args>
        requires OrderBySelectorsAreSingleColumn<Args...>
    struct OrderByClause {
        // Count field args (skip bool and Collate modifiers)
        static constexpr std::size_t count = ((detail::is_field_arg<Args>() ? 1 : 0) + ... + 0);

        static constexpr auto fields = [] consteval // NOSONAR(cpp:S1659)
                -> std::array<detail::OrderByField, count> {
            std::array<detail::OrderByField, count> result{};
            std::size_t                             idx = 0;
            (detail::process_order_by_arg<count, Args>(result, idx), ...);
            return result;
        }();

        static constexpr auto empty() -> bool {
            return count == 0;
        }

        // Generate ORDER BY SQL fragment at compile-time
        template <std::size_t BufferSize = buffer_size::SQL_MEDIUM>
        static consteval auto to_sql() -> ConstexprString<BufferSize> {
            ConstexprString<BufferSize> result;

            if constexpr (count == 0) {
                return result;
            }

            result.append(" ORDER BY ");

            for (std::size_t i = 0; i < count; ++i) {
                if (i > 0) {
                    result.append(", ");
                }

                constexpr auto field_info = fields[i].field;
                // #570: the ORDER BY column, not the member identifier — an FK member
                // `sender` sorts by the column `sender_id`. column_name_view is sized
                // from column_name_size, so the FK suffix can never overflow silently.
                result.append(storm::meta::column_name_view<field_info>);

                constexpr auto collation = fields[i].col;
                result.append(collate_to_sql(collation));

                constexpr bool ascending = fields[i].asc;
                if (ascending) {
                    result.append(" ASC");
                } else {
                    result.append(" DESC");
                }
            }

            return result;
        }

        // Generate ORDER BY SQL fragment at runtime. A non-void Owner (the join's base
        // model, #785) prefixes that model's columns with join_base_alias_prefix; the
        // default void Owner emits the bare non-join spelling.
        template <typename Owner = void> static auto to_sql_runtime() -> std::string {
            if constexpr (count == 0) {
                return "";
            }

            std::string result = " ORDER BY ";

            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                ((append_field<Is, Owner>(result, Is > 0)), ...);
            }(std::make_index_sequence<count>{});

            return result;
        }

      private:
        template <std::size_t I, typename Owner> static auto append_field(std::string& result, bool add_comma) -> void {
            if (add_comma) {
                result += ", ";
            }

            constexpr auto field_info = fields[I].field;
            if constexpr (is_column_of<Owner>(field_info)) {
                result += join_base_alias_prefix;
            }
            // #570: same derivation as the consteval writer above, so the two can
            // never disagree. column_name_view is a view over static storage, so no
            // temporary std::string is built here.
            result += storm::meta::column_name_view<field_info>;

            constexpr auto collation = fields[I].col;
            result += collate_to_sql(collation);

            constexpr bool ascending = fields[I].asc;
            result += ascending ? " ASC" : " DESC";
        }
    };

    // ============================================================================
    // ORDER BY Wrapper (following JoinStatementWrapper pattern)
    // ============================================================================
    // Lightweight type-erased wrapper for ORDER BY clauses
    // Uses function pointer to static compile-time generated SQL (zero heap allocation)
    struct OrderByWrapper {
        auto (*get_order_by_sql_fn)() -> const std::string&;
        // The same clause with the base model's columns written "t1.<col>", for the
        // FK / reverse-FK / m2m join SQL whose base table is aliased t1 (#785).
        auto (*get_join_order_by_sql_fn)() -> const std::string&;

        [[nodiscard]] auto get_order_by_sql() const -> const std::string& {
            return get_order_by_sql_fn();
        }

        [[nodiscard]] auto get_join_order_by_sql() const -> const std::string& {
            return get_join_order_by_sql_fn();
        }

        [[nodiscard]] auto empty() const -> bool {
            return get_order_by_sql().empty();
        }
    };

    // Helper to create OrderByWrapper from template args
    // Each unique Model + Args... combination gets its own static SQL strings. Model is
    // the query's base model: its columns are the ones the join form qualifies (#785).
    template <typename Model, auto... Args> inline auto make_order_by_wrapper() -> OrderByWrapper {
        // Static compile-time generated SQL (one instance per unique Args... combination)
        // Stored in .rodata section (no heap allocation)
        static const std::string order_by_sql      = OrderByClause<Args...>::to_sql_runtime();
        static const std::string join_order_by_sql = OrderByClause<Args...>::template to_sql_runtime<Model>();

        return OrderByWrapper{
                .get_order_by_sql_fn      = []() -> const std::string& { return order_by_sql; },
                .get_join_order_by_sql_fn = []() -> const std::string& { return join_order_by_sql; }
        };
    }

} // namespace storm::orm::statements
