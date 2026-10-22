module;

// Compile-time DELETE SQL grammar (#501): the consteval "how DELETE SQL is spelled"
// helpers, split out of EraseStatement so that class stays cohesive and the file stays
// under the size threshold — the same split update_grammar.cppm got in #434. Stateless
// and connection-free: every member derives purely from reflection over the model type T
// (via Base = BaseStatement<T>).
//
// The composite-PK widening (#501) lives here. Both DELETE shapes are written once in
// terms of PK_COLUMNS, so a single-PK model produces byte-identical SQL to what
// EraseStatement emitted before composite keys existed:
//
//   single row, 1 PK   DELETE FROM t WHERE id = ?
//   single row, N PKs  DELETE FROM t WHERE a = ? AND b = ?
//   bulk,       1 PK   DELETE FROM t WHERE id IN (?,?,?)
//   bulk,       N PKs  DELETE FROM t WHERE (a, b) IN ((?,?),(?,?),(?,?))
//
// The bulk composite form is a row-value IN list. Row values are standard SQL:
// PostgreSQL has always accepted them, SQLite since 3.15 — well under this project's
// 3.35 floor. The per-column alternative "a IN (...) AND b IN (...)" is WRONG: it
// matches the cross product of the parts, so it would delete keys never listed.

#include <meta>

export module storm_orm_statements_erase_grammar;

import std;

import storm_orm_field_attr; // column_name_size / append_column_name (#422)
import storm_orm_statements_base;
import storm_orm_statements_field_names; // pk_where_clause_size / append_pk_where_clause (#501)
import storm_orm_utilities;

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    template <typename T> struct EraseGrammar {
        using Base = BaseStatement<T>;

        // Number of columns in the primary key: 1 for a single PK, N for a composite one.
        // Every size and placeholder calculation below is expressed in terms of it, so the
        // single-PK arithmetic falls out unchanged at PK_COLUMNS == 1.
        static constexpr std::size_t PK_COLUMNS = Base::primary_key_members_.size();

        // Common prefix: "DELETE FROM <table> WHERE ". The single-row and bulk builders
        // differ only in what follows (the AND-joined key clause vs the IN opener).
        static consteval auto delete_prefix_size() -> std::size_t {
            using utilities::sql_len::DELETE_FROM;
            using utilities::sql_len::WHERE;
            return DELETE_FROM + Base::table_name_.size() + WHERE;
        }

        template <typename Buf> static consteval auto append_delete_prefix(Buf& buf) -> void {
            buf.append("DELETE FROM ");
            buf.append(Base::table_name_);
            buf.append(" WHERE ");
        }

        // --- single-row DELETE ------------------------------------------------

        // Exact size of "DELETE FROM <t> WHERE <a> = ? AND <b> = ?". Exactness matters:
        // ConstexprString truncates SILENTLY on overflow, so an under-count would emit
        // wrong SQL with no diagnostic.
        static consteval auto calculate_single_delete_sql_size() -> std::size_t {
            return delete_prefix_size() + pk_where_clause_size(Base::primary_key_members_) + 1; // + null terminator
        }

        static consteval auto build_single_delete_sql_array() {
            constexpr std::size_t     sql_size = calculate_single_delete_sql_size() + utilities::sql_len::LARGE_BUFFER;
            ConstexprString<sql_size> result;
            append_delete_prefix(result);
            append_pk_where_clause(result, Base::primary_key_members_);
            return result;
        }

        // --- all-rows DELETE --------------------------------------------------

        static consteval auto build_delete_all_sql_array() {
            using utilities::sql_len::DELETE_FROM;
            constexpr std::size_t sql_size = DELETE_FROM + Base::table_name_.size() + utilities::sql_len::LARGE_BUFFER;
            ConstexprString<sql_size> result;
            result.append("DELETE FROM ");
            result.append(Base::table_name_);
            return result;
        }

        // --- bulk DELETE: the IN opener ---------------------------------------

        // "<pk> IN (" for a single PK, "(a, b) IN (" for a composite key.
        static consteval auto in_opener_size() -> std::size_t {
            std::size_t size = 0;
            for (const std::meta::info member : Base::primary_key_members_) {
                size += storm::meta::column_name_size(member);
            }
            if constexpr (PK_COLUMNS > 1) {
                size += 2;                    // "()" around the row-value column list
                size += 2 * (PK_COLUMNS - 1); // ", " between column names
            }
            return size + utilities::sql_len::IN_OPEN;
        }

        template <typename Buf> static consteval auto append_in_opener(Buf& buf) -> void {
            if constexpr (PK_COLUMNS > 1) {
                buf.append("(");
            }
            bool first = true;
            for (const std::meta::info member : Base::primary_key_members_) {
                if (!first) {
                    buf.append(", ");
                }
                storm::meta::append_column_name(buf, member);
                first = false;
            }
            if constexpr (PK_COLUMNS > 1) {
                buf.append(")");
            }
            buf.append(" IN (");
        }

        static consteval auto calculate_bulk_delete_prefix_size() -> std::size_t {
            return delete_prefix_size() + in_opener_size() + 1; // + null terminator
        }

        static consteval auto build_bulk_delete_prefix() {
            constexpr std::size_t prefix_size = calculate_bulk_delete_prefix_size() + utilities::sql_len::LARGE_BUFFER;
            ConstexprString<prefix_size> result;
            append_delete_prefix(result);
            append_in_opener(result);
            return result;
        }

        // --- bulk DELETE: the per-row placeholder group -----------------------

        // One row's placeholders: "?" for a single PK, "(?,?)" for a composite key.
        static constexpr std::size_t ROW_PLACEHOLDER_SIZE =
                PK_COLUMNS == 1 ? 1 : (2 + PK_COLUMNS + (PK_COLUMNS - 1)); // "(" + N "?" + N-1 "," + ")"

        // Written once for BOTH the consteval max-chunk builder and the runtime
        // per-count builder (ConstexprString and std::string both append). Keeping one
        // definition is what stops the two from drifting into different SQL for the
        // same model — the failure mode would be a prepared statement whose placeholder
        // count disagrees with the bind loop.
        template <typename Buf> static constexpr auto append_row_placeholder(Buf& buf) -> void {
            if constexpr (PK_COLUMNS == 1) {
                buf.append("?");
            } else {
                buf.append("(");
                for (std::size_t col = 0; col < PK_COLUMNS; ++col) {
                    if (col > 0) {
                        buf.append(",");
                    }
                    buf.append("?");
                }
                buf.append(")");
            }
        }

        // Append `count` comma-separated placeholder groups plus the closing paren —
        // the tail of every bulk DELETE, compile-time or runtime. count == 0 yields an
        // empty "IN ()", which no execution path can reach (both execute() and to_sql()
        // return early on an empty span) but which keeps the size calculation above
        // consistent with what is actually written.
        template <typename Buf> static constexpr auto append_row_placeholder_list(Buf& buf, std::size_t count) -> void {
            for (std::size_t i = 0; i < count; ++i) {
                if (i > 0) {
                    buf.append(",");
                }
                append_row_placeholder(buf);
            }
            buf.append(")");
        }

        // Exact size of `count` placeholder groups + separating commas + ")".
        // count == 0 has no meaningful list — the unsigned `count - 1` would wrap — and no
        // caller wants one: an empty batch is short-circuited before any SQL is built.
        static constexpr auto row_placeholder_list_size(std::size_t count) -> std::size_t {
            return count == 0 ? 1 : (count * ROW_PLACEHOLDER_SIZE) + (count - 1) + 1;
        }

        // The complete bulk DELETE SQL for `count` keys, built at runtime. Connection-free
        // (the text is pure reflection), so it is also the seam the SQL-shape tests assert
        // through — EraseStatement caches the result per count rather than carrying its own
        // builder, which keeps that class off the cpp:S1448 method ceiling.
        [[nodiscard]] static auto bulk_delete_sql_for(std::size_t count) -> std::string {
            std::string sql;
            sql.reserve((calculate_bulk_delete_prefix_size() - 1) + row_placeholder_list_size(count));
            sql = std::string(build_bulk_delete_prefix());
            append_row_placeholder_list(sql, count);
            return sql;
        }

        // --- bulk DELETE: chunking -------------------------------------------

        // 80% of the DB variable ceiling, for safety.
        static constexpr std::size_t MAX_CHUNK_SIZE = (Base::MAX_DB_VARIABLES * 4) / 5; // 799

        // Rows per chunk. Each row costs PK_COLUMNS bound parameters, not one, so a
        // composite key fits proportionally fewer rows under the same variable ceiling
        // (#501). Single-PK models keep the historical 799.
        static constexpr std::size_t MAX_CHUNK_ROWS = MAX_CHUNK_SIZE / PK_COLUMNS;

        static consteval auto calculate_max_bulk_delete_sql_size() -> std::size_t {
            return (calculate_bulk_delete_prefix_size() - 1) + row_placeholder_list_size(MAX_CHUNK_ROWS) + 1;
        }

        static consteval auto build_max_bulk_delete_sql() {
            constexpr std::size_t     sql_size = calculate_max_bulk_delete_sql_size() + 50; // Safety buffer
            ConstexprString<sql_size> result;
            result.append(build_bulk_delete_prefix());
            append_row_placeholder_list(result, MAX_CHUNK_ROWS);
            return result;
        }
    };

} // namespace storm::orm::statements
