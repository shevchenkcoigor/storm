module;
#ifdef STORM_CLANG_TIDY
#include <meta> // clang-tidy segfaults on a TU without a textual <meta> (#756); the real build never defines this
#endif

export module storm_orm_fields;

import std;
import storm_std_meta;

import storm_orm_field_attr;    // ValidFieldInfo
import storm_orm_relation_meta; // is_relation_field
import storm_orm_where;         // Field<M> — the proxy's base class

// Field-selector proxies (#518). NOT a leaf module: FieldRef derives from
// where::Field<M>, so this sits ABOVE storm_orm_where in the layering
//   field_attr, relation_meta -> where -> fields -> queryset/aggregate/orderby/insert
// Never make where.cppm import this module — that would be a cycle. (This is why
// the removed f<> helper could never have been widened to take a proxy: it lived
// below this module and could not call selector_info.)
export namespace storm::meta {

    // The public field-selector proxy.
    //
    // Deriving from Field<M> is what makes the bare form `fields::Person.age == 30`
    // work: Field<M> is STATELESS — every operator reads field_name_sv, a static
    // constexpr off the template parameter — so inheritance donates all twelve
    // comparison/in/like/between/is_null/collate operators with no duplication and
    // no per-instance data. Mutation-verified: removing the base breaks member
    // access on the proxy.
    //
    // The member reflection also lives in the TYPE (via M), which is what lets
    // `fields::Person.age` be used as a template argument for the NTTP positions.
    template <std::meta::info M>
        requires(ValidFieldInfo<M> && !is_relation_field(M))
    struct FieldRef : storm::orm::where::Field<M> {
        static constexpr std::meta::info member = M;
    };

    // The relation-member proxy: m2m and reverse-FK containers.
    //
    // Deliberately does NOT derive from where::Field<M>. A relation is not a
    // column — it has no name in the row and cannot be compared — so the proxy
    // carries no comparison/in/like/between operators. That is what keeps
    // `where(fields::Article.tags == x)` a compile error (#408) while still
    // letting `join<fields::Article.tags>()` name the relation. The two proxies
    // are distinguished by TYPE, not by a runtime flag, so the wrong one in the
    // wrong position fails at the call site.
    //
    // where::Field<M>'s own constraint is !is_relation_field(M), so a relation
    // member could not be given a FieldRef even if we wanted to.
    template <std::meta::info M>
        requires(ValidFieldInfo<M> && is_relation_field(M))
    struct RelationRef {
        static constexpr std::meta::info member = M;

        // Comparison is DELETED WITH A REASON rather than simply absent. Both
        // reject `where(fields::Article.tags == x)`, but an absent operator
        // yields only "invalid operands to binary expression ('const
        // RelationRef<^^(declaration)>' and 'int')" — which names neither the
        // member nor the fix. `= delete("...")` puts the guidance in the
        // diagnostic itself. One template covers every comparison operator; the
        // rewritten candidates from operator<=> are not synthesised because the
        // deleted operator is still a declared, selected candidate.
        //
        // Comparing a relation to a SCALAR is meaningless and stays rejected.
        // Filtering THROUGH a relation ("articles having a tag named X") is a
        // real capability Storm does not have yet — tracked as #553, which would
        // add a traversal spelling such as .any(expr) emitting an EXISTS
        // subquery. It would not loosen these operators.
        template <typename V>
        auto operator==(const V&) const
                -> bool = delete ("this member is a relation (many_to_many / reverse_fk), not a column: it cannot be "
                                  "compared in where(). Use join<fields::Model.relation>() to eager-load it.");
        template <typename V>
        auto operator<=>(const V&) const
                -> std::partial_ordering = delete ("this member is a relation (many_to_many / reverse_fk), not a "
                                                   "column: it cannot be ordered or compared in where(). Use "
                                                   "join<fields::Model.relation>() to eager-load it.");
    };

    namespace detail {
        template <typename S> constexpr bool        is_field_ref              = false;
        template <std::meta::info M> constexpr bool is_field_ref<FieldRef<M>> = true;

        template <typename S> constexpr bool        is_relation_ref                 = false;
        template <std::meta::info M> constexpr bool is_relation_ref<RelationRef<M>> = true;
    } // namespace detail

    // True iff S is a generated column proxy (i.e. some FieldRef<M>).
    template <typename S>
    concept FieldSelector = detail::is_field_ref<std::remove_cvref_t<S>>;

    // True iff S is a generated relation proxy (i.e. some RelationRef<M>).
    template <typename S>
    concept RelationSelector = detail::is_relation_ref<std::remove_cvref_t<S>>;

    // Either generated proxy. The NTTP positions that accept BOTH (join/left_join,
    // which legitimately take relation members) convert through this.
    template <typename S>
    concept AnySelector = FieldSelector<S> || RelationSelector<S>;

    // Object -> info converter for the NTTP selector positions (order_by, distinct,
    // values, group_by, the aggregates, join, update, upsert). Deliberately a free
    // function template rather than a member (`fields::Person.age.to_info()`): a
    // member would leak the internal representation into the public surface, and
    // std::meta::info is consteval-only so it cannot be returned into a runtime
    // expression.
    //
    // Proxy-only: the transitional raw-`std::meta::info` branch is gone, along
    // with the last ^^Model::member call site it existed for. A raw info in a
    // selector position is now a compile error, which is the point — one public
    // spelling, not two.
    //
    // CONSTRAINED, deliberately: unconstrained, this would treat any NTTP as a
    // proxy and hard-error on `::member` deep inside the function. Call sites
    // rely on a bad selector being a SUBSTITUTION FAILURE — e.g.
    // tests/schema/test_fk_fields.cpp asserts `!join_accepts<&Task::assignee>`
    // (the removed pointer-to-member syntax) via a requires-expression, which
    // needs a soft failure to yield `false`.
    template <auto S>
        requires AnySelector<std::remove_cvref_t<decltype(S)>>
    consteval auto selector_info() -> std::meta::info {
        return std::remove_cvref_t<decltype(S)>::member;
    }

    // info -> proxy VALUE, picking FieldRef or RelationRef by what the member is.
    // The inverse of selector_info, for generic code that resolves a member by
    // NAME at compile time (the YAML-driven query builder and test runner) and
    // must then hand a selector to the public API. A hand-written call site
    // spells `fields::Model.member` and never needs this.
    template <std::meta::info M>
        requires ValidFieldInfo<M>
    consteval auto selector_for() {
        if constexpr (is_relation_field(M)) {
            return RelationRef<M>{};
        } else {
            return FieldRef<M>{};
        }
    }

    // The gate for the COLUMN selector positions (order_by, distinct, values,
    // group_by, the aggregates, update, upsert). Rejects a relation proxy on two
    // counts: FieldSelector excludes RelationRef by type, and !is_relation_field
    // re-checks the member itself. join/left_join do NOT use this concept — they
    // take relations legitimately and gate on JoinableFields instead.
    //
    // The first conjunct is REDUNDANT with selector_info's own constraint: a bad S
    // already makes selector_info<S>() a substitution failure, which maps to false
    // inside an atomic constraint regardless of conjunct order (verified by
    // compiling this concept with the conjuncts swapped — clean either way). It is
    // kept as an explicit statement of what this concept accepts, NOT as a
    // load-bearing guard, so do not preserve it under that mistaken rationale.
    template <auto S>
    concept ValidSelector = FieldSelector<std::remove_cvref_t<decltype(S)>> && ValidFieldInfo<selector_info<S>()> &&
                            !is_relation_field(selector_info<S>());

    // Selector form of SingleColumnMember (field_attr.cppm) — unwraps a field-selector NTTP
    // via selector_info<S>() before testing it. False only for an FK whose target has a
    // composite primary key: that member spreads over "<member>_<part>" columns, and
    // neither COUNT(DISTINCT) nor ORDER BY has a correct multi-column form to fall back to
    // — COUNT(DISTINCT a, b) is a SYNTAX ERROR in SQLite, while PostgreSQL parses it as a
    // row constructor and counts something else entirely; ORDER BY's ASC/DESC/COLLATE
    // suffix attaches once, after ONE name, so a comma list would silently apply it to the
    // LAST part only. Both reject the member at the call site instead (#570). Consolidates
    // what used to be two separate concepts making the same call (#613): aggregate.cppm's
    // COUNT(DISTINCT) gate and orderby.cppm's ORDER BY gate (which ORs in its own
    // modifier-arg escape on top). Not self-guarding — selector_info<S>() still needs S to
    // satisfy AnySelector, but per ValidSelector's comment above a bad S maps to `false`
    // here rather than hard-erroring, so an unguarded call site degrades gracefully.
    template <auto S>
    concept SingleColumnSelector = SingleColumnMember<selector_info<S>()>;

    // The define_aggregate spec list for `owner`: one proxy-typed member per
    // model member, named after it. A persisted column gets a FieldRef (usable
    // everywhere); an m2m/reverse-FK relation gets a RelationRef (join targets
    // only, no comparison operators). Emitting BOTH is what lets
    // `join<fields::Article.tags>()` exist while `where(fields::Article.tags == x)`
    // stays a compile error — the distinction is carried by the proxy TYPE.
    consteval auto field_specs_for(std::meta::info owner) -> std::vector<std::meta::info> {
        std::vector<std::meta::info> specs;
        for (const std::meta::info member :
             std::meta::nonstatic_data_members_of(owner, std::meta::access_context::unchecked())) {
            const std::meta::info proxy = is_relation_field(member) ? ^^RelationRef : ^^FieldRef;
            specs.push_back(
                    std::meta::data_member_spec(
                            std::meta::substitute(proxy, {std::meta::reflect_constant(member)}),
                            {.name = std::meta::identifier_of(member)}
                    )
            );
        }
        return specs;
    }

} // namespace storm::meta
