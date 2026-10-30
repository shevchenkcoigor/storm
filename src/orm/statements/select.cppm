module;

// Single cohesive class template; thresholds intentionally relaxed (see #264 finding).
// `duplicate` removed in #277 Phase 3 (build_sql() shared by to_sql/prepare_statement/rows_generator + collapsed
// coroutine step-loop).

#include <meta>
#include <cassert>
#include <plf_hive/plf_hive.h>

export module storm_orm_statements_select;

import std;

import storm_orm_generator;
import storm_orm_statements_aggregate;
import storm_orm_statements_base;
import storm_orm_statements_projection;
import storm_orm_statements_select_grammar;
import storm_orm_fields;
import storm_orm_statements_join;
import storm_orm_statements_orderby;
import storm_orm_transaction;
import storm_orm_utilities;
import storm_orm_where;
import storm_db_concept;
import storm_db_default_connection;

// Textual fragments of this module (#671): the m2m stitch-key helpers (included here) and
// the Query proxies (included inside the class body below). Both compile into this
// module's BMI; each #errors unless this macro is defined, so a stray #include elsewhere
// fails with one clear message.
#define STORM_SELECT_FRAGMENTS
#include "select_stitch.hpp"

export namespace storm::orm::statements {

    // Import utilities for code convenience
    using storm::orm::utilities::ConstexprString;

    // Selector gates shared by the projection / aggregate terminals in select_query.hpp.
    template <auto... S>
    concept ProjectionSelectors =
            (storm::meta::ValidSelector<S> && ...) && (storm::meta::SingleColumnSelector<S> && ...);

    template <auto... S>
    concept NumericAggregateSelectors =
            (storm::meta::ValidSelector<S> && ...) && AllNumericAggregateable<storm::meta::selector_info<S>()...>;

    // Statement class for ORM select operations
    template <typename T, storm::db::DatabaseConnection ConnType>
    class BasicSelectStatement : private BaseStatement<T> {
        friend class BaseStatement<T>; // Allow BaseStatement to access protected/private members
        using Base         = BaseStatement<T>;
        using Grammar      = SelectGrammar<T>;
        using Error        = typename ConnType::Error;
        using Statement    = typename ConnType::Statement;
        using PkKeyType    = detail::pk_key_type_t<T>;  // Primary key type for m2m stitch maps (#507)
        using Stitch       = SelectStitch<T, ConnType>; // m2m stitch-key helpers (#671, select_stitch.hpp)
        using StitchMapKey = typename Stitch::StitchMapKey;

      public:
        // Generate SELECT SQL string (compile-time computed, runtime accessible)
        static auto get_select_sql() -> const std::string& {
            return Grammar::select_sql_string;
        }

        explicit BasicSelectStatement(
                std::shared_ptr<ConnType> conn = storm::db::BasicDefaultConnection<ConnType>::get()
        )
            : conn_(std::move(conn)) {}

        [[nodiscard]] auto connection() const -> const std::shared_ptr<ConnType>& {
            return conn_;
        }

        // Build SQL string without preparing or binding (for testing/debugging).
        // Shared by to_sql(), prepare_statement() (dynamic path), and rows_generator().
        // Marked always_inline so the hot path retains the same codegen as the
        // previously-inlined builder (see #264 Phase 2 finding on call-overhead).
        // Templated on PkKeyType (#507) to support int64_t and UUID PKs.
        template <typename PkKeyType>
        [[nodiscard]] __attribute__((always_inline)) static auto build_sql(
                const std::optional<JoinStatementWrapper<PkKeyType>>& join_wrapper,
                const orm::where::ExpressionVariantPtr&               where_expr,
                const std::optional<int>&                             limit,
                const std::optional<int>&                             offset,
                const std::optional<OrderByWrapper>&                  order_by_wrapper
        ) -> std::string {
            // M2M joins (#391, #392): the eager load is 1 + N queries — Q1 selects
            // the base entities once, each relation's Q2 selects (owner_pk,
            // related.*) filtered by the same base subquery. build_sql is only a
            // debugging/introspection surface for m2m (.sql()); execution runs them
            // separately in execute_m2m_2query. Join them with "; " so .sql()
            // shows the full plan.
            if (join_wrapper && join_wrapper->is_m2m()) {
                std::string sql = join_wrapper->build_q1_sql_fn(where_expr, order_by_wrapper, limit, offset);
                for (const auto& rel : join_wrapper->m2m_relations) {
                    sql += "; ";
                    sql += rel.build_q2_sql_fn(where_expr, order_by_wrapper, limit, offset);
                }
                return sql;
            }
            std::string sql = join_wrapper ? join_wrapper->get_complete_sql() : std::string(get_select_sql());
            if (where_expr) {
                sql += " WHERE ";
                sql += orm::where::to_sql(*where_expr);
            }
            Base::template append_order_by<ConnType>(sql, order_by_wrapper);
            Base::template append_limit_offset<ConnType>(sql, limit, offset);
            return sql;
        }

        // Returns the SQL that would be executed by select()
        [[nodiscard]] auto
        to_sql(std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper     = std::nullopt,
               const orm::where::ExpressionVariantPtr&        where_expr       = nullptr,
               const std::optional<int>&                      limit            = std::nullopt,
               const std::optional<int>&                      offset           = std::nullopt,
               const std::optional<OrderByWrapper>&           order_by_wrapper = std::nullopt)
                -> std::expected<std::string, Error> {
            std::string sql = build_sql<PkKeyType>(join_wrapper, where_expr, limit, offset, order_by_wrapper);

            auto stmt_result = conn_->prepare_cached(sql);
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            auto* stmt_ptr = *stmt_result;
            stmt_ptr->reset();
            if (where_expr) {
                auto bind_result = Base::template bind_where_params<Statement, Error>(stmt_ptr, where_expr);
                if (!bind_result) {
                    return std::unexpected(bind_result.error());
                }
            }
            return stmt_ptr->expanded_sql();
        }

        // Returns the SQL that would be executed by first() (with LIMIT 1)
        [[nodiscard]] auto to_sql_first(
                std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper     = std::nullopt,
                const orm::where::ExpressionVariantPtr&        where_expr       = nullptr,
                const std::optional<int>&                      offset           = std::nullopt,
                const std::optional<OrderByWrapper>&           order_by_wrapper = std::nullopt
        ) -> std::expected<std::string, Error> {
            std::optional<int> const limit_one = 1;
            return to_sql(std::move(join_wrapper), where_expr, limit_one, offset, order_by_wrapper);
        }

        // Returns the SQL that would be executed by get() (with LIMIT 2)
        [[nodiscard]] auto to_sql_get(
                std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper     = std::nullopt,
                const orm::where::ExpressionVariantPtr&        where_expr       = nullptr,
                const std::optional<int>&                      offset           = std::nullopt,
                const std::optional<OrderByWrapper>&           order_by_wrapper = std::nullopt
        ) -> std::expected<std::string, Error> {
            std::optional<int> const limit_two = 2;
            return to_sql(std::move(join_wrapper), where_expr, limit_two, offset, order_by_wrapper);
        }

#include "select_query.hpp"

        [[nodiscard]] auto where(orm::where::ExpressionVariantPtr expr) -> Query {
            return {*this, std::nullopt, std::move(expr), std::nullopt, std::nullopt, std::nullopt};
        }

        // SELECT every row: no WHERE, JOIN, ORDER BY or LIMIT.
        [[nodiscard]] auto all() -> Query {
            return where(nullptr);
        }

        // Unified SELECT execution - handles all combinations of JOIN and WHERE
        // NOTE: The if/else branch is OUTSIDE the loop intentionally - checking inside would
        // add overhead per row (10k checks vs 1 check). Keep separate execute_query_loop calls.
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto
        execute(std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper     = std::nullopt,
                const orm::where::ExpressionVariantPtr&        where_expr       = nullptr,
                const std::optional<int>&                      limit            = std::nullopt,
                const std::optional<int>&                      offset           = std::nullopt,
                const std::optional<OrderByWrapper>&           order_by_wrapper = std::nullopt)
                -> std::expected<plf::hive<T>, Error> {
            QueryClauses const clauses{join_wrapper, where_expr, limit, offset, order_by_wrapper};
            // M2M eager loads use the two-query predicate-pushdown path (#391):
            // Q1 base entities + Q2 (owner_pk, related.*) stitched by a pk→entity map.
            // if constexpr gates instantiation: a model with no m2m field can never
            // receive an m2m wrapper, so execute_m2m_2query is not even compiled for it.
            if constexpr (Base::has_m2m_field_ || Base::has_reverse_fk_field_) {
                if (join_wrapper && join_wrapper->is_m2m()) {
                    return execute_m2m_2query(clauses);
                }
            }
            return prepare_and_dispatch(clauses, [this](Statement* stmt, const auto& extractor) {
                return execute_query_loop(stmt, extractor);
            });
        }

        // Zero-parameter fast path for first() — no checks, no parameter passing overhead
        // Called by QuerySet::first() when no WHERE/JOIN/ORDER BY/OFFSET modifiers are set
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto execute_one_fast()
                -> std::expected<std::optional<T>, Error> {
            auto prepare_result = conn_->prepare_cached(Grammar::select_limit1_sql_string);
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            Base::request_binary_results(*prepare_result, Base::pg_binary_safe_row_);
            return execute_single_row(*prepare_result, [](Statement* stmt, T& obj) {
                Base::extract_all_columns(stmt, obj);
            });
        }

        // first() with modifiers (WHERE/JOIN/ORDER BY/OFFSET) — applies LIMIT 1.
        // The `limit` parameter is accepted for API symmetry with execute()/execute_get()
        // and is intentionally overridden to 1; `[[maybe_unused]]` marks it as such.
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto execute_one(
                std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper     = std::nullopt,
                const orm::where::ExpressionVariantPtr&        where_expr       = nullptr,
                [[maybe_unused]] const std::optional<int>&     limit            = std::nullopt,
                const std::optional<int>&                      offset           = std::nullopt,
                const std::optional<OrderByWrapper>&           order_by_wrapper = std::nullopt
        ) -> std::expected<std::optional<T>, Error> {
            return execute_one_or_get<false>(join_wrapper, where_expr, offset, order_by_wrapper);
        }

        // Zero-parameter fast path for get() — no checks, no parameter passing overhead
        // Called by QuerySet::get() when no WHERE/JOIN/ORDER BY/OFFSET modifiers are set
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto execute_get_fast() -> std::expected<T, Error> {
            auto prepare_result = conn_->prepare_cached(Grammar::select_limit2_sql_string);
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            Base::request_binary_results(*prepare_result, Base::pg_binary_safe_row_);
            return execute_exact_one(*prepare_result, [](Statement* stmt, T& obj) {
                Base::extract_all_columns(stmt, obj);
            });
        }

        // get() with modifiers — applies LIMIT 2 (limit parameter ignored, see execute_one).
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto execute_get(
                std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper     = std::nullopt,
                const orm::where::ExpressionVariantPtr&        where_expr       = nullptr,
                [[maybe_unused]] const std::optional<int>&     limit            = std::nullopt,
                const std::optional<int>&                      offset           = std::nullopt,
                const std::optional<OrderByWrapper>&           order_by_wrapper = std::nullopt
        ) -> std::expected<T, Error> {
            return execute_one_or_get<true>(join_wrapper, where_expr, offset, order_by_wrapper);
        }

        // Lazy row-by-row iteration via coroutine — yields std::expected<T, Error> per row
        // Uses a DEDICATED (non-cached) statement to avoid conflicts with cached queries
        // The statement lives in the coroutine frame and is finalized on generator destruction
        auto rows_generator(
                std::shared_ptr<ConnType>                      conn,
                std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper     = std::nullopt,
                orm::where::ExpressionVariantPtr               where_expr       = nullptr,
                std::optional<int>                             limit            = std::nullopt,
                std::optional<int>                             offset           = std::nullopt,
                std::optional<OrderByWrapper>                  order_by_wrapper = std::nullopt
        ) -> storm::generator<std::expected<T, Error>&&> {
            // M2M (#391): the two-query predicate-pushdown load needs the full base
            // set before Q2 can run, so true streaming is impossible — materialize
            // eagerly, then yield each entity. Q1+Q2 run in a transaction inside
            // execute_m2m_2query. if constexpr gates instantiation for non-m2m models.
            if constexpr (Base::has_m2m_field_ || Base::has_reverse_fk_field_) {
                if (join_wrapper && join_wrapper->is_m2m()) {
                    return rows_m2m_materialized(
                            std::move(conn),
                            std::move(*join_wrapper), // owns the relation vector — move into the coroutine frame
                            std::move(where_expr),
                            limit,
                            offset,
                            std::move(order_by_wrapper)
                    );
                }
            }

            std::string sql = build_sql(join_wrapper, where_expr, limit, offset, order_by_wrapper);

            auto stmt_result = conn->prepare(sql);
            if (!stmt_result) {
                return yield_error(stmt_result.error());
            }
            auto stmt = std::move(stmt_result.value());

            if (where_expr) {
                auto bind_result = Base::template bind_where_params<Statement, Error>(&stmt, where_expr);
                if (!bind_result) {
                    return yield_error(bind_result.error());
                }
            }
            // Same rule as execute()'s dispatch (#600), !join_wrapper included and
            // equally defensive-only. The statement is dedicated (prepare(), not the
            // cache) and moves into the coroutine frame from here, so the flag has to
            // survive Statement's swap-based move — which is why swap() carries it.
            Base::request_binary_results(&stmt, Base::pg_binary_safe_row_ && !join_wrapper);

            // conn and stmt move into the selected coroutine's frame — the statement
            // (and the connection it needs) live until generator destruction.
            return rows_plain_loop(std::move(conn), std::move(stmt), std::move(join_wrapper));
        }

      private:
        // Single-error generator for prepare/bind failures in rows_generator.
        static auto yield_error(Error error) -> storm::generator<std::expected<T, Error>&&> {
            co_yield std::unexpected(std::move(error));
        }

        // M2M rows() (#391): eager 2-query load, then yield each entity. Builds a
        // SelectStatement on the passed connection to reuse execute_m2m_2query.
        static auto rows_m2m_materialized(
                std::shared_ptr<ConnType>        conn,
                JoinStatementWrapper<PkKeyType>  join_wrapper,
                orm::where::ExpressionVariantPtr where_expr,
                std::optional<int>               limit,
                std::optional<int>               offset,
                std::optional<OrderByWrapper>    order_by_wrapper
        ) -> storm::generator<std::expected<T, Error>&&> {
            BasicSelectStatement                           self{std::move(conn)};
            std::optional<JoinStatementWrapper<PkKeyType>> wrapper_opt{join_wrapper};
            QueryClauses const clauses{wrapper_opt, where_expr, limit, offset, order_by_wrapper};
            auto               rows = self.execute_m2m_2query(clauses);
            if (!rows) {
                co_yield std::unexpected(std::move(rows.error()));
                co_return;
            }
            for (auto it = rows->begin(); it != rows->end(); ++it) {
                co_yield std::move(*it);
            }
        }

        // Single step/yield loop — extraction differs only in one line between
        // the join and non-join cases. co_yield must stay in the coroutine
        // body (not in a lambda), so we branch only on the per-row extract.
        static auto rows_plain_loop(
                std::shared_ptr<ConnType> /*conn*/,
                Statement                                      stmt,
                std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper
        ) -> storm::generator<std::expected<T, Error>&&> {
            int step_result = 0;
            while ((step_result = stmt.step_raw()) == Statement::ROW_AVAILABLE) {
                T obj;
                if (join_wrapper) {
                    join_wrapper->extract_row(&stmt, &obj);
                } else {
                    Base::extract_all_columns(&stmt, obj);
                }
                co_yield std::move(obj);
            }
            if (step_result != Statement::NO_MORE_ROWS) {
                co_yield std::unexpected(Error{step_result, stmt.get_error_message()});
            }
        }

        // =====================================================================
        // STATEMENT PREPARATION - Unified caching for all query types
        // =====================================================================

        // Prepare and cache statement based on query type
        // Unified approach: simple SELECT uses dedicated cache, all others use string-based cache
        // OPTIMIZATION: WHERE expression address caching - skips SQL building AND param binding
        // when the same expression is used repeatedly (sqlite3_reset preserves bindings)
        // Bind WHERE parameters to the given statement and return any binding error in
        // the shape callers expect.
        [[nodiscard]] __attribute__((always_inline)) auto
        bind_where_or_propagate(Statement* stmt, const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<void, Error> {
            auto bind_result = Base::template bind_where_params<Statement, Error>(stmt, where_expr);
            if (!bind_result) [[unlikely]] {
                return std::unexpected(bind_result.error());
            }
            return {};
        }

        // Fast path: simple SELECT (no WHERE / JOIN / modifiers). Prepares the static SQL.
        [[nodiscard]] __attribute__((always_inline)) auto prepare_simple_path() -> std::expected<Statement*, Error> {
            return conn_->prepare_cached(get_select_sql());
        }

        // Dynamic path: prepare statement keyed by SQL string, then bind WHERE
        // params if any. Always_inline so the call site keeps the same codegen as the
        // previously-inlined builder (see #264 Phase 2 finding).
        [[nodiscard]] __attribute__((always_inline)) auto
        prepare_and_bind(std::string sql, const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<Statement*, Error> {
            auto prepare_result = conn_->prepare_cached(sql);
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(Error{-1, "Failed to prepare statement"});
            }
            Statement* stmt = *prepare_result;
            if (where_expr) {
                if (auto r = bind_where_or_propagate(stmt, where_expr); !r) {
                    return std::unexpected(r.error());
                }
            }
            return stmt;
        }

        // Each of the three stage-selectors lives in its own consteval-friendly
        // inline helper. Pulling the boolean expressions out of prepare_statement
        // keeps its cyclomatic complexity to one branch per dispatch arm.
        [[nodiscard]] __attribute__((always_inline)) static auto is_simple_select(
                const std::optional<JoinStatementWrapper<PkKeyType>>& join_wrapper,
                const orm::where::ExpressionVariantPtr&               where_expr,
                const std::optional<int>&                             limit,
                const std::optional<int>&                             offset,
                const std::optional<OrderByWrapper>&                  order_by_wrapper
        ) -> bool {
            const bool has_modifiers = order_by_wrapper.has_value() || limit.has_value() || offset.has_value();
            const bool has_filters   = where_expr || join_wrapper.has_value();
            return !has_filters && !has_modifiers;
        }

        [[nodiscard]] __attribute__((always_inline)) auto prepare_statement(
                const std::optional<JoinStatementWrapper<PkKeyType>>& join_wrapper,
                const orm::where::ExpressionVariantPtr&               where_expr,
                const std::optional<int>&                             limit,
                const std::optional<int>&                             offset,
                const std::optional<OrderByWrapper>&                  order_by_wrapper
        ) -> std::expected<Statement*, Error> {
            if (is_simple_select(join_wrapper, where_expr, limit, offset, order_by_wrapper)) {
                return prepare_simple_path();
            }
            // NOTE: Do NOT add sql.reserve() here - benchmarks show ~2% regression due to
            // extra function call overhead outweighing reallocation savings for typical SQL sizes
            return prepare_and_bind(
                    build_sql<PkKeyType>(join_wrapper, where_expr, limit, offset, order_by_wrapper), where_expr
            );
        }

        // =====================================================================
        // QUERY LOOPS
        // =====================================================================

        // Unified query loop template - handles both direct and JOIN extraction
        // DATABASE-AGNOSTIC: Uses Statement methods with templates for cross-module inlining
        // Extractor signature: void(Statement* stmt, T& obj)
        template <typename Extractor>
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto
        execute_query_loop(Statement* stmt, const Extractor& extractor) noexcept -> std::expected<plf::hive<T>, Error> {
            plf::hive<T> results;
            int          step_result = 0;

            // DATABASE-AGNOSTIC: Template step_raw() enables cross-module inlining
            while ((step_result = stmt->step_raw()) == Statement::ROW_AVAILABLE) {
                T obj;
                extractor(stmt, obj);
                results.insert(std::move(obj));
            }

            return finish_query_loop(stmt, step_result, std::move(results));
        }

        // Shared loop tail: classify the final step result, reset, return rows or error.
        // Used by execute_query_loop and execute_m2m_loop.
        [[nodiscard]] __attribute__((always_inline)) static auto
        finish_query_loop(Statement* stmt, int step_result, plf::hive<T>&& results) noexcept
                -> std::expected<plf::hive<T>, Error> {
            if (step_result != Statement::NO_MORE_ROWS) {
                stmt->reset();
                return std::unexpected(Error{step_result, stmt->get_error_message()});
            }
            stmt->reset();
            return std::move(results);
        }

        // Bundles the five query clauses threaded from QuerySet through the proxies
        // (reference semantics — call-scoped only). Templated on PkKeyType (#507).
        struct QueryClauses {
            const std::optional<JoinStatementWrapper<PkKeyType>>& join_wrapper;
            const orm::where::ExpressionVariantPtr&               where_expr;
            const std::optional<int>&                             limit;
            const std::optional<int>&                             offset;
            const std::optional<OrderByWrapper>&                  order_by_wrapper;
        };

        // Prepare the statement (cached, WHERE bound), then dispatch to the FK-join
        // extraction path or the plain-columns path. M2M eager loads are handled by
        // execute_m2m_2query (#391) BEFORE this is reached, so there is no m2m arm
        // here. Shared by execute / execute_one / execute_get.
        template <typename LoopFn>
        [[nodiscard]] __attribute__((always_inline)) auto
        prepare_and_dispatch(const QueryClauses& clauses, const LoopFn& loop_fn)
                -> decltype(loop_fn(std::declval<Statement*>(), std::declval<void (*)(Statement*, T&)>())) {
            auto prepare_result = prepare_statement(
                    clauses.join_wrapper, clauses.where_expr, clauses.limit, clauses.offset, clauses.order_by_wrapper
            );
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            const auto& join_wrapper = clauses.join_wrapper;
            // #600: a joined row carries the JOINED model's columns too, and the
            // wrapper is type-erased by the time it reaches here — so only an
            // unjoined row can be classified. A join keeps the text path.
            // !join_wrapper is defensive rather than load-bearing: an FK join can
            // only exist on a model with an FK member, which member_pg_binary_safe
            // already forces unsafe, so the left operand is false there anyway.
            Base::request_binary_results(*prepare_result, Base::pg_binary_safe_row_ && !join_wrapper);
            if (join_wrapper) {
                return loop_fn(*prepare_result, [&join_wrapper](Statement* stmt, T& obj) {
                    join_wrapper->extract_row(stmt, &obj);
                });
            }
            return loop_fn(*prepare_result, [](Statement* stmt, T& obj) { Base::extract_all_columns(stmt, obj); });
        }

        // Shared body of execute_one (ExactOne=false, LIMIT 1 → optional<T>) and
        // execute_get (ExactOne=true, LIMIT 2 → T or 0/>1-row error). The LIMIT is
        // applied here; for m2m joins it lands INSIDE the base-table subquery, so it
        // bounds base entities, never the related collection (#203).
        template <bool ExactOne>
        [[nodiscard]] auto execute_one_or_get(
                const std::optional<JoinStatementWrapper<PkKeyType>>& join_wrapper,
                const orm::where::ExpressionVariantPtr&               where_expr,
                const std::optional<int>&                             offset,
                const std::optional<OrderByWrapper>&                  order_by_wrapper
        ) -> std::expected<std::conditional_t<ExactOne, T, std::optional<T>>, Error> {
            std::optional<int> const limit_value = ExactOne ? 2 : 1;
            QueryClauses const       clauses{join_wrapper, where_expr, limit_value, offset, order_by_wrapper};
            // M2M first()/get() (#391): the LIMIT lands inside Q1's base subquery, so
            // it bounds base entities (1 or 2), then the eager 2-query load + stitch
            // returns fully-populated entities — multiple related rows for one entity
            // are one result, never a uniqueness violation. if constexpr gates
            // instantiation for non-m2m models.
            if constexpr (Base::has_m2m_field_ || Base::has_reverse_fk_field_) {
                if (join_wrapper && join_wrapper->is_m2m()) {
                    auto rows = execute_m2m_2query(clauses);
                    if (!rows) {
                        return std::unexpected(rows.error());
                    }
                    return Stitch::template m2m_one_from_hive<ExactOne>(std::move(*rows));
                }
            }
            return prepare_and_dispatch(clauses, [this](Statement* stmt, const auto& extractor) {
                if constexpr (ExactOne) {
                    return execute_exact_one(stmt, extractor);
                } else {
                    return execute_single_row(stmt, extractor);
                }
            });
        }

        // M2M two-query predicate-pushdown execution (#391, #392). Q1 loads the
        // base entities once; each relation's Q2 loads (owner_pk, related.*)
        // filtered by the same base subquery; all are stitched client-side
        // through one pk→entity map. Everything runs inside a transaction for
        // snapshot consistency. INNER drops entities empty in any inner
        // relation after the stitch; LEFT keeps them.
        [[nodiscard]] auto execute_m2m_2query(const QueryClauses& c) noexcept -> std::expected<plf::hive<T>, Error> {
            const auto& wrapper = *c.join_wrapper;

            auto txn = storm::orm::utilities::TransactionGuard<ConnType>::begin(conn_);
            if (!txn) {
                return std::unexpected(txn.error());
            }

            // Q1 — base entities. plf::hive keeps pointers stable across inserts,
            // so the pk→entity map can hold T* into the result hive.
            auto q1 = run_q1(wrapper, c);
            if (!q1) {
                return std::unexpected(q1.error());
            }
            plf::hive<T> results = std::move(*q1);

            // Key type is picked at compile time (see SelectStitch::StitchMapKey): a single-column
            // PK — integer or storm::UUID (#507) — narrows back to one uint64_t word,
            // a composite PK (#504) keeps the full StitchKey buffer.
            std::unordered_map<StitchMapKey, T*> by_pk;
            by_pk.reserve(results.size());
            for (T& obj : results) {
                by_pk.emplace(Stitch::narrow_stitch_key(Stitch::build_owner_stitch_key(obj)), &obj);
            }

            // Q2 per relation (#392) — related rows, stitched into their owner's
            // container through the shared map.
            for (const auto& rel : wrapper.m2m_relations) {
                if (auto stitched = run_q2_stitch(rel, c, by_pk); !stitched) {
                    return std::unexpected(stitched.error());
                }
            }

            // INNER semantics: drop entities empty in ANY inner relation.
            Stitch::drop_empty_relations(results, wrapper);

            if (auto committed = txn->commit(); !committed) {
                return std::unexpected(committed.error());
            }
            return std::move(results);
        }

        // Prepare a clause-built SQL (cached), reset it, and bind the WHERE params
        // if present. Shared preamble of Q1 and Q2 — they differ only in the SQL
        // builder (build_q1_sql_fn vs build_q2_sql_fn) and what they do with the
        // returned statement.
        [[nodiscard]] auto prepare_clause_sql(M2MClauseSqlFn build_fn, const QueryClauses& c) noexcept
                -> std::expected<Statement*, Error> {
            std::string sql  = build_fn(c.where_expr, c.order_by_wrapper, c.limit, c.offset);
            auto        prep = conn_->prepare_cached(sql);
            if (!prep) {
                return std::unexpected(prep.error());
            }
            Statement* stmt = *prep;
            stmt->reset();
            if (c.where_expr) {
                if (auto bound = bind_where_or_propagate(stmt, c.where_expr); !bound) {
                    return std::unexpected(bound.error());
                }
            }
            return stmt;
        }

        // Q1: prepare the base subquery, bind WHERE, extract all entities.
        [[nodiscard]] auto run_q1(const JoinStatementWrapper<PkKeyType>& wrapper, const QueryClauses& c) noexcept
                -> std::expected<plf::hive<T>, Error> {
            auto stmt = prepare_clause_sql(wrapper.build_q1_sql_fn, c);
            if (!stmt) {
                return std::unexpected(stmt.error());
            }
            // Q1 returns the BASE entity's own columns and nothing else, so it
            // takes the base model's classification (#600). Q2 — the related /
            // owner rows — deliberately does not: that model is not in scope here.
            Base::request_binary_results(*stmt, Base::pg_binary_safe_row_);
            return execute_query_loop(*stmt, [](Statement* s, T& obj) { Base::extract_all_columns(s, obj); });
        }

        // Q2: prepare one relation's junction⋈related query, bind the SAME WHERE
        // (its IN-subquery), step rows, append each related object to its owner.
        // No key template parameter (#504): every PK shape — integer, storm::UUID
        // (#507), and composite — keys the map on the single StitchKey type.
        [[nodiscard]] auto run_q2_stitch(
                const M2MRelation& rel, const QueryClauses& c, std::unordered_map<StitchMapKey, T*>& by_pk
        ) noexcept -> std::expected<void, Error> {
            auto prep = prepare_clause_sql(rel.build_q2_sql_fn, c);
            if (!prep) {
                return std::unexpected(prep.error());
            }
            Statement* stmt        = *prep;
            int        step_result = 0;
            while ((step_result = stmt->step_raw()) == Statement::ROW_AVAILABLE) {
                const auto owner = Stitch::extract_owner_key(rel, stmt);
                if (auto it = by_pk.find(owner); it != by_pk.end()) {
                    rel.append_related_q2_fn(stmt, it->second);
                }
            }
            if (step_result != Statement::NO_MORE_ROWS) {
                Error err{step_result, stmt->get_error_message()};
                stmt->reset();
                return std::unexpected(std::move(err));
            }
            stmt->reset();
            return {};
        }

      public:
        // =====================================================================
        // SINGLE-ROW QUERY LOOPS
        // =====================================================================

        // Step once and classify the result.
        //   true  → ROW_AVAILABLE (caller extracts)
        //   false → NO_MORE_ROWS (caller decides: nullopt vs "no row found" error)
        //   error → driver/binding error (statement already reset)
        // Shared by execute_single_row and execute_exact_one — both used to spell out
        // the same NO_MORE_ROWS / ROW_AVAILABLE / error triage inline.
        [[nodiscard]] __attribute__((always_inline)) static auto step_first_row(Statement* stmt) noexcept
                -> std::expected<bool, Error> {
            int const step_result = stmt->step_raw();
            if (step_result == Statement::ROW_AVAILABLE) {
                return true;
            }
            if (step_result == Statement::NO_MORE_ROWS) {
                stmt->reset();
                return false;
            }
            stmt->reset();
            return std::unexpected(Error{step_result, stmt->get_error_message()});
        }

        // Single-row query execution - fetches at most one row, returns optional
        // DATABASE-AGNOSTIC: Uses Statement methods with templates for cross-module inlining
        template <typename Extractor>
        [[nodiscard]] __attribute__((hot)) auto execute_single_row(Statement* stmt, const Extractor& extractor) noexcept
                -> std::expected<std::optional<T>, Error> {
            auto step = step_first_row(stmt);
            if (!step) [[unlikely]] {
                return std::unexpected(step.error());
            }
            if (!*step) {
                return std::optional<T>{std::nullopt};
            }

            T obj;
            extractor(stmt, obj);
            stmt->reset();

            return std::optional<T>{std::move(obj)};
        }

        // Exact-one query execution for get() - returns T or error on 0/>1 rows
        // Avoids plf::hive allocation — just steps up to 2 rows inline
        template <typename Extractor>
        [[nodiscard]] __attribute__((hot)) auto execute_exact_one(Statement* stmt, const Extractor& extractor) noexcept
                -> std::expected<T, Error> {
            auto step = step_first_row(stmt);
            if (!step) [[unlikely]] {
                return std::unexpected(step.error());
            }
            if (!*step) {
                return std::unexpected(Error{-1, "No row found matching query"});
            }

            T obj;
            extractor(stmt, obj);

            // Check for second row (LIMIT 2 ensures at most 2 steps)
            int const second_step = stmt->step_raw();
            stmt->reset();

            if (second_step == Statement::ROW_AVAILABLE) [[unlikely]] {
                return std::unexpected(Error{-1, "Multiple rows found matching query"});
            }

            return std::move(obj);
        }

        // SQL clause helpers are inherited from BaseStatement (append_order_by, append_limit_offset, bind_where_params)

        std::shared_ptr<ConnType> conn_;
    };

} // namespace storm::orm::statements
