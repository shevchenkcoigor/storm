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
import storm_orm_statements_field_names;
import storm_orm_fields;
import storm_orm_statements_join;
import storm_orm_statements_orderby;
import storm_orm_transaction;
import storm_orm_utilities;
import storm_orm_where;
import storm_db_concept;
import storm_db_default_connection;

export namespace storm::orm::statements {

    // Import utilities for code convenience
    using storm::orm::utilities::ConstexprString;

    // Selector gates shared by the projection / aggregate terminals below.
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
        using Base      = BaseStatement<T>;
        using Error     = typename ConnType::Error;
        using Statement = typename ConnType::Statement;
        using PkKeyType = detail::pk_key_type_t<T>; // Primary key type for m2m stitch maps (#507)

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

      public:
        // Generate SELECT SQL string (compile-time computed, runtime accessible)
        static auto get_select_sql() -> const std::string& {
            return select_sql_string;
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

        // Proxy types returned by factory methods (used by QuerySet one-liner delegations).
        // QueryBase::forward() funnels the five stored fields into any callable, so the
        // derived Query/FirstQuery/GetQuery don't each spell the field list out — that
        // five-line forwarding block used to repeat four times.
        struct QueryBase {
            BasicSelectStatement                           stmt;
            std::optional<JoinStatementWrapper<PkKeyType>> join_wrapper;
            orm::where::ExpressionVariantPtr               where_expr;
            std::optional<int>                             limit_value;
            std::optional<int>                             offset_value;
            std::optional<OrderByWrapper>                  order_by_wrapper;

            template <typename Fn> __attribute__((always_inline)) auto forward(Fn&& callback) -> decltype(auto) {
                return std::forward<Fn>(callback)(
                        this->join_wrapper,
                        this->where_expr,
                        this->limit_value,
                        this->offset_value,
                        this->order_by_wrapper
                );
            }

            // No WHERE / JOIN / ORDER BY / OFFSET: the only shape first()/get() may serve from the
            // cached fast statement. Re-checked at execute time, since chain methods inherited by
            // FirstQuery/GetQuery can add any of these after first()/get() was called.
            [[nodiscard]] auto is_plain() const -> bool {
                return !where_expr && !join_wrapper && !order_by_wrapper && !offset_value.has_value();
            }

            // Chain methods mirror QuerySet. `this Self self` takes the proxy by value: an
            // lvalue is copied (the original stays reusable, Django-style), an rvalue moved.
            template <typename Self>
            [[nodiscard]] auto where(this Self self, orm::where::ExpressionVariantPtr expr) -> Self {
                self.where_expr = orm::where::and_combine(std::move(self.where_expr), std::move(expr));
                return self;
            }

            template <typename Self> [[nodiscard]] auto limit(this Self self, int n) -> Self {
                // Tracked by #225: becomes a P2900 `pre(n >= 0)` contract once clang-p2996 supports it.
                assert(n >= 0 && "limit() requires a non-negative value");
                self.limit_value = n;
                return self;
            }

            template <typename Self> [[nodiscard]] auto offset(this Self self, int n) -> Self {
                assert(n >= 0 && "offset() requires a non-negative value");
                self.offset_value = n;
                return self;
            }

            // #570: constrained here so a composite-PK FK selector is diagnosed at this call site.
            template <auto... Args, typename Self>
                requires OrderBySelectorsAreSingleColumn<Args...>
            [[nodiscard]] auto order_by(this Self self) -> Self {
                self.order_by_wrapper = make_order_by_wrapper<Args...>();
                return self;
            }

            template <auto... S, typename Self>
                requires JoinableFields<T, storm::meta::selector_info<S>()...>
            [[nodiscard]] auto join(this Self self) -> Self {
                self.join_wrapper = make_joined<JoinType::Inner, storm::meta::selector_info<S>()...>();
                return self;
            }

            template <auto... S, typename Self>
                requires JoinableFields<T, storm::meta::selector_info<S>()...>
            [[nodiscard]] auto left_join(this Self self) -> Self {
                self.join_wrapper = make_joined<JoinType::Left, storm::meta::selector_info<S>()...>();
                return self;
            }

          private:
            // Shared body of join() / left_join(): the m2m, reverse-FK or plain FK wrapper.
            template <JoinType Type, std::meta::info... FKFields>
            static auto make_joined() -> JoinStatementWrapper<PkKeyType> {
                if constexpr ((M2MFieldOf<T, FKFields> && ...)) {
                    return make_m2m_join_wrapper<T, ConnType, Type, FKFields...>();
                } else if constexpr ((ReverseFKJoinable<T, FKFields> && ...)) {
                    return make_reverse_fk_join_wrapper<T, ConnType, Type, FKFields...>();
                } else {
                    return make_join_wrapper<T, ConnType, Type, FKFields...>();
                }
            }
        };

        struct Query : QueryBase {
            // Projection / aggregate terminals — each hands the accumulated state to its own
            // statement, so `SelectStatement<T>{}.where(c).count<...>()` equals `QuerySet.where(c).count<...>()`.
            template <auto... S>
                requires ProjectionSelectors<S...>
            [[nodiscard]] auto distinct() const {
                if constexpr (sizeof...(S) == 0) {
                    return projection<DistinctStatement, BaseStatement<T>::primary_key_>();
                } else {
                    return projection<DistinctStatement, storm::meta::selector_info<S>()...>();
                }
            }

            template <auto... S>
                requires(sizeof...(S) > 0 && ProjectionSelectors<S...>)
            [[nodiscard]] auto values() const {
                return projection<ValuesStatement, storm::meta::selector_info<S>()...>();
            }

            template <auto... S>
                requires(sizeof...(S) > 0 && ProjectionSelectors<S...>)
            [[nodiscard]] auto group_by() const {
                return GroupByBuilder<T, ConnType, storm::meta::selector_info<S>()...>{aggregate_params(true)};
            }

            template <auto... S>
                requires NumericAggregateSelectors<S...>
            [[nodiscard]] auto sum() const {
                return aggregate<AggregateType::SUM, storm::meta::selector_info<S>()...>();
            }

            template <auto... S>
                requires ProjectionSelectors<S...>
            [[nodiscard]] auto count() const {
                return aggregate<AggregateType::COUNT, storm::meta::selector_info<S>()...>();
            }

            template <auto... S>
                requires NumericAggregateSelectors<S...>
            [[nodiscard]] auto avg() const {
                return aggregate<AggregateType::AVG, storm::meta::selector_info<S>()...>();
            }

            template <auto... S>
                requires NumericAggregateSelectors<S...>
            [[nodiscard]] auto min() const {
                return aggregate<AggregateType::MIN, storm::meta::selector_info<S>()...>();
            }

            template <auto... S>
                requires NumericAggregateSelectors<S...>
            [[nodiscard]] auto max() const {
                return aggregate<AggregateType::MAX, storm::meta::selector_info<S>()...>();
            }

            template <auto S>
                requires ProjectionSelectors<S>
            [[nodiscard]] auto count_distinct() const {
                return aggregate<AggregateType::COUNT_DISTINCT, storm::meta::selector_info<S>()>();
            }

            // first() / get() consume a copy of the proxy (lvalue) or the proxy itself (rvalue).
            [[nodiscard]] auto first(this Query self) {
                return FirstQuery{{std::move(self)}};
            }

            [[nodiscard]] auto get(this Query self) {
                return GetQuery{{std::move(self)}};
            }

            // Lazy row-by-row iteration — no full materialization.
            [[nodiscard]] auto rows() {
                return this->stmt.rows_generator(
                        this->stmt.conn_,
                        this->join_wrapper,
                        this->where_expr,
                        this->limit_value,
                        this->offset_value,
                        this->order_by_wrapper
                );
            }

          private:
            template <
                    template <typename, storm::db::DatabaseConnection, std::meta::info...> class Projection,
                    std::meta::info... Fields>
            [[nodiscard]] auto projection() const {
                return Projection<T, ConnType, Fields...>{
                        this->stmt.conn_,
                        this->where_expr,
                        this->join_wrapper,
                        this->limit_value,
                        this->offset_value,
                        this->order_by_wrapper
                };
            }

            [[nodiscard]] auto aggregate_params(bool with_clauses) const -> AggregateParams<T, ConnType> {
                return {this->stmt.conn_,
                        this->where_expr,
                        this->join_wrapper,
                        with_clauses ? this->limit_value : std::optional<int>{},
                        with_clauses ? this->offset_value : std::optional<int>{},
                        with_clauses ? this->order_by_wrapper : std::optional<OrderByWrapper>{},
                        nullptr};
            }

            template <AggregateType Type, std::meta::info... Fields> [[nodiscard]] auto aggregate() const {
                return AggregateStatement<T, ConnType, NoGroupBy, AggregateOp<Type, Fields...>>{
                        aggregate_params(false)
                };
            }

          public:
            [[nodiscard]] auto execute() -> std::expected<plf::hive<T>, Error> {
                return this->forward([this](auto&... args) { return this->stmt.execute(args...); });
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return this->forward([this](auto&... args) { return this->stmt.to_sql(args...); });
            }
            [[nodiscard]] auto sql() -> std::string {
                return this->forward([](auto&... args) { return build_sql(args...); });
            }
        };

        struct FirstQuery : QueryBase {
            [[nodiscard]] auto execute() -> std::expected<std::optional<T>, Error> {
                if (this->is_plain()) {
                    return this->stmt.execute_one_fast();
                }
                return this->forward([this](auto&... args) { return this->stmt.execute_one(args...); });
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return this->stmt
                        .to_sql_first(this->join_wrapper, this->where_expr, this->offset_value, this->order_by_wrapper);
            }
            [[nodiscard]] auto sql() -> std::string {
                std::optional<int> const limit_one = 1;
                return build_sql(
                        this->join_wrapper, this->where_expr, limit_one, this->offset_value, this->order_by_wrapper
                );
            }
        };

        struct GetQuery : QueryBase {
            [[nodiscard]] auto execute() -> std::expected<T, Error> {
                if (this->is_plain()) {
                    return this->stmt.execute_get_fast();
                }
                return this->forward([this](auto&... args) { return this->stmt.execute_get(args...); });
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return this->stmt
                        .to_sql_get(this->join_wrapper, this->where_expr, this->offset_value, this->order_by_wrapper);
            }
            [[nodiscard]] auto sql() -> std::string {
                std::optional<int> const limit_two = 2;
                return build_sql(
                        this->join_wrapper, this->where_expr, limit_two, this->offset_value, this->order_by_wrapper
                );
            }
        };

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
            auto prepare_result = conn_->prepare_cached(select_limit1_sql_string);
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
            auto prepare_result = conn_->prepare_cached(select_limit2_sql_string);
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
                    return m2m_one_from_hive<ExactOne>(std::move(*rows));
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

        // Reduce a 2-query m2m hive (already LIMIT-bounded to 1 or 2 base entities)
        // to the first()/get() result. ExactOne=false → optional<T>; ExactOne=true →
        // T with 0-row / >1-row errors.
        template <bool ExactOne>
        [[nodiscard]] auto m2m_one_from_hive(plf::hive<T>&& rows) noexcept
                -> std::expected<std::conditional_t<ExactOne, T, std::optional<T>>, Error> {
            if constexpr (ExactOne) {
                if (rows.empty()) {
                    return std::unexpected(Error{-1, "No row found matching query"});
                }
                if (rows.size() > 1) {
                    return std::unexpected(Error{-1, "Multiple rows found matching query"});
                }
                return std::move(*rows.begin());
            } else {
                if (rows.empty()) {
                    return std::optional<T>{std::nullopt};
                }
                return std::optional<T>{std::move(*rows.begin())};
            }
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

            // Key type is picked at compile time (see StitchMapKey): a single-column
            // PK — integer or storm::UUID (#507) — narrows back to one uint64_t word,
            // a composite PK (#504) keeps the full StitchKey buffer.
            std::unordered_map<StitchMapKey, T*> by_pk;
            by_pk.reserve(results.size());
            for (T& obj : results) {
                by_pk.emplace(narrow_stitch_key(build_owner_stitch_key(obj)), &obj);
            }

            // Q2 per relation (#392) — related rows, stitched into their owner's
            // container through the shared map.
            for (const auto& rel : wrapper.m2m_relations) {
                if (auto stitched = run_q2_stitch(rel, c, by_pk); !stitched) {
                    return std::unexpected(stitched.error());
                }
            }

            // INNER semantics: drop entities empty in ANY inner relation.
            drop_empty_relations(results, wrapper);

            if (auto committed = txn->commit(); !committed) {
                return std::unexpected(committed.error());
            }
            return std::move(results);
        }

        // The stitch map's key type (#504 perf). Both the Q1 insert and the Q2 probe
        // build a StitchKey, but a SINGLE-column PK — the overwhelmingly common shape
        // — fills exactly one 8-byte word of it, so the map is keyed on that word
        // instead of the 33-byte class. That is not a micro-optimization: it restores
        // libc++'s identity std::hash<integral> (which compiles to nothing) in place
        // of hash<StitchKey>'s FNV loop, and an inlined one-instruction operator== in
        // place of an out-of-line word-compare call. Both run once per Q2 row, and
        // together they were the entire +3.95% the StitchKey migration cost the
        // single-PK path. Composite models keep the full key — correctness there is
        // never traded for the narrowing (see narrow_stitch_key).
        static constexpr bool narrow_stitch_key_ = Base::primary_key_column_count_ == 1;

        using StitchMapKey = std::conditional_t<narrow_stitch_key_, std::uint64_t, storm::orm::utilities::StitchKey>;

        // Narrow a built StitchKey to the map's key type. For a composite PK this is
        // the identity — every part is retained, so distinct composite keys stay
        // distinct and can never mis-stitch. Only the provably single-part case takes
        // the word (StitchKey::first_word asserts that in debug).
        [[nodiscard]] static auto narrow_stitch_key(const storm::orm::utilities::StitchKey& key) noexcept
                -> StitchMapKey {
            if constexpr (narrow_stitch_key_) {
                return key.first_word();
            } else {
                return key;
            }
        }

        // Read a Q2 row's owner key through whichever extractor the descriptor carries.
        // make_relation_descriptor populates exactly one of the pair, keyed on the same
        // PK width narrow_stitch_key_ tests, so the branch taken here always matches the
        // one that was set — asserted rather than left implicit, since calling the null
        // one would be a null-fn-pointer jump.
        [[nodiscard]] static auto extract_owner_key(const M2MRelation& rel, Statement* stmt) noexcept -> StitchMapKey {
            if constexpr (narrow_stitch_key_) {
                assert(rel.extract_q2_owner_pk_word_fn != nullptr);
                return rel.extract_q2_owner_pk_word_fn(stmt);
            } else {
                assert(rel.extract_q2_owner_pk_fn != nullptr);
                return rel.extract_q2_owner_pk_fn(stmt);
            }
        }

        // Build the Q1 hash-map key for `obj`'s own primary key (#504 Task 8) — the
        // SAME logical key TwoQueryJoinBase::extract_q2_owner_pk builds from a Q2
        // row's owner-key columns, so a matching row stitches correctly. Each PK
        // part is dispatched to append_int64/append_string by ITS OWN stored type
        // (mirrors bind_one_pk_part in base.cppm and the statement-side extractor in
        // join.cppm); an FK part's stitch value is the referenced row's own key
        // (matching the "<name>_id" column both sides key on), not the FK member's
        // whole C++ object.
        [[nodiscard]] static auto build_owner_stitch_key(const T& obj) noexcept -> storm::orm::utilities::StitchKey {
            storm::orm::utilities::StitchKey key;
            build_owner_stitch_key_impl(key, obj, std::make_index_sequence<Base::primary_key_members_.size()>{});
            return key;
        }

        template <std::size_t... Is>
        static void build_owner_stitch_key_impl(
                storm::orm::utilities::StitchKey& key, const T& obj, std::index_sequence<Is...> /*unused*/
        ) noexcept {
            (append_obj_pk_part_to_key<Base::primary_key_members_[Is]>(key, obj), ...);
        }

        template <std::meta::info Member>
        static void append_obj_pk_part_to_key(storm::orm::utilities::StitchKey& key, const T& obj) noexcept {
            if constexpr (Base::is_fk_field(Member)) {
                using FKType         = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                using InnerFK        = utilities::optional_inner_type_t<FKType>;
                constexpr auto fk_pk = Base::template find_fk_primary_key<FKType>();
                using PartType       = std::remove_cvref_t<decltype(std::declval<InnerFK>().[:fk_pk:])>;
                append_obj_dispatched_part<PartType>(key, obj.[:Member:].[:fk_pk:]);
            } else {
                using PartType = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                append_obj_dispatched_part<PartType>(key, obj.[:Member:]);
            }
        }

        // Mirrors TwoQueryJoinBase::append_dispatched_part's arms exactly — the two
        // must agree byte-for-byte or a Q2 row never finds its Q1 owner. storm::UUID
        // (#507) belongs with the text types: it is stored as TEXT/UUID, and its
        // implicit operator std::string_view feeds append_string directly.
        template <typename PartType>
        static void append_obj_dispatched_part(storm::orm::utilities::StitchKey& key, const auto& value) noexcept {
            if constexpr (std::same_as<PartType, std::string> || std::same_as<PartType, std::string_view> ||
                          std::same_as<PartType, storm::orm::utilities::UUID>) {
                key.append_string(value);
            } else {
                key.append_int64(static_cast<std::int64_t>(value));
            }
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
                const auto owner = extract_owner_key(rel, stmt);
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

        // INNER-join semantics (#392): remove entities whose container stayed
        // empty in ANY inner relation; LEFT relations never drop. When every
        // relation is LEFT the predicate is constant-false and nothing drops.
        static auto drop_empty_relations(plf::hive<T>& results, const JoinStatementWrapper<PkKeyType>& wrapper) noexcept
                -> void {
            for (auto it = results.begin(); it != results.end();) {
                T&         obj  = *it;
                const bool drop = std::ranges::any_of(wrapper.m2m_relations, [&obj](const M2MRelation& rel) {
                    return !rel.is_left && rel.container_empty_fn(&obj);
                });
                if (drop) {
                    it = results.erase(it);
                } else {
                    ++it;
                }
            }
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
