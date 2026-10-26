module;

#include <meta>

export module storm_orm_relation_meta;

import std;

import storm_orm_reflection_cache; // memoised (#694)
import storm_orm_ref_action;       // RefAction (junction ON DELETE policy) — #562

// Dependency-free leaf module (#408): the relation-annotation TYPES (ManyToMany,
// ReverseFk) and the "is this member a relation, not a persisted column?" detection
// predicates. Extracted here from storm_orm_statements_base so storm_orm_where can
// reject relation members in the field proxy without importing the statement module (which
// would create a layering cycle — base.cppm already imports where.cppm). Same leaf
// pattern as storm_orm_field_attr (#387), which holds the FK annotation + is_fk_field.
//
// Symbols live in storm::meta (alongside is_fk_field); storm_orm_model_concepts
// re-exposes them into storm::orm::statements::meta (storm_orm_statements_base
// re-exports that module) so existing meta:: callers and the public storm.cppm
// re-exports are unchanged.
export namespace storm::meta {

    // Many-to-many annotation (#203, #431). Phase 1 (auto-generated junction table):
    //   [[= storm::meta::many_to_many<>]] std::vector<Course> courses;
    //   [[= storm::meta::many_to_many<RefAction::Restrict>]] std::vector<Course> courses;
    // Phase 2 (explicit junction model):
    //   [[= storm::meta::many_to_many_through<Enrollment>]] std::vector<Course> courses;
    // A class-template annotation carries the optional through-model type AND the
    // junction ON DELETE policy (#431, both junction sides). Through type stays at
    // template arg [0] so is_m2m_auto / m2m_through_t are unchanged; the RefAction is
    // arg [1], defaulting to CASCADE (an orphaned junction row is meaningless).
    template <typename Through = void, RefAction JunctionOnDelete = RefAction::Cascade> struct ManyToMany {
        using through_type                            = Through;
        static constexpr RefAction junction_on_delete = JunctionOnDelete;
    };
    template <RefAction JunctionOnDelete = RefAction::Cascade>
    inline constexpr ManyToMany<void, JunctionOnDelete>              many_to_many{};
    template <typename Through> inline constexpr ManyToMany<Through> many_to_many_through{};

    // The TYPE of `member`'s annotation that is a specialization of `template_info`,
    // if any. Shared by m2m_annotation_type_of / reverse_fk_annotation_type_of — the
    // two class-template annotations differ only by which template to match.
    // Memoised per (member, template) (#694): is_relation_field asks it for both relation
    // templates on every member, from every concept and member scan.
    namespace detail {
        consteval auto scan_annotation_type_specializing(std::meta::info member, std::meta::info template_info)
                -> std::optional<std::meta::info> {
            for (const auto annotation : std::meta::annotations_of(member)) {
                const auto type = std::meta::type_of(annotation);
                if (std::meta::has_template_arguments(type) && std::meta::template_of(type) == template_info) {
                    return type;
                }
            }
            return std::nullopt;
        }
        template <std::meta::info Member, std::meta::info Template>
        inline constexpr std::optional<std::meta::info> annotation_type_specializing_v =
                scan_annotation_type_specializing(Member, Template);
    } // namespace detail

    consteval auto annotation_type_specializing(std::meta::info member, std::meta::info template_info)
            -> std::optional<std::meta::info> {
        return memoised<std::optional<std::meta::info>>(
                ^^detail::annotation_type_specializing_v,
                {std::meta::reflect_constant(member), std::meta::reflect_constant(template_info)}
        );
    }

    // Reflection of the ManyToMany<...> annotation TYPE carried by `member`, if any.
    consteval auto m2m_annotation_type_of(std::meta::info member) -> std::optional<std::meta::info> {
        return annotation_type_specializing(member, ^^ManyToMany);
    }

    consteval auto is_m2m_field(std::meta::info member) -> bool {
        return m2m_annotation_type_of(member).has_value();
    }

    // Reverse-FK annotation (#398): on a container member of the base model, it
    // declares the eager-load destination for "all <Base>, each with the <Owner>s
    // that point at them". The template argument is EITHER:
    //   - the owning model type — [[= reverse_fk<^^Task>]] vector<Task> tasks;
    //     (resolved to that model's unique FK back at the base), OR
    //   - a specific FK field — [[= reverse_fk<^^Task::assignee>]] vector<Task> tasks;
    //     (names the exact FK; the disambiguator when the owner has several FKs to
    //     the base, e.g. assignee vs reviewer).
    // The type form works when the owner is only forward-declared at the annotation
    // site (the cyclic Base⟷Owner case); the field form needs the owner complete.
    // Like ManyToMany, it is a class-template annotation (the flag annotations carry no
    // templated enumerator is impossible).
    template <std::meta::info Target> struct ReverseFk {
        static constexpr std::meta::info target = Target;
    };
    template <std::meta::info Target> inline constexpr ReverseFk<Target> reverse_fk{};

    // Reflection of the ReverseFk<...> annotation TYPE carried by `member`, if any.
    consteval auto reverse_fk_annotation_type_of(std::meta::info member) -> std::optional<std::meta::info> {
        return annotation_type_specializing(member, ^^ReverseFk);
    }

    consteval auto is_reverse_fk_field(std::meta::info member) -> bool {
        return reverse_fk_annotation_type_of(member).has_value();
    }

    // Combined relation-field predicate (#398): m2m container OR reverse_fk container.
    // The single chokepoint for "not a persisted column" — schema/CRUD invisibility
    // for both relation kinds falls out of every persisted-field array filtering here.
    // Gates where::Field<M> / meta::FieldRef<M> (#408) so a WHERE clause on a relation
    // member fails at the call site.
    consteval auto is_relation_field(std::meta::info member) -> bool {
        return is_m2m_field(member) || is_reverse_fk_field(member);
    }

} // namespace storm::meta
