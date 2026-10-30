// Textual fragment of module storm_orm_statements_select (#671) — NOT a header.
// #included once, INSIDE the body of BasicSelectStatement (select.cppm), where the
// nested proxies used to be written; it compiles into that module's BMI. A file split
// rather than a module split: a new module costs ~0.09 s in every TU that imports storm.
//
// The fluent query proxies: the chain surface (where / limit / offset / order_by / join /
// left_join), the projection and aggregate terminals, and the first() / get() / rows() /
// execute() / to_sql() / sql() entry points that hand the accumulated clauses back to the
// statement. They stay NESTED in the class (not namespace-scope templates), so they see
// its Error / PkKeyType / build_sql directly.

#ifndef STORM_SELECT_FRAGMENTS
#error "select_query.hpp is a textual fragment of storm_orm_statements_select; #include it only from select.cppm"
#endif

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
                this->join_wrapper, this->where_expr, this->limit_value, this->offset_value, this->order_by_wrapper
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
    template <typename Self> [[nodiscard]] auto where(this Self self, orm::where::ExpressionVariantPtr expr) -> Self {
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
    template <JoinType Type, std::meta::info... FKFields> static auto make_joined() -> JoinStatementWrapper<PkKeyType> {
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
        return AggregateStatement<T, ConnType, NoGroupBy, AggregateOp<Type, Fields...>>{aggregate_params(false)};
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
        return build_sql(this->join_wrapper, this->where_expr, limit_one, this->offset_value, this->order_by_wrapper);
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
        return this->stmt.to_sql_get(this->join_wrapper, this->where_expr, this->offset_value, this->order_by_wrapper);
    }
    [[nodiscard]] auto sql() -> std::string {
        std::optional<int> const limit_two = 2;
        return build_sql(this->join_wrapper, this->where_expr, limit_two, this->offset_value, this->order_by_wrapper);
    }
};
