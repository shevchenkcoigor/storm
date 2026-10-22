module;

// Compile-time UPDATE SQL grammar (#434): the consteval "how UPDATE SQL is spelled" helpers,
// split out of UpdateStatement so that class stays cohesive and under the method threshold
// (cpp:S1448). Stateless and connection-free — every member derives purely from reflection
// over the model type T (via Base = BaseStatement<T>).

#include <meta>

export module storm_orm_statements_update_grammar;

import std;

import storm_orm_statements_base;
import storm_orm_statements_field_names; // pk_where_clause_size / append_pk_where_clause (#501)
import storm_orm_utilities;

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    template <typename T> struct UpdateGrammar {
        using Base = BaseStatement<T>;

        // Helper to build field assignments string for UPDATE SQL
        static consteval auto build_field_assignments() {
            // Iterate the relation-filtered persisted members (#485): m2m/reverse_fk
            // containers are not columns, so they must not appear in the SET clause.
            // Base::all_members_ already excludes them and matches the bind order used
            // by inline_bind_all_fields.
            //
            // EVERY primary-key member is skipped, not just primary_key_ (#501): on a
            // composite model the old `member != primary_key_` test left the other parts
            // in the SET list, so the statement would rewrite part of the key it matches
            // on. Identical to the old test for a single-PK model.
            ConstexprString<utilities::buffer_size::SQL_MEDIUM> result;
            bool                                                first = true;

            for (const auto& member : Base::all_members_) {
                if (!Base::is_pk_member(member)) {
                    if (!first) {
                        result.append(", ");
                    }
                    // "<col>[_id]=?" — column-name writer (#422) emits the FK "_id" suffix.
                    meta::append_column_name(result, member);
                    result.append("=?");
                    first = false;
                }
            }

            return result;
        }

        static constexpr auto field_assignments_ = build_field_assignments();

        // --- Conditional bulk UPDATE (#403): SET-clause built from explicit member NTTPs ---

        // Index of a member info within Base::all_members_ (compile-time).
        static consteval auto index_of_member(std::meta::info member) -> std::size_t {
            for (std::size_t i = 0; i < Base::all_members_.size(); ++i) {
                if (Base::all_members_[i] == member) {
                    return i;
                }
            }
            std::unreachable(); // guarded by is_settable_member() at the call site
        }

        // Each SET target must be a non-static data member of T, not ANY part of the
        // primary key, and not a relation container (#486) — m2m / reverse_fk members are
        // not persisted columns, so setting them would emit a non-existent column.
        //
        // The PK test is is_pk_member, not `Member != primary_key_` (#501): the latter
        // excludes only the FIRST part, so on a composite model every other part stayed a
        // legal SET target and `SET b=? WHERE a=? AND b=?` would rewrite the key being
        // matched on. Unchanged for a single-PK model.
        template <std::meta::info Member> static consteval auto is_settable_member() -> bool {
            return std::meta::is_nonstatic_data_member(Member) && !Base::is_pk_member(Member) &&
                   !meta::is_relation_field(Member);
        }

        // Append "<name>=?" (or "<name>_id=?" for FK fields) for one member.
        template <typename Buf> static consteval auto append_one_assignment(Buf& buf, std::meta::info member) -> void {
            meta::append_column_name(buf, member); // #422 — emits the FK "_id" suffix
            buf.append("=?");
        }

        // True when `member` carries the auto_update timestamp annotation (#209).
        // Forwards to the storm_orm_field_attr leaf (#421) — the single source of
        // truth — since BaseStatement::is_auto_update_field is protected and
        // UpdateGrammar does not inherit from BaseStatement.
        static consteval auto is_auto_update_field(std::meta::info member) -> bool {
            return meta::is_auto_update(member);
        }

        // Is `member` an auto_update field NOT already present in the explicit pack?
        //
        // Excludes primary-key members exactly as is_settable_member above does (#511):
        // without that gate an auto_update PK was appended to the SET clause and stamped
        // now(), rewriting the key the statement matches on. ModelTimestampPkValid<T>
        // rejects such a model outright, so this gate is unreachable for any model that
        // instantiates a statement — it is kept as defence in depth, so the SET clause
        // cannot emit a key column if that concept is ever loosened. is_pk_member covers
        // composite primary_part members too (#500).
        template <std::meta::info... Members>
        static consteval auto is_unlisted_auto_update(std::meta::info member) -> bool {
            return is_auto_update_field(member) && !Base::is_pk_member(member) && ((member != Members) && ...);
        }

        // Build the SET clause for the explicit Members... pack, then append any
        // auto_update field of T that the caller did not list. The column ORDER
        // here is the canonical bind order used by bind_conditional_set().
        template <std::meta::info... Members> static consteval auto build_conditional_set_clause() {
            ConstexprString<utilities::buffer_size::SQL_MEDIUM> result;
            bool                                                first = true;
            // (1) explicit members, in the given order
            (
                    [&] {
                        if (!first) {
                            result.append(", ");
                        }
                        append_one_assignment(result, Members);
                        first = false;
                    }(),
                    ...
            );
            // (2) auto_update fields not already listed (stamped now() at bind time)
            for (const auto& member : Base::all_members_) {
                if (is_unlisted_auto_update<Members...>(member)) {
                    if (!first) {
                        result.append(", ");
                    }
                    // Same writer as the explicit-pack branch above (#542). The
                    // divergence is unreachable — ValidTimestampField bars an FK from
                    // carrying a timestamp — but sharing the writer (#422) makes that
                    // structural rather than coincidental.
                    append_one_assignment(result, member);
                    first = false;
                }
            }
            return result;
        }

        // Compile-time UPDATE SQL size calculation. The key clause is "<a> = ? AND <b> = ?"
        // — one column per PK part (#501), which for a single PK is the same " <pk> = ?"
        // this used to hardcode. Exact: ConstexprString truncates SILENTLY on overflow, so
        // an under-count would emit wrong SQL with no diagnostic.
        static consteval auto calculate_update_sql_size() -> std::size_t {
            using utilities::sql_len::SET;
            using utilities::sql_len::UPDATE;
            using utilities::sql_len::WHERE;
            std::size_t size = 0;
            size += UPDATE; // "UPDATE "
            size += Base::table_name_.size();
            size += SET; // " SET "
            size += field_assignments_.len;
            size += WHERE; // " WHERE "
            size += pk_where_clause_size(Base::primary_key_members_);
            size += 1; // null terminator
            return size;
        }

        // Build UPDATE SQL at compile-time using ConstexprString.
        // Single PK  → "UPDATE t SET a=?, b=? WHERE id = ?"   (byte-identical to pre-#501)
        // Composite  → "UPDATE t SET c=? WHERE a = ? AND b = ?"
        static consteval auto build_update_sql_array() {
            constexpr std::size_t     sql_size = calculate_update_sql_size() + utilities::sql_len::LARGE_BUFFER;
            ConstexprString<sql_size> result;

            result.append("UPDATE ");
            result.append(Base::table_name_);
            result.append(" SET ");
            result.append(std::string_view(field_assignments_.data.data(), field_assignments_.len));
            result.append(" WHERE ");
            append_pk_where_clause(result, Base::primary_key_members_);

            return result;
        }

        // Pre-computed UPDATE SQL generated at compile-time
        static constexpr auto           update_sql_array  = build_update_sql_array();
        static inline const std::string update_sql_string = std::string(update_sql_array);
    };

} // namespace storm::orm::statements
