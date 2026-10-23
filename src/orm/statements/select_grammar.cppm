module;

// Compile-time SELECT SQL grammar (#671): the consteval "how the base SELECT is
// spelled" helpers — the full-column SELECT ... FROM <table> text and its LIMIT 1 /
// LIMIT 2 fast-path variants — split out of SelectStatement so that class keeps only
// the runtime, ConnType-dependent query building and execution. Stateless and
// connection-free: every member derives purely from reflection over the model type T
// (via Base = BaseStatement<T>).

#include <meta>

export module storm_orm_statements_select_grammar;

import std;

import storm_orm_statements_base;
import storm_orm_statements_field_names;
import storm_orm_utilities;

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    template <typename T> struct SelectGrammar {
        using Base = BaseStatement<T>;

        // Compile-time SQL size calculation for SELECT statement
        static consteval auto calculate_select_sql_size() -> std::size_t {
            using utilities::sql_len::FROM;
            using utilities::sql_len::SELECT;
            std::size_t size = 0;
            size += SELECT; // "SELECT "
            size += FieldNameGrammar<Base>::calculate_field_names_size();
            size += FROM; // " FROM "
            size += Base::table_name_.size();
            size += 1; // null terminator
            return size;
        }

        // Build SELECT SQL at compile-time using ConstexprString
        static consteval auto build_select_sql_array() {
            constexpr std::size_t     sql_size = calculate_select_sql_size() + utilities::sql_len::LARGE_BUFFER;
            ConstexprString<sql_size> result;

            result.append("SELECT ");
            result.append(FieldNameGrammar<Base>::build_all_field_names_list());
            result.append(" FROM ");
            result.append(Base::table_name_);

            return result;
        }

        // Pre-computed SELECT SQL generated at compile-time
        static constexpr auto           select_sql_array  = build_select_sql_array();
        static inline const std::string select_sql_string = std::string(select_sql_array);

        // Pre-computed SELECT ... LIMIT 1 SQL for first() fast path
        static inline const std::string select_limit1_sql_string = std::string(select_sql_array) + " LIMIT 1";

        // Pre-computed SELECT ... LIMIT 2 SQL for get() fast path
        static inline const std::string select_limit2_sql_string = std::string(select_sql_array) + " LIMIT 2";
    };

} // namespace storm::orm::statements
