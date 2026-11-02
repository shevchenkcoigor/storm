module;

// `duplicate` removed in #277 Phase 3 (add_operand helper folds union_/union_all/except_/intersect_ bodies;
// ready_statement shared by execute() and to_sql()).

#include <plf_hive/plf_hive.h>

export module storm_orm_statements_setop;

import std;
import storm_std_meta;

import storm_orm_statements_base;
import storm_orm_statements_join;
import storm_orm_statements_orderby;
import storm_orm_where;
import storm_db_concept;

export namespace storm::orm::statements {

    enum class SetOpType : std::uint8_t { Union, UnionAll, Except, Intersect };

    template <typename T> struct SetOpOperand {
        std::string                      sql;
        orm::where::ExpressionVariantPtr where_expr;
    };

    template <typename T, storm::db::DatabaseConnection ConnType> class SetOpBuilder : private BaseStatement<T> {
        using Base      = BaseStatement<T>;
        using Error     = typename ConnType::Error;
        using Statement = typename ConnType::Statement;

      public:
        SetOpBuilder(
                std::shared_ptr<ConnType> conn, std::vector<SetOpOperand<T>> operands, std::vector<SetOpType> operators
        )
            : conn_(std::move(conn)), operands_(std::move(operands)), operators_(std::move(operators)) {}

        // Append a new operand with the given set operator. Each public
        // union_/union_all/except_/intersect_ used to spell this body out
        // three lines at a time.
        template <typename U>
            requires std::same_as<U, T>
        auto add_operand(SetOpOperand<U> operand, SetOpType op) -> SetOpBuilder&& {
            operands_.push_back(std::move(operand));
            operators_.push_back(op);
            return std::move(*this);
        }

        // clang-format off
        template <typename U> requires std::same_as<U, T> [[nodiscard]] auto union_    (SetOpOperand<U> operand) -> SetOpBuilder&& { return add_operand(std::move(operand), SetOpType::Union);     }
        template <typename U> requires std::same_as<U, T> [[nodiscard]] auto union_all (SetOpOperand<U> operand) -> SetOpBuilder&& { return add_operand(std::move(operand), SetOpType::UnionAll);  }
        template <typename U> requires std::same_as<U, T> [[nodiscard]] auto except_   (SetOpOperand<U> operand) -> SetOpBuilder&& { return add_operand(std::move(operand), SetOpType::Except);    }
        template <typename U> requires std::same_as<U, T> [[nodiscard]] auto intersect_(SetOpOperand<U> operand) -> SetOpBuilder&& { return add_operand(std::move(operand), SetOpType::Intersect); }
        // clang-format on

        // #570: constrained here as well as on OrderByClause, so a composite-PK FK
        // selector is diagnosed at THIS call site rather than inside make_order_by_wrapper.
        template <auto... Args>
            requires OrderBySelectorsAreSingleColumn<Args...>
        [[nodiscard]] auto order_by() -> SetOpBuilder&& {
            order_by_wrapper_ = make_order_by_wrapper<Args...>();
            return std::move(*this);
        }

        [[nodiscard]] auto limit(int n) -> SetOpBuilder&& {
            limit_value_ = n;
            return std::move(*this);
        }

        [[nodiscard]] auto offset(int n) -> SetOpBuilder&& {
            offset_value_ = n;
            return std::move(*this);
        }

        // Prepare via the L3 connection cache, reset it, rebind all params, and
        // return the ready-to-use Statement*. Shared by execute() and to_sql()
        // — previously each method spelled out the prepare/reset/bind dance.
        [[nodiscard]] auto ready_statement() -> std::expected<Statement*, Error> {
            std::string sql         = build_combined_sql();
            auto        stmt_result = conn_->prepare_cached(sql);
            if (!stmt_result) [[unlikely]] {
                return std::unexpected(stmt_result.error());
            }
            Statement* stmt = *stmt_result;
            stmt->reset();
            if (auto bind = bind_all_params(stmt); !bind) [[unlikely]] {
                return std::unexpected(bind.error());
            }
            return stmt;
        }

        [[nodiscard]] auto execute() -> std::expected<plf::hive<T>, Error> {
            auto stmt = ready_statement();
            if (!stmt) [[unlikely]] {
                return std::unexpected(stmt.error());
            }
            // #600: requested HERE and not in the shared ready_statement() prologue.
            // to_sql() shares that prologue but never executes or resets afterwards,
            // so opting in there would strand result_format_ = 1 on a CACHED statement
            // for the next caller that finds it by identical SQL text — precisely the
            // inheritance reset() exists to prevent. execute_query_loop() resets on
            // its way out, so the flag never outlives this call.
            //
            // The invariant that makes the base model's whole-row classification the
            // right question is NOT "every operand selects the same column list" — an
            // operand carrying a join contributes its join's complete SQL, which is
            // base + JOINED columns (see QuerySet::capture_operand). It is that the
            // reader here only ever consumes the LEADING field_count_ columns, via
            // extract_all_columns, and both join SQL builders emit the base model's
            // columns first. So the columns actually decoded are always exactly T's
            // own, whatever trails them.
            Base::request_binary_results(*stmt, Base::pg_binary_safe_row_);
            return execute_query_loop(*stmt);
        }

        // Return the SQL template without preparing/binding (for testing/debugging)
        [[nodiscard]] auto sql() -> std::string {
            return build_combined_sql();
        }

        [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
            auto stmt = ready_statement();
            if (!stmt) [[unlikely]] {
                return std::unexpected(stmt.error());
            }
            return (*stmt)->expanded_sql();
        }

      private:
        [[nodiscard]] auto build_combined_sql() const -> std::string {
            std::string sql;

            for (std::size_t i = 0; i < operands_.size(); ++i) {
                if (i > 0) {
                    sql += op_to_sql(operators_[i - 1]);
                }
                sql += operands_[i].sql;
            }

            Base::template append_order_by<ConnType>(sql, order_by_wrapper_);
            Base::template append_limit_offset<ConnType>(sql, limit_value_, offset_value_);

            return sql;
        }

        [[nodiscard]] auto bind_all_params(Statement* stmt_ptr) const -> std::expected<void, Error> {
            int param_index = 1;
            for (const auto& operand : operands_) {
                if (operand.where_expr) {
                    std::expected<void, Error> bind_result = orm::where::bind_params_direct<Statement, Error>(
                            *operand.where_expr, stmt_ptr, param_index
                    );
                    if (!bind_result) [[unlikely]] {
                        stmt_ptr->reset();
                        return std::unexpected(bind_result.error());
                    }
                }
            }
            return {};
        }

        [[nodiscard]] auto execute_query_loop(Statement* stmt_ptr) const noexcept
                -> std::expected<plf::hive<T>, Error> {
            plf::hive<T> results;
            int          step_result = 0;

            while ((step_result = stmt_ptr->step_raw()) == Statement::ROW_AVAILABLE) {
                T obj;
                Base::extract_all_columns(stmt_ptr, obj);
                results.insert(std::move(obj));
            }

            if (step_result != Statement::NO_MORE_ROWS) {
                stmt_ptr->reset();
                return std::unexpected(Error{step_result, stmt_ptr->get_error_message()});
            }

            stmt_ptr->reset();
            return results;
        }

        static constexpr auto op_to_sql(SetOpType op) -> std::string_view {
            using enum SetOpType;
            switch (op) {
            case Union:
                return " UNION ";
            case UnionAll:
                return " UNION ALL ";
            case Except:
                return " EXCEPT ";
            case Intersect:
                return " INTERSECT ";
            } // LCOV_EXCL_START
            std::unreachable();
        }
        // LCOV_EXCL_STOP

        std::shared_ptr<ConnType>     conn_;
        std::vector<SetOpOperand<T>>  operands_;
        std::vector<SetOpType>        operators_;
        std::optional<OrderByWrapper> order_by_wrapper_;
        std::optional<int>            limit_value_;
        std::optional<int>            offset_value_;
    };

} // namespace storm::orm::statements
