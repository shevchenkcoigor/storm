module;

#include <cassert>
#include <meta>
#include <plf_hive/plf_hive.h>

export module storm_orm_queryset;

import std;

import storm_db_concept;
import storm_db_sqlite;
import storm_orm_field_attr;
import storm_orm_statements_base;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_db_default_connection;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_statements_join;
import storm_orm_statements_orderby;
import storm_orm_where;
import storm_orm_fields; // selector_info / ValidSelector — #518 proxy selectors
import storm_orm_statements_setop;
import storm_orm_utilities;

export namespace storm {

    template <
            class T,
            storm::db::DatabaseConnection ConnType   = storm::db::sqlite::Connection,
            bool                          Finalized_ = false>
        requires storm::meta::Entity<T>
    class QuerySet { // NOSONAR(cpp:S1448) — ORM facade class; method count grows with supported operations
        using Error     = typename ConnType::Error;
        using Statement = typename ConnType::Statement;
        using Select    = orm::statements::SelectStatement<T, ConnType>;
        using Query     = typename Select::Query;

        template <class U, storm::db::DatabaseConnection C, bool F>
            requires storm::meta::Entity<U>
        friend class QuerySet;

      public:
        // Default constructor using default connection
        QuerySet() : q_(Select{}.all()) {}

        // LCOV_EXCL_START — compile-time only, used in static_assert
        static constexpr auto is_finalized() -> bool {
            return Finalized_;
        }
        // LCOV_EXCL_STOP

        // Erase single object - returns proxy with .execute() and .to_sql()
        [[nodiscard]] auto erase(const T& obj [[clang::lifetimebound]]) {
            return orm::statements::EraseStatement<T, ConnType>(conn()).query(obj);
        }

        // Bulk erase - returns proxy with .execute() and .to_sql()
        [[nodiscard]] auto erase(std::span<const T> objects [[clang::lifetimebound]]) {
            return orm::statements::EraseStatement<T, ConnType>(conn()).query(objects);
        }

        // Conditional bulk DELETE (#198) — deletes rows matching the current where().
        // qs.where(cond).erase().execute() → std::expected<void, Error>.
        // With NO where() set, .execute()/.to_sql() return std::unexpected and refuse
        // to wipe the table; call erase_all() for an explicit full-table delete.
        [[nodiscard]] auto erase() {
            return orm::statements::EraseStatement<T, ConnType>(conn()).query_where(q_.where_expr);
        }

        // Erase all rows — executes DELETE FROM <table> with no WHERE clause
        [[nodiscard]] auto erase_all() {
            return orm::statements::EraseStatement<T, ConnType>(conn()).query_all();
        }

        // Insert single object - returns proxy with .execute() and .to_sql()
        // ReturnId::Yes (single-integer-PK default): .execute() returns std::expected<int64_t, Error>
        // ReturnId::No: .execute() returns std::expected<void, Error> (faster, no RETURNING clause)
        // Composite-PK (#502) and storm::UUID-PK (#572) models default to ReturnId::No —
        // neither key is ever DB-generated, so there is nothing to return — and reject an
        // explicit ReturnId::Yes at compile time (ReturnIdSupported).
        // (SFINAE: only accept T, not span/container)
        template <orm::statements::ReturnId R = orm::statements::default_return_id<T>(), typename U = T>
            requires std::same_as<std::remove_cvref_t<U>, T> && orm::statements::ReturnIdSupported<T, R>
        [[nodiscard]] auto insert(const U& obj [[clang::lifetimebound]]) {
            return orm::statements::InsertStatement<T, ConnType>(conn()).template query<R>(obj);
        }

        // Bulk insert - returns proxy with .execute() and .to_sql()
        // Default (no template arg): .execute() returns std::expected<void, Error>
        // ReturnId::Yes: .execute() returns std::expected<std::vector<int64_t>, Error>
        [[nodiscard]] auto
        insert(std::span<const T>                            objects [[clang::lifetimebound]],
               std::optional<orm::statements::InsertOptions> opts = std::nullopt) {
            return orm::statements::InsertStatement<T, ConnType>(conn()).query(objects, opts);
        }
        template <orm::statements::ReturnId R>
            requires orm::statements::ReturnIdSupported<T, R>
        [[nodiscard]] auto
        insert(std::span<const T>                            objects [[clang::lifetimebound]],
               std::optional<orm::statements::InsertOptions> opts = std::nullopt) {
            return orm::statements::InsertStatement<T, ConnType>(conn()).template query<R>(objects, opts);
        }

        // WHERE clause support - builder pattern with method chaining using type-safe expressions
        //
        // Usage examples:
        //   queryset.where(fields::Person.age > 25).select()
        //   queryset.where(fields::Person.id.in(1, 2, 3)).select()
        //   queryset.where(fields::Person.name.like("A%")).select()
        //   queryset.where(fields::Person.age.between(25, 50)).select()
        //
        // Chaining with AND composition:
        //   queryset.where(fields::Person.age > 25)
        //           .where(fields::Person.name == "Alice")
        //           .select()
        //
        // Complex expressions with AND/OR:
        //   queryset.where(fields::Person.age > 25 && fields::Person.is_active == true).select()
        //   queryset.where((fields::Person.age > 25) || (fields::Person.name.like("A%"))).select()
        //
        [[nodiscard]] auto where(orm::where::ExpressionVariantPtr expr) const -> QuerySet {
            return QuerySet{q_.where(std::move(expr))};
        }

        // LIMIT clause support - returns finalized QuerySet (prevents use as set-op operand)
        // Usage: queryset.limit(10).select()
        //        queryset.where(age > 25).limit(10).select()
        [[nodiscard]] constexpr auto limit(int n) -> QuerySet<T, ConnType, true> {
            // Precondition: LIMIT must be non-negative (issue #362, item C).
            // Tracked by #225: becomes a P2900 `pre(n >= 0)` contract once clang-p2996 supports it.
            assert(n >= 0 && "limit() requires a non-negative value");
            auto result = to_finalized();
            result.q_   = std::move(result.q_).limit(n);
            return result;
        }

        // OFFSET clause support - returns finalized QuerySet (prevents use as set-op operand)
        // Usage: queryset.offset(5).select()
        //        queryset.limit(10).offset(5).select()
        [[nodiscard]] constexpr auto offset(int n) -> QuerySet<T, ConnType, true> {
            // Precondition: OFFSET must be non-negative (issue #362, item C).
            // Tracked by #225: becomes a P2900 `pre(n >= 0)` contract once clang-p2996 supports it.
            assert(n >= 0 && "offset() requires a non-negative value");
            auto result = to_finalized();
            result.q_   = std::move(result.q_).offset(n);
            return result;
        }

        // ORDER BY clause support - returns finalized QuerySet (prevents use as set-op operand)
        // Supports single/multiple fields with ASC (default) or DESC direction
        // #570: constrained here as well as on OrderByClause itself, so a composite-PK FK
        // selector is diagnosed at THIS call site rather than deep inside
        // make_order_by_wrapper. The clause on OrderByClause stays as the backstop that
        // also covers SetOpBuilder::order_by.
        template <auto... Args>
            requires orm::statements::OrderBySelectorsAreSingleColumn<Args...>
        [[nodiscard]] constexpr auto order_by() -> QuerySet<T, ConnType, true> {
            auto result = to_finalized();
            result.q_   = std::move(result.q_).template order_by<Args...>();
            return result;
        }

        // Select - returns proxy with .execute() and .to_sql()
        // NOTE: WHERE and JOIN state is preserved after select() for query reusability.
        // Call reset() to clear state when needed.
        [[nodiscard]] __attribute__((hot)) auto select() {
            return q_;
        }

        // Lazy row-by-row iteration via std::generator — no full materialization
        // Each iteration yields std::expected<T, Error> — one row at a time from DB
        // Uses a dedicated (non-cached) prepared statement per generator instance
        // Usage: for (auto&& result : qs.where(age > 30).rows()) { ... }
        [[nodiscard]] auto rows() {
            return q_.rows();
        }

        // First - returns proxy with .execute() and .to_sql()
        // Automatically applies LIMIT 1 to the query for optimal performance
        // Usage: auto result = queryset.where(age > 30).first().execute();
        [[nodiscard]] __attribute__((hot)) auto first() {
            return q_.first();
        }

        // Get - returns proxy with .execute() and .to_sql()
        // Returns exactly one matching row, or an error if 0 or >1 rows match
        // Does NOT apply LIMIT — validates uniqueness of the result (like Django's get())
        // Uses LIMIT 2 internally for efficient duplicate detection without plf::hive allocation
        // Usage: auto result = queryset.where(id == 42).get().execute();
        [[nodiscard]] __attribute__((hot)) auto get() {
            return q_.get();
        }

        // Field-specific DISTINCT support using reflection
        // Usage:
        //   auto names = queryset.distinct<fields::Person.name>().execute();  // plf::hive<std::string>
        //   auto pairs = queryset.distinct<fields::Person.name, fields::Person.age>().execute();
        //   // Returns plf::hive<std::tuple<std::string, int>>
        //   auto ids = queryset.distinct().execute();  // plf::hive<int> (defaults to PK)
        //
        // OPTIMIZATION: Returns cached DistinctStatement for optimal performance
        // Returns DistinctStatement by value - connection-level prepare_cached() handles SQL caching
        // No static thread_local needed since actual statement caching is at connection level
        template <auto... S>
            requires orm::statements::ProjectionSelectors<S...>
        [[nodiscard]] constexpr auto distinct() {
            return q_.template distinct<S...>();
        }

        // Column projection support using reflection (SELECT specific columns)
        // Usage:
        //   auto names = queryset.values<fields::Person.name>().execute();  // plf::hive<std::string>
        //   auto pairs = queryset.values<fields::Person.name, fields::Person.age>().execute();
        //   // Returns plf::hive<std::tuple<std::string, int>>
        //
        // Unlike distinct(), values() does NOT apply DISTINCT — all rows are returned
        // including duplicates. Works with WHERE, JOIN, ORDER BY, LIMIT/OFFSET.
        template <auto... S>
            requires(sizeof...(S) > 0 && orm::statements::ProjectionSelectors<S...>)
        [[nodiscard]] constexpr auto values() {
            return q_.template values<S...>();
        }

        // INNER JOIN support for FK fields or many-to-many fields (#203, #392)
        // Immutable: returns a new QuerySet with the join attached (Django-style).
        // Usage:
        //   Single FK: message_qs.join<fields::Message.sender>().select()
        //   Multi FK:  message_qs.join<fields::Message.sender, fields::Message.receiver>().select()
        //   M2M:       student_qs.join<fields::Student.courses>().select()
        //   Multi M2M: member_qs.join<fields::Member.courses, fields::Member.clubs>().select()
        // Mixed FK + m2m in one call is rejected (out of scope, #392).
        template <auto... S>
            requires orm::statements::JoinableFields<T, storm::meta::selector_info<S>()...>
        [[nodiscard]] auto join() const -> QuerySet {
            return QuerySet{q_.template join<S...>()};
        }

        // LEFT JOIN support for FK fields or many-to-many fields (#203, #392).
        // For m2m, LEFT JOIN keeps base entities with no relations — each
        // relation's container fills independently (empty when no rows).
        // Immutable: returns a new QuerySet with the join attached.
        // Usage:
        //   Single FK: message_qs.left_join<fields::Message.sender>().select()
        //   Multi FK:  message_qs.left_join<fields::Message.sender, fields::Message.receiver>().select()
        //   M2M:       student_qs.left_join<fields::Student.courses>().select()
        //   Multi M2M: member_qs.left_join<fields::Member.courses, fields::Member.clubs>().select()
        template <auto... S>
            requires orm::statements::JoinableFields<T, storm::meta::selector_info<S>()...>
        [[nodiscard]] auto left_join() const -> QuerySet {
            return QuerySet{q_.template left_join<S...>()};
        }

        // Update single object - returns proxy with .execute() and .to_sql()
        // (SFINAE: only accept T, not span/container)
        template <typename U = T>
            requires std::same_as<std::remove_cvref_t<U>, T>
        [[nodiscard]] auto update(const U& obj [[clang::lifetimebound]]) {
            return orm::statements::UpdateStatement<T, ConnType>(conn()).query(obj);
        }

        // Bulk update - returns proxy with .execute() and .to_sql()
        [[nodiscard]] auto update(std::span<const T> objects [[clang::lifetimebound]]) {
            return orm::statements::UpdateStatement<T, ConnType>(conn()).query(objects);
        }

        // Conditional bulk UPDATE (#403) — updates rows matching the current where() filter.
        // SET columns are the Members... NTTPs; values come from `proto`.
        // qs.where(cond).update<fields::T.a, fields::T.b>(T{...}).execute() → std::expected<void, Error>.
        // With NO where() set, .execute()/.to_sql() return std::unexpected and refuse to
        // write the whole table.
        template <auto... S>
            requires(sizeof...(S) > 0 && (storm::meta::ValidSelector<S> && ...))
        [[nodiscard]] auto update(const T& proto [[clang::lifetimebound]]) {
            return orm::statements::UpdateStatement<T, ConnType>(conn())
                    .template query_where<storm::meta::selector_info<S>()...>(proto, q_.where_expr);
        }

        // Update all rows (#409) — UPDATE <table> SET <set> with NO WHERE clause.
        // SET columns are the Members... NTTPs; values come from `proto`. The explicit
        // full-table escape hatch named by update<>()'s empty-WHERE refusal (mirrors erase_all()).
        template <auto... S>
            requires(sizeof...(S) > 0 && (storm::meta::ValidSelector<S> && ...))
        [[nodiscard]] auto update_all(const T& proto [[clang::lifetimebound]]) {
            return orm::statements::UpdateStatement<T, ConnType>(conn())
                    .template query_all<storm::meta::selector_info<S>()...>(proto);
        }

        // Reset WHERE, JOIN, LIMIT, and OFFSET state
        // Use this to clear conditions and start fresh with the same QuerySet instance
        // Example:
        //   auto qs = QuerySet<Person>(conn);
        //   qs.where(age > 25).limit(10).select();  // WHERE age > 25 LIMIT 10
        //   qs.reset();                              // Clear state
        //   qs.select();                             // No WHERE, no LIMIT
        auto reset() noexcept -> void {
            q_ = Select(conn()).all();
        }

        // GROUP BY - returns GroupByBuilder for fluent aggregate chaining
        // Usage:
        //   qs.group_by<fields::Person.department>().count().execute()    // grouped → .execute()
        //   qs.group_by<fields::Person.dept, fields::Person.role>().sum<fields::Person.salary>().execute()
        //   qs.where(age > 25).group_by<fields::Person.years_exp>().count().execute()
        template <auto... S>
            requires(sizeof...(S) > 0 && orm::statements::ProjectionSelectors<S...>)
        [[nodiscard]] auto group_by() {
            return q_.template group_by<S...>();
        }
        // =====================================================================
        // SET OPERATIONS: UNION, UNION ALL, EXCEPT, INTERSECT
        // =====================================================================

        template <bool F_ = Finalized_>
        [[nodiscard]] auto union_(const QuerySet<T, ConnType, false>& other)
            requires(!F_)
        {
            return make_setop_builder(other, orm::statements::SetOpType::Union);
        }

        template <bool F_ = Finalized_>
        [[nodiscard]] auto union_all(const QuerySet<T, ConnType, false>& other)
            requires(!F_)
        {
            return make_setop_builder(other, orm::statements::SetOpType::UnionAll);
        }

        template <bool F_ = Finalized_>
        [[nodiscard]] auto except_(const QuerySet<T, ConnType, false>& other)
            requires(!F_)
        {
            return make_setop_builder(other, orm::statements::SetOpType::Except);
        }

        template <bool F_ = Finalized_>
        [[nodiscard]] auto intersect_(const QuerySet<T, ConnType, false>& other)
            requires(!F_)
        {
            return make_setop_builder(other, orm::statements::SetOpType::Intersect);
        }

        template <bool F_ = Finalized_>
        auto capture_operand() const -> orm::statements::SetOpOperand<T>
            requires(!F_)
        {
            std::string sql = q_.join_wrapper.has_value() ? std::string(q_.join_wrapper->get_complete_sql())
                                                          : Select::get_select_sql();
            if (q_.where_expr) {
                sql += " WHERE ";
                sql += orm::where::to_sql(*q_.where_expr);
            }
            return {std::move(sql), q_.where_expr};
        }

        // SUM aggregate (multi-field: SUM(f1 + f2 + ...))
        // Supports WHERE and JOIN clauses
        // Usage: queryset.sum<fields::Person.age>().execute()
        //        queryset.where(age > 30).sum<fields::Person.age>().execute()
        //        queryset.join<FK>().sum<fields::Person.salary>().execute()
        // Returns statement by value - connection-level prepare_cached() handles SQL caching
        template <auto... S>
            requires orm::statements::NumericAggregateSelectors<S...>
        [[nodiscard]] auto sum() {
            return q_.template sum<S...>();
        }

        // COUNT aggregate (defaults to COUNT(*) if no fields)
        // Supports WHERE and JOIN clauses
        // Usage: queryset.count().execute()  // COUNT(*)
        //        queryset.where(age > 30).count().execute()
        //        queryset.join<FK>().count().execute()
        // Returns statement by value - connection-level prepare_cached() handles SQL caching
        template <auto... S>
            requires orm::statements::ProjectionSelectors<S...>
        [[nodiscard]] auto count() {
            return q_.template count<S...>();
        }

        // AVG aggregate (multi-field: AVG(f1 + f2 + ...))
        // Supports WHERE and JOIN clauses
        // Usage: queryset.avg<fields::Person.salary>().execute()
        //        queryset.where(department == "Engineering").avg<fields::Person.salary>().execute()
        // Returns statement by value - connection-level prepare_cached() handles SQL caching
        template <auto... S>
            requires orm::statements::NumericAggregateSelectors<S...>
        [[nodiscard]] auto avg() {
            return q_.template avg<S...>();
        }

        // MIN aggregate (multi-field: MIN(f1 + f2 + ...))
        // Supports WHERE and JOIN clauses
        // Usage: queryset.min<fields::Person.age>().execute()
        //        queryset.where(active == true).min<fields::Person.age>().execute()
        // Returns statement by value - connection-level prepare_cached() handles SQL caching
        template <auto... S>
            requires orm::statements::NumericAggregateSelectors<S...>
        [[nodiscard]] auto min() {
            return q_.template min<S...>();
        }

        // MAX aggregate (multi-field: MAX(f1 + f2 + ...))
        // Supports WHERE and JOIN clauses
        // Usage: queryset.max<fields::Person.age>().execute()
        //        queryset.where(department == "Sales").max<fields::Person.salary>().execute()
        // Returns statement by value - connection-level prepare_cached() handles SQL caching
        template <auto... S>
            requires orm::statements::NumericAggregateSelectors<S...>
        [[nodiscard]] auto max() {
            return q_.template max<S...>();
        }

        // COUNT(DISTINCT field) aggregate
        // Supports WHERE and JOIN clauses
        // Usage: queryset.count_distinct<fields::Person.age>().execute()
        //        queryset.where(active == true).count_distinct<fields::Person.department>().execute()
        // Returns statement by value - connection-level prepare_cached() handles SQL caching
        template <auto S>
            requires orm::statements::ProjectionSelectors<S>
        [[nodiscard]] auto count_distinct() {
            return q_.template count_distinct<S>();
        }

        [[nodiscard]] auto to_finalized() const -> QuerySet<T, ConnType, true> {
            return QuerySet<T, ConnType, true>{q_};
        }

      private:
        explicit QuerySet(Query query) : q_(std::move(query)) {}

        [[nodiscard]] auto conn() const -> const std::shared_ptr<ConnType>& {
            return q_.stmt.connection();
        }

        auto make_setop_builder(const QuerySet<T, ConnType, false>& other, orm::statements::SetOpType op)
                -> orm::statements::SetOpBuilder<T, ConnType> {
            auto                                          left  = capture_operand();
            auto                                          right = other.capture_operand();
            std::vector<orm::statements::SetOpOperand<T>> operands;
            operands.push_back(std::move(left));
            operands.push_back(std::move(right));
            std::vector<orm::statements::SetOpType> operators;
            operators.push_back(op);
            return {conn(), std::move(operands), std::move(operators)};
        }

        Query q_;
    };

} // namespace storm
