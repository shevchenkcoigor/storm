module;

// Single cohesive class template; thresholds intentionally relaxed (see #264 finding).
// `duplicate` removed in #277 Phase 3 (append_group_by_tail helper folds the HAVING + ORDER/LIMIT/OFFSET tail repeated
// across execute_where / execute_join / execute_where_join).

#include <meta>
#include <plf_hive/plf_hive.h>

export module storm_orm_statements_aggregate;

import std;

import storm_db_concept;
import storm_orm_statements_base;
import storm_orm_statements_extract;
import storm_orm_statements_join;
import storm_orm_statements_orderby;
import storm_orm_utilities;
import storm_orm_where;
import storm_orm_fields;     // selector_info / ValidSelector — #518 proxy selectors
import storm_orm_field_attr; // append_column_name — #422/#570 canonical column name

// AggregateOp / OpResult / aggregate SQL-text builders (#671) — a textual fragment of
// this module; it #errors unless this macro is defined.
#define STORM_AGGREGATE_FRAGMENTS
#include "aggregate_grammar.hpp"

export namespace storm::orm::statements {
    using storm::orm::utilities::ConstexprString;

    // Common parameter bundle for AggregateStatement and GroupByBuilder
    template <typename T, typename ConnType> struct AggregateParams {
        std::shared_ptr<ConnType>                                     conn;
        orm::where::ExpressionVariantPtr                              where_expr;
        std::optional<JoinStatementWrapper<detail::pk_key_type_t<T>>> join_stmt;
        std::optional<int>                                            limit;
        std::optional<int>                                            offset;
        std::optional<OrderByWrapper>                                 order_by_wrapper;
        orm::where::ExpressionVariantPtr                              having_expr;
    };

    // ============================================================================
    // AggregateStatement - Single class for all aggregate queries
    // ============================================================================
    template <typename T, storm::db::DatabaseConnection ConnType, typename GroupFields, typename... Ops>
    class AggregateStatement : private BaseStatement<T> {
        using Base      = BaseStatement<T>;
        using Error     = typename ConnType::Error;
        using Statement = typename ConnType::Statement;

        static constexpr bool        HasGroupBy     = !std::is_same_v<GroupFields, NoGroupBy>;
        static constexpr std::size_t NumGroupFields = GroupFields::size;
        static constexpr std::size_t NumOps         = sizeof...(Ops);

        // ---- Result Type Deduction ----
        template <std::size_t Idx> struct GroupFieldType {
            static constexpr auto field_info = GroupFields::template at<Idx>();
            using type                       = std::remove_cvref_t<decltype(std::declval<T>().[:field_info:])>;
        };

        // LCOV_EXCL_START - compile-time only
        template <std::size_t... GIs, std::size_t... AIs>
        static consteval auto
        deduce_grouped_type(std::index_sequence<GIs...> /*unused*/, std::index_sequence<AIs...> /*unused*/) {
            return std::tuple<
                    typename GroupFieldType<GIs>::type...,
                    OpResult<std::tuple_element_t<AIs, std::tuple<Ops...>>>...>{};
        }

        static consteval auto deduce_simple_type() {
            if constexpr (NumOps == 1) {
                return OpResult<std::tuple_element_t<0, std::tuple<Ops...>>>{};
            } else {
                return []<std::size_t... Is>(std::index_sequence<Is...> /*unused*/)
                               -> std::tuple<OpResult<std::tuple_element_t<Is, std::tuple<Ops...>>>...> {
                    return std::tuple<OpResult<std::tuple_element_t<Is, std::tuple<Ops...>>>...>{};
                }(std::make_index_sequence<NumOps>{});
            }
        }
        // LCOV_EXCL_STOP

        using GroupedTuple = decltype(deduce_grouped_type(
                std::make_index_sequence<NumGroupFields>{}, std::make_index_sequence<NumOps>{}
        ));

      public:
        using ResultType = std::conditional_t<HasGroupBy, plf::hive<GroupedTuple>, decltype(deduce_simple_type())>;

        explicit AggregateStatement(AggregateParams<T, ConnType> p)
            : conn_(std::move(p.conn))
            , where_expr_(std::move(p.where_expr))
            , join_stmt_(std::move(p.join_stmt))
            , limit_(p.limit)
            , offset_(p.offset)
            , order_by_wrapper_(std::move(p.order_by_wrapper))
            , having_expr_(std::move(p.having_expr)) {}

        // HAVING clause - only available when GROUP BY is present
        [[nodiscard]] auto having(orm::where::ExpressionVariantPtr expr)
            requires HasGroupBy
        {
            AggregateParams<T, ConnType> params{
                    conn_, where_expr_, join_stmt_, limit_, offset_, order_by_wrapper_, std::move(expr)
            };
            return AggregateStatement<T, ConnType, GroupFields, Ops...>{std::move(params)};
        }

        // Chaining methods (only for non-GROUP BY queries building aggregates).
        // sum/avg/min/max share one body — chain_op<Type>() — differing only by
        // AggregateType; the numeric methods gate their target fields through
        // AllNumericAggregateable. count() accepts any field TYPE (COUNT is
        // type-agnostic) but still requires a single-column selector (#570, #611).
        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto sum() {
            return chain_op<AggregateType::SUM, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires((storm::meta::ValidSelector<S> && ...) && (storm::meta::SingleColumnSelector<S> && ...))
        [[nodiscard]] auto count() {
            return chain_op<AggregateType::COUNT, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto avg() {
            return chain_op<AggregateType::AVG, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto min() {
            return chain_op<AggregateType::MIN, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto max() {
            return chain_op<AggregateType::MAX, storm::meta::selector_info<S>()...>();
        }

        // Return the SQL that would be executed (for testing/debugging). Assembles
        // the complete aggregate SQL (JOIN/WHERE/GROUP BY/HAVING/modifiers); to_sql()
        // reuses this to build the string it then prepares and binds.
        [[nodiscard]] auto sql() -> std::string
            requires(NumOps > 0)
        {
            std::string result;
            if (join_stmt_.has_value()) {
                result = build_join_sql();
            } else {
                result = base_sql_;
            }
            if (where_expr_) {
                insert_where_clause(result);
            }
            if constexpr (HasGroupBy) {
                if (having_expr_) {
                    insert_having_clause(result);
                }
            }
            append_modifiers(result);
            return result;
        }

        // Return the SQL with bound parameters inlined (for debugging). Builds the
        // same SQL as sql(), prepares it, binds WHERE then HAVING (matching the
        // execute paths' param order), and hands back the statement's expanded SQL.
        [[nodiscard]] auto to_sql() -> std::expected<std::string, Error>
            requires(NumOps > 0)
        {
            std::string built = sql();
            auto        stmt  = ready_aggregate_statement(built);
            if (!stmt) [[unlikely]] {
                return std::unexpected(stmt.error());
            }
            if (auto bound = bind_where_then_having(*stmt); !bound) [[unlikely]] {
                return std::unexpected(bound.error());
            }
            return (*stmt)->expanded_sql();
        }

        [[nodiscard]] __attribute__((flatten)) auto execute() -> std::expected<ResultType, Error>
            requires(NumOps > 0)
        {
            // Inline dispatch to avoid lambda capture + call overhead on hot path
            if (join_stmt_.has_value() && where_expr_) {
                return execute_where_join();
            }
            if (join_stmt_.has_value()) {
                return execute_join();
            }
            if (where_expr_) {
                return execute_where();
            }
            return execute_simple();
        }

      private:
        // ---- Compile-time SQL Generation ----
        // LCOV_EXCL_START - compile-time only
        static consteval auto build_select_clause() {
            ConstexprString<utilities::buffer_size::SQL_MEDIUM> result;

            if constexpr (HasGroupBy) {
                constexpr auto group_sql =
                        build_group_fields_sql<GroupFields>(std::make_index_sequence<NumGroupFields>{});
                result.append(group_sql);
                result.append(", ");
            }

            constexpr auto ops_sql = build_ops_list<Ops...>(std::make_index_sequence<NumOps>{});
            result.append(ops_sql);
            return result;
        }

        static consteval auto build_base_sql() {
            ConstexprString<utilities::buffer_size::SQL_LARGE> result;
            result.append("SELECT ");
            constexpr auto select_clause = build_select_clause();
            result.append(select_clause);
            result.append(" FROM ");
            result.append(Base::get_table_name());

            if constexpr (HasGroupBy) {
                result.append(" GROUP BY ");
                constexpr auto group_sql =
                        build_group_fields_sql<GroupFields>(std::make_index_sequence<NumGroupFields>{});
                result.append(group_sql);
            }
            return result;
        }
        // LCOV_EXCL_STOP

        static constexpr auto sql_array_         = build_base_sql();
        static constexpr auto select_clause_arr_ = build_select_clause();
        static constexpr auto group_fields_arr_ =
                build_group_fields_sql<GroupFields>(std::make_index_sequence<NumGroupFields>{});

        // Static string cache
        static inline const std::string base_sql_{sql_array_.data.data(), sql_array_.len};
        static inline const std::string select_clause_{select_clause_arr_.data.data(), select_clause_arr_.len};
        static inline const std::string group_clause_{group_fields_arr_.data.data(), group_fields_arr_.len};

        // Simple aggregate extraction (single row)
        template <std::size_t... Is>
        [[nodiscard]] auto extract_simple_result(Statement* stmt, std::index_sequence<Is...> /*unused*/)
                -> std::expected<ResultType, Error> {
            int step_result = stmt->step_raw();

            // LCOV_EXCL_START — LLVM instrumentation bug: counters don't fire despite
            // code executing (verified via fprintf). if constexpr branches also untraceable.
            // Tested by mock: AggregateWithWhereStepErrorInExtractSimpleResult,
            //                 AggregateWithWhereStepNoRowsInExtractSimpleResult
            if (step_result != Statement::ROW_AVAILABLE) {
                stmt->reset();
                if (step_result == Statement::NO_MORE_ROWS) {
                    if constexpr (NumOps == 1) {
                        return ResultType{};
                    } else {
                        return ResultType{OpResult<std::tuple_element_t<Is, std::tuple<Ops...>>>{}...};
                    }
                }
                return std::unexpected(Error{step_result, stmt->get_error_message()});
            }
            // LCOV_EXCL_STOP

            ResultType result;
            // LCOV_EXCL_START — if constexpr: only one branch instantiated per NumOps
            if constexpr (NumOps == 1) {
                result = ColumnExtractor::template extract_column_value<ResultType>(stmt, 0);
            } else {
                result = ResultType{ColumnExtractor::template extract_column_value<
                        OpResult<std::tuple_element_t<Is, std::tuple<Ops...>>>>(stmt, Is)...};
            }
            // LCOV_EXCL_STOP

            stmt->reset();
            return result;
        }

        // GROUP BY extraction (multiple rows)
        template <std::size_t... GIs, std::size_t... AIs>
        static auto extract_grouped_row(
                Statement* stmt, std::index_sequence<GIs...> /*unused*/, std::index_sequence<AIs...> /*unused*/
        ) -> GroupedTuple {
            return GroupedTuple{
                    ColumnExtractor::template extract_column_value<typename GroupFieldType<GIs>::type>(stmt, GIs)...,
                    ColumnExtractor::template extract_column_value<
                            OpResult<std::tuple_element_t<AIs, std::tuple<Ops...>>>>(stmt, NumGroupFields + AIs)...
            };
        }

        [[nodiscard]] auto extract_grouped_results(Statement* stmt) -> std::expected<ResultType, Error> {
            ResultType results;
            int        step_result = Statement::NO_MORE_ROWS;

            while ((step_result = stmt->step_raw()) == Statement::ROW_AVAILABLE) {
                results.insert(extract_grouped_row(
                        stmt, std::make_index_sequence<NumGroupFields>{}, std::make_index_sequence<NumOps>{}
                ));
            }

            if (step_result != Statement::NO_MORE_ROWS) {
                stmt->reset();
                return std::unexpected(Error{step_result, stmt->get_error_message()});
            }

            stmt->reset();
            return results;
        }

        // ---- Unified Extraction Dispatcher ----
        [[nodiscard]] auto extract_results(Statement* stmt) -> std::expected<ResultType, Error> {
            if constexpr (HasGroupBy) {
                return extract_grouped_results(stmt);
            } else {
                return extract_simple_result(stmt, std::make_index_sequence<NumOps>{});
            }
        }

        // ---- Inline Helpers ----
        void insert_where_clause(std::string& sql) const {
            if constexpr (HasGroupBy) {
                std::size_t const group_pos    = sql.find(" GROUP BY ");
                std::string       where_clause = " WHERE ";
                where_clause += orm::where::to_sql(*where_expr_);
                sql.insert(group_pos, where_clause);
            } else {
                sql += " WHERE ";
                sql += orm::where::to_sql(*where_expr_);
            }
        }

        void insert_having_clause(std::string& sql) const {
            sql += " HAVING ";
            sql += orm::where::to_sql(*having_expr_);
        }

        void append_modifiers(std::string& sql) const {
            Base::template append_order_by<ConnType>(sql, order_by_wrapper_);
            Base::template append_limit_offset<ConnType>(sql, limit_, offset_);
        }

        [[nodiscard]] auto prepare_and_extract(const std::string& sql) -> std::expected<ResultType, Error> {
            auto prepare_result = conn_->prepare_cached(sql);
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            return extract_results(*prepare_result);
        }

        // Shared prepare→check prefix for the bind+extract helpers below.
        // No explicit reset here: the extract_* helpers reset the statement on
        // their trailing edge (after stepping to completion), so a cached
        // statement is already reset before the next bind — matching the
        // pre-refactor behavior, which bound directly after prepare_cached.
        // (Unlike erase, which resets on the leading edge for its bulk paths.)
        // Explicit-check form (not and_then): monadic chaining benched slower
        // on aggregate paths (#363).
        [[nodiscard]] __attribute__((always_inline)) auto ready_aggregate_statement(const std::string& sql) noexcept
                -> std::expected<Statement*, Error> {
            auto prepare_result = conn_->prepare_cached(sql);
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            return *prepare_result;
        }

        // Bind WHERE params (if any) then HAVING params (if any) onto an already
        // prepared statement, in the execute paths' param order. Shared by
        // prepare_bind_extract() and to_sql(). On WHERE-bind failure the statement
        // is reset here; bind_having_params resets itself on HAVING failure.
        [[nodiscard]] auto bind_where_then_having(Statement* stmt) -> std::expected<void, Error> {
            int param_index = 1;
            if (where_expr_) {
                auto bind_result = // NOSONAR(S1659)
                        orm::where::bind_params_direct<Statement, Error>(*where_expr_, stmt, param_index);
                if (!bind_result) [[unlikely]] {
                    stmt->reset();
                    return std::unexpected(bind_result.error());
                }
            }
            if (having_expr_) {
                auto having_bind = Base::template bind_having_params<Statement, Error>(stmt, having_expr_, param_index);
                if (!having_bind) [[unlikely]] {
                    return std::unexpected(having_bind.error());
                }
            }
            return {};
        }

        [[nodiscard]] auto prepare_bind_extract(const std::string& sql) -> std::expected<ResultType, Error> {
            auto prepare_result = ready_aggregate_statement(sql);
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            if (auto bound = bind_where_then_having(*prepare_result); !bound) [[unlikely]] {
                return std::unexpected(bound.error());
            }
            return extract_results(*prepare_result);
        }

        [[nodiscard]] auto prepare_bind_having_extract(const std::string& sql) -> std::expected<ResultType, Error> {
            auto prepare_result = ready_aggregate_statement(sql);
            if (!prepare_result) [[unlikely]] {
                return std::unexpected(prepare_result.error());
            }
            int  param_index = 1;
            auto having_bind =
                    Base::template bind_having_params<Statement, Error>(*prepare_result, having_expr_, param_index);
            if (!having_bind) [[unlikely]] {
                return std::unexpected(having_bind.error());
            }
            return extract_results(*prepare_result);
        }

        // ---- SQL Builders ----
        [[nodiscard]] auto build_join_sql() const -> std::string {
            const std::string& join_sql = join_stmt_->get_complete_sql();
            // join_sql always contains " FROM " — built at compile time by
            // JoinStatement::build_complete_sql_array() which unconditionally appends it.
            const std::size_t from_pos = join_sql.find(" FROM ");

            std::string result;
            result.reserve(select_clause_.size() + join_sql.size() + utilities::sql_len::MEDIUM_BUFFER);
            result = "SELECT ";
            result += select_clause_;
            result += join_sql.substr(from_pos);

            if constexpr (HasGroupBy) {
                result += " GROUP BY ";
                result += group_clause_;
            }

            return result;
        }

        // ---- Inline extraction for simple aggregate (no trailing reset) ----
        // OPTIMIZATION: Step + extract single result row without trailing reset.
        // The next call will reset at the top, eliminating double-reset overhead.
        template <std::size_t... Is>
        [[nodiscard]] __attribute__((always_inline)) static auto
        extract_simple_no_reset(Statement* stmt, std::index_sequence<Is...> /*unused*/)
                -> std::expected<ResultType, Error> {
            int step_result = stmt->step_raw();

            if (step_result != Statement::ROW_AVAILABLE) [[unlikely]] {
                stmt->reset();
                if (step_result == Statement::NO_MORE_ROWS) {
                    return ResultType{};
                }
                return std::unexpected(Error{step_result, stmt->get_error_message()});
            }

            if constexpr (NumOps == 1) {
                return ColumnExtractor::template extract_column_value<ResultType>(stmt, 0);
            } else {
                return ResultType{ColumnExtractor::template extract_column_value<
                        OpResult<std::tuple_element_t<Is, std::tuple<Ops...>>>>(stmt, Is)...};
            }
        }

        // ---- Execution Paths ----
        [[nodiscard]] __attribute__((hot)) auto execute_simple() -> std::expected<ResultType, Error> {
            if constexpr (HasGroupBy) {
                const bool has_modifiers = order_by_wrapper_.has_value() || limit_.has_value() || offset_.has_value();
                if (having_expr_) {
                    std::string sql = base_sql_;
                    insert_having_clause(sql);
                    if (has_modifiers) {
                        append_modifiers(sql);
                    }
                    return prepare_bind_having_extract(sql);
                }
                if (has_modifiers) {
                    std::string sql = base_sql_;
                    append_modifiers(sql);
                    return prepare_and_extract(sql);
                }
                return prepare_and_extract(base_sql_);
            } else {
                // Issue #269: an earlier thread-local Statement* cache here was
                // a use-after-free waiting to happen — when the default Connection
                // was torn down, the cached pointer dangled, and the next
                // Connection allocated at the same address let the identity check
                // pass and reset() be called on freed memory. Routing through
                // prepare_cached() matches every other statement class and keeps
                // the per-connection cache as the single source of truth.
                auto prepare_result = conn_->prepare_cached(base_sql_);
                if (!prepare_result) [[unlikely]] {
                    return std::unexpected(prepare_result.error());
                }
                // prepare_cached already reset the statement on cache hit.
                return extract_simple_no_reset(*prepare_result, std::make_index_sequence<NumOps>{});
            }
        }

        // Append HAVING + GROUP BY modifiers (ORDER BY / LIMIT / OFFSET) to the
        // SQL — but only when this aggregate is parameterised with HasGroupBy.
        // The three execute_* paths used to spell this `if constexpr / having /
        // append_modifiers` body out inline.
        __attribute__((always_inline)) auto append_group_by_tail(std::string& sql) -> void {
            if constexpr (HasGroupBy) {
                if (having_expr_) {
                    insert_having_clause(sql);
                }
                append_modifiers(sql);
            }
        }

        [[nodiscard]] __attribute__((hot)) auto execute_where() -> std::expected<ResultType, Error> {
            std::string sql = base_sql_;
            insert_where_clause(sql);
            append_group_by_tail(sql);
            return prepare_bind_extract(sql);
        }

        [[nodiscard]] __attribute__((hot)) auto execute_join() -> std::expected<ResultType, Error> {
            std::string sql = build_join_sql();
            append_group_by_tail(sql);
            if (having_expr_) {
                return prepare_bind_having_extract(sql);
            }
            return prepare_and_extract(sql);
        }

        [[nodiscard]] __attribute__((hot)) auto execute_where_join() -> std::expected<ResultType, Error> {
            std::string sql = build_join_sql();
            insert_where_clause(sql);
            append_group_by_tail(sql);
            return prepare_bind_extract(sql);
        }

        // Shared body for the chaining aggregate methods: append one AggregateOp
        // of the given AggregateType and return the extended statement.
        template <AggregateType AType, std::meta::info... FieldInfos> [[nodiscard]] auto chain_op() {
            return AggregateStatement<T, ConnType, GroupFields, Ops..., AggregateOp<AType, FieldInfos...>>{
                    make_params()
            };
        }

        auto make_params() -> AggregateParams<T, ConnType> {
            return {conn_, where_expr_, join_stmt_, limit_, offset_, order_by_wrapper_, having_expr_};
        }

        std::shared_ptr<ConnType>                                     conn_;
        orm::where::ExpressionVariantPtr                              where_expr_;
        std::optional<JoinStatementWrapper<detail::pk_key_type_t<T>>> join_stmt_;
        std::optional<int>                                            limit_;
        std::optional<int>                                            offset_;
        std::optional<OrderByWrapper>                                 order_by_wrapper_;
        orm::where::ExpressionVariantPtr                              having_expr_;
    };

    // ============================================================================
    // GroupByBuilder - Fluent builder for GROUP BY queries
    // ============================================================================
    // Provides a fluent interface for building GROUP BY aggregate queries.
    // Created by QuerySet::group_by<>() and returns AggregateStatement
    // when an aggregate method (count, sum, avg, min, max) is called.
    // ============================================================================
    template <typename T, storm::db::DatabaseConnection ConnType, std::meta::info... GroupFieldInfos>
        requires(sizeof...(GroupFieldInfos) > 0)
    class GroupByBuilder {
        using GBFields = GroupByFields<GroupFieldInfos...>;

      public:
        explicit GroupByBuilder(AggregateParams<T, ConnType> p) : params_(std::move(p)) {}

        // HAVING clause - stores expression and returns new GroupByBuilder
        [[nodiscard]] auto having(orm::where::ExpressionVariantPtr expr) {
            auto p        = params_;
            p.having_expr = std::move(expr);
            return GroupByBuilder<T, ConnType, GroupFieldInfos...>{std::move(p)};
        }

        // count/count_distinct accept any field TYPE (COUNT is type-agnostic), but both
        // still require a SINGLE-COLUMN selector (#570, #611) — an FK to a composite-PK
        // target has no correct COUNT(field)/COUNT(DISTINCT field) form.
        // sum/avg/min/max gate their target fields through AllNumericAggregateable.
        // All delegate to grouped_op<Type>(), which appends one AggregateOp.
        template <auto... S>
            requires((storm::meta::ValidSelector<S> && ...) && (storm::meta::SingleColumnSelector<S> && ...))
        [[nodiscard]] auto count() {
            return grouped_op<AggregateType::COUNT, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires((storm::meta::ValidSelector<S> && ...) && (storm::meta::SingleColumnSelector<S> && ...))
        [[nodiscard]] auto count_distinct() {
            return grouped_op<AggregateType::COUNT_DISTINCT, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto sum() {
            return grouped_op<AggregateType::SUM, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto avg() {
            return grouped_op<AggregateType::AVG, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto min() {
            return grouped_op<AggregateType::MIN, storm::meta::selector_info<S>()...>();
        }

        template <auto... S>
            requires(
                    (storm::meta::ValidSelector<S> && ...) &&
                    AllNumericAggregateable<storm::meta::selector_info<S>()...>
            )
        [[nodiscard]] auto max() {
            return grouped_op<AggregateType::MAX, storm::meta::selector_info<S>()...>();
        }

      private:
        // Shared body: append one AggregateOp of the given type to the GROUP BY
        // aggregate statement.
        template <AggregateType AType, std::meta::info... FieldInfos> [[nodiscard]] auto grouped_op() {
            return AggregateStatement<T, ConnType, GBFields, AggregateOp<AType, FieldInfos...>>{params_};
        }

        AggregateParams<T, ConnType> params_;
    };

} // namespace storm::orm::statements
