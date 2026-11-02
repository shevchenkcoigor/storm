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

        // Generate ORDER BY SQL fragment at runtime
        static auto to_sql_runtime() -> std::string {
            if constexpr (count == 0) {
                return "";
            }

            std::string result = " ORDER BY ";

            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                ((append_field<Is>(result, Is > 0)), ...);
            }(std::make_index_sequence<count>{});

            return result;
        }

      private:
        template <std::size_t I> static auto append_field(std::string& result, bool add_comma) -> void {
            if (add_comma) {
                result += ", ";
            }

            constexpr auto field_info = fields[I].field;
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

        [[nodiscard]] auto get_order_by_sql() const -> const std::string& {
            return get_order_by_sql_fn();
        }

        [[nodiscard]] auto empty() const -> bool {
            return get_order_by_sql().empty();
        }
    };

    // Helper to create OrderByWrapper from template args
    // Each unique Args... combination gets its own static SQL string
    template <auto... Args> inline auto make_order_by_wrapper() -> OrderByWrapper {
        // Static compile-time generated SQL (one instance per unique Args... combination)
        // Stored in .rodata section (no heap allocation)
        static const std::string order_by_sql = OrderByClause<Args...>::to_sql_runtime();

        return OrderByWrapper{.get_order_by_sql_fn = []() -> const std::string& { return order_by_sql; }};
    }

} // namespace storm::orm::statements
