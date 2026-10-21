#pragma once

// TODO: Convert to storm_orm_query_builder module once clang-p2996 no longer
// crashes when a consumer imports a module that exports templates containing
// ^^QuerySet<Model>::template join<FKs...> splice expressions. Tracked in
// https://github.com/spiritEcosse/storm/issues/256

// Must be included AFTER `import storm;` in each consumer TU.

#include <meta>
import storm_orm_utilities;
import storm_orm_fields;

namespace storm::orm::query_builder {

    using storm::meta::selector_for;
    using storm::orm::utilities::Collate;

    // ========================================================================
    // Field dispatcher — compile-time field lookup by name
    // ========================================================================
    template <typename Model> consteval auto dispatch_field(std::string_view field_name) -> std::meta::info {
        for (std::meta::info member :
             std::meta::nonstatic_data_members_of(^^Model, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(member) == field_name)
                return member;
        }
        std::unreachable();
    }

    template <typename Model> consteval auto has_field(std::string_view field_name) -> bool {
        for (std::meta::info member :
             std::meta::nonstatic_data_members_of(^^Model, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(member) == field_name)
                return true;
        }
        return false;
    }

    // ========================================================================
    // Operator/aggregate validation concepts
    // ========================================================================
    template <auto op_str, typename FieldType>
    concept ValidOperator = []() consteval {
        constexpr std::string_view op = op_str.view();
        // Ordering/BETWEEN on a UUID column is rejected here too (#622), matching
        // where.cppm's OrderableOperand — otherwise rejection lands deep inside
        // resolve_comparison_op's reflection splice instead of at this named gate.
        constexpr bool is_uuid = std::is_same_v<orm::utilities::optional_inner_type_t<FieldType>, orm::utilities::UUID>;
        if (op == "==" || op == "!=" || op == "LIKE" || op == "IN") {
            return true;
        }
        if (op == ">" || op == ">=" || op == "<" || op == "<=" || op == "BETWEEN") {
            return !is_uuid;
        }
        if (op == "IS NULL" || op == "IS NOT NULL") {
            return orm::where::NullableField<FieldType>;
        }
        return false;
    }();

    template <auto func_str>
    concept ValidAggregate = []() consteval {
        constexpr std::string_view func = func_str.view();
        return func == "count" || func == "count_distinct" || func == "sum" || func == "avg" || func == "min" ||
               func == "max";
    }();

    // Extract numeric value from any TypedValue-like NTTP (duck-typed on Kind/as_double/as_int).
    template <auto tv> consteval auto spec_numeric_value() {
        if constexpr (tv.kind == std::remove_cvref_t<decltype(tv)>::Kind::Double) {
            return tv.as_double;
        } else {
            return tv.as_int;
        }
    }

    // ========================================================================
    // QueryBuilder<Model, spec, fk_resolver>
    //
    // Schema-agnostic: `spec` is any NTTP whose nested fields satisfy the
    // spec protocol (spec.where, spec.join, spec.order_by[], spec.group_by,
    // spec.distinct, spec.limit, spec.aggregate, spec.setop, spec.operation).
    //
    // fk_resolver: consteval callable (string_view) -> std::meta::info for
    // JOIN field dispatch. Pass a no-op for tests that don't use JOINs.
    // ========================================================================
    template <typename Model, auto spec, auto fk_resolver, typename ConnType = storm::db::sqlite::Connection>
    class QueryBuilder {
        // ----------------------------------------------------------------
        // Operator dispatch — split into 3 helpers to stay under CCN limit
        // ----------------------------------------------------------------
        template <typename FE, auto op_str, typename... Ts>
        static consteval auto resolve_comparison_op() -> std::meta::info {
            constexpr std::string_view op = op_str.view();
            if constexpr (op == ">") {
                return ^^FE::template operator><Ts...>;
            }
            if constexpr (op == ">=") {
                return ^^FE::template operator>= <Ts...>;
            }
            if constexpr (op == "<") {
                return ^^FE::template operator< <Ts...>;
            }
            if constexpr (op == "<=") {
                return ^^FE::template operator<= <Ts...>;
            }
            if constexpr (op == "==") {
                return ^^FE::template operator== <Ts...>;
            }
            if constexpr (op == "!=") {
                return ^^FE::template operator!= <Ts...>;
            }
            std::unreachable();
        }

        template <typename FE, auto op_str, typename... Ts>
        static consteval auto resolve_range_op() -> std::meta::info {
            constexpr std::string_view op = op_str.view();
            if constexpr (op == "LIKE") {
                return ^^FE::like;
            }
            if constexpr (op == "BETWEEN") {
                using V = std::tuple_element_t<0, std::tuple<Ts..., void>>;
                return ^^FE::template between<V>;
            }
            if constexpr (op == "IN") {
                return ^^FE::template in<Ts...>;
            }
            std::unreachable();
        }

        template <typename FE, typename FieldType, auto op_str>
        static consteval auto resolve_null_op() -> std::meta::info {
            constexpr std::string_view op = op_str.view();
            if constexpr (op == "IS NULL" && orm::where::NullableField<FieldType>) {
                return ^^FE::is_null;
            }
            if constexpr (op == "IS NOT NULL" && orm::where::NullableField<FieldType>) {
                return ^^FE::is_not_null;
            }
            std::unreachable();
        }

        template <auto fi, auto op_str, typename... Ts>
            requires ValidOperator<op_str, typename storm::meta::FieldRef<fi>::FieldType>
        static consteval auto resolve_operator() -> std::meta::info {
            using FE                      = storm::meta::FieldRef<fi>;
            using FieldType               = typename FE::FieldType;
            constexpr std::string_view op = op_str.view();

            if constexpr (op == ">" || op == ">=" || op == "<" || op == "<=" || op == "==" || op == "!=")
                return resolve_comparison_op<FE, op_str, Ts...>();
            else if constexpr (op == "LIKE" || op == "BETWEEN" || op == "IN")
                return resolve_range_op<FE, op_str, Ts...>();
            else
                return resolve_null_op<FE, FieldType, op_str>();
        }

        template <auto fi, auto op_str, typename... Ts>
            requires ValidOperator<op_str, typename storm::meta::FieldRef<fi>::FieldType>
        static auto build_compare_expr(Ts&&... vs) {
            constexpr auto method = resolve_operator<fi, op_str, std::remove_cvref_t<Ts>...>();
            return (storm::meta::FieldRef<fi>{}.[:method:](std::forward<Ts>(vs)...));
        }

        // ----------------------------------------------------------------
        // typed_value_as
        // ----------------------------------------------------------------
        template <typename FieldType, auto tv> static consteval auto typed_value_as() {
            if constexpr (std::is_same_v<FieldType, bool>)
                return bool{tv.as_bool};
            else if constexpr (std::is_floating_point_v<FieldType>)
                return FieldType{tv.as_double};
            else
                return FieldType{tv.as_int};
        }

        // ----------------------------------------------------------------
        // WHERE
        // ----------------------------------------------------------------
        template <typename WhereModel, std::size_t I> static auto build_condition_expr() {
            constexpr auto& cond = spec.where.conditions[I];
            static_assert(has_field<WhereModel>(cond.field.view()), "WHERE condition field not found on model");
            constexpr auto             fi = dispatch_field<WhereModel>(cond.field.view());
            constexpr std::string_view op = cond.op.view();
            using FieldType               = typename[:std::meta::type_of(fi):];

            if constexpr (op == "LIKE") {
                return build_compare_expr<fi, cond.op>(cond.values[0].as_string.view());
            } else if constexpr (op == "BETWEEN") {
                return build_compare_expr<fi, cond.op>(
                        typed_value_as<FieldType, cond.values[0]>(), typed_value_as<FieldType, cond.values[1]>()
                );
            } else if constexpr (op == "IN") {
                return []<std::size_t... Is>(std::index_sequence<Is...>) {
                    constexpr auto& c = spec.where.conditions[I];
                    return build_compare_expr<fi, c.op>(typed_value_as<FieldType, c.values[Is]>()...);
                }(std::make_index_sequence<cond.value_count>{});
            } else if constexpr (op == "IS NULL" || op == "IS NOT NULL") {
                return build_compare_expr<fi, cond.op>();
            } else if constexpr (cond.values[0].kind == decltype(cond.values[0].kind)::String) {
                return build_compare_expr<fi, cond.op>(cond.values[0].as_string.view());
            } else {
                return build_compare_expr<fi, cond.op>(typed_value_as<FieldType, cond.values[0]>());
            }
        }

        template <typename WhereModel> static auto apply_where(QuerySet<Model, ConnType>& qs) -> void {
            if constexpr (spec.where.enabled) {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    if constexpr (spec.where.combine_and)
                        qs = qs.where((build_condition_expr<WhereModel, Is>() && ...));
                    else
                        qs = qs.where((build_condition_expr<WhereModel, Is>() || ...));
                }(std::make_index_sequence<spec.where.condition_count>{});
            }
        }

        // ----------------------------------------------------------------
        // JOIN
        // ----------------------------------------------------------------
        static consteval auto join_fk_name(std::size_t i) -> std::string_view {
            if (spec.join.fk_count >= 2)
                return spec.join.fks[i].view();
            return spec.join.fk.view();
        }

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

        static auto apply_join(auto& qs) -> void {
            if constexpr (spec.join.enabled) {
                constexpr std::size_t N = spec.join.fk_count >= 2 ? spec.join.fk_count : std::size_t{1};
                [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    dispatch_join_type<selector_for<fk_resolver(join_fk_name(Is))>()...>(qs);
                }(std::make_index_sequence<N>{});
            }
        }

        // ----------------------------------------------------------------
        // ORDER BY
        // ----------------------------------------------------------------
        static consteval auto parse_collate(std::string_view s) -> Collate {
            if (s == "BINARY")
                return Collate::Binary;
            if (s == "RTRIM")
                return Collate::RTrim;
            return Collate::NoCase;
        }

        static consteval auto enabled_order_by_count() -> std::size_t {
            std::size_t n = 0;
            for (std::size_t i = 0; i < std::remove_cvref_t<decltype(spec)>::MAX_ORDER_BY; ++i)
                if (spec.order_by[i].enabled)
                    ++n;
            return n;
        }

        template <std::size_t I> static auto apply_order_by_impl(auto qs) {
            constexpr std::size_t MAX_OB = std::remove_cvref_t<decltype(spec)>::MAX_ORDER_BY;
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
        template <typename Stmt, bool HasField = !spec.aggregate.field.empty()>
            requires(!HasField) || ValidAggregate<spec.aggregate.func>
        static consteval auto resolve_aggregate() -> std::meta::info {
            if constexpr (!HasField) {
                return ^^Stmt::template count<>;
            } else {
                static_assert(has_field<Model>(spec.aggregate.field.view()), "aggregate field not found on model");
                constexpr std::string_view func = spec.aggregate.func.view();
                constexpr auto             fi   = selector_for<dispatch_field<Model>(spec.aggregate.field.view())>();
                if (func == "count")
                    return ^^Stmt::template count<fi>;
                if (func == "count_distinct")
                    return ^^Stmt::template count_distinct<fi>;
                if (func == "sum")
                    return ^^Stmt::template sum<fi>;
                if (func == "avg")
                    return ^^Stmt::template avg<fi>;
                if (func == "min")
                    return ^^Stmt::template min<fi>;
                if (func == "max")
                    return ^^Stmt::template max<fi>;
                std::unreachable();
            }
        }

        template <typename Stmt> static auto apply_aggregate(Stmt& stmt) {
            constexpr auto method = resolve_aggregate<Stmt>();
            return stmt.[:method:]();
        }

        static auto build_distinct_stmt(auto& qs) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs
                        .template distinct<selector_for<dispatch_field<Model>(spec.distinct.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.distinct.field_count()>{});
        }

        static auto build_group_by_query(auto& qs) {
            auto gb = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs
                        .template group_by<selector_for<dispatch_field<Model>(spec.group_by.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.group_by.field_count()>{});

            if constexpr (spec.group_by.having.enabled) {
                static_assert(has_field<Model>(spec.group_by.having.field.view()), "HAVING field not found on model");
                constexpr auto hf = dispatch_field<Model>(spec.group_by.having.field.view());
                gb                = gb.having(
                        build_compare_expr<hf, spec.group_by.having.op>(
                                auto{spec_numeric_value<spec.group_by.having.value>()}
                        )
                );
            }
            return apply_aggregate(gb);
        }

        static consteval auto resolve_setop() -> std::meta::info {
            constexpr std::string_view type = spec.setop.type.view();
            if (type == "union")
                return ^^QuerySet<Model, ConnType>::template union_<>;
            if (type == "union_all")
                return ^^QuerySet<Model, ConnType>::template union_all<>;
            if (type == "except")
                return ^^QuerySet<Model, ConnType>::template except_<>;
            return ^^QuerySet<Model, ConnType>::template intersect_<>;
        }

        struct SetOpThresholds {
            static constexpr int OVERLAP_LEFT  = 55;
            static constexpr int OVERLAP_RIGHT = 35;
            static constexpr int SPLIT_BOUND   = 45;
            int                  left          = 0;
            int                  right         = 0;
            bool                 overlap       = false;
        };
        static consteval auto setop_thresholds() -> SetOpThresholds {
            constexpr std::string_view type    = spec.setop.type.view();
            constexpr bool             overlap = (type == "except" || type == "intersect");
            return {overlap ? SetOpThresholds::OVERLAP_LEFT : SetOpThresholds::SPLIT_BOUND,
                    overlap ? SetOpThresholds::OVERLAP_RIGHT : SetOpThresholds::SPLIT_BOUND,
                    overlap};
        }

        static auto build_setop(auto& /*qs*/) {
            constexpr auto            thr = setop_thresholds();
            QuerySet<Model, ConnType> qs_left_base;
            // The split field is resolved by NAME through the same dispatch_field the
            // rest of this builder uses, rather than splicing ^^Model::age directly:
            // Model is a template parameter, so `fields::Model.age` cannot be spelled
            // here (a fields:: proxy is generated per concrete model, #518).
            constexpr auto            age_fi  = dispatch_field<Model>("age");
            auto                      qs_left = qs_left_base.where(storm::meta::FieldRef<age_fi>{} < thr.left);
            QuerySet<Model, ConnType> qs_right_base;
            auto           qs_right = thr.overlap ? qs_right_base.where(storm::meta::FieldRef<age_fi>{} > thr.right)
                                                  : qs_right_base.where(storm::meta::FieldRef<age_fi>{} >= thr.right);
            constexpr auto method   = resolve_setop();
            auto           builder  = qs_left.[:method:](qs_right);
            builder                 = apply_order_by(std::move(builder));
            builder                 = apply_limit(std::move(builder));
            return builder;
        }

        static consteval auto is_setop() -> bool {
            return spec.setop.enabled;
        }
        static consteval auto is_group_by_op() -> bool {
            return spec.group_by.enabled;
        }
        static consteval auto is_aggregate_op() -> bool {
            return spec.aggregate.enabled && !spec.group_by.enabled;
        }
        static consteval auto is_distinct_op() -> bool {
            return spec.distinct.enabled;
        }
        static consteval auto is_first_op() -> bool {
            constexpr auto op = spec.operation.view();
            return op == "first" || op == "first_where";
        }
        static consteval auto is_get_op() -> bool {
            return spec.operation.view() == "get_where";
        }

      public:
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
                return build_setop(qs);
            } else if constexpr (is_aggregate_op()) {
                return apply_aggregate(qs);
            } else if constexpr (is_group_by_op()) {
                return build_group_by_query(qs);
            } else if constexpr (is_distinct_op()) {
                return build_distinct_stmt(qs);
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
