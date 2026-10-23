module;

// Compile-time UPSERT (ON CONFLICT) SQL grammar (#205): the consteval helpers
// that spell the conflict-target list and the `excluded.col` DO UPDATE SET
// clause, split out of InsertStatement to keep that class cohesive. Stateless
// and connection-free — every member derives purely from reflection over T.

#include <meta>

export module storm_orm_statements_upsert_grammar;

import std;

import storm_orm_statements_base;
import storm_orm_statements_insert_grammar;
import storm_orm_utilities;
import storm_orm_indexes;
import storm_orm_field_attr;

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    template <typename T> struct UpsertGrammar {
        using Base = BaseStatement<T>;

        // Append ", " before every element after the first.
        template <typename Buf> static consteval auto append_separator(Buf& buf, bool& first) -> void {
            if (!first) {
                buf.append(", ");
            }
            first = false;
        }

        // (col1, col2, ...) — conflict-target column list, FK "_id"-aware (#422).
        template <std::meta::info... Target> static consteval auto build_conflict_target() {
            ConstexprString<utilities::buffer_size::SQL_SMALL> result;
            result.append("(");
            bool first = true;
            (
                    [&] {
                        append_separator(result, first);
                        meta::append_column_name(result, Target);
                    }(),
                    ...
            );
            result.append(")");
            return result;
        }

        // Each SET target must be a non-static data member of T, not ANY part of the PK,
        // and not a relation container (#486) — m2m / reverse_fk members are not
        // persisted columns, so "col=excluded.col" would reference a missing column.
        //
        // The PK test is is_pk_member, not `Member != primary_key_` (#501): the latter
        // excludes only the FIRST part, leaving every other part of a composite key a
        // legal DO UPDATE target. Unchanged for a single-PK model. (ON CONFLICT targeting
        // a composite key is #503; keeping the key parts out of the SET list is the half
        // that belongs here.)
        template <std::meta::info Member> static consteval auto is_settable_member() -> bool {
            return std::meta::is_nonstatic_data_member(Member) && !Base::is_pk_member(Member) &&
                   !meta::is_relation_field(Member);
        }

        // True when `member` carries auto_update (#209), is not a primary-key member, and
        // is NOT in the explicit pack. The PK gate mirrors is_settable_member above and the
        // UpdateGrammar twin (#511) — an auto_update key would otherwise be stamped now()
        // in the DO UPDATE tail, rewriting the key the conflict target matched on.
        // ModelTimestampPkValid<T> already rejects such a model; this is defence in depth.
        template <std::meta::info... SetCols>
        static consteval auto is_unlisted_auto_update(std::meta::info member) -> bool {
            return meta::is_auto_update(member) && !Base::is_pk_member(member) && ((member != SetCols) && ...);
        }

        // "col=excluded.col, ..." for the explicit SetCols pack, then any
        // auto_update field of T not already listed, appended as "col=?" (bound
        // now() at execution). The column ORDER of the auto_update tail is the
        // canonical bind order used by the execute path.
        template <std::meta::info... SetCols> static consteval auto build_excluded_set_clause() {
            ConstexprString<utilities::buffer_size::SQL_MEDIUM> result;
            bool                                                first = true;
            (
                    [&] {
                        append_separator(result, first);
                        meta::append_column_name(result, SetCols);
                        result.append("=excluded.");
                        meta::append_column_name(result, SetCols);
                    }(),
                    ...
            );
            for (const auto& member : Base::all_members_) {
                if (is_unlisted_auto_update<SetCols...>(member)) {
                    append_separator(result, first);
                    // Canonical column-name writer (#422), the same spelling the
                    // explicit-pack branch above emits (no shared helper: that branch
                    // writes "col=excluded.col"). The divergence is unreachable —
                    // ValidTimestampField bars an FK from carrying a timestamp — but
                    // this keeps the agreement structural (#542).
                    meta::append_column_name(result, member);
                    result.append("=?");
                }
            }
            return result;
        }

        // Full "INSERT ... ON CONFLICT (...) DO NOTHING [RETURNING <pk>]" statement.
        // RETURNING is emitted only for a key the DATABASE generates: a composite
        // key (#502/#503) and a storm::UUID key (#572) are both caller data, so
        // there is nothing for the clause to echo back and it is omitted
        // entirely. Single-integer-PK text is unchanged. The gate is
        // Base::pk_is_db_generated_(), shared with the runner in insert.cppm that
        // decides whether to READ a returned row — the two must agree.
        //
        // The "INSERT INTO <table> (<cols>) VALUES (<placeholders>)" head is
        // InsertGrammar's constexpr insert_sql_array (#671) — the exact text a plain
        // INSERT emits, read from a constant expression rather than re-derived here
        // or taken from a dynamically-initialised std::string static.
        template <std::meta::info... Target> static auto nothing_sql() -> const std::string& {
            static const std::string sql = [] {
                std::string out(InsertGrammar<T>::insert_sql_array);
                out += " ON CONFLICT ";
                out += std::string(build_conflict_target<Target...>());
                out += " DO NOTHING";
                if constexpr (Base::pk_is_db_generated_()) {
                    out += " RETURNING ";
                    out += std::string(Base::pk_name_);
                }
                return out;
            }();
            return sql;
        }

        // Full "INSERT ... ON CONFLICT (...) DO UPDATE SET <set_clause> [RETURNING <pk>]"
        // statement. The SET clause is passed in as a precomputed value (built by
        // build_excluded_set_clause<SetCols...>()) because Target... and SetCols...
        // cannot share one template parameter list. RETURNING is omitted whenever
        // the key is caller data — same reasoning as nothing_sql() above.
        template <std::meta::info... Target, std::size_t N>
        static auto update_sql(const ConstexprString<N>& set_clause) -> std::string {
            std::string out(InsertGrammar<T>::insert_sql_array);
            out += " ON CONFLICT ";
            out += std::string(build_conflict_target<Target...>());
            out += " DO UPDATE SET ";
            out += std::string(set_clause);
            if constexpr (Base::pk_is_db_generated_()) {
                out += " RETURNING ";
                out += std::string(Base::pk_name_);
            }
            return out;
        }
    };

    // True if the two field lists are identical element-wise. Conflict-target
    // columns must be supplied in the same declared order as the matching
    // UniqueIndex. This is a v1 simplification; set-comparison (order-insensitive
    // matching) can be added later if needed.
    consteval auto fields_equal_ordered(const auto& lhs, const auto& rhs) -> bool {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (lhs[i] != rhs[i]) {
                return false;
            }
        }
        return true;
    }

    // True if the Target... column set exactly matches some UniqueIndex<...> in Indexes<T>.
    template <typename T, std::meta::info... Target> consteval auto target_matches_unique_index() -> bool {
        constexpr std::array targets{Target...};
        bool                 matched = false;
        [&]<typename... Idx>(std::tuple<Idx...>*) {
            (
                    [&] {
                        if constexpr (Idx::unique) {
                            if (fields_equal_ordered(Idx::fields, targets)) {
                                matched = true;
                            }
                        }
                    }(),
                    ...
            );
        }(static_cast<storm::indexes_t<T>*>(nullptr));
        return matched;
    }

    // True if the Target... column set is exactly T's primary key, in declaration
    // order (#503/#585). A PK is a unique constraint over its columns, so the full
    // set is a valid conflict target — but a STRICT SUBSET is not unique on its own
    // (order-sensitive match, same as target_matches_unique_index, so a partial or
    // reordered target is correctly rejected).
    //
    // Gated on !pk_is_db_generated_(), i.e. "does INSERT actually EMIT this key",
    // NOT on has_composite_pk_. A key the DATABASE generates is absent from the
    // statement, so it can never collide and must stay rejected — true of a
    // single-column INTEGER key. A key that is CALLER DATA is in the VALUES list
    // and a duplicate really does raise a PK violation, so ON CONFLICT over it is
    // meaningful: that covers a composite key (#503) and, since #565 put the UUID
    // key column back in the INSERT, a single-column storm::UUID key too (#585).
    // The pre-#585 has_composite_pk_ gate rejected the UUID case as well, whose
    // stated reason ("INSERT omits it") had silently stopped being true for it.
    template <typename T, std::meta::info... Target> consteval auto target_matches_primary_key() -> bool {
        if constexpr (BaseStatement<T>::pk_is_db_generated_()) {
            return false;
        } else {
            constexpr std::array targets{Target...};
            return fields_equal_ordered(BaseStatement<T>::primary_key_members_, targets);
        }
    }

    // A valid conflict target: every Target is a data member of T, AND either a
    // single storm::unique field, a matching UniqueIndex<...>, or the full
    // primary-key column set of a key INSERT emits (#503/#585). A DB-generated
    // single-column PK is omitted from the statement, so it alone cannot form a
    // useful conflict target — that case is unaffected here.
    template <typename T, std::meta::info... Target>
    concept ConflictTargetUnique =
            ((std::meta::is_nonstatic_data_member(Target)) && ...) &&
            ((sizeof...(Target) == 1 && (storm::meta::is_unique(Target) && ...)) ||
             target_matches_unique_index<T, Target...>() || target_matches_primary_key<T, Target...>());

    // A valid SET target: a non-static data member of T that is not the PK.
    template <typename T, std::meta::info... SetCols>
    concept UpsertSettable = (UpsertGrammar<T>::template is_settable_member<SetCols>() && ...);

} // namespace storm::orm::statements
