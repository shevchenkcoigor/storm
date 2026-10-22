module;

// Single cohesive class template; thresholds intentionally relaxed (see #264 finding).
// `duplicate` removed in #277 Phase 3 (prepare_stmt + reset_bind_execute helpers; UpdateQueryBase::sql()
// shared by SingleQuery/BulkQuery proxies). Query-proxy structs lifted to namespace `detail` in #438 so
// their methods stop counting toward UpdateStatement's S1448 method total.

#include <meta>

export module storm_orm_statements_update;

import std;

import storm_orm_statements_base;
import storm_orm_statements_update_grammar;
import storm_orm_utilities;
import storm_orm_transaction;
import storm_orm_where;
import storm_db_concept;
import storm_db_default_connection;

export namespace storm::orm::statements {

    // Import utilities for code convenience
    using storm::orm::utilities::ConstexprString;
    using storm::orm::utilities::TransactionGuard;

    template <typename T, storm::db::DatabaseConnection ConnType> class BasicUpdateStatement;

    // Query-proxy structs (#438): lifted OUT of UpdateStatement so their sql()/execute()/to_sql()
    // methods no longer count toward UpdateStatement's method total (cpp:S1448 — SonarCloud sums
    // the methods of nested classes into the enclosing class). Each proxy is the terminal of one
    // UpdateStatement::query*() builder: it owns the moved-in statement and forwards .execute()/
    // .to_sql() to the matching UpdateStatement method. They name the public UpdateStatement API
    // only, so namespace scope changes nothing about access.
    namespace detail {

        // Both single/bulk proxies' static `sql()` returned the same compile-time UPDATE string.
        template <typename T> struct UpdateQueryBase {
            [[nodiscard]] static auto sql() -> std::string {
                return UpdateGrammar<T>::update_sql_string;
            }
        };

        template <typename T, storm::db::DatabaseConnection ConnType> struct SingleQuery : UpdateQueryBase<T> {
            using Error = typename ConnType::Error;
            BasicUpdateStatement<T, ConnType> stmt;
            const T&                          obj;
            [[nodiscard]] auto                execute() -> std::expected<void, Error> {
                return stmt.execute_single_optimized(obj);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.to_sql(obj);
            }
        };

        template <typename T, storm::db::DatabaseConnection ConnType> struct BulkQuery : UpdateQueryBase<T> {
            using Error = typename ConnType::Error;
            BasicUpdateStatement<T, ConnType> stmt;
            std::span<const T>                objects;
            [[nodiscard]] auto                execute() -> std::expected<void, Error> {
                return stmt.execute(objects);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.to_sql(objects);
            }
        };

        // Conditional bulk UPDATE (#403): UPDATE <table> SET <set> WHERE <where_expr>.
        // SET columns are the Members... pack; values come from `proto`. A null where_expr
        // is refused at execute()/to_sql() time so a dropped where() cannot write the whole
        // table (update_all() is the explicit full-table path — see UpdateAllQuery, #409).
        template <typename T, storm::db::DatabaseConnection ConnType, std::meta::info... Members>
        struct ConditionalUpdateQuery {
            using Error = typename ConnType::Error;
            BasicUpdateStatement<T, ConnType> stmt;
            const T&                          proto;
            orm::where::ExpressionVariantPtr  where_expr;
            [[nodiscard]] auto                execute() -> std::expected<void, Error> {
                return stmt.template execute_where<Members...>(proto, where_expr);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.template to_sql_where<Members...>(proto, where_expr);
            }
        };

        // Full-table UPDATE (#409): UPDATE <table> SET <set> with NO WHERE. The explicit
        // escape hatch named by the empty-WHERE refusal, mirroring DeleteAllQuery.
        template <typename T, storm::db::DatabaseConnection ConnType, std::meta::info... Members>
        struct UpdateAllQuery {
            using Error = typename ConnType::Error;
            BasicUpdateStatement<T, ConnType> stmt;
            const T&                          proto;
            [[nodiscard]] auto                execute() -> std::expected<void, Error> {
                return stmt.template execute_all<Members...>(proto);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.template to_sql_all<Members...>(proto);
            }
        };

    } // namespace detail

    // Statement class for ORM update operations
    template <typename T, storm::db::DatabaseConnection ConnType>
    class BasicUpdateStatement : private BaseStatement<T> {
        friend class BaseStatement<T>; // Allow BaseStatement to access protected/private members
        using Base       = BaseStatement<T>;
        using Connection = ConnType;
        using Error      = typename ConnType::Error;
        using Statement  = typename ConnType::Statement;

        // Compile-time UPDATE SQL grammar (#434): every "how the SQL is spelled" helper —
        // build_field_assignments, build_conditional_set_clause, the SQL string, the
        // is_settable_member/index_of_member/is_unlisted_auto_update reflection predicates —
        // lives in UpdateGrammar<T>. Split out to keep this class cohesive (cpp:S1448).
        using Grammar = UpdateGrammar<T>;

      private:
        // Generate UPDATE SQL string (compile-time computed, runtime accessible)
        static auto get_update_sql() -> const std::string& {
            return Grammar::update_sql_string;
        }

        // std::unexpected returned when update<...>() is called with no WHERE filter,
        // refusing to write the whole table by accident. -1 is the backend-agnostic
        // "client-side validation" code (same shape as the conditional DELETE refusal).
        static auto empty_where_error() -> std::unexpected<Error> {
            return std::unexpected(Error{-1, "update() requires a WHERE clause; use update_all() to update all rows"});
        }

        // Conditional UPDATE (#403): build "UPDATE <table> SET <set_clause> WHERE <expr>".
        // The SET clause is compile-time (keyed on the Members... pack); the WHERE body is
        // dynamic (runtime to_sql), so the full string is assembled at runtime like SELECT's
        // dynamic path — not a compile-time ConstexprString.
        template <std::meta::info... Members>
        static auto build_conditional_update_sql(const orm::where::ExpressionVariant& expr) -> std::string {
            static const std::string set_clause =
                    std::string(Grammar::template build_conditional_set_clause<Members...>());
            std::string sql;
            sql.reserve(Base::table_name_.size() + set_clause.size() + 32);
            sql += "UPDATE ";
            sql += Base::table_name_;
            sql += " SET ";
            sql += set_clause;
            sql += " WHERE ";
            sql += orm::where::to_sql(expr);
            return sql;
        }

        // Full-table UPDATE (#409): build "UPDATE <table> SET <set_clause>" with NO WHERE.
        // Same SET clause as the conditional path (Members... + unlisted auto_update fields);
        // it just omits the WHERE — the explicit escape hatch mirroring DELETE's delete_all_sql.
        template <std::meta::info... Members> static auto build_update_all_sql() -> std::string {
            static const std::string set_clause =
                    std::string(Grammar::template build_conditional_set_clause<Members...>());
            std::string sql;
            sql.reserve(Base::table_name_.size() + set_clause.size() + 16);
            sql += "UPDATE ";
            sql += Base::table_name_;
            sql += " SET ";
            sql += set_clause;
            return sql;
        }

      public:
        explicit BasicUpdateStatement(
                std::shared_ptr<ConnType> conn = storm::db::BasicDefaultConnection<ConnType>::get()
        )
            : conn_(std::move(conn)) {}

        [[nodiscard]] auto query(const T& obj [[clang::lifetimebound]]) -> detail::SingleQuery<T, ConnType> {
            return {{}, std::move(*this), obj};
        }
        // LIFETIME CONTRACT (issue #357, finding B): the returned BulkQuery holds a
        // std::span<const T> aliasing the caller's container. `[[clang::lifetimebound]]`
        // catches the inline-temporary case at compile time, but a span built from a
        // container that dies before .execute()/.to_sql() runs still dangles silently at
        // runtime. Treat the proxy as single-expression-use: keep the backing container
        // alive until the terminal call completes.
        [[nodiscard]] auto query(std::span<const T> objects [[clang::lifetimebound]])
                -> detail::BulkQuery<T, ConnType> {
            return {{}, std::move(*this), objects};
        }

        template <std::meta::info... Members>
            requires(sizeof...(Members) > 0 && (Grammar::template is_settable_member<Members>() && ...))
        [[nodiscard]] auto
        query_where(const T& proto [[clang::lifetimebound]], orm::where::ExpressionVariantPtr where_expr)
                -> detail::ConditionalUpdateQuery<T, ConnType, Members...> {
            return {std::move(*this), proto, std::move(where_expr)};
        }

        template <std::meta::info... Members>
            requires(sizeof...(Members) > 0 && (Grammar::template is_settable_member<Members>() && ...))
        [[nodiscard]] auto query_all(const T& proto [[clang::lifetimebound]])
                -> detail::UpdateAllQuery<T, ConnType, Members...> {
            return {std::move(*this), proto};
        }

        // Returns SQL string with bound parameters inlined for a single UPDATE (for debugging)
        [[nodiscard]] auto to_sql(const T& obj) -> std::expected<std::string, Error> {
            auto stmt_result = conn_->prepare_cached(get_update_sql());
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            auto* stmt        = *stmt_result;
            auto  bind_result = inline_bind_all_fields(stmt, obj, typename Base::field_indices_t{});
            if (!bind_result) {
                return std::unexpected(bind_result.error());
            }
            return stmt->expanded_sql();
        }

        // Returns SQL strings for bulk UPDATE (one SQL per object, for debugging)
        [[nodiscard]] auto to_sql(std::span<const T> objects) -> std::expected<std::string, Error> {
            if (objects.empty()) {
                return std::string{};
            }
            // For bulk, just show the first UPDATE SQL as representative
            return to_sql(objects[0]);
        }

        // Prepare (L3-cached) the UPDATE statement. Both execute paths share
        // this so the prepare + error-check sequence lives in one place.
        [[nodiscard]] __attribute__((always_inline)) auto prepare_stmt() noexcept -> std::expected<Statement*, Error> {
            auto prepare_result = conn_->prepare_cached(get_update_sql());
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            return *prepare_result;
        }

        // reset → inline-bind → execute on the given statement. The batch
        // loop and execute_single_row used to repeat this body verbatim.
        [[nodiscard]] __attribute__((always_inline)) auto reset_bind_execute(Statement* stmt, const T& obj) noexcept
                -> std::expected<void, Error> {
            stmt->reset();
            auto bind_result = inline_bind_all_fields(stmt, obj, typename Base::field_indices_t{});
            if (!bind_result) {
                return std::unexpected(bind_result.error());
            }
            auto exec_result = stmt->execute();
            if (!exec_result) {
                return std::unexpected(exec_result.error());
            }
            return {};
        }

        // Optimized batch execute - flattened, no nested lambdas
        [[nodiscard]] __attribute__((hot)) auto execute(std::span<const T> objects) noexcept
                -> std::expected<void, Error> {
            if (objects.empty()) {
                return {};
            }

            // Prepare the statement pointer once (avoid hash lookup per row)
            auto prepared = prepare_stmt();
            if (!prepared) [[unlikely]] {
                return std::unexpected(prepared.error());
            }
            Statement* stmt = *prepared;

            // Single object - no transaction needed
            if (objects.size() == 1) {
                return execute_single_row(stmt, objects[0]);
            }

            // Multiple objects - use RAII transaction guard
            auto txn = TransactionGuard<ConnType>::begin(conn_);
            if (!txn) {
                return std::unexpected(txn.error());
            }

            for (const auto& obj : objects) {
                if (auto step = reset_bind_execute(stmt, obj); !step) {
                    return step;
                }
            }

            return txn->commit();
        }

        // Execute single row with the given statement (no transaction)
        [[nodiscard]] __attribute__((always_inline)) auto execute_single_row(Statement* stmt, const T& obj) noexcept
                -> std::expected<void, Error> {
            return reset_bind_execute(stmt, obj);
        }

        // Helper to unroll inline binding for all fields (skips PK, then binds PK last).
        // The SET values occupy placeholders 1..N_set and the key follows, so for a
        // composite key the WHERE parameters start at N_set+1 and run N_pk wide (#501) —
        // bind_pk_values threads param_index through, so the offset is never recomputed.
        template <std::size_t... Is>
        [[nodiscard]] __attribute__((always_inline)) static auto
        inline_bind_all_fields(Statement* stmt, const T& obj, std::index_sequence<Is...> /*unused*/) noexcept
                -> std::expected<void, Error> {
            int param_index = 1;

            // Bind all non-PK fields at compile time using unified binder.
            // SkipAllPK=true skips EVERY primary-key member, matching the SET clause
            // build_field_assignments emits (#501). IsUpdate=true: auto_update stamps
            // now(); auto_create binds the object's stored value (preserving created_at).
            // One now() read shared across fields (#209).
            // Named rather than three bare `true`s at the call site — they are three
            // different policies and their order is not guessable from the call.
            constexpr bool SKIP_PK     = true; // the key is bound separately, after the SET values
            constexpr bool IS_UPDATE   = true; // auto_create keeps its stored value
            constexpr bool SKIP_ALL_PK = true; // every part of a composite key, not just the first

            std::expected<void, Error> result{};
            const auto                 now = Base::batch_now();
            ((result = Base::template bind_field_at_index<ConnType, Is, SKIP_PK, IS_UPDATE, SKIP_ALL_PK>(
                      stmt, obj, param_index, now
              ),
              result.has_value()) &&
             ...);

            if (!result) {
                return result;
            }

            // Bind the whole primary key last — one value per PK column, in declaration
            // order, matching the AND-joined WHERE clause.
            return Base::template bind_pk_values<ConnType>(*stmt, obj, param_index);
        }

        // Ultra-optimized single UPDATE - pre-cached statement, fully inlined binding.
        // Diverges from execute_single_row in that the reset happens AFTER execute,
        // not before bind (one less statement reset on the steady-state path).
        [[nodiscard]] __attribute__((hot)) auto execute_single_optimized(const T& obj) noexcept
                -> std::expected<void, Error> {
            auto prepared = prepare_stmt();
            if (!prepared) [[unlikely]] {
                return std::unexpected(prepared.error());
            }
            Statement* stmt = *prepared;
            // FULLY INLINED BINDING - all compile-time dispatched, no function calls
            auto bind_result = inline_bind_all_fields(stmt, obj, typename Base::field_indices_t{});
            if (!bind_result) {
                return std::unexpected(bind_result.error());
            }

            // Execute and reset for next use
            auto exec_result = stmt->execute();
            if (!exec_result) {
                stmt->reset();
                return std::unexpected(exec_result.error());
            }

            stmt->reset();
            return {};
        }

        // --- Conditional bulk UPDATE (#403) binding + execution ---

        // Compile-time fold over field indices: for each index whose member is an
        // unlisted auto_update field, bind now() at param_index (declaration order).
        // `if constexpr` keeps consteval-only std::meta::info out of any runtime context.
        template <std::meta::info... Members, std::size_t... Is>
        [[nodiscard]] auto bind_unlisted_auto_updates(
                Statement*                            stmt,
                int&                                  param_index,
                std::chrono::system_clock::time_point now,
                std::index_sequence<Is...> /*unused*/
        ) noexcept -> std::expected<void, Error> {
            std::expected<void, Error> result{};
            (
                    [&] {
                        if constexpr (Grammar::template is_unlisted_auto_update<Members...>(Base::all_members_[Is])) {
                            if (result.has_value()) {
                                result = Base::template bind_one<ConnType>(stmt, param_index, now);
                            }
                        }
                    }(),
                    ...
            );
            return result;
        }

        // Bind the SET values in the SAME order build_conditional_set_clause emits them:
        // (1) explicit Members... from `proto`, (2) unlisted auto_update fields stamped now().
        // param_index advances; WHERE binding continues from where this leaves off.
        template <std::meta::info... Members>
        [[nodiscard]] auto bind_conditional_set(Statement* stmt, const T& proto, int& param_index) noexcept
                -> std::expected<void, Error> {
            const auto now = Base::batch_now(); // one clock read; compiles away if no timestamp field

            // (1) explicit members, in pack order. IsUpdate=true so an explicitly-listed
            //     auto_update member is stamped now() (matches single-row UPDATE), and a
            //     listed auto_create member binds the proto's stored value.
            std::expected<void, Error> result{};
            ((result = Base::template bind_field_at_index<ConnType, Grammar::index_of_member(Members), false, true>(
                      stmt, proto, param_index, now
              ),
              result.has_value()) &&
             ...);
            if (!result) {
                return result;
            }

            // (2) unlisted auto_update fields — stamp now() in declaration order.
            return bind_unlisted_auto_updates<Members...>(stmt, param_index, now, typename Base::field_indices_t{});
        }

        // Prepare `sql`, reset, then bind the SET values — the prefix shared by the
        // conditional (WHERE, #403) and full-table (#409) paths. Conditional appends WHERE.
        template <std::meta::info... Members>
        [[nodiscard]] auto prepare_bind_set(const std::string& sql, const T& proto, int& param_index)
                -> std::expected<Statement*, Error> {
            auto prepare_result = conn_->prepare_cached(sql);
            if (!prepare_result) {
                return std::unexpected(prepare_result.error());
            }
            Statement* stmt = *prepare_result;
            stmt->reset();
            param_index = 1;
            if (auto set_result = bind_conditional_set<Members...>(stmt, proto, param_index); !set_result) {
                return std::unexpected(set_result.error());
            }
            return stmt;
        }

        // Empty-WHERE check + prepare + SET-then-WHERE bind. Shared by execute/to_sql.
        template <std::meta::info... Members>
        [[nodiscard]] auto ready_conditional_update(const T& proto, const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<Statement*, Error> {
            if (!where_expr) [[unlikely]] {
                return empty_where_error();
            }
            int  param_index = 0; // set by prepare_bind_set; WHERE bind continues from it
            auto stmt_result = prepare_bind_set<Members...>(
                    build_conditional_update_sql<Members...>(*where_expr), proto, param_index
            );
            if (!stmt_result) {
                return stmt_result;
            }
            Statement* stmt = *stmt_result;
            // WHERE params continue from param_index (same continuation as bind_having_params).
            if (auto where_result = Base::template bind_having_params<Statement, Error>(stmt, where_expr, param_index);
                !where_result) {
                return std::unexpected(where_result.error());
            }
            return stmt;
        }

        // Full-table UPDATE (#409): prepare + bind SET only (no WHERE). No empty-WHERE
        // guard — writing the whole table is the documented intent here.
        template <std::meta::info... Members>
        [[nodiscard]] auto ready_update_all(const T& proto) -> std::expected<Statement*, Error> {
            int param_index = 0; // out-param; no WHERE bind follows, so the final value is unused
            return prepare_bind_set<Members...>(build_update_all_sql<Members...>(), proto, param_index);
        }

        // Terminal shapes shared by the conditional and full-table paths: execute / expand.
        [[nodiscard]] static auto run_execute(std::expected<Statement*, Error> stmt_result)
                -> std::expected<void, Error> {
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            if (auto exec_result = (*stmt_result)->execute(); !exec_result) {
                return std::unexpected(exec_result.error());
            }
            return {};
        }
        [[nodiscard]] static auto run_to_sql(std::expected<Statement*, Error> stmt_result)
                -> std::expected<std::string, Error> {
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            return (*stmt_result)->expanded_sql();
        }

        template <std::meta::info... Members>
        [[nodiscard]] auto execute_where(const T& proto, const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<void, Error> {
            return run_execute(ready_conditional_update<Members...>(proto, where_expr));
        }

        template <std::meta::info... Members>
        [[nodiscard]] auto to_sql_where(const T& proto, const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<std::string, Error> {
            return run_to_sql(ready_conditional_update<Members...>(proto, where_expr));
        }

        template <std::meta::info... Members>
        [[nodiscard]] auto execute_all(const T& proto) -> std::expected<void, Error> {
            return run_execute(ready_update_all<Members...>(proto));
        }

        template <std::meta::info... Members>
        [[nodiscard]] auto to_sql_all(const T& proto) -> std::expected<std::string, Error> {
            return run_to_sql(ready_update_all<Members...>(proto));
        }

      private:
        std::shared_ptr<ConnType> conn_;
    };

} // namespace storm::orm::statements
