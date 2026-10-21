module;

// `duplicate` removed in #277 Phase 3 (delete_prefix_size + append_delete_prefix helpers shared by single-row and bulk
// DELETE SQL builders).

#include <meta>

export module storm_orm_statements_erase;

import std;

import storm_orm_statements_base;
import storm_orm_statements_erase_grammar;
import storm_orm_utilities;
import storm_orm_transaction;
import storm_orm_where;
import storm_db_concept;
import storm_db_default_connection;
import storm_db_sqlite;

export namespace storm::orm::statements {

    // Import utilities for code convenience
    using storm::orm::utilities::BulkSQLCache;
    using storm::orm::utilities::ConstexprString;
    using storm::orm::utilities::TransactionGuard;

    // Statement class for ORM erase operations
    template <typename T, storm::db::DatabaseConnection ConnType = storm::db::sqlite::Connection>
    class EraseStatement : private BaseStatement<T> {
        friend class BaseStatement<T>; // Allow BaseStatement to access protected/private members
        using Base       = BaseStatement<T>;
        using Connection = ConnType;
        using Error      = typename ConnType::Error;
        using Statement  = typename ConnType::Statement;

        // Compile-time DELETE SQL grammar (#501): every "how the SQL is spelled" helper —
        // the prefix, the AND-joined key clause, the row-value IN list and the chunk
        // arithmetic — lives in EraseGrammar<T>. Split out to keep this file under the
        // size threshold, mirroring UpdateGrammar (#434).
        using Grammar = EraseGrammar<T>;

        // Rows per chunk. Each row costs one bound parameter per PK column, so a
        // composite key fits proportionally fewer rows under the same variable ceiling
        // (#501); single-PK models keep the historical 799.
        static constexpr std::size_t MAX_CHUNK_ROWS = Grammar::MAX_CHUNK_ROWS;

        // Pre-computed single DELETE SQL generated at compile-time.
        // Single PK  → "DELETE FROM t WHERE id = ?"          (byte-identical to pre-#501)
        // Composite  → "DELETE FROM t WHERE a = ? AND b = ?"
        static inline const std::string single_delete_sql_string =
                std::string(Grammar::build_single_delete_sql_array());

        // Pre-computed all-rows DELETE SQL
        static inline const std::string delete_all_sql_string = std::string(Grammar::build_delete_all_sql_array());

      private:
        // Pre-computed max-chunk bulk DELETE SQL generated at compile-time
        static inline const std::string max_bulk_delete_sql = std::string(Grammar::build_max_bulk_delete_sql());

        // Generate single DELETE SQL string (compile-time computed, runtime accessible)
        static auto get_single_delete_sql() -> const std::string& {
            return single_delete_sql_string;
        }

        // Generate bulk DELETE SQL string for IN clause (with thread-local caching)
        // Returns const reference to avoid expensive string copy
        static auto get_bulk_delete_sql(std::size_t count) -> const std::string& {
            if (count == 1) {
                return get_single_delete_sql();
            }

            // Thread-local cache for common batch sizes (same pattern as INSERT)
            static thread_local BulkSQLCache cache;

            // Check cache first
            if (const auto* cached = cache.find(count)) {
                return *cached; // Return by reference - no copy
            }

            // Built by the same grammar the consteval max-chunk builder uses, so the
            // runtime and compile-time SQL for a given count cannot drift apart.
            cache.insert(count, Grammar::bulk_delete_sql_for(count));
            return *cache.find(count); // Guaranteed to exist after insert
        }

      public:
        explicit EraseStatement(std::shared_ptr<ConnType> conn = storm::db::DefaultConnection<ConnType>::get())
            : conn_(std::move(conn)) {}

        struct SingleQuery {
            EraseStatement     stmt;
            const T&           obj;
            [[nodiscard]] auto execute() -> std::expected<void, Error> {
                return stmt.execute_one(obj);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.to_sql(obj);
            }
            [[nodiscard]] static auto sql() -> std::string {
                return single_delete_sql_string;
            }
        };

        struct BulkQuery {
            EraseStatement     stmt;
            std::span<const T> objects;
            [[nodiscard]] auto execute() -> std::expected<void, Error> {
                return stmt.execute(objects);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.to_sql(objects);
            }
            [[nodiscard]] auto sql() -> std::string {
                return std::string(get_bulk_delete_sql(objects.size()));
            }
        };

        struct DeleteAllQuery {
            EraseStatement     stmt;
            [[nodiscard]] auto execute() -> std::expected<void, Error> {
                return stmt.execute_all();
            }
            [[nodiscard]] auto to_sql() -> std::string {
                return delete_all_sql_string;
            }
            [[nodiscard]] static auto sql() -> std::string {
                return delete_all_sql_string;
            }
        };

        // Conditional bulk DELETE (#198): DELETE FROM <table> WHERE <where_expr>.
        // Holds the QuerySet's where_expr_ by shared_ptr. A null expr is refused
        // at execute()/to_sql() time so a dropped where() cannot wipe the table
        // (erase_all() is the explicit full-wipe path).
        struct ConditionalDeleteQuery {
            EraseStatement                   stmt;
            orm::where::ExpressionVariantPtr where_expr;
            [[nodiscard]] auto               execute() -> std::expected<void, Error> {
                return stmt.execute_where(where_expr);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.to_sql_where(where_expr);
            }
        };

        [[nodiscard]] auto query(const T& obj [[clang::lifetimebound]]) -> SingleQuery {
            return {std::move(*this), obj};
        }
        // LIFETIME CONTRACT (issue #357, finding B): the returned BulkQuery holds a
        // std::span<const T> aliasing the caller's container. `[[clang::lifetimebound]]`
        // catches the inline-temporary case at compile time, but a span built from a
        // container that dies before .execute()/.to_sql() runs still dangles silently at
        // runtime. Treat the proxy as single-expression-use: keep the backing container
        // alive until the terminal call completes.
        [[nodiscard]] auto query(std::span<const T> objects [[clang::lifetimebound]]) -> BulkQuery {
            return {std::move(*this), objects};
        }
        [[nodiscard]] auto query_all() -> DeleteAllQuery {
            return {std::move(*this)};
        }
        [[nodiscard]] auto query_where(orm::where::ExpressionVariantPtr where_expr) -> ConditionalDeleteQuery {
            return {std::move(*this), std::move(where_expr)};
        }

        // Returns SQL string with bound parameters inlined for a single DELETE (for debugging)
        [[nodiscard]] auto to_sql(const T& obj) -> std::expected<std::string, Error> {
            auto stmt_result = ready_delete_statement(get_single_delete_sql());
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            auto* stmt        = *stmt_result;
            int   param_index = 1;
            if (auto bind_result = bind_pk_at(*stmt, obj, param_index); !bind_result) {
                return std::unexpected(bind_result.error());
            }
            return stmt->expanded_sql();
        }

        // Returns SQL string with bound parameters inlined for a bulk DELETE (for debugging)
        [[nodiscard]] auto to_sql(std::span<const T> objects) -> std::expected<std::string, Error> {
            if (objects.empty()) {
                return std::string{};
            }
            const auto& bulk_sql = get_bulk_delete_sql(objects.size());
            auto        stmt_res = ready_delete_statement(bulk_sql);
            if (!stmt_res) {
                return std::unexpected(stmt_res.error());
            }
            auto* stmt = *stmt_res;
            // `param_index` stays `int` by design (issue #362, item B — deferred):
            // sqlite3_bind_* takes an `int` index, so the accumulator must narrow to `int`
            // at the bind call regardless of its own width. Overflow would need > INT_MAX
            // (~2.1B) bound parameters; the execute path is chunk-capped at 799, so this
            // unbounded debug-only to_sql path is the only place a large span reaches here,
            // and 2.1B PKs is not a reachable input. Widening to size_t adds casts in a
            // hot binder hierarchy for no real safety gain.
            int param_index = 1;
            for (const auto& obj : objects) {
                // bind_pk_at advances param_index past the whole key (N parameters for an
                // N-column PK), so no manual stride here.
                if (auto result = bind_pk_at(*stmt, obj, param_index); !result) {
                    return std::unexpected(result.error());
                }
            }
            return stmt->expanded_sql();
        }

        [[nodiscard]] auto execute(std::span<const T> objects) -> std::expected<void, Error> {
            if (objects.empty()) {
                return {};
            }

            // Fast path for single delete - bypass all batch machinery
            if (objects.size() == 1) {
                return execute_one(objects[0]);
            }

            // Strategy 2: Bulk IN clause (2-799 rows) - no transaction needed
            if (objects.size() <= MAX_CHUNK_ROWS) {
                return execute_bulk(objects);
            }

            // Strategy 3: Chunked IN clause (800+ rows) - RAII transaction guard
            auto txn = TransactionGuard<ConnType>::begin(conn_);
            if (!txn) {
                return std::unexpected(txn.error());
            }

            auto result = execute_chunked(objects);
            if (!result) {
                return result; // ~TransactionGuard will ROLLBACK
            }

            return txn->commit();
        }

        // Delete all rows — executes DELETE FROM <table> with no WHERE clause
        [[nodiscard]] auto execute_all() -> std::expected<void, Error> {
            return conn_->execute(delete_all_sql_string);
        }

        // Conditional DELETE (#198): build "DELETE FROM <table> WHERE <expr>".
        // The WHERE body is dynamic (runtime to_sql), so the string is assembled
        // at runtime like SELECT's dynamic path — not a compile-time ConstexprString.
        static auto build_conditional_delete_sql(const orm::where::ExpressionVariant& expr) -> std::string {
            std::string sql = delete_all_sql_string;
            sql += " WHERE ";
            sql += orm::where::to_sql(expr);
            return sql;
        }

        // std::unexpected returned when erase() is called with no WHERE filter,
        // refusing to wipe the whole table by accident. -1 is the backend-agnostic
        // "client-side validation" code (same as PG's "Connection not open").
        static auto empty_where_error() -> std::unexpected<Error> {
            return std::unexpected(Error{-1, "erase() requires a WHERE clause; use erase_all() to delete all rows"});
        }

        // Empty-WHERE check + prepare + bind, shared by execute_where()/to_sql_where().
        // Returns a prepared, parameter-bound Statement* ready to execute or expand.
        [[nodiscard]] auto ready_conditional_delete(const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<Statement*, Error> {
            if (!where_expr) [[unlikely]] {
                return empty_where_error();
            }
            auto stmt_result = ready_delete_statement(build_conditional_delete_sql(*where_expr));
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            auto* stmt = *stmt_result;
            if (auto bind_result = Base::template bind_where_params<Statement, Error>(stmt, where_expr); !bind_result) {
                return std::unexpected(bind_result.error());
            }
            return stmt;
        }

        // Execute a conditional DELETE. Refuses an empty WHERE before any DB call.
        [[nodiscard]] auto execute_where(const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<void, Error> {
            auto stmt_result = ready_conditional_delete(where_expr);
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            if (auto exec_result = (*stmt_result)->execute(); !exec_result) {
                return std::unexpected(exec_result.error());
            }
            return {};
        }

        // Return the conditional DELETE SQL with bound parameters inlined (debugging).
        [[nodiscard]] auto to_sql_where(const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<std::string, Error> {
            auto stmt_result = ready_conditional_delete(where_expr);
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            return (*stmt_result)->expanded_sql();
        }

        // Single DELETE - prepare from L3 cache per call, inlined execution
        [[nodiscard]] __attribute__((hot)) auto execute_one(const T& obj) noexcept -> std::expected<void, Error> {
            // Prepare statement (cached internally by connection)
            auto stmt_result = ready_delete_statement(get_single_delete_sql());
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            auto* stmt = *stmt_result;

            // Bind PK and execute (statement already reset by ready_delete_statement)
            int param_index = 1;
            if (auto bind_result = bind_pk_at(*stmt, obj, param_index); !bind_result) {
                return std::unexpected(bind_result.error());
            }

            auto exec_result = stmt->execute();
            if (!exec_result) {
                return std::unexpected(exec_result.error());
            }

            return {};
        }

        // Bulk execute using IN clause for better performance on small batches
        // Flat code pattern for hot path performance (avoids lambda overhead)
        [[nodiscard]] __attribute__((hot)) auto execute_bulk(std::span<const T> objects) -> std::expected<void, Error> {
            const auto& bulk_sql = get_bulk_delete_sql(objects.size());

            // Prepare statement (cached internally by connection)
            auto stmt_result = conn_->prepare_cached(bulk_sql);
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }

            return bind_pks_and_execute(**stmt_result, objects);
        }

      protected: // Changed to protected so BaseStatement can access
        // Execute large batches using chunked IN clauses
        // NOTE: Transaction is handled by the caller via TransactionGuard (#415).
        // Optimized: pre-cache max bulk statement, only lookup remainder once
        [[nodiscard]] __attribute__((hot)) auto execute_chunked(std::span<const T> objects)
                -> std::expected<void, Error> {
            // Prepare max bulk statement once per call (compile-time SQL, reused across full chunks)
            auto max_bulk_result = conn_->prepare_cached(max_bulk_delete_sql);
            if (!max_bulk_result) {
                return std::unexpected(max_bulk_result.error());
            }
            Statement* max_bulk_stmt = *max_bulk_result;

            // Calculate remainder size upfront
            const std::size_t remainder_size = objects.size() % MAX_CHUNK_ROWS;
            Statement*        remainder_stmt = nullptr;

            // Cache remainder statement if needed (only one hash lookup per batch)
            if (remainder_size > 0) {
                const auto& remainder_sql = get_bulk_delete_sql(remainder_size);
                auto        stmt_result   = conn_->prepare_cached(remainder_sql);
                if (!stmt_result) {
                    return std::unexpected(stmt_result.error());
                }
                remainder_stmt = *stmt_result;
            }

            // Process full chunks using local pointer (no hash lookups in loop)
            std::size_t offset = 0;
            while (offset + MAX_CHUNK_ROWS <= objects.size()) {
                auto chunk = objects.subspan(offset, MAX_CHUNK_ROWS);
                if (auto result = bind_pks_and_execute(*max_bulk_stmt, chunk); !result) {
                    return std::unexpected(result.error());
                }
                offset += MAX_CHUNK_ROWS;
            }

            // Process remainder with pre-cached statement
            if (remainder_size > 0) {
                auto chunk = objects.subspan(offset, remainder_size);
                if (auto result = bind_pks_and_execute(*remainder_stmt, chunk); !result) {
                    return std::unexpected(result.error());
                }
            }

            return {};
        }

        // Bind and execute bulk operation (single statement, multiple parameters)
        // Note: Takes reference since we use prepare_cached which returns a pointer to cached statement
        [[nodiscard]] __attribute__((always_inline)) auto
        bind_pks_and_execute(Statement& stmt, std::span<const T> objects) noexcept -> std::expected<void, Error> {
            // Reset before binding - required for cached statement reuse
            stmt.reset();

            // Bind all primary key values. bind_pk_at advances param_index past each
            // object's whole key — N parameters per row for an N-column PK (#501).
            int param_index = 1;
            for (const auto& obj : objects) {
                if (auto result = bind_pk_at(stmt, obj, param_index); !result) {
                    return std::unexpected(result.error());
                }
            }

            // Execute once with all parameters bound
            if (auto result = stmt.execute(); !result) {
                return std::unexpected(result.error());
            }

            return {};
        }

        // Prepare via the L3 cache and reset — the prepare→check→reset prefix
        // shared by to_sql(obj), to_sql(span) and execute_one(). Binding stays
        // at the call site (single PK / span of PKs / none differ per caller).
        // Explicit-check form (not and_then): monadic chaining benched slower
        // on hot delete paths (#363).
        [[nodiscard]] __attribute__((always_inline)) auto ready_delete_statement(const std::string& sql) noexcept
                -> std::expected<Statement*, Error> {
            auto stmt_result = conn_->prepare_cached(sql);
            if (!stmt_result) [[unlikely]] {
                return std::unexpected(stmt_result.error());
            }
            Statement* stmt = *stmt_result;
            stmt->reset();
            return stmt;
        }

        // Bind the object's whole primary key starting at `index`, advancing it past the
        // key (#501). A single-PK model binds one value and advances by one, exactly as
        // before; a composite key binds every part in declaration order — the same order
        // the WHERE clause and the row-value placeholder group name them in.
        //
        // `index` is an in/out reference precisely so the callers below cannot get the
        // stride wrong: the old `++param_index` per object silently assumed one parameter
        // per row, which is the bug this issue exists to fix.
        [[nodiscard]] __attribute__((always_inline)) auto bind_pk_at(Statement& stmt, const T& obj, int& index) noexcept
                -> std::expected<void, Error> {
            return Base::template bind_pk_values<ConnType>(stmt, obj, index);
        }

      private:
        std::shared_ptr<ConnType> conn_;
    };

} // namespace storm::orm::statements
