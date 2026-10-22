module;

#include <meta>

export module storm_meta;

// storm::meta flag objects, RefAction, is_primary_member: re-exported to users
export import storm_orm_field_attr;

// Implementation dependencies: users see only the aliases declared below
import storm_orm_model_concepts;
import storm_orm_fields;

export namespace storm {

    namespace meta {
        // Many-to-many (#203)
        using orm::statements::meta::many_to_many;         // NOLINT(misc-unused-using-decls)
        using orm::statements::meta::many_to_many_through; // NOLINT(misc-unused-using-decls)
        using orm::statements::meta::ManyToMany;           // NOLINT(misc-unused-using-decls)

        // Reverse FK (#398)
        using orm::statements::meta::reverse_fk; // NOLINT(misc-unused-using-decls)
        using orm::statements::meta::ReverseFk;  // NOLINT(misc-unused-using-decls)

        // Foreign key (#431)
        using orm::statements::meta::fk; // NOLINT(misc-unused-using-decls)
        using orm::statements::meta::Fk; // NOLINT(misc-unused-using-decls)

        // Text length (#493)
        using orm::statements::meta::max_length; // NOLINT(misc-unused-using-decls)
        using orm::statements::meta::MaxLength;  // NOLINT(misc-unused-using-decls)
    } // namespace meta

    // Short spellings: primary, fk<>, ...
    using meta::auto_create;           // NOLINT(misc-unused-using-decls)
    using meta::auto_update;           // NOLINT(misc-unused-using-decls)
    using meta::full_unsigned;         // NOLINT(misc-unused-using-decls)
    using meta::indexed;               // NOLINT(misc-unused-using-decls)
    using meta::primary;               // NOLINT(misc-unused-using-decls)
    using meta::primary_assigned;      // NOLINT(misc-unused-using-decls)
    using meta::primary_autoincrement; // NOLINT(misc-unused-using-decls)
    using meta::primary_part;          // NOLINT(misc-unused-using-decls)
    using meta::signed_storage;        // NOLINT(misc-unused-using-decls)
    using meta::unique;                // NOLINT(misc-unused-using-decls)

    using meta::fk;                   // NOLINT(misc-unused-using-decls)
    using meta::Fk;                   // NOLINT(misc-unused-using-decls)
    using meta::many_to_many;         // NOLINT(misc-unused-using-decls)
    using meta::many_to_many_through; // NOLINT(misc-unused-using-decls)
    using meta::ManyToMany;           // NOLINT(misc-unused-using-decls)
    using meta::max_length;           // NOLINT(misc-unused-using-decls)
    using meta::MaxLength;            // NOLINT(misc-unused-using-decls)
    using meta::RefAction;            // NOLINT(misc-unused-using-decls)
    using meta::reverse_fk;           // NOLINT(misc-unused-using-decls)
    using meta::ReverseFk;            // NOLINT(misc-unused-using-decls)

    // Field-selector proxies (issue 518)
    using meta::AnySelector;      // NOLINT(misc-unused-using-decls)
    using meta::field_specs_for;  // NOLINT(misc-unused-using-decls)
    using meta::FieldRef;         // NOLINT(misc-unused-using-decls)
    using meta::FieldSelector;    // NOLINT(misc-unused-using-decls)
    using meta::RelationRef;      // NOLINT(misc-unused-using-decls)
    using meta::RelationSelector; // NOLINT(misc-unused-using-decls)
    using meta::selector_for;     // NOLINT(misc-unused-using-decls)
    using meta::selector_info;    // NOLINT(misc-unused-using-decls)
    using meta::ValidSelector;    // NOLINT(misc-unused-using-decls)
} // namespace storm
