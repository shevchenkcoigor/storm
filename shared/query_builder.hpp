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
    // By-name member lookup — shared by the YAML test runners and the benchmark
    // ========================================================================

    // Reflection of the ONE public member function template of class T named
    // `name`, or a null info when T has none or several (#834). Used to call a query
    // method chosen by a YAML string without a branch per method. Overloads are
    // rejected: a splice calls the reflected template directly, so with two
    // candidates overload resolution would be skipped and the first one picked.
    template <typename T> consteval auto member_function_named(std::string_view name) -> std::meta::info {
        std::meta::info found;
        std::size_t     matches = 0;
        for (std::meta::info m : std::meta::members_of(^^T, std::meta::access_context::unchecked()))
            if (std::meta::is_function_template(m) && std::meta::is_public(m) && std::meta::has_identifier(m) &&
                std::meta::identifier_of(m) == name) {
                found = m;
                ++matches;
            }
        return matches == 1 ? found : std::meta::info{};
    }

    // The member named by `field`, or a null info for an empty name (COUNT(*)).
    template <typename Model> consteval auto field_or_null(std::string_view field) -> std::meta::info {
        return field.empty() ? std::meta::info{} : dispatch_field<Model>(field);
    }

    // Call q.Fn<FieldRef<Field>{}>(), or q.Fn<>() for a null Field (#834). The
    // query methods the YAML names (count/sum/avg/min/max/count_distinct) are all
    // selector-pack templates, so one splice serves every one of them.
    template <std::meta::info Fn, std::meta::info Field> auto call_query_method(auto&& q) {
        static_assert(
                Fn != std::meta::info{}, "YAML names no single public member function template of the query type"
        );
        // Only COUNT has a no-field form (COUNT(*)). Every other aggregate's
        // constraints accept an empty pack, which would emit e.g. SUM(*) and fail
        // only at runtime, so a missing field is rejected here at compile time.
        static_assert(
                Field != std::meta::info{} || std::meta::identifier_of(Fn) == "count",
                "YAML aggregate needs a field: only count may omit it"
        );
        if constexpr (Field == std::meta::info{})
            return std::forward<decltype(q)>(q).template[:Fn:]<>();
        else
            return std::forward<decltype(q)>(q).template[:Fn:]<storm::meta::FieldRef<Field>{}>();
    }

    // ========================================================================
    // WHERE operator resolution by name (#835)
    //
    // A symbolic YAML operator (> >= < <= == !=) is the C++ operator's own symbol,
    // found via operator_of/symbol_of. A word operator maps to a member name by
    // lower-casing and turning spaces into underscores: LIKE -> like,
    // IS NOT NULL -> is_not_null. No per-operator branch.
    // ========================================================================
    consteval auto ascii_lower(char c) -> char {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    consteval auto is_symbolic_operator(std::string_view op) -> bool {
        return !op.empty() && !(op.front() >= 'A' && op.front() <= 'Z');
    }

    consteval auto word_operator_matches(std::string_view identifier, std::string_view op) -> bool {
        if (identifier.size() != op.size())
            return false;
        for (std::size_t i = 0; i < op.size(); ++i) {
            const char c        = op[i];
            const char expected = c == ' ' ? '_' : ascii_lower(c);
            if (identifier[i] != expected)
                return false;
        }
        return true;
    }

    // Does member m of a field proxy implement `op`? Symbolic operators are matched
    // on operator templates only: the non-template nullopt_t overloads of ==/!= are
    // a different spelling (`== std::nullopt`) of IS NULL, not a value comparison.
    consteval auto operator_member_matches(std::meta::info m, std::string_view op) -> bool {
        if (!std::meta::is_public(m))
            return false;
        if (is_symbolic_operator(op))
            return std::meta::is_operator_function_template(m) && std::meta::symbol_of(std::meta::operator_of(m)) == op;
        return (std::meta::is_function(m) || std::meta::is_function_template(m)) && std::meta::has_identifier(m) &&
               word_operator_matches(std::meta::identifier_of(m), op);
    }

    // The ONE callable implementing `op` on T (or on a base of T) for argument types
    // Ts, or a null info. IN is the only variadic operator and takes every value
    // type; every other operator template has a single type parameter (BETWEEN's two
    // values share it).
    //
    // A template is judged by its SPECIALIZATION for Ts, because a name can carry
    // several templates: where::Field declares a real `operator==` and a `= delete`d
    // twin that only a composite-FK member satisfies. The reflection of a template
    // does not say it is deleted (is_deleted is false); the specialization does, and
    // an unsatisfied constraint makes can_substitute false. Both are skipped.
    template <typename T, typename... Ts> consteval auto operator_callable(std::string_view op) -> std::meta::info {
        std::vector<std::meta::info> args{^^Ts...};
        args.resize(op == "IN" ? sizeof...(Ts) : std::min<std::size_t>(1, sizeof...(Ts)));
        std::meta::info found;
        std::size_t     matches = 0;
        const auto      scan    = [&](std::meta::info cls) consteval {
            for (std::meta::info m : std::meta::members_of(cls, std::meta::access_context::unchecked())) {
                if (!operator_member_matches(m, op))
                    continue;
                std::meta::info callable = m;
                if (std::meta::is_function_template(m)) {
                    if (!std::meta::can_substitute(m, args))
                        continue;
                    callable = std::meta::substitute(m, args);
                }
                if (std::meta::is_deleted(callable))
                    continue;
                found = callable;
                ++matches;
            }
        };
        scan(^^T);
        for (std::meta::info base : std::meta::bases_of(^^T, std::meta::access_context::unchecked()))
            scan(std::meta::type_of(base));
        return matches == 1 ? found : std::meta::info{};
    }

    template <typename FE, auto op_str, typename... Ts> consteval auto resolve_operator() -> std::meta::info {
        constexpr std::meta::info callable = operator_callable<FE, Ts...>(op_str.view());
        static_assert(
                callable != std::meta::info{},
                "YAML operator names no single callable of the field proxy for these value types"
        );
        return callable;
    }

    // ========================================================================
    // Operator/aggregate validation concepts
    // ========================================================================
    template <auto op_str, typename FieldType>
    concept ValidOperator = []() consteval {
        constexpr std::string_view op = op_str.view();
        // Ordering/BETWEEN on a UUID column is rejected here too (#622), matching
        // where.cppm's OrderableOperand — otherwise rejection lands deep inside
        // resolve_operator's reflection lookup instead of at this named gate.
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

    // Value-count shape per operator (#835): a comparison and LIKE take exactly one
    // value, BETWEEN two, IN any number (empty is "1 = 0"), IS [NOT] NULL none. Before this check a
    // `values: []` LIKE read values[0] and compiled to LIKE '' (matches nothing).
    template <auto op_str, std::size_t count>
    concept ValidValueCount = []() consteval {
        constexpr std::string_view op = op_str.view();
        if (op == "BETWEEN")
            return count == 2;
        if (op == "IN")
            return true;
        if (op == "IS NULL" || op == "IS NOT NULL")
            return count == 0;
        return count == 1;
    }();

    consteval auto iequals(std::string_view a, std::string_view b) -> bool {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (ascii_lower(a[i]) != ascii_lower(b[i]))
                return false;
        return true;
    }

    // The Collate enumerator named `name` (case-insensitive), or a null info.
    consteval auto find_collate(std::string_view name) -> std::meta::info {
        for (std::meta::info e : std::meta::enumerators_of(^^storm::orm::utilities::Collate))
            if (e != std::meta::enumerators_of(^^storm::orm::utilities::Collate).front() &&
                iequals(std::meta::identifier_of(e), name))
                return e;
        return std::meta::info{};
    }

    // An unknown collation is a compile error rather than a silent NoCase.
    template <auto collate_str>
    concept ValidCollate = collate_str.view().empty() || find_collate(collate_str.view()) != std::meta::info{};

    // Extract numeric value from any TypedValue-like NTTP (duck-typed on Kind/as_double/as_int).
    template <auto tv> consteval auto spec_numeric_value() {
        if constexpr (tv.kind == std::remove_cvref_t<decltype(tv)>::Kind::Double) {
            return tv.as_double;
        } else {
            return tv.as_int;
        }
    }

    // The WHERE/HAVING condition `FieldRef<fi> <op> vs...` for a YAML operator.
    // Shared by QueryBuilder and the test runners' where-tree builder, so the
    // operator-name mapping exists once (#835).
    template <auto fi, auto op_str, typename... Ts>
        requires ValidOperator<op_str, typename storm::meta::FieldRef<fi>::FieldType> &&
                 ValidValueCount<op_str, sizeof...(Ts)>
    auto build_compare_expr(Ts&&... vs) {
        constexpr auto method = resolve_operator<storm::meta::FieldRef<fi>, op_str, std::remove_cvref_t<Ts>...>();
        // The operator templates are instantiated for the decayed type, so their
        // forwarding parameter is an rvalue reference: pass a copy of each value, which
        // also lets a caller hand over a const lvalue.
        return (storm::meta::FieldRef<fi>{}.[:method:](std::remove_cvref_t<Ts>(vs)...));
    }

    // Default FK resolver (#837): a JOIN names its FK by the member name on Model,
    // which dispatch_field already resolves. The benchmark overrides it with its own
    // registry lookup.
    template <typename Model> struct MemberNameResolver {
        consteval auto operator()(std::string_view name) const -> std::meta::info {
            return dispatch_field<Model>(name);
        }
    };
    template <typename Model> inline constexpr MemberNameResolver<Model> member_name_resolver{};

    // ========================================================================
    // QueryBuilder<Model, spec, fk_resolver>
    //
    // Schema-agnostic: `spec` is any NTTP whose nested fields satisfy the
    // spec protocol (spec.where, spec.join, spec.order_by[], spec.group_by,
    // spec.distinct, spec.limit, spec.aggregate, spec.setop, spec.operation).
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

    template <
            typename Model,
            auto spec,
            auto fk_resolver  = member_name_resolver<Model>,
            typename ConnType = storm::db::sqlite::Connection>
        requires SpecWithinLimits<spec>
    class QueryBuilder {
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
            return std::meta::extract<Collate>(find_collate(s));
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
        // The YAML `func` IS the member name (count/count_distinct/sum/avg/min/max),
        // resolved by reflection; ValidAggregate stays as the allow-list so a YAML
        // string can only reach those members, not any same-named one. A case with
        // no aggregate field is COUNT(*).
        template <typename Stmt, bool HasField = !spec.aggregate.field.empty()>
            requires(!HasField) || ValidAggregate<spec.aggregate.func>
        static consteval auto resolve_aggregate() -> std::meta::info {
            return member_function_named<std::remove_cvref_t<Stmt>>(HasField ? spec.aggregate.func.view() : "count");
        }

        template <typename Stmt> static auto apply_aggregate(Stmt& stmt) {
            static_assert(
                    spec.aggregate.field.empty() || has_field<Model>(spec.aggregate.field.view()),
                    "aggregate field not found on model"
            );
            constexpr auto method = resolve_aggregate<Stmt>();
            return call_query_method<method, field_or_null<Model>(spec.aggregate.field.view())>(stmt);
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
        static auto build_distinct_stmt(auto& qs) {
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                return qs
                        .template distinct<selector_for<dispatch_field<Model>(spec.distinct.fields[Is].view())>()...>();
            }(std::make_index_sequence<spec.distinct.field_count()>{});
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
