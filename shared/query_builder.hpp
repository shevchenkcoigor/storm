#pragma once

// TODO: Convert to storm_orm_query_builder module once clang-p2996 no longer
// crashes when a consumer imports a module that exports templates containing
// ^^QuerySet<Model>::template join<FKs...> splice expressions. Tracked in
// https://github.com/spiritEcosse/storm/issues/256

// Must be included AFTER `import storm;` in each consumer TU.
//
// The by-name resolution (fields, methods, operators, collations) and the typed values
// live in query_builder_resolve.hpp and query_builder_values.hpp (#869); this header
// is the QueryBuilder itself.

#include <meta>
import storm_orm_utilities;
import storm_orm_fields;

#include "query_builder_values.hpp" // NOLINT(misc-header-include-cycle)

namespace storm::orm::query_builder {

    using storm::meta::selector_for;
    using storm::orm::utilities::Collate;

    // Default FK resolver (#837): a JOIN names its FK by the member name on Model. The
    // benchmark overrides it with its own registry lookup. An unknown name is a null info,
    // which QueryBuilder::join_member rejects with a named static_assert (#869).
    template <typename Model> struct MemberNameResolver {
        consteval auto operator()(std::string_view name) const -> std::meta::info {
            return find_member(^^Model, name);
        }
    };
    template <typename Model> inline constexpr MemberNameResolver<Model> member_name_resolver{};

    // ========================================================================
    // QueryBuilder<Model, spec, fk_resolver>
    //
    // Schema-agnostic: `spec` is any NTTP whose nested fields satisfy the
    // spec protocol (spec.where, spec.join, spec.order_by[], spec.group_by,
    // spec.distinct, spec.limit, spec.aggregate, spec.setop (a chain of operands, #870), spec.operation).
    //
    // fk_resolver: consteval callable (string_view) -> std::meta::info for
    // JOIN field dispatch. Defaults to a by-name member lookup on Model.
    //
    // spec.within_limits() (#843): a case that listed more entries than a capped
    // array holds is not silently truncated; it fails the SpecWithinLimits
    // constraint at the QueryBuilder<...> use site.
    // ========================================================================
    template <auto Spec>
    concept SpecWithinLimits = Spec.within_limits();

    // The WHERE tree can be built (#843): children exist, joins are AND/OR, and each leaf has the
    // operands its operator reads. Without it a leaf would index the shared value pool past its own.
    // The join too (#869): an inner or left join, its members named by `fk` or by `fks`, not both.
    template <auto Spec>
    concept SpecWellFormed = Spec.where.is_well_formed() && Spec.group_by.having.is_well_formed() &&
                             Spec.group_by.having_fields_are_grouped() && Spec.join.is_well_formed();

    // Every aggregate step can be applied (#868): a GROUP BY has at least one, and only count may name no
    // field (COUNT(*)), and each func is a ValidAggregate name. Whether a field suits sum/avg/min/max is the
    // statement's own NumericAggregateSelectors.
    template <auto Spec>
    concept SpecAggregatesWellFormed = []<std::size_t... Is>(std::index_sequence<Is...>) consteval {
        constexpr auto& steps = Spec.aggregations;
        return !(Spec.group_by.enabled && sizeof...(Is) == 0) &&
               ((ValidAggregate<steps[Is].func> &&
                 (steps[Is].fields.declared_count > 0 || steps[Is].func.view() == "count")) &&
                ...);
    }(std::make_index_sequence<std::min(Spec.aggregation_count, Spec.aggregations.size())>{});

    // The YAML set operation -> the member that applies it (#870), on QuerySet and SetOpBuilder alike;
    // empty for any other spelling. ValidSetOp is the allow-list, as ValidAggregate is for a func.
    consteval auto set_op_member(std::string_view op) -> std::string_view {
        if (op == "union")
            return "union_";
        if (op == "union_all")
            return "union_all";
        if (op == "except")
            return "except_";
        if (op == "intersect")
            return "intersect_";
        return {};
    }

    template <auto op_str>
    concept ValidSetOp = !set_op_member(op_str.view()).empty();

    // The chain's shape (#870): 2..4 operands, an `op` on each but the first, no unknown key, the operands'
    // WHERE trees a well-formed forest of the case's `where` pool. Its values are checked by SpecValuesFit,
    // which walks the whole pool.
    template <auto Spec>
    concept SpecSetOpShape = !Spec.setop.enabled || Spec.setop.is_well_formed(Spec.where);

    // A top-level `where:` / `join:` beside `setop:` would not say which operand, or the combined result, it
    // filters (#870). The operands' trees share the `where` pool but leave it disabled.
    template <auto Spec>
    concept SpecSetOpNoTopLevelFilter = !Spec.setop.enabled || (!Spec.where.enabled && !Spec.join.enabled);

    // Operand I's `op` names a set operation; spelled once per operand so the diagnostic names I and the op.
    template <auto Spec, std::size_t I>
    concept SetOpOperandOpKnown =
            I == 0 || I >= Spec.setop.stored_operand_count() || ValidSetOp<Spec.setop.operands[I].op>;

    template <auto Spec>
    concept SpecSetOpOpsKnown =
            SetOpOperandOpKnown<Spec, 1> && SetOpOperandOpKnown<Spec, 2> && SetOpOperandOpKnown<Spec, 3>;

    // The chain returns the same rows on every backend (#870): no INTERSECT after another operator.
    // Storm emits no parentheses; SQLite reads the chain left to right, PostgreSQL binds INTERSECT tighter.
    template <auto Spec>
    concept SpecSetOpPortable = Spec.setop.is_portable_chain();

    template <
            typename Model,
            auto spec,
            auto fk_resolver  = member_name_resolver<Model>,
            typename ConnType = storm::db::sqlite::Connection>
        requires SpecWithinLimits<spec> && SpecWellFormed<spec> && SpecAggregatesWellFormed<spec> &&
                 (spec.join.enabled || SpecValuesFit<Model, spec>) && SpecHavingValuesFit<Model, spec> &&
                 SpecSetOpShape<spec> && SpecSetOpNoTopLevelFilter<spec> && SpecSetOpOpsKnown<spec> &&
                 SpecSetOpPortable<spec>
    class QueryBuilder {
        // typed_value_as (a value in its field's type) is a free function in query_builder_values.hpp (#871).

        // ----------------------------------------------------------------
        // WHERE — one tree of WhereNodes (#843)
        // ----------------------------------------------------------------
        // A leaf: its operands are Tree.values[first_value .. first_value + value_count).
        template <const auto& Tree, typename WhereModel, std::size_t N> static auto build_where_leaf() {
            constexpr auto& cond  = Tree.nodes[N];
            constexpr auto& pool  = Tree.values;
            constexpr auto  first = cond.first_value;
            static_assert(has_field<WhereModel>(cond.field.view()), "condition field not found on model");
            constexpr auto             fi = dispatch_field<WhereModel>(cond.field.view());
            constexpr std::string_view op = cond.op.view();
            using FieldType               = typename[:std::meta::type_of(fi):];

            using Kind  = decltype(pool[first].kind);
            using Inner = orm::utilities::optional_inner_type_t<FieldType>;

            if constexpr (op == "LIKE" || op == "ILIKE") {
                return build_compare_expr<fi, cond.op, cond.collate>(pool[first].as_string.view());
            } else if constexpr (cond.value_count == 1 && pool[first].kind == Kind::Null) {
                // `== null` / `!= null`: the same IS [NOT] NULL as `== std::nullopt` (#844).
                constexpr auto null_op =
                        orm::utilities::ConstexprString<16>(op == "==" ? kIsNullOp.data() : kIsNotNullOp.data());
                return build_compare_expr<fi, null_op, cond.collate>();
            } else if constexpr (op == "BETWEEN") {
                return build_compare_expr<fi, cond.op, cond.collate>(
                        typed_value_as<FieldType, pool[first]>(), typed_value_as<FieldType, pool[first + 1]>()
                );
            } else if constexpr (op == "IN") {
                return []<std::size_t... Is>(std::index_sequence<Is...>) {
                    return build_compare_expr<fi, Tree.nodes[N].op, Tree.nodes[N].collate>(
                            typed_value_as<FieldType, Tree.values[Tree.nodes[N].first_value + Is]>()...
                    );
                }(std::make_index_sequence<cond.value_count>{});
            } else if constexpr (pool[first].kind == Kind::String && !NonTextString<Inner>) {
                return build_compare_expr<fi, cond.op, cond.collate>(pool[first].as_string.view());
            } else {
                return build_compare_expr<fi, cond.op, cond.collate>(typed_value_as<FieldType, pool[first]>());
            }
        }

        // The tree below node N as one expression: a leaf is a comparison, an inner node AND/OR.
        template <const auto& Tree, typename WhereModel, std::size_t N> static auto build_where_node() {
            constexpr auto& node = Tree.nodes[N];
            if constexpr (node.is_leaf()) {
                return build_where_leaf<Tree, WhereModel, N>();
            } else if constexpr (node.op == "AND") {
                return build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.left)>() &&
                       build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.right)>();
            } else {
                static_assert(node.op == "OR", "a condition-tree join node is AND or OR");
                return build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.left)>() ||
                       build_where_node<Tree, WhereModel, static_cast<std::size_t>(node.right)>();
            }
        }

        template <typename WhereModel> static auto apply_where(QuerySet<Model, ConnType>& qs) -> void {
            if constexpr (spec.where.enabled) {
                // The class constraint checked the base model; a WHERE on a related model reads
                // its values as that model's field types.
                static_assert(SpecValuesFit<WhereModel, spec>, "a WHERE value does not fit its field's type");
                qs = qs.where(build_where_node<spec.where, WhereModel, static_cast<std::size_t>(spec.where.root)>());
            }
        }

        // ----------------------------------------------------------------
        // JOIN
        // ----------------------------------------------------------------
        // SpecWellFormed has checked the type is "inner" or "left".
        template <auto... FKs> static consteval auto resolve_join() -> std::meta::info {
            constexpr std::string_view jt = spec.join.type.view();
            if (jt == "left")
                return ^^QuerySet<Model, ConnType>::template left_join<FKs...>;
            return ^^QuerySet<Model, ConnType>::template join<FKs...>;
        }

        template <auto... FKs> static auto dispatch_join_type(auto& qs) -> void {
            constexpr auto method = resolve_join<FKs...>();
            qs                    = qs.[:method:]();
        }

        // Member I of the join, as fk_resolver reads its name. A resolver that returns a null info for
        // an unknown name (member_name_resolver, the test's yaml_join_resolver) gets a named compile
        // error here (#869) rather than one inside selector_for; a resolver that fails on its own
        // (the benchmark's registry lookup) fails first. Whether the members can be joined
        // together (one kind, no duplicates) is join<>'s own JoinableFields constraint.
        template <std::size_t I> static consteval auto join_member() -> std::meta::info {
            constexpr std::meta::info member = fk_resolver(spec.join.member(I));
            static_assert(member != std::meta::info{}, "join.fk / join.fks names no member the fk_resolver knows");
            return member;
        }

        static auto apply_join(auto& qs) -> void {
            if constexpr (spec.join.enabled) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    dispatch_join_type<selector_for<join_member<Is>()>()...>(qs);
                }(std::make_index_sequence<spec.join.member_count()>{});
            }
        }

        // ----------------------------------------------------------------
        // ORDER BY
        // ----------------------------------------------------------------
        static consteval auto parse_collate(std::string_view s) -> Collate {
            return collate_of(s);
        }

        static consteval auto enabled_order_by_count() -> std::size_t {
            std::size_t n = 0;
            for (std::size_t i = 0; i < std::remove_cvref_t<decltype(spec)>::MAX_ORDER_BY; ++i)
                if (spec.order_by[i].enabled)
                    ++n;
            return n;
        }

        static constexpr std::size_t MAX_OB = std::remove_cvref_t<decltype(spec)>::MAX_ORDER_BY;

        template <std::size_t I>
            requires(I >= MAX_OB || !spec.order_by[I].enabled || ValidCollate<spec.order_by[I].collate>)
        static auto apply_order_by_impl(auto qs) {
            if constexpr (I >= MAX_OB) {
                return qs;
            } else if constexpr (!spec.order_by[I].enabled) {
                return apply_order_by_impl<I + 1>(std::move(qs));
            } else {
                constexpr auto& ob = spec.order_by[I];
                static_assert(has_field<Model>(ob.field.view()), "ORDER BY field not found on model");
                constexpr auto fi  = dispatch_field<Model>(ob.field.view());
                constexpr bool asc = (ob.direction.view() != "DESC");
                if constexpr (ob.collate.view() != "") {
                    constexpr auto col = parse_collate(ob.collate.view());
                    return apply_order_by_impl<I + 1>(qs.template order_by<storm::meta::FieldRef<fi>{}, asc, col>());
                } else {
                    return apply_order_by_impl<I + 1>(qs.template order_by<storm::meta::FieldRef<fi>{}, asc>());
                }
            }
        }

        static auto apply_order_by(auto qs) {
            if constexpr (enabled_order_by_count() == 0) {
                return qs;
            } else {
                return apply_order_by_impl<0>(std::move(qs));
            }
        }

        static auto apply_limit(auto qs) {
            if constexpr (!spec.limit.enabled) {
                return qs;
            } else if constexpr (spec.limit.value > 0 && spec.limit.offset > 0) {
                return qs.limit(spec.limit.value).offset(spec.limit.offset);
            } else if constexpr (spec.limit.value > 0) {
                return qs.limit(spec.limit.value);
            } else if constexpr (spec.limit.offset > 0) {
                return qs.offset(spec.limit.offset);
            } else {
                return qs;
            }
        }

        // ----------------------------------------------------------------
        // Aggregate / GROUP BY / DISTINCT / SET OP
        // ----------------------------------------------------------------
        // The aggregate chain (#868): step I is `func` over its field pack, applied to step I-1's
        // statement (every step changes the type, so it is a recursion, not a loop). The YAML `func`
        // IS the member name, resolved by reflection; ValidAggregate stays as the allow-list so a
        // YAML string can only reach those members, not any same-named one.
        template <std::size_t I>
            requires ValidAggregate<spec.aggregations[I].func>
        static auto apply_aggregation(auto&& stmt) {
            constexpr auto& step = spec.aggregations[I];
            return [&]<std::size_t... Fs>(std::index_sequence<Fs...>) {
                static_assert(
                        (has_field<Model>(step.fields.fields[Fs].view()) && ...), "aggregate field not found on model"
                );
                constexpr auto method = member_function_named<std::remove_cvref_t<decltype(stmt)>>(step.func.view());
                return call_query_method<method, dispatch_field<Model>(step.fields.fields[Fs].view())...>(
                        std::forward<decltype(stmt)>(stmt)
                );
            }(std::make_index_sequence<step.fields.declared_count>{});
        }

        template <std::size_t I = 0> static auto apply_aggregations(auto&& stmt) {
            auto next = apply_aggregation<I>(std::forward<decltype(stmt)>(stmt));
            if constexpr (I + 1 >= spec.aggregation_count)
                return next;
            else
                return apply_aggregations<I + 1>(std::move(next));
        }

        static auto build_group_by_query(auto& qs) {
            auto gb = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs
                        .template group_by<selector_for<dispatch_field<Model>(spec.group_by.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.group_by.field_count()>{});

            return apply_aggregations(with_having(std::move(gb)));
        }

        // Operand I of the set-op chain (#870): its WHERE tree over the base model, or the whole table.
        template <std::size_t I> static auto setop_operand() {
            constexpr int             root = spec.setop.operands[I].root;
            QuerySet<Model, ConnType> qs;
            if constexpr (root >= 0)
                return qs.where(build_where_node<spec.where, Model, static_cast<std::size_t>(root)>());
            else
                return qs;
        }

        // The member of Owner (QuerySet for the first pair, SetOpBuilder after) that applies operand I's op.
        template <std::size_t I, typename Owner> static consteval auto setop_method() -> std::meta::info {
            return member_function_named<Owner>(set_op_member(spec.setop.operands[I].op.view()));
        }

        // Operands I.. joined onto the chain, each by its own op. Each step returns the builder by rvalue
        // reference, so it is moved into the next step's parameter rather than assigned to itself.
        template <std::size_t I, typename Chain> static auto append_setop_operands(Chain chain) {
            if constexpr (I >= spec.setop.operand_count) {
                return chain;
            } else {
                constexpr auto method = setop_method<I, Chain>();
                return append_setop_operands<I + 1>(
                        Chain{chain.template[:method:]<Model>(setop_operand<I>().capture_operand())}
                );
            }
        }

        // ORDER BY term I as SetOpBuilder::order_by reads it: the field, its direction, its collation.
        template <std::size_t I>
        static constexpr auto setop_order_field =
                storm::meta::FieldRef<dispatch_field<Model>(spec.order_by[I].field.view())>{};
        template <std::size_t I> static constexpr bool setop_order_asc = spec.order_by[I].direction.view() != "DESC";
        template <std::size_t I>
            requires ValidCollate<spec.order_by[I].collate>
        static constexpr Collate setop_order_collate =
                spec.order_by[I].collate.empty() ? Collate::None : collate_of(spec.order_by[I].collate.view());

        // A SetOpBuilder keeps only its LAST order_by() call (the compound SELECT has one ORDER BY), unlike
        // a QuerySet, which appends; so the terms are collected into one call. The parser stores the
        // enabled terms at [0, enabled_order_by_count()).
        template <std::size_t I, auto... Terms> static auto apply_setop_order_by(auto chain) {
            if constexpr (I < enabled_order_by_count()) {
                static_assert(has_field<Model>(spec.order_by[I].field.view()), "ORDER BY field not found on model");
                return apply_setop_order_by<
                        I + 1,
                        Terms...,
                        setop_order_field<I>,
                        setop_order_asc<I>,
                        setop_order_collate<I>>(std::move(chain));
            } else if constexpr (sizeof...(Terms) == 0) {
                return chain;
            } else {
                return decltype(chain){chain.template order_by<Terms...>()};
            }
        }

        static auto build_setop() {
            static_assert(
                    std::remove_cvref_t<decltype(spec.setop)>::MAX_OPERANDS == 4,
                    "SpecSetOpOpsKnown spells out operands 1..3"
            );
            constexpr auto first = setop_method<1, QuerySet<Model, ConnType>>();
            auto           chain = append_setop_operands<2>(setop_operand<0>().template[:first:]<>(setop_operand<1>()));
            return apply_limit(apply_setop_order_by<0>(std::move(chain)));
        }

        static consteval auto is_setop() -> bool {
            return spec.setop.enabled;
        }
        static consteval auto is_group_by_op() -> bool {
            return spec.group_by.enabled;
        }
        static consteval auto is_aggregate_op() -> bool {
            return spec.aggregation_count > 0 && !spec.group_by.enabled;
        }
        static consteval auto is_distinct_op() -> bool {
            return spec.distinct.enabled;
        }
        static consteval auto is_values_op() -> bool {
            return spec.values.enabled;
        }
        static consteval auto is_first_op() -> bool {
            constexpr auto op = spec.operation.view();
            return op == "first" || op == "first_where";
        }
        static consteval auto is_get_op() -> bool {
            return spec.operation.view() == "get_where";
        }

        // GROUP BY ... HAVING: the HAVING tree is built exactly like the WHERE tree, over the base model.
        static auto with_having(auto&& grouped) {
            if constexpr (spec.group_by.having.enabled) {
                return std::forward<decltype(grouped)>(grouped).having(
                        build_where_node<
                                spec.group_by.having,
                                Model,
                                static_cast<std::size_t>(spec.group_by.having.root)>()
                );
            } else {
                return std::forward<decltype(grouped)>(grouped);
            }
        }

      public:
        static auto build_distinct_stmt(auto& qs) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs
                        .template distinct<selector_for<dispatch_field<Model>(spec.distinct.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.distinct.field_count()>{});
        }

        // `values<f1, ...>()` (#867): the column projection, duplicates kept. Same field-pack
        // expansion as build_distinct_stmt.
        static auto build_values_stmt(auto& qs) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs.template values<selector_for<dispatch_field<Model>(spec.values.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.values.field_count()>{});
        }

        // WhereModel defaults to Model. Pass a related model type for JOIN tests
        // where WHERE conditions live on the joined table rather than Model.
        template <typename WhereModel = Model> static auto build_qs() -> QuerySet<Model, ConnType, true> {
            QuerySet<Model, ConnType> qs;
            apply_join(qs);
            apply_where<WhereModel>(qs);
            return apply_limit(apply_order_by(std::move(qs))).to_finalized();
        }

        static auto build_terminal(auto& qs) {
            if constexpr (is_setop()) {
                return build_setop();
            } else if constexpr (is_aggregate_op()) {
                return apply_aggregations(qs);
            } else if constexpr (is_group_by_op()) {
                return build_group_by_query(qs);
            } else if constexpr (is_distinct_op()) {
                return build_distinct_stmt(qs);
            } else if constexpr (is_values_op()) {
                return build_values_stmt(qs);
            } else if constexpr (is_first_op()) {
                return qs.first();
            } else if constexpr (is_get_op()) {
                return qs.get();
            } else {
                return qs.select();
            }
        }
    };

} // namespace storm::orm::query_builder
