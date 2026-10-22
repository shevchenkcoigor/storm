module;

// Compile-time field-name SQL grammar (#434): builds the comma-separated column lists that
// INSERT and SELECT splice into their statements ("col1, col2, fk_id, ..."). Split out of
// BaseStatement so that class stays cohesive (cpp:S1448). Stateless — every member derives
// from the persisted-field reflection BaseStatement already computed (all_members_,
// primary_key_, field_count_), passed in as the Base template parameter.

#include <meta>

export module storm_orm_statements_field_names;

import std;

import storm_orm_field_attr;
import storm_orm_utilities;

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    template <typename Base> struct FieldNameGrammar {
        // How many SQL columns member at all_members_[Index] maps to: N for a
        // composite-FK member (one column per target PK part, #504), 1 for
        // everything else (a plain field, or a single-column FK — byte-identical
        // to the pre-#504 shape). Index-templated (not a runtime-loop value) so the
        // FK's inner type can be named as a template argument to
        // fk_primary_key_count — a runtime loop variable cannot splice into a
        // template parameter.
        template <std::size_t Index> static consteval auto column_count_of() -> std::size_t {
            constexpr auto member = Base::all_members_[Index];
            if constexpr (storm::meta::is_fk_field(member)) {
                using FKType = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
                return Base::template fk_primary_key_count<FKType>();
            } else {
                return 1;
            }
        }

        // Byte size of the column name(s) member at Index emits: the composite-aware
        // sibling of storm::meta::column_name_size, routed through fk_column_names_size
        // (Task 3) for an FK member — degenerates to column_name_size's exact output
        // for a single-column FK (the N == 1 branch there) and is untouched for a
        // non-FK member.
        template <std::size_t Index> static consteval auto column_names_size_of() -> std::size_t {
            constexpr auto member = Base::all_members_[Index];
            if constexpr (storm::meta::is_fk_field(member)) {
                using FKType           = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
                constexpr auto targets = Base::template find_fk_primary_key_members<FKType>();
                return storm::meta::fk_column_names_size(member, targets, ", ");
            } else {
                return storm::meta::column_name_size(member);
            }
        }

        // Append the column name(s) member at Index emits into buf (comma-joined
        // already, for a composite-FK member — see append_fk_column_names).
        template <std::size_t Index, typename Buf> static consteval void append_column_names_of(Buf& buf) {
            constexpr auto member = Base::all_members_[Index];
            if constexpr (storm::meta::is_fk_field(member)) {
                using FKType           = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
                constexpr auto targets = Base::template find_fk_primary_key_members<FKType>();
                storm::meta::append_fk_column_names(buf, member, targets, ", ");
            } else {
                storm::meta::append_column_name(buf, member);
            }
        }

        // Is all_members_[Index] the (single-column) PK skipped by INSERT? Mirrors
        // the pre-#504 SkipPrimaryKey test. INSERT omits the key column exactly when
        // the DATABASE generates it, which is what Base::pk_is_db_generated_() answers
        // for RETURNING too (#502/#507/#572) — one predicate, not a second spelling of
        // the same condition. A composite key has no auto-generation mechanism (#502),
        // and a storm::UUID key is the same case one shape earlier (#507):
        // AUTOINCREMENT and GENERATED ... AS IDENTITY are single-INTEGER-column
        // features, so a UUID key is always caller-supplied and must be INSERTed like
        // any other column. Skipping it emitted "INSERT INTO UuidDoc (title) VALUES
        // (?)" — the caller's key silently dropped, every row landing with a NULL id.
        // Kept in lockstep with BaseStatement::skips_pk_column, which makes the
        // identical decision for the BIND sequence; the two must agree or the columns
        // and values misalign.
        template <std::size_t Index> static consteval auto is_skipped_pk() -> bool {
            return Base::pk_is_db_generated_() && Base::all_members_[Index] == Base::primary_key_;
        }

        // Shared iterator over data members, honouring SkipPrimaryKey, invoking
        // `body.template operator()<Index>(needs_comma)` once per emitted MEMBER (not
        // per column — the body decides internally how many columns/placeholders a
        // composite-FK member needs, via column_count_of<Index>()). Index-sequence
        // fold rather than a runtime `for` loop: naming the FK's inner type needs
        // Index as a template argument, which a runtime loop variable cannot supply.
        template <bool SkipPrimaryKey, typename Body, std::size_t... Is>
        static consteval auto for_each_field_name_impl(Body body, std::index_sequence<Is...> /*unused*/) -> void {
            bool first = true;
            (
                    [&] {
                        if constexpr (SkipPrimaryKey) {
                            if (is_skipped_pk<Is>()) {
                                return;
                            }
                        }
                        body.template operator()<Is>(!first);
                        first = false;
                    }(),
                    ...
            );
        }

        template <bool SkipPrimaryKey, typename Body> static consteval auto for_each_field_name(Body body) -> void {
            for_each_field_name_impl<SkipPrimaryKey>(body, std::make_index_sequence<Base::field_count_>{});
        }

        // Unified field name size calculation at compile-time
        // Template parameter controls whether to skip primary key (for INSERT vs SELECT)
        template <bool SkipPrimaryKey> static consteval auto calculate_field_names_size_impl() -> std::size_t {
            std::size_t size = 0;
            for_each_field_name<SkipPrimaryKey>([&]<std::size_t Index>(bool needs_comma) {
                if (needs_comma) {
                    size += 2; // ", "
                }
                size += column_names_size_of<Index>();
            });
            return size;
        }

        // Calculate size of all field names string at compile-time
        static consteval auto calculate_field_names_size() -> std::size_t {
            return calculate_field_names_size_impl<false>();
        }

        // Calculate size of non-PK field names string at compile-time
        static consteval auto calculate_non_pk_field_names_size() -> std::size_t {
            return calculate_field_names_size_impl<true>();
        }

        // Unified field name list builder at compile-time
        // Template parameter controls whether to skip primary key (for INSERT vs SELECT)
        template <bool SkipPrimaryKey> static consteval auto build_field_names_list_impl() {
            constexpr std::size_t size = calculate_field_names_size_impl<SkipPrimaryKey>() + 10;
            ConstexprString<size> result;
            for_each_field_name<SkipPrimaryKey>([&]<std::size_t Index>(bool needs_comma) {
                if (needs_comma) {
                    result.append(", ");
                }
                append_column_names_of<Index>(result);
            });
            return result;
        }

        // Build comma-separated list of all field names (for SELECT statements)
        // FK fields are mapped to their column names (User sender → sender_id)
        static consteval auto build_all_field_names_list() {
            return build_field_names_list_impl<false>();
        }

        // Build the INSERT column list: excludes a DB-generated single-column PK
        // (auto-increment); a composite key is caller data, so all fields (#502).
        static consteval auto build_non_pk_field_names_list() {
            return build_field_names_list_impl<true>();
        }

        // "?, ?, ..." placeholders for the SQL VALUES clause, one per bound COLUMN
        // (a DB-generated single-column PK is skipped; a composite key is not,
        // #502; a composite-FK member contributes one "?" per target PK part,
        // #504, matching bind_fk_parts's per-part bind loop). Shared by
        // InsertStatement's VALUES clause and UpsertGrammar's re-derived INSERT
        // prefix (#205).
        static consteval auto build_placeholders() {
            ConstexprString<utilities::buffer_size::SQL_SMALL> result;
            for_each_field_name<true>([&]<std::size_t Index>(bool needs_comma) {
                bool              comma = needs_comma;
                const std::size_t cols  = column_count_of<Index>();
                for (std::size_t c = 0; c < cols; ++c) {
                    if (comma) {
                        result.append(", ");
                    }
                    result.append("?");
                    comma = true;
                }
            });
            return result;
        }

        // How many placeholders build_placeholders() above emits. Counted through the
        // SAME for_each_field_name<true> + column_count_of<Index> pair that writes them,
        // so the number and the text cannot drift — which is the whole point: the upsert
        // DO UPDATE path offsets its auto_update now() tail past the VALUES parameters
        // by this count, and re-deriving it arithmetically (field_count_ - 1, adjusted
        // per key shape) went stale every time a new "the key is caller data" shape
        // appeared — composite (#502), then UUID (#565) — binding the tail over the last
        // VALUES slot and leaving one parameter unbound. It also counts a composite-FK
        // member's N columns, which the arithmetic form never did.
        static consteval auto placeholder_count() -> std::size_t {
            std::size_t count = 0;
            for_each_field_name<true>([&]<std::size_t Index>(bool /*needs_comma*/) {
                count += column_count_of<Index>();
            });
            return count;
        }
    };

    // ── Primary-key WHERE clause (#501) ──────────────────────────────────────────
    // The by-key WHERE that UPDATE-by-object and DELETE-by-object share. For a single
    // PK this emits exactly the "<pk> = ?" the two statements spelled inline before, so
    // single-PK SQL stays byte-identical; for a composite key it AND-joins every part in
    // declaration order — the same order pk_members binds in.
    //
    // Free function templates over the PK-member array rather than BaseStatement methods:
    // SchemaStatement sits at the cpp:S1448 35-method ceiling and BaseStatement is the
    // class both statements inherit, so anything added there is paid by every statement
    // type. Taking the array as a parameter also lets erase.cppm and update_grammar.cppm
    // call these without either one depending on the other.

    // Byte length of "<a> = ? AND <b> = ?" for the given PK members. Exact — both callers
    // size a ConstexprString with it, and ConstexprString truncates SILENTLY on overflow,
    // so an under-count is a wrong-SQL bug with no diagnostic. Column names go through
    // the canonical writer (#422) so an FK part is measured as "<name>_id".
    template <std::size_t N>
    consteval auto pk_where_clause_size(const std::array<std::meta::info, N>& pk_members) -> std::size_t {
        constexpr std::size_t EQUALS_PLACEHOLDER = 4; // " = ?"
        constexpr std::size_t AND_JOIN           = 5; // " AND "
        std::size_t           size               = 0;
        for (const std::meta::info member : pk_members) {
            size += storm::meta::column_name_size(member) + EQUALS_PLACEHOLDER;
        }
        return size + (AND_JOIN * (N - 1));
    }

    // Append "<a> = ? AND <b> = ?" for the given PK members, in declaration order.
    template <typename Buf, std::size_t N>
    consteval auto append_pk_where_clause(Buf& buf, const std::array<std::meta::info, N>& pk_members) -> void {
        bool first = true;
        for (const std::meta::info member : pk_members) {
            if (!first) {
                buf.append(" AND ");
            }
            storm::meta::append_column_name(buf, member); // #422 — FK parts emit "<name>_id"
            buf.append(" = ?");
            first = false;
        }
    }
} // namespace storm::orm::statements
