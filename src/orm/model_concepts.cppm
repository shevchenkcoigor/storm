module;

// Model-validation concepts and the reflection helpers they rest on, split out of
// storm_orm_statements_base so code that only needs to ask "is this a valid model?"
// (storm_model, concept tests) does not import the whole statement hub. Depends only on
// the dependency-free leaves (field_attr, relation_meta, utilities); base.cppm
// re-exports this module, so every statement module keeps seeing these names unchanged.

#include <meta>

export module storm_orm_model_concepts;

import std;

import storm_orm_field_attr;
import storm_orm_relation_meta;
import storm_orm_utilities;

export namespace storm::orm::statements {

    namespace meta {
        // Canonical flag annotations + is_primary_member / find_primary_key / has_primary_key live in the
        // dependency-free storm_orm_field_attr leaf module (#387, #492); re-exposed here so statement modules keep
        // using the meta:: qualifier.
        using storm::meta::find_primary_key; // NOLINT(misc-unused-using-decls) — used by storm_orm_statements_base
        using storm::meta::has_primary_key;  // the ModelWithPrimaryKey<T> core; also runs on an info VALUE
        using storm::meta::is_primary_member;
        using storm::meta::is_primary_part_member; // NOLINT(misc-unused-using-decls) — #500 composite PK
        using storm::meta::ref_action_sql;         // NOLINT(misc-unused-using-decls) — used by storm_orm_schema
        using storm::meta::RefAction;

        // Per-attribute predicates (#421) re-exposed from the leaf so statement modules
        // keep the meta:: qualifier.
        using storm::meta::has_full_unsigned_attr;  // NOLINT(misc-unused-using-decls)
        using storm::meta::has_signed_storage_attr; // NOLINT(misc-unused-using-decls)
        using storm::meta::is_auto_create;
        using storm::meta::is_auto_update;
        using storm::meta::is_indexed; // NOLINT(misc-unused-using-decls) — used by statement modules
        using storm::meta::is_unique;  // NOLINT(misc-unused-using-decls) — used by statement modules

        // Many-to-many (#203, #431) and reverse-FK (#398) annotation TYPES and their
        // "is this a relation member, not a persisted column?" detection predicates live
        // in the storm_orm_relation_meta leaf module (#408) so storm_orm_where can gate
        // f<>() without importing this module. Re-exposed here so statement modules keep
        // the meta:: qualifier (mirrors how FK detection is re-exposed from field_attr).
        using storm::meta::is_m2m_field;
        using storm::meta::is_relation_field; // NOLINT(misc-unused-using-decls) — #408 chokepoint
        using storm::meta::is_reverse_fk_field;
        using storm::meta::m2m_annotation_type_of;
        using storm::meta::many_to_many;         // NOLINT(misc-unused-using-decls) — model-declaration spelling
        using storm::meta::many_to_many_through; // NOLINT(misc-unused-using-decls) — model-declaration spelling
        using storm::meta::ManyToMany;           // NOLINT(misc-unused-using-decls) — re-exported for storm.cppm
        using storm::meta::reverse_fk;           // NOLINT(misc-unused-using-decls) — model-declaration spelling
        using storm::meta::reverse_fk_annotation_type_of;
        using storm::meta::ReverseFk; // NOLINT(misc-unused-using-decls) — re-exported for storm.cppm

        // Foreign-key annotation (#431) lives in the storm_orm_field_attr leaf module so
        // every statement module can detect FK fields without importing this one. Re-exposed
        // here so statement modules keep using the meta:: qualifier.
        using storm::meta::
                append_column_name; // NOLINT(misc-unused-using-decls) — #422 canonical <identifier>[_id] writer
        using storm::meta::column_name_size; // NOLINT(misc-unused-using-decls) — #422 its byte-exact size companion
        using storm::meta::fk;               // NOLINT(misc-unused-using-decls)
        using storm::meta::Fk;               // NOLINT(misc-unused-using-decls)
        using storm::meta::fk_annotation_type_of; // NOLINT(misc-unused-using-decls)
        using storm::meta::fk_on_delete_action_of;
        using storm::meta::is_fk_field;

        // Text-length bound annotation (#493) — lives in the storm_orm_field_attr leaf
        // module so schema.cppm can emit VARCHAR(N)/CHECK without importing this one.
        using storm::meta::is_text_member; // NOLINT(misc-unused-using-decls)
        using storm::meta::max_length;     // NOLINT(misc-unused-using-decls) — model-declaration spelling
        using storm::meta::max_length_of;  // NOLINT(misc-unused-using-decls)
        using storm::meta::MaxLength;      // NOLINT(misc-unused-using-decls) — re-exported for storm.cppm

        // std::optional<T> → T (dealiased); any other type returned dealiased unchanged.
        // Shared optional-unwrap for FK-target queries. Detection goes through the
        // leaf module's is_optional_type (#509) — this site previously spelled the
        // same check as template_of(dealias(fields::std.optional<int>)), which is
        // equivalent but was the copy most likely to drift from the other four.
        consteval auto unwrap_optional_type(std::meta::info type) -> std::meta::info {
            const auto t = std::meta::dealias(type);
            if (storm::meta::is_optional_type(t)) {
                return std::meta::dealias(std::meta::template_arguments_of(t)[0]);
            }
            return t;
        }

        // True iff the FK field type `fk_type` refers to an entity that has a primary key.
        // The info-value equivalent of the ValidForeignKey<FieldType> concept: unwrap an
        // optional<Related> to Related structurally (same pattern as fk_member_points_at),
        // then delegate to has_primary_key. Used by FKFieldOf (#474), which cannot splice
        // its range-for loop variable into ValidForeignKey<typename[:...:]> directly (see
        // has_primary_key in storm_orm_field_attr). Single-level: only the target's own PK, never recursing
        // into the target's FKs. Intentionally parallel to ValidForeignKey<FieldType>: the
        // concept serves the type-argument path (find_fk_primary_key), this the loop-variable
        // path (FKFieldOf); both compute "the FK target, optional-unwrapped, has a PK".
        consteval auto valid_fk_target(std::meta::info fk_type) -> bool {
            return has_primary_key(unwrap_optional_type(fk_type));
        }

        // The raw template argument of a reverse_fk member's annotation — either an
        // owner type (^^Task) or an FK field (fields::Task.assignee). The join machinery
        // resolves it to the concrete FK field via resolve_reverse_fk_target.
        // Precondition: `member` is a reverse_fk field (callers gate on
        // is_reverse_fk_field), so the annotation is always present.
        consteval auto reverse_fk_target_of(std::meta::info member) -> std::meta::info {
            const auto annotation_type =
                    reverse_fk_annotation_type_of(member).value(); // NOLINT(bugprone-unchecked-optional-access)
            return std::meta::extract<std::meta::info>(std::meta::template_arguments_of(annotation_type)[0]);
        }

        // True when the FK member `fk_member` (an fk<...> data member) points back
        // at base_t — its declared type, optional-unwrapped, is exactly base_t. The
        // single "does this FK reverse to the base?" check across the reverse-FK code.
        consteval auto fk_member_points_at(std::meta::info fk_member, std::meta::info base_t) -> bool {
            return unwrap_optional_type(std::meta::type_of(fk_member)) == base_t;
        }

        // Count of fk<...> members of `owner` whose type points back at base_t.
        consteval auto count_fks_to(std::meta::info owner, std::meta::info base_t) -> std::size_t {
            std::size_t count = 0;
            for (auto m : std::meta::nonstatic_data_members_of(owner, std::meta::access_context::unchecked())) {
                if (is_fk_field(m) && fk_member_points_at(m, base_t)) {
                    ++count;
                }
            }
            return count;
        }

        // Validate a reverse_fk member's target against base_t (the model owning the
        // container). A type target requires the owner to expose exactly ONE FK back at
        // base_t; a field target must be an fk<...> member of another model that
        // points at base_t. Precondition: `member` is a reverse_fk field.
        consteval auto reverse_fk_member_valid(std::meta::info member, std::meta::info base_t) -> bool {
            const auto target = reverse_fk_target_of(member);
            if (std::meta::is_type(target)) {
                return count_fks_to(target, base_t) == 1;
            }
            if (!std::meta::is_nonstatic_data_member(target) || std::meta::parent_of(target) == base_t) {
                return false;
            }
            return is_fk_field(target) && fk_member_points_at(target, base_t);
        }

        // Resolve a reverse_fk target (owner type OR FK field) to the concrete FK field
        // pointing back at base_t. A type target picks the unique fk<...> member
        // whose (optional-unwrapped) type is base_t; a field target is used directly.
        // Takes both as runtime consteval args so it works on loop-variable members.
        consteval auto resolve_reverse_fk_target(std::meta::info target, std::meta::info base_t) -> std::meta::info {
            if (std::meta::is_type(target)) {
                for (auto m : std::meta::nonstatic_data_members_of(target, std::meta::access_context::unchecked())) {
                    if (is_fk_field(m) && fk_member_points_at(m, base_t)) {
                        return m;
                    }
                }
                std::unreachable(); // ReverseFKFieldOf guarantees a unique FK exists
            }
            return target; // already an FK field
        }

        // True for m2m members WITHOUT a through model (auto-generated junction table).
        consteval auto is_m2m_auto(std::meta::info member) -> bool {
            auto type = m2m_annotation_type_of(member);
            return type.has_value() && std::meta::dealias(std::meta::template_arguments_of(type.value())[0]) == ^^void;
        }

        // Junction ON DELETE RefAction of an auto-junction m2m member (#431): template arg
        // [1] of its ManyToMany annotation, applied to BOTH junction FK sides. Defaults to
        // CASCADE. Precondition: `member` is an m2m field (callers gate on is_m2m_field).
        consteval auto m2m_junction_on_delete_of(std::meta::info member) -> RefAction {
            const auto type = m2m_annotation_type_of(member).value(); // NOLINT(bugprone-unchecked-optional-access)
            return std::meta::extract<RefAction>(std::meta::template_arguments_of(type)[1]);
        }

        // Through model of an m2m member (void = auto-generated junction table).
        template <std::meta::info Member>
        using m2m_through_t = typename[:std::meta::template_arguments_of(m2m_annotation_type_of(Member).value())[0]:];

        // Related model type extracted from a container field type via C++26 std::meta (#203):
        // vector<Course> → Course, plf::hive<Track> → Track,
        // vector<shared_ptr<Course>> / vector<unique_ptr<Course>> → Course.
        consteval auto related_type_from_container(std::meta::info container_type) -> std::meta::info {
            const auto first =
                    std::meta::dealias(std::meta::template_arguments_of(std::meta::dealias(container_type))[0]);
            if (std::meta::has_template_arguments(first)) {
                // fields::std.shared_ptr names a using-declarator under `import std;` and can't
                // be reflected directly — derive the canonical template from a concrete
                // specialization instead.
                const auto tmpl            = std::meta::template_of(first);
                const auto shared_ptr_tmpl = std::meta::template_of(std::meta::dealias(^^std::shared_ptr<int>));
                const auto unique_ptr_tmpl = std::meta::template_of(std::meta::dealias(^^std::unique_ptr<int>));
                if (tmpl == shared_ptr_tmpl || tmpl == unique_ptr_tmpl) {
                    return std::meta::dealias(std::meta::template_arguments_of(first)[0]);
                }
            }
            return first;
        }

        template <typename Container> using m2m_related_t = typename[:related_type_from_container(^^Container):];

        // Detect std::shared_ptr container elements (m2m append path wraps the
        // extracted related object in make_shared, #203).
        template <typename T> struct is_shared_ptr : std::false_type {};
        template <typename TValue> struct is_shared_ptr<std::shared_ptr<TValue>> : std::true_type {};
        template <typename TValue> constexpr bool is_shared_ptr_v = is_shared_ptr<TValue>::value;
    } // namespace meta

    // Concept: T must have at least one field annotated with storm::primary,
    // storm::primary_autoincrement, or storm::primary_part (#492, #500). A composite key
    // (two or more primary_part members) satisfies it exactly like a single-column one;
    // whether the PK DECLARATION is coherent is the separate ModelPrimaryKeyValid<T>.
    //
    // Because a primary key is itself a non-static data member, satisfying this concept
    // also guarantees `field_count_ >= 1`. That invariant is what makes the INSERT batch
    // divides `MAX_DB_VARIABLES / field_count_` (insert.cppm) safe from division by zero —
    // the divisor can never be 0 for any T that reaches a statement class (issue #362, item A).
    template <typename T>
    concept ModelWithPrimaryKey = meta::has_primary_key(^^T);

    namespace meta {
        // True iff `member`'s (dealiased) type is a signed integral type admitted as a
        // primary key (#505) — DECIDED 2026-07-22: short/int/long/long long and their
        // fixed-width spellings (std::int16_t, std::int32_t, std::int64_t, ...); NOT
        // char/signed char (a 1-byte identity is pathological) or bool (integral, but
        // a two-valued PK is nonsense — same carve-out NumericAggregateable makes,
        // #475). std::optional<T> is rejected outright — a nullable primary key is
        // meaningless, so unlike is_unsigned64_member this does NOT unwrap optional
        // first. A bare 64-bit unsigned type is rejected unless
        // annotated storm::signed_storage: that annotation routes through the same
        // bind_int64/extract_int64 path every hardcoded-int64 PK site already uses
        // (select.cppm, join.cppm, insert.cppm); storm::full_unsigned stores as
        // zero-padded TEXT and would silently misread through those same sites, so it
        // stays rejected even though it is an explicit storage annotation.
        consteval auto is_primary_key_typed_member(std::meta::info member) -> bool {
            const std::meta::info t = std::meta::dealias(std::meta::type_of(member));
            if (t == ^^bool) {
                return false;
            }
            if (t == ^^short || t == ^^int || t == ^^long || t == ^^long long) {
                return true;
            }
            if (t == ^^unsigned long || t == ^^unsigned long long) {
                return meta::has_signed_storage_attr(member);
            }
            if (t == ^^storm::orm::utilities::UUID) {
                return true;
            }
            return false;
        }

        // True iff the FK field type `fk_type` refers to an entity whose SINGLE primary key
        // is itself well-typed for use as a KEY (#517). Strictly stronger than
        // valid_fk_target (above), which only asks whether a PK EXISTS.
        //
        // The extra hop matters only where the FK is itself part of a primary key. An FK
        // part binds the REFERENCED row's key (bind_one_pk_part splices
        // find_fk_primary_key<FKType>()), so the value that lands in the composite key
        // column is the target's PK — if that is a `double`, the part-type policy has been
        // circumvented one hop away. Presence alone would accept it, because
        // BaseStatement<Target> is only instantiated when the target is separately used as
        // a model in its own right.
        //
        // A COMPOSITE-keyed target is refused outright — and NOT merely because composite
        // FKs are unsupported: #504 shipped them for ordinary FK fields. The blocker is
        // specific to the PK-part path. bind_one_pk_part still splices the SINGLE-column
        // find_fk_primary_key<FKType>() (in storm_orm_statements_base), so an FK part whose
        // target has an N-part key would bind ONE column for a key that spans N — a
        // silently wrong key, not a missing feature. Lifting this restriction means
        // widening bind_one_pk_part to find_fk_primary_key_members first; until then the
        // guard must stay, so do not delete it on the grounds that #504 has landed.
        //
        // The is_primary_part_member test MUST come first: is_primary_member subsumes it
        // (field_attr.cppm), so testing is_primary_member first would route a composite
        // target's first part into the SINGLE-PK allowlist and answer wrongly in both
        // directions — accepting an int-first composite whose later parts were never seen,
        // and rejecting a legitimate TEXT-first one.
        //
        // Deliberately a SEPARATE helper rather than tightening valid_fk_target: that one
        // is shared with FKFieldOf (#474), where an ordinary (non-key) FK field has no
        // reason to constrain its target's PK type. Single-level, like its sibling — the
        // target's own PK only, never recursing into the target's FKs, so a self- or
        // mutually-referential model still terminates in one step.
        consteval auto valid_fk_key_target(std::meta::info fk_type) -> bool {
            const std::meta::info target = unwrap_optional_type(fk_type);
            for (auto m : std::meta::nonstatic_data_members_of(target, std::meta::access_context::unchecked())) {
                if (storm::meta::is_primary_part_member(m)) {
                    return false; // composite-keyed target: bind_one_pk_part binds only 1 column
                }
                if (storm::meta::is_primary_member(m)) {
                    return is_primary_key_typed_member(m);
                }
            }
            return false; // no PK at all — the same verdict valid_fk_target would give
        }

        // True iff `member` is a well-typed COMPOSITE primary-key part (#517). The #505
        // single-PK allowlist WIDENED WITH TEXT, plus an FK carve-out.
        //
        // TEXT is admitted for parts but not for a single PK, and the split is not
        // arbitrary. #505 rejects a TEXT single PK because this tree has not separated
        // "is the key" from "is DB-generated": the RETURNING id / last_insert_rowid /
        // hardcoded-int64 PK sites (select.cppm, join.cppm, insert.cppm) assume an
        // integer identity. A composite key is never DB-generated (#502 — no
        // AUTOINCREMENT, no IDENTITY, no RETURNING; every part is caller data), so that
        // rationale does not reach a part. It also matches every mainstream ORM and SQL
        // itself: SQLAlchemy's Column(String, primary_key=True), Django's
        // CharField(primary_key=True) and JPA's @Id String are all ordinary, and a
        // (warehouse, sku) key is the canonical association-table shape.
        //
        // ORDER IS LOAD-BEARING: is_text_member (#493) looks THROUGH std::optional<>, so
        // the nullable rejection must come first or optional<std::string> would be
        // admitted as a key part. ModelPrimaryKeyValid also rejects a nullable PK, but
        // this concept must reject it independently — the two are ANDed separately onto
        // BaseStatement and the tests assert on this one alone; letting them disagree
        // would make the verdict depend on which concept a diagnostic happens to name.
        //
        // An FK part is validated through its TARGET's primary key, not its own declared
        // type: it binds the referenced row's key (the is_fk_field branch of
        // bind_one_pk_part), so "the member's type" is the wrong question — the right one
        // is asked by valid_fk_key_target just above.
        consteval auto is_primary_key_part_typed_member(std::meta::info member) -> bool {
            if (storm::meta::is_optional_member(member)) {
                return false;
            }
            if (storm::meta::is_fk_field(member)) {
                return valid_fk_key_target(std::meta::type_of(member));
            }
            return is_primary_key_typed_member(member) || storm::meta::is_text_member(member);
        }
    } // namespace meta

    // Concept: T's primary-key member(s) have a type admitted as a primary key. Covers BOTH
    // shapes: a single storm::primary / primary_autoincrement member goes through the #505
    // allowlist (meta::is_primary_key_typed_member), and every storm::primary_part member of
    // a composite key (#500) goes through meta::is_primary_key_part_typed_member — the same
    // allowlist widened with TEXT, FK parts excepted (#517). A composite model is accepted
    // only if EVERY part passes.
    //
    // ANDed alongside ModelWithPrimaryKey on BaseStatement's constraint list — a model
    // with NO primary key already fails that sibling concept, so the loop here always
    // finds a match when both concepts are evaluated together on a single-PK model.
    // Checked directly on T's own members, from data_members_of_v<T> (held by value, so
    // BMI-safe — #262, #694), matching the sibling model-policy concepts
    // (ModelStorageAnnotated, ModelFkPoliciesValid, ...).
    //
    // A failing BaseStatement<T> instantiation names both T and PrimaryKeyType in the
    // "constraints not satisfied" / "because 'T' does not satisfy 'PrimaryKeyType'"
    // diagnostic trail (matching every sibling concept here — none of them name the
    // specific member either). A static_assert with a dynamic message naming the PK
    // member was tried and rejected: static_assert failure inside a concept's consteval
    // lambda is a hard compile error during constraint checking, not a SFINAE-friendly
    // false, so it would break every negative `!PrimaryKeyType<Bad>` usage (including
    // the tests) instead of merely gating BaseStatement.
    template <typename T>
    concept PrimaryKeyType = []() consteval {
        for (auto m : storm::meta::data_members_of_v<T>) {
            if (meta::is_primary_part_member(m)) {
                // Composite (#517): every part is checked; keep scanning after a pass.
                if (!meta::is_primary_key_part_typed_member(m)) {
                    return false;
                }
            } else if (meta::is_primary_member(m)) {
                return meta::is_primary_key_typed_member(m);
            }
        }
        return true; // every part passed, or no PK member: ModelWithPrimaryKey rejects that
    }();

    // A field type is a valid FK target iff its referenced entity (optional-unwrapped)
    // has a primary key. Names the boundary find_fk_primary_key relies on and that
    // FKFieldOf did not previously enforce at the call site (#474). Single-level: it
    // checks only the target's PK, never recursing into the target's own FKs — so a
    // self-referential (an FK field whose target is its own owning model) or
    // mutually-referential model terminates in one step.
    template <typename FieldType>
    concept ValidForeignKey = ModelWithPrimaryKey<utilities::optional_inner_type_t<FieldType>>;

    // Concept: every 64-bit unsigned field of T must carry an explicit storage
    // annotation — storm::signed_storage or storm::full_unsigned (#436). A bare
    // unsigned-64 field would silently store > INT64_MAX values as a negative int64
    // (equality round-trips, but ORDER BY and external readers see the signed value),
    // so we refuse it at the call site instead. Signed-64 and all smaller types are
    // exempt. optional<uint64_t> is checked on its inner type. The annotation is read
    // directly on T's own members from data_members_of_v<T> (the ModelWithPrimaryKey
    // pattern), which is BMI-safe (#262, #694).
    template <typename T>
    concept ModelStorageAnnotated =
            std::ranges::all_of(storm::meta::data_members_of_v<T>, [](std::meta::info m) consteval {
                return !storm::meta::is_unsigned64_member(m) || meta::has_signed_storage_attr(m) ||
                       meta::has_full_unsigned_attr(m);
            });

    // Concept: every FK field carrying fk<RefAction::SetNull> must be a nullable FK —
    // std::optional<Related> (#431). ON DELETE SET NULL writes NULL into the child column,
    // which a NOT NULL FK column cannot hold, so we refuse it at the call site with a clear
    // constraint violation instead of letting the database reject the DELETE at runtime.
    // The annotation is read directly on T's own members from data_members_of_v<T> (#262,
    // #694); optional-ness goes through the shared storm::meta::is_optional_member predicate (#509).
    template <typename T>
    concept ModelFkPoliciesValid = []() consteval {
        for (auto m : storm::meta::data_members_of_v<T>) {
            auto action = meta::fk_on_delete_action_of(m);
            if (action.has_value() && action.value() == meta::RefAction::SetNull) {
                if (!storm::meta::is_optional_member(m)) {
                    return false;
                }
            }
        }
        return true;
    }();

    // Concept: no member of T carries a conflicting pair of flag annotations (#492).
    // The former `enum class FieldAttr` made these conflicts unrepresentable — a field
    // could carry at most one enumerator. The free-standing annotation objects can be
    // stacked, so this concept restores the exclusivity guarantee at compile time,
    // rejecting per member:
    //   * both primary and primary_autoincrement (two primary-key modes),
    //   * both signed_storage and full_unsigned (two 64-bit storage modes).
    // Scope is exactly those two mutually-exclusive mode pairs; other flag properties
    // are validated by the sibling concepts (storage-type presence by ModelStorageAnnotated,
    // FK-policy nullability by ModelFkPoliciesValid, timestamp field type by the
    // ValidTimestampField static_assert). Members are read directly from
    // data_members_of_v<T> (BMI-safe, #262, #694), matching those siblings.
    template <typename T>
    concept ModelAnnotationsValid =
            std::ranges::all_of(storm::meta::data_members_of_v<T>, [](std::meta::info m) consteval {
                const std::size_t pk_mode_count =
                        static_cast<std::size_t>(storm::meta::has_annotation_type<storm::meta::Primary>(m)) +
                        static_cast<std::size_t>(
                                storm::meta::has_annotation_type<storm::meta::PrimaryAutoincrement>(m)
                        ) +
                        static_cast<std::size_t>(storm::meta::has_annotation_type<storm::meta::PrimaryAssigned>(m));
                const bool storage_conflict = meta::has_signed_storage_attr(m) && meta::has_full_unsigned_attr(m);
                return pk_mode_count <= 1 && !storage_conflict;
            });

    // Counts of each primary-key annotation flavour on T (#500), computed in one pass
    // so ModelPrimaryKeyValid can express its rules as plain arithmetic on the result.
    // A struct rather than a pair: the four counts are read by name at every use.
    struct PrimaryKeyCounts {
        std::size_t primary{};       // storm::primary
        std::size_t autoincrement{}; // storm::primary_autoincrement
        std::size_t assigned{};      // storm::primary_assigned
        std::size_t part{};          // storm::primary_part (composite)
        std::size_t on_relation{};   // any PK annotation sitting on an m2m/reverse_fk member
        std::size_t nullable{};      // any PK annotation sitting on a std::optional<T> member
        std::size_t unique_part{};   // a primary_part member that also carries storm::unique
        // How many key COLUMNS the model declares (#537) — PK members that are actually
        // persisted, i.e. BaseStatement::is_primary_key_column's rule. Deliberately the
        // column count and not `primary + autoincrement + part`: it must match the
        // primary_key_members_ array the stitch key walks, which excludes relation
        // members. Counted here rather than in a second loop of its own so every
        // whole-model PK fact stays in this one pass.
        std::size_t columns{};
    };

    template <typename T> consteval auto count_primary_key_flavours() -> PrimaryKeyCounts {
        PrimaryKeyCounts counts;
        for (auto m : storm::meta::data_members_of_v<T>) {
            counts.primary += static_cast<std::size_t>(storm::meta::has_annotation_type<storm::meta::Primary>(m));
            counts.autoincrement +=
                    static_cast<std::size_t>(storm::meta::has_annotation_type<storm::meta::PrimaryAutoincrement>(m));
            counts.assigned +=
                    static_cast<std::size_t>(storm::meta::has_annotation_type<storm::meta::PrimaryAssigned>(m));
            counts.part += static_cast<std::size_t>(storm::meta::is_primary_part_member(m));
            if (storm::meta::is_primary_member(m)) {
                // A PK on a relation container names something that is never a column,
                // and a nullable PK column is not a key at all (SQLite's legacy NULL
                // quirk even lets duplicate keys through, diverging from PG on identical
                // DDL). Counted here so ModelPrimaryKeyValid can refuse both.
                counts.on_relation += static_cast<std::size_t>(storm::meta::is_relation_field(m));
                counts.columns += static_cast<std::size_t>(!storm::meta::is_relation_field(m)); // #537
                // UNIQUE on one part of a composite key defeats the key: it allows at
                // most one row per that part alone, making the other parts pointless.
                counts.unique_part +=
                        static_cast<std::size_t>(storm::meta::is_primary_part_member(m) && storm::meta::is_unique(m));
                counts.nullable += static_cast<std::size_t>(storm::meta::is_optional_member(m));
            }
        }
        return counts;
    }

    // The counts for T, evaluated once (#686). Clang does not memoise consteval calls, so
    // ModelPrimaryKeyValid and ModelPrimaryKeyPartLimit each re-ran the member scan; a
    // variable template is instantiated once per T, on first use (still lazy).
    template <typename T> inline constexpr PrimaryKeyCounts primary_key_counts_v = count_primary_key_flavours<T>();

    // Concept: T's primary-key declaration is coherent (#500). Composite support widens
    // the PK from "exactly one member" to "one or more", which makes four new
    // combinations expressible that must not be:
    //
    //   * primary + primary_part — two competing PK declarations; which one is the key?
    //   * primary_autoincrement + primary_part — additionally UNREPRESENTABLE in SQL.
    //     SQLite rejects both spellings at parse time: a column-level PRIMARY KEY cannot
    //     coexist with a table-level PRIMARY KEY (...), and AUTOINCREMENT is only
    //     grammatical directly after INTEGER PRIMARY KEY. PG is equivalent — GENERATED
    //     AS IDENTITY is single-column, there is no multi-column identity. Because it
    //     cannot be spelled at all, emitting the DDL and letting the backend object
    //     would surface a runtime error about primary keys that never names the
    //     annotation responsible; rejecting here gives a constraint violation at the
    //     model definition instead.
    //   * primary_assigned + primary_part — likewise UNREPRESENTABLE in SQL: a
    //     caller-assigned key is still a single-column INTEGER PRIMARY KEY, which cannot
    //     coexist with a table-level PRIMARY KEY (...).
    //   * exactly one primary_part — that is a plain PK written the wrong way; tell the
    //     user to spell it `primary` rather than silently accepting a 1-column
    //     "composite" key that emits different DDL than the equivalent `primary`.
    //   * two or more `primary` / `primary_autoincrement` / `primary_assigned` members —
    //     the accidental-double-`primary` typo the separate primary_part tag exists to
    //     keep an error.
    //   * a PK annotation on an m2m/reverse_fk container, or on a std::optional<T>, or
    //     `unique` on a single part — none of which is a well-formed key column.
    //
    // Consequence carried into #502: since no part of a composite PK can be DB-generated,
    // the caller always supplies the full key — those columns are always INSERTed and
    // there is nothing to RETURNING.
    //
    // Zero primary_part members is the single-PK case, unconstrained here and validated
    // by ModelWithPrimaryKey as before. Sits next to ModelAnnotationsValid in the
    // BaseStatement constraint list; that concept keeps rejecting per-member conflicts
    // (primary + primary_autoincrement on ONE member), this one whole-model PK coherence.
    template <typename T>
    concept ModelPrimaryKeyValid = []() consteval {
        constexpr auto counts = primary_key_counts_v<T>;
        // A PK annotation on a member that is not a plain persisted column: an m2m /
        // reverse_fk container (never emitted as a column, so the key would reference a
        // name no column definition provides — the #485 class of leak) or a nullable
        // std::optional<T> (a nullable PK is not a key, and SQLite vs PG disagree on it).
        if (counts.on_relation > 0 || counts.nullable > 0) {
            return false;
        }
        // UNIQUE on a single part contradicts the composite key it belongs to (see
        // count_primary_key_flavours). Valid SQL, but never what the author meant.
        if (counts.unique_part > 0) {
            return false;
        }
        // Several column-level PK declarations: `primary` (or primary_autoincrement) on
        // two different members. This is the accidental-double-`primary` typo the separate
        // primary_part tag exists to keep an ERROR — without this clause the widened PK
        // machinery would happily treat it as a composite key and silently emit different
        // DDL than the same model produced before composite support existed.
        if (counts.primary + counts.autoincrement + counts.assigned > 1) {
            return false;
        }
        if (counts.part == 0) {
            return true; // single-PK model — unchanged rules
        }
        if (counts.primary > 0 || counts.autoincrement > 0 || counts.assigned > 0) {
            return false; // primary_part mixed with a column-level PK declaration
        }
        return counts.part >= 2; // exactly one part is a plain `primary`
    }();

    // Concept: T's primary key has no more parts than the m2m/reverse-FK stitch key
    // can hold (#537).
    //
    // The m2m and reverse-FK two-query eager load stitches Q2 rows onto their Q1 owner
    // through a hash map keyed on the owner's PK. For a composite key that key is
    // utilities::StitchKey, a FIXED 32-byte inline buffer into which every part writes
    // exactly one 8-byte word — so StitchKey::MAX_PARTS (= 4) parts fill it exactly and
    // a 5th writes past the end. The only guard was an assert in StitchKey::append_word,
    // which NDEBUG compiles out: it was absent from exactly the Release configuration
    // where the overrun would matter, making a 5-part key a SILENT buffer overrun there.
    //
    // Rejected here at the model rather than at the stitch, for two reasons. (1) The
    // stitch runs behind M2MRelation's type-erased fn-pointer vtable, where T is no
    // longer in scope — there is no call site there at which to name the model in a
    // diagnostic. (2) Gating only models that CURRENTLY declare a relation would make
    // adding an unrelated m2m field to a 5-part model fail in a distant file; the limit
    // is a property of the key, so the key's owner is where it belongs.
    //
    // Growing CAPACITY instead was considered and NOT done: #504 measured a wider key
    // costing ~4% on the stitch hot path (which is why single-PK models bypass StitchKey
    // for a bare std::uint64_t). Folding the surplus parts was rejected in #504 as well —
    // two distinct keys colliding in the fold mis-stitches rows onto the wrong owner,
    // a correctness regression rather than a map collision. If a real 5-part key ever
    // turns up, raise StitchKey::CAPACITY and re-run the M2M benchmarks; this concept
    // reads MAX_PARTS, so it follows automatically.
    //
    // Single-PK models are unaffected (count 1). Bound at 4, so every composite model in
    // the tree today (2 and 3 parts) keeps compiling unchanged. Reads the COLUMN count
    // from primary_key_counts_v (is_primary_key_column's rule — a PK annotation on
    // a relation container is not a column, and is rejected by ModelPrimaryKeyValid
    // anyway), so the number checked here is the same one primary_key_members_ holds and
    // the stitch actually walks. Cannot read BaseStatement<T>::primary_key_column_count_
    // for that: this concept is ON that class's constraint list, so touching it here
    // would be circular.
    template <typename T>
    concept ModelPrimaryKeyPartLimit = primary_key_counts_v<T>.columns <= storm::orm::utilities::StitchKey::MAX_PARTS;

    // Concept: every max_length<N> annotation of T must sit on a text field (#493).
    // max_length bounds a text column's length (VARCHAR(N) on PG, CHECK(length) on SQLite);
    // on a non-text field it is meaningless, so we refuse it at the call site with a clear
    // constraint violation instead of silently ignoring it (unlike Django/SQLAlchemy). Only
    // std::string / std::string_view (optional-unwrapped) accept it. Members are read
    // directly from data_members_of_v<T> (BMI-safe, #262, #694), matching the sibling
    // concepts above.
    template <typename T>
    concept ModelMaxLengthValid =
            std::ranges::all_of(storm::meta::data_members_of_v<T>, [](std::meta::info m) consteval {
                return !meta::max_length_of(m).has_value() || meta::is_text_member(m);
            });

    // Concept: no primary-key member of T carries auto_create or auto_update (#511).
    //
    // An auto-stamped key rewrites itself: auto_update on a PK put that column into the
    // SET clause of qs.where(...).update<...>(), so the statement stamped now() over the
    // very key it was matching on (the #501 class of bug, for the implicit timestamp tail
    // rather than the explicit SET targets). auto_create is rejected on the same grounds —
    // it makes the key value un-supplied by the caller on INSERT, so a composite key part
    // could not be chosen at all.
    //
    // DECIDED 2026-07-29: reject the model rather than silently drop the annotation, so a
    // wrong PK annotation is a NAMED error — the same frame as ModelPrimaryKeyValid (#500),
    // which rejects nullable and relation-container PKs. The is_unlisted_auto_update
    // predicates in update_grammar/upsert_grammar ALSO gate on is_pk_member, so the SET
    // clause stays correct by construction even if this concept is later loosened; this
    // concept is the diagnostic, that gate is the defence.
    //
    // NOTE TO A FUTURE EDITOR: those two gates are currently UNREACHABLE — a model that
    // would exercise them cannot instantiate BaseStatement while this concept is on the
    // constraint list, so no test discriminates them (the test file says so explicitly
    // rather than asserting something vacuous). If you ever remove or narrow this concept,
    // the gates become load-bearing and you inherit the obligation to give them a
    // discriminating test.
    //
    // Covers composite keys: is_primary_member matches primary_part (#500), which was the
    // genuinely unprotected path when this concept landed — PrimaryKeyType (#505) exempted
    // primary_part members entirely back then, and blocked a SINGLE time_point PK only
    // incidentally, by that type falling off the end of its integral/UUID whitelist. #517
    // has since removed that exemption, so a time_point part is now rejected by both
    // concepts independently; this one keeps naming the timestamp case explicitly, which is
    // the clearer diagnostic. Members are read directly from
    // data_members_of_v<T> (BMI-safe, #262, #694), matching the sibling concepts above.
    template <typename T>
    concept ModelTimestampPkValid =
            std::ranges::all_of(storm::meta::data_members_of_v<T>, [](std::meta::info m) consteval {
                return !meta::is_primary_member(m) || (!meta::is_auto_create(m) && !meta::is_auto_update(m));
            });

    // A field carrying auto_create/auto_update must be a system_clock::time_point (#209).
    // Referenced by a static_assert in bind_field_at_index so a wrong-typed timestamp field
    // fails to compile with a clear message rather than deep inside parameter binding.
    template <std::meta::info Member>
    concept ValidTimestampField = std::
            same_as<std::remove_cvref_t<typename[:std::meta::type_of(Member):]>, std::chrono::system_clock::time_point>;

    // A JOIN field selector must reflect a non-static data member of T annotated with
    // an fk<...> annotation (#388). Constrains QuerySet::join/left_join and
    // JoinStatement so a non-member or non-FK argument fails at the call site with a
    // clear constraint violation.
    //
    // The annotation is read from the member matched by identifier against
    // data_members_of_v<T>, NOT from Member itself: annotation_of_type on a reflection that
    // crossed a BMI boundary segfaults clang-p2996 (#262), while structural queries
    // (is_nonstatic_data_member / parent_of / identifier_of) are safe on it.
    template <typename T, std::meta::info Member>
    concept FKFieldOf = []() consteval {
        if (!std::meta::is_nonstatic_data_member(Member) || !std::meta::has_identifier(Member)) {
            return false;
        }
        if (std::meta::parent_of(Member) != ^^T) {
            return false;
        }
        for (auto m : storm::meta::data_members_of_v<T>) {
            if (std::meta::identifier_of(m) == std::meta::identifier_of(Member)) {
                return meta::is_fk_field(m) && meta::valid_fk_target(std::meta::type_of(m));
            }
        }
        return false;
    }();

    // A many-to-many JOIN field selector must reflect a non-static data member of T
    // carrying a ManyToMany annotation (#203). Same BMI-boundary discipline as
    // FKFieldOf: the annotation is read from the member re-derived out of ^^T.
    template <typename T, std::meta::info Member>
    concept M2MFieldOf = []() consteval {
        if (!std::meta::is_nonstatic_data_member(Member) || !std::meta::has_identifier(Member)) {
            return false;
        }
        if (std::meta::parent_of(Member) != ^^T) {
            return false;
        }
        for (auto m : storm::meta::data_members_of_v<T>) {
            if (std::meta::identifier_of(m) == std::meta::identifier_of(Member)) {
                return meta::is_m2m_field(m);
            }
        }
        return false;
    }();

    // A cross-model reverse-FK selector (#398): Member is an fk<...> data member
    // of ANOTHER model whose FK type (unwrapping std::optional) is the base model T.
    //   QuerySet<Person>().left_join<fields::Task.assignee>()  // Member = fields::Task.assignee
    // The owning model (Task), FK field, and FK target (Person) all come from Member;
    // naming the field disambiguates several FKs to the same target (assignee vs reviewer).
    // Structural-only queries on Member (parent_of/type_of/identifier_of) — safe across a
    // BMI boundary; the annotation is read from the member re-derived out of the owner.
    template <typename T, std::meta::info Member>
    concept ReverseFKSelector = []() consteval {
        if (!std::meta::is_nonstatic_data_member(Member) || !std::meta::has_identifier(Member)) {
            return false;
        }
        const auto owner = std::meta::parent_of(Member);
        if (owner == ^^T) {
            return false; // a reverse FK must point from a DIFFERENT model back at T
        }
        // The FK type, optional-unwrapped, must be exactly T.
        if (!meta::fk_member_points_at(Member, ^^T)) {
            return false;
        }
        // Member must carry an fk<...> annotation — read from the member re-derived out of owner.
        for (auto m : std::meta::nonstatic_data_members_of(owner, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(m) == std::meta::identifier_of(Member)) {
                return meta::is_fk_field(m);
            }
        }
        return false;
    }();

    // A reverse-FK container destination (#398): a non-static data member of T carrying a
    // ReverseFk annotation, whose carried FK field is itself a valid ReverseFKSelector<T>.
    // This is the key+destination form (select()); ReverseFKSelector is the key alone
    // (aggregate/filter chains). Same BMI discipline: annotation re-derived from ^^T.
    template <typename T, std::meta::info Member>
    concept ReverseFKFieldOf = []() consteval {
        if (!std::meta::is_nonstatic_data_member(Member) || !std::meta::has_identifier(Member)) {
            return false;
        }
        if (std::meta::parent_of(Member) != ^^T) {
            return false;
        }
        for (auto m : storm::meta::data_members_of_v<T>) {
            if (std::meta::identifier_of(m) == std::meta::identifier_of(Member)) {
                return meta::is_reverse_fk_field(m) && meta::reverse_fk_member_valid(m, ^^T);
            }
        }
        return false;
    }();

    // A through model must carry exactly one fk<...> member of type Side
    // (#203 Phase 2) — that member names the junction FK column (<identifier>_id).
    template <typename Through, typename Side>
    concept ThroughWithFKTo = []() consteval {
        std::size_t count = 0;
        for (auto m : storm::meta::data_members_of_v<Through>) {
            if (meta::is_fk_field(m) && std::meta::dealias(std::meta::type_of(m)) == std::meta::dealias(^^Side)) {
                ++count;
            }
        }
        return count == 1;
    }();
} // namespace storm::orm::statements
