module;

// `duplicate` removed in #277 Phase 3 (build_field_list_with_prefix consteval helper shared by JOIN/non-JOIN field-list
// builders).

#include <plf_hive/plf_hive.h>

export module storm_orm_statements_projection;

import std;
import storm_std_meta;

import storm_db_concept;
import storm_orm_field_attr; // storm::meta::has_full_unsigned_attr (#600 projection classification)
import storm_orm_statements_base;
import storm_orm_statements_extract;
import storm_orm_statements_join;
import storm_orm_statements_orderby;
import storm_orm_utilities;
import storm_orm_where;

export namespace storm::orm::statements {

    // Import utilities for compile-time SQL generation
    using storm::orm::utilities::ConstexprString;

    // ProjectionMode controls whether DISTINCT keyword is included in SQL
    enum class ProjectionMode : std::uint8_t { Values, Distinct };

    // ProjectionStatement - executes SELECT or SELECT DISTINCT on specified field(s) and returns tuple data
    // Supports 1+ fields with compile-time type safety
    // Mode controls whether DISTINCT keyword is applied
    //
    // Architecture: Persistent instance with proxy pattern (matches SelectStatement)
    // - Cached via static thread_local in QuerySet::distinct()/values()
    // - query() returns lightweight Query proxy holding references
    // - Instance-level caching eliminates per-call TLS access and object construction
    //
    // API: Use ^^ operator to pass reflected field information directly
    // Example: qs.distinct<fields::Person.name>().execute()
    //          qs.values<fields::Person.name, fields::Person.age>().execute()
    template <typename T, storm::db::DatabaseConnection ConnType, ProjectionMode Mode, std::meta::info... FieldInfos>
        requires(sizeof...(FieldInfos) > 0)
    class ProjectionStatement : private BaseStatement<T> {
        using Base = BaseStatement<T>;

      public:
        using Error     = typename ConnType::Error;
        using Statement = typename ConnType::Statement;

        static constexpr std::size_t NumFields = sizeof...(FieldInfos);

      private:
        // Field information is already std::meta::info - no conversion needed!
        // This eliminates the need for __PRETTY_FUNCTION__ string parsing
        static constexpr auto member_infos_ = std::array{FieldInfos...};

        // Deduce field types from member_info array
        template <std::size_t... Is>
        static consteval auto get_field_types_helper(std::index_sequence<Is...> /*unused*/) {
            return std::tuple<std::remove_cvref_t<decltype(std::declval<T>().[:member_infos_[Is]:])>...>{};
        }

        using FieldTypesTuple = decltype(get_field_types_helper(std::make_index_sequence<NumFields>{}));

        // Calculate field size at compile-time
        template <std::size_t I> static consteval auto get_field_size() -> std::size_t {
            std::size_t size = meta::column_name_size(member_infos_[I]);
            if constexpr (I > 0) {
                size += 2; // ", "
            }
            return size;
        }

        // Calculate total size of all fields
        template <std::size_t... Is>
        static consteval auto calculate_field_list_size(std::index_sequence<Is...> /*unused*/) -> std::size_t {
            return (get_field_size<Is>() + ...);
        }

        // Append column name for field I (with FK _id suffix if needed)
        template <std::size_t I, std::size_t N> static consteval void append_column_name(ConstexprString<N>& result) {
            meta::append_column_name(result, member_infos_[I]);
        }

        // Build a "<prefix>col1, <prefix>col2, ..." field list at compile time.
        // The non-JOIN list uses no prefix; the JOIN list uses "t1.". The two
        // builders used to spell the loop out independently.
        template <std::size_t Extra, std::size_t... Is>
        static consteval auto
        build_field_list_with_prefix(std::string_view prefix, std::index_sequence<Is...> /*unused*/) {
            constexpr std::size_t total_size = calculate_field_list_size(std::make_index_sequence<NumFields>{}) + Extra;
            ConstexprString<total_size + 10> result;
            auto                             append_field = [&result, prefix]<std::size_t I>() {
                if constexpr (I > 0) {
                    result.append(", ");
                }
                if (!prefix.empty()) {
                    result.append(prefix);
                }
                append_column_name<I>(result);
            };
            (append_field.template operator()<Is>(), ...);
            return result;
        }

        // Compile-time field list generation (no alias prefix)
        template <std::size_t... Is> static consteval auto build_field_list_constexpr(std::index_sequence<Is...> seq) {
            return build_field_list_with_prefix<0, Is...>("", seq);
        }

        // Pre-computed field list for use in non-JOIN path
        static constexpr auto field_list_constexpr_ = build_field_list_constexpr(std::make_index_sequence<NumFields>{});

        // Compile-time field list with "t1." table alias prefix for JOIN queries
        template <std::size_t... Is>
        static consteval auto build_join_field_list_constexpr(std::index_sequence<Is...> seq) {
            return build_field_list_with_prefix<NumFields * 3, Is...>(join_base_alias_prefix, seq);
        }

        static constexpr auto join_field_list_constexpr_ =
                build_join_field_list_constexpr(std::make_index_sequence<NumFields>{});

        // Calculate SQL size at compile-time
        static consteval auto calculate_select_sql_size() -> std::size_t {
            using utilities::sql_len::FROM;
            using utilities::sql_len::SELECT;
            using utilities::sql_len::SELECT_DISTINCT;
            constexpr auto field_list = build_field_list_constexpr(std::make_index_sequence<NumFields>{});
            std::size_t    size       = 0;
            if constexpr (Mode == ProjectionMode::Distinct) {
                size += SELECT_DISTINCT; // "SELECT DISTINCT "
            } else {
                size += SELECT; // "SELECT "
            }
            size += field_list.len;
            size += FROM; // " FROM "
            size += Base::table_name_.size();
            size += 1; // null terminator
            return size;
        }

        // Build SELECT or SELECT DISTINCT at compile-time
        static consteval auto build_projection_sql_array() {
            constexpr std::size_t     sql_size = calculate_select_sql_size() + utilities::sql_len::LARGE_BUFFER;
            ConstexprString<sql_size> result;

            if constexpr (Mode == ProjectionMode::Distinct) {
                result.append("SELECT DISTINCT ");
            } else {
                result.append("SELECT ");
            }
            constexpr auto field_list = build_field_list_constexpr(std::make_index_sequence<NumFields>{});
            result.append(field_list);
            result.append(" FROM ");
            result.append(Base::table_name_);

            return result;
        }

        // PostgreSQL binary result format (#600). A projection returns only the
        // fields it names, so it is classified from those types directly rather
        // than from the whole model — a model with one unsafe column can still
        // project its safe ones over the binary path. An FK selector's type is
        // the target STRUCT, which is not in the safe set, so those fall back to
        // text exactly like BaseStatement's own FK members do.
        //
        // The full_unsigned check cannot be folded into the type predicate: such
        // a member is PG NUMERIC(20,0) yet shares std::uint64_t with an ordinary
        // signed_storage member, so only the ANNOTATION tells them apart — the
        // same split BaseStatement::member_pg_binary_safe makes.
        template <std::size_t I> static consteval auto projected_field_pg_binary_safe() -> bool {
            // An FK selector's column holds the TARGET's key, not this member's own
            // (struct) type — rejected explicitly, mirroring
            // BaseStatement::member_pg_binary_safe, rather than left to fall out of
            // "a model struct happens not to be in the safe set". full_unsigned is
            // rejected alongside it for the same reason as that function: it shares
            // std::uint64_t with an ordinary signed_storage member, so only the
            // ANNOTATION tells them apart.
            if constexpr (storm::meta::is_fk_field(member_infos_[I]) ||
                          storm::meta::has_full_unsigned_attr(member_infos_[I])) {
                return false;
            } else {
                return ColumnExtractor::is_pg_binary_safe_column_v<std::tuple_element_t<I, FieldTypesTuple>>;
            }
        }
        template <std::size_t... Is>
        static consteval auto projection_pg_binary_safe_impl(std::index_sequence<Is...> /*unused*/) -> bool {
            return (projected_field_pg_binary_safe<Is>() && ...);
        }
        // Public so the classification can be asserted directly: it is the one
        // decision here that no round-trip can observe (a projection wrongly called
        // UNSAFE still returns correct values, just over the text path), so without
        // a static_assert on it this could be pinned to a constant and every
        // behavioural test would still pass.
      public:
        static constexpr bool projection_pg_binary_safe_ =
                projection_pg_binary_safe_impl(std::make_index_sequence<NumFields>{});

      private:
        // Pre-computed SQL generated at compile-time
        static constexpr auto           projection_sql_array_  = build_projection_sql_array();
        static inline const std::string projection_sql_string_ = std::string(projection_sql_array_);
        static inline const std::string field_list_string_{
                field_list_constexpr_.data.data(), field_list_constexpr_.len
        };
        static inline const std::string join_field_list_string_{
                join_field_list_constexpr_.data.data(), join_field_list_constexpr_.len
        };

      public:
        // Result type: hive of single field OR hive of tuple
        using ResultType = std::conditional_t<
                NumFields == 1,
                plf::hive<std::tuple_element_t<0, FieldTypesTuple>>,
                plf::hive<FieldTypesTuple>>;

        explicit ProjectionStatement(
                std::shared_ptr<ConnType>                                            conn,
                orm::where::ExpressionVariantPtr                                     where_expr       = nullptr,
                const std::optional<JoinStatementWrapper<detail::pk_key_type_t<T>>>& join_stmt        = std::nullopt,
                const std::optional<int>&                                            limit            = std::nullopt,
                const std::optional<int>&                                            offset           = std::nullopt,
                const std::optional<OrderByWrapper>&                                 order_by_wrapper = std::nullopt
        )
            : conn_(std::move(conn))
            , where_expr_(std::move(where_expr))
            , join_stmt_(join_stmt)
            , limit_(limit)
            , offset_(offset)
            , order_by_wrapper_(order_by_wrapper) {}

        // Return the SQL that would be executed (for testing/debugging)
        [[nodiscard]] auto sql() -> std::string {
            return build_sql();
        }

        // Return the SQL with bound parameters inlined (for debugging). Mirrors
        // execute()'s prepare+bind, then hands back the statement's expanded SQL.
        [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
            auto stmt_result = prepare_and_bind();
            if (!stmt_result) [[unlikely]] {
                return std::unexpected(stmt_result.error());
            }
            return (*stmt_result)->expanded_sql();
        }

        // Execute SELECT or SELECT DISTINCT query on the specified field(s)
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto execute() -> std::expected<ResultType, Error> {
            auto stmt_result = prepare_and_bind();
            if (!stmt_result) [[unlikely]] {
                return std::unexpected(stmt_result.error());
            }
            // #600: requested HERE and not in the shared prepare_and_bind() prologue.
            // to_sql() shares that prologue but never executes or resets afterwards,
            // so opting in there would strand result_format_ = 1 on a CACHED statement
            // for the next caller that finds it by identical SQL text — precisely the
            // inheritance reset() exists to prevent. execute_query_loop() resets on
            // its way out, so the flag never outlives this call.
            //
            // Unlike SelectStatement, the JOIN form needs no extra guard: build_sql
            // replaces the join's field list with THIS projection's ("t1.col, ...") —
            // the same NumFields columns, of the same types — so a join changes which
            // rows come back, never their shape.
            Base::request_binary_results(*stmt_result, projection_pg_binary_safe_);
            return execute_query_loop(*stmt_result);
        }

      private:
        // Build the projection SQL, prepare it via the connection cache, and bind
        // any WHERE params. Shared prepare→bind prologue for execute()/to_sql().
        [[nodiscard]] auto prepare_and_bind() -> std::expected<Statement*, Error> {
            auto sql = build_sql();

            auto stmt_result = conn_->prepare_cached(sql);
            if (!stmt_result) [[unlikely]] {
                return std::unexpected(stmt_result.error());
            }

            if (where_expr_) {
                auto bind_result = Base::template bind_where_params<Statement, Error>(*stmt_result, where_expr_);
                if (!bind_result) [[unlikely]] {
                    return std::unexpected(bind_result.error());
                }
            }

            return *stmt_result;
        }

        // Build the complete SQL string for this projection query
        [[nodiscard]] auto build_sql() -> std::string {
            std::string sql;
            if (join_stmt_.has_value()) {
                const std::string& join_sql = join_stmt_->get_complete_sql();
                sql.reserve(join_sql.size() + utilities::sql_len::LARGE_BUFFER);
                // Replace the JOIN's full field list with only our projected field(s).
                // Uses t1-qualified names (join_field_list_string_) to avoid column ambiguity.
                auto from_pos = join_sql.find(" FROM ");
                if constexpr (Mode == ProjectionMode::Distinct) { // LCOV_EXCL_START — if constexpr: only one branch is
                                                                  // instantiated per Mode
                    sql = "SELECT DISTINCT ";
                } else {
                    sql = "SELECT ";
                } // LCOV_EXCL_STOP
                sql += join_field_list_string_;
                sql.append(join_sql, from_pos, std::string::npos);
                // Base table aliased t1: its WHERE / ORDER BY columns are qualified (#785).
                Base::append_join_where(sql, where_expr_);
                Base::template append_join_order_by<ConnType>(sql, order_by_wrapper_);
            } else {
                sql = projection_sql_string_;
                if (where_expr_) {
                    sql += " WHERE ";
                    sql += orm::where::to_sql(*where_expr_);
                }
                Base::template append_order_by<ConnType>(sql, order_by_wrapper_);
            }
            Base::template append_limit_offset<ConnType>(sql, limit_, offset_);
            return sql;
        }

        // Extract results from prepared statement
        [[nodiscard]] __attribute__((hot)) __attribute__((flatten)) auto execute_query_loop(Statement* stmt)
                -> std::expected<ResultType, Error> {
            ResultType results;
            int        rc = 0;
            while ((rc = stmt->step_raw()) == Statement::ROW_AVAILABLE) {
                if constexpr (NumFields == 1) {
                    using FieldType = std::tuple_element_t<0, FieldTypesTuple>;
                    results.insert(ColumnExtractor::template extract_column_value<FieldType>(stmt, 0));
                } else {
                    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                        results.insert(
                                std::make_tuple(
                                        ColumnExtractor::template extract_column_value<
                                                std::tuple_element_t<Is, FieldTypesTuple>>(stmt, Is)...
                                )
                        );
                    }(std::make_index_sequence<NumFields>{});
                }
            }
            stmt->reset();
            if (rc != Statement::NO_MORE_ROWS) [[unlikely]] {
                return std::unexpected(Error{rc, stmt->get_error_message()});
            }
            return results;
        }

        // =====================================================================
        // MEMBERS
        // =====================================================================

        std::shared_ptr<ConnType>                                     conn_;
        orm::where::ExpressionVariantPtr                              where_expr_;
        std::optional<JoinStatementWrapper<detail::pk_key_type_t<T>>> join_stmt_;
        std::optional<int>                                            limit_;
        std::optional<int>                                            offset_;
        std::optional<OrderByWrapper>                                 order_by_wrapper_;
    };

    // Type aliases for backward compatibility and convenience
    template <typename T, typename ConnType, std::meta::info... FieldInfos>
    using DistinctStatement = ProjectionStatement<T, ConnType, ProjectionMode::Distinct, FieldInfos...>;

    template <typename T, typename ConnType, std::meta::info... FieldInfos>
    using ValuesStatement = ProjectionStatement<T, ConnType, ProjectionMode::Values, FieldInfos...>;

} // namespace storm::orm::statements
