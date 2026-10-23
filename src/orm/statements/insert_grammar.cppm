module;

// Compile-time INSERT SQL grammar (#671): the consteval/static "how INSERT SQL is
// spelled" helpers — single-row INSERT [RETURNING], the bulk prefix and VALUES body,
// the RETURNING suffix and the placeholder count — split out of InsertStatement so
// that class keeps only caching, binding and execution. Stateless and connection-free:
// every member derives purely from reflection over the model type T (via
// Base = BaseStatement<T>). UpsertGrammar reuses the non-RETURNING INSERT head
// (insert_sql_array) instead of re-deriving it.

#include <meta>

export module storm_orm_statements_insert_grammar;

import std;

import storm_orm_statements_base;
import storm_orm_statements_field_names;
import storm_orm_utilities;

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    template <typename T> struct InsertGrammar {
        using Base = BaseStatement<T>;

        // Pre-computed placeholders (excludes PK for auto-increment). Shared with
        // UpsertGrammar through insert_sql_array (#205, #671).
        static constexpr auto placeholders_ = FieldNameGrammar<Base>::build_placeholders();

        // Compile-time SQL size calculation
        static consteval auto calculate_insert_sql_size() -> std::size_t {
            using utilities::sql_len::INSERT_INTO;
            using utilities::sql_len::VALUES_OPEN;
            std::size_t size = 0;
            size += INSERT_INTO; // "INSERT INTO "

            // Table name length
            size += Base::table_name_.size();

            size += 2; // " ("

            // Field names length
            size += FieldNameGrammar<Base>::calculate_field_names_size();

            size += VALUES_OPEN; // ") VALUES ("

            // Placeholders length
            size += placeholders_.len;

            size += 1; // ")"
            size += 1; // null terminator

            return size;
        }

        // Shared compile-time INSERT SQL builder for the RETURNING and non-RETURNING variants.
        // The +64 padding on the RETURNING path covers " RETURNING <pk_name>".
        template <bool WithReturning> static consteval auto build_insert_sql_array_impl() {
            constexpr std::size_t extra =
                    WithReturning ? (utilities::sql_len::XL_BUFFER + 64) : utilities::sql_len::XL_BUFFER;
            constexpr std::size_t     sql_size = calculate_insert_sql_size() + extra;
            ConstexprString<sql_size> result;

            result.append("INSERT INTO ");
            result.append(Base::table_name_);
            result.append(" (");
            result.append(FieldNameGrammar<Base>::build_non_pk_field_names_list());
            result.append(") VALUES (");
            result.append(placeholders_);
            if constexpr (WithReturning) {
                result.append(") RETURNING ");
                result.append(Base::pk_name_);
            } else {
                result.append(")");
            }

            return result;
        }

        // Pre-computed INSERT ... RETURNING SQL (both SQLite 3.35+ and PostgreSQL)
        static constexpr auto           insert_returning_sql_array  = build_insert_sql_array_impl<true>();
        static inline const std::string insert_returning_sql_string = std::string(insert_returning_sql_array);

        // Pre-computed INSERT SQL without RETURNING (faster path when ID not needed)
        static constexpr auto           insert_sql_array  = build_insert_sql_array_impl<false>();
        static inline const std::string insert_sql_string = std::string(insert_sql_array);

        // Compile-time bulk INSERT prefix calculation
        static consteval auto calculate_bulk_insert_prefix_size() -> std::size_t {
            using utilities::sql_len::INSERT_INTO;
            using utilities::sql_len::VALUES_OPEN;
            std::size_t size = 0;
            size += INSERT_INTO; // "INSERT INTO "
            size += Base::table_name_.size();
            size += 2; // " ("
            size += FieldNameGrammar<Base>::calculate_field_names_size();
            size += VALUES_OPEN; // ") VALUES "
            size += 1;           // null terminator
            return size;
        }

        // Build bulk INSERT prefix at compile-time using ConstexprString
        static consteval auto build_bulk_insert_prefix() {
            constexpr std::size_t prefix_size = calculate_bulk_insert_prefix_size() + utilities::sql_len::LARGE_BUFFER;
            ConstexprString<prefix_size> result;

            result.append("INSERT INTO ");
            result.append(Base::table_name_);
            result.append(" (");
            result.append(FieldNameGrammar<Base>::build_non_pk_field_names_list());
            result.append(") VALUES ");

            return result;
        }

        // Pre-computed bulk INSERT prefix generated at compile-time
        static constexpr auto           bulk_insert_prefix_array = build_bulk_insert_prefix();
        static inline const std::string bulk_insert_prefix       = std::string(bulk_insert_prefix_array);
        static constexpr std::size_t    bulk_insert_prefix_size =
                calculate_bulk_insert_prefix_size() - 1; // Exclude null terminator

        // Pre-computed RETURNING suffix: " RETURNING <pk_name>"
        static constexpr auto returning_suffix_array = [] consteval {
            ConstexprString<64> result;
            result.append(" RETURNING ");
            result.append(Base::pk_name_);
            return result;
        }();
        static inline const std::string returning_suffix = std::string(returning_suffix_array);

        // Build bulk INSERT SQL body (shared by both returning and non-returning variants)
        static __attribute__((cold)) auto build_bulk_insert_body(std::size_t count) -> std::string {
            // Guard count == 0: (count - 1) below wraps to SIZE_MAX, and an empty
            // VALUES list is invalid SQL anyway. Return the bare prefix. See #359.
            if (count == 0) {
                return bulk_insert_prefix;
            }

            std::string value_template = "(";
            value_template += placeholders_;
            value_template += ")";

            const std::size_t value_size     = value_template.size();
            const std::size_t separator_size = 2; // ", "
            const std::size_t total_size =
                    bulk_insert_prefix_size + (value_size * count) + (separator_size * (count - 1));

            std::string sql;
            sql.reserve(total_size);

            sql = bulk_insert_prefix;
            for (std::size_t i = 0; i < count; ++i) {
                if (i > 0) {
                    sql += ", ";
                }
                sql += value_template;
            }
            return sql;
        }

        // The number of VALUES placeholders in a plain INSERT. Used by the DO UPDATE
        // upsert path to find where the auto_update now() tail starts binding (right
        // after the VALUES params).
        //
        // Delegated to the grammar rather than re-derived here: this must equal exactly
        // what build_placeholders() WROTE. The former arithmetic form
        // (field_count_ - 1, special-cased per key shape) went stale every time a new
        // "the key is caller data" shape appeared — composite (#502), then UUID (#565) —
        // binding the tail over the last VALUES slot and leaving one parameter unbound.
        // PG rejects that outright ("bind message supplies 3 parameters, but prepared
        // statement requires 4"); SQLite binds NULL and silently writes the auto_update
        // column NULL. Counting through the writer's own iterator also fixes the latent
        // undercount for a composite-FK member, which contributes N placeholders.
        static consteval auto placeholders_count() -> std::size_t {
            return FieldNameGrammar<Base>::placeholder_count();
        }
    };

} // namespace storm::orm::statements
