module;

// Column-type-aware operand normalization for WHERE/HAVING (#622): normalize_operand only ever
// saw the OPERAND's type, so a string-spelled filter (`== ""`, or the YAML/JSON
// query_builder.hpp path — its typed_value_as has no storm::UUID branch) against a storm::UUID
// column bypassed #609's empty/malformed-UUID rejection entirely. Split into its own leaf
// module (mirrors field_attr.cppm) purely to keep where.cppm under its line budget — no
// dependency reason; everything here stays in the storm::orm::where namespace.

#include <meta>

export module storm_orm_where_operand;

import std;

import storm_orm_utilities;
import storm_orm_field_attr; // is_fk_field/is_primary_member — FK-target PK resolution

export namespace storm::orm::where {

    // Normalize a comparison operand to the type stored in ExpressionVariant. The node can
    // outlive the operand source buffer (#352), so string_view/const char*/char[] operands are
    // copied into an owning std::string. UUID is excluded from that fold (#609) so it keeps
    // ComparisonExpr<UUID>'s own bind dispatch. Enum -> underlying int; narrow/unsigned int
    // folds to int/int64_t (#407); bool keeps its own arm; everything else is decayed.
    template <typename V>
        requires utilities::BindableType<std::decay_t<V>>
    [[nodiscard]] auto normalize_operand(V&& value) {
        using D = std::decay_t<V>;
        if constexpr (std::is_enum_v<D>) {
            return static_cast<int>(static_cast<std::underlying_type_t<D>>(value));
        } else if constexpr (!std::is_same_v<D, bool> && utilities::is_int64_source_v<D>) {
            return static_cast<std::int64_t>(value);
        } else if constexpr (!std::is_same_v<D, bool> && utilities::is_int_source_v<D>) {
            return static_cast<int>(value);
        } else if constexpr (std::is_convertible_v<D, std::string_view> && !std::is_same_v<D, std::string> &&
                             !std::is_same_v<D, utilities::UUID>) {
            return std::string(std::string_view(std::forward<V>(value)));
        } else {
            return D{std::forward<V>(value)};
        }
    }

    // Converts a UUID-constructible operand (string/string_view/const char*) to the COLUMN's own
    // type first when that column is a (nullable) storm::UUID, then defers to normalize_operand
    // — closes #622. An operand already spelled as storm::UUID is untouched (falls straight to
    // normalize_operand, same as before). ColumnType is the column's resolved type (see
    // ComparisonColumnType below), passed explicitly by callers in where.cppm.
    template <typename ColumnType, typename V>
        requires utilities::BindableType<std::decay_t<V>>
    [[nodiscard]] auto normalize_column_operand(V&& value) {
        using D         = std::decay_t<V>;
        using Unwrapped = utilities::optional_inner_type_t<std::remove_cvref_t<ColumnType>>;
        if constexpr (std::is_same_v<Unwrapped, utilities::UUID> && !std::is_same_v<D, utilities::UUID> &&
                      std::is_constructible_v<utilities::UUID, V>) {
            return normalize_operand(utilities::UUID(std::forward<V>(value)));
        } else {
            return normalize_operand(std::forward<V>(value));
        }
    }

    // Operand type for in() (#610): a plain member's declared type with std::optional unwrapped
    // (#625), or — for a single-column FK member — the FK TARGET's primary-key type
    // (FieldType is the RELATED MODEL struct,
    // which normalize_operand can never bind). Reimplements find_fk_primary_key's lookup
    // (base.cppm) locally — importing storm_orm_statements_base back would be a module cycle
    // — but does NOT recurse when the target's PK is itself an FK (base's bind_one_pk_part
    // does; no in-tree model has that shape). Returns nullopt for an FK target with no PK at
    // all (ValidForeignKey, #474, can't gate this for the same cycle reason); HasInTargetType
    // below turns that into a clean rejection instead of an unguarded splice. `if constexpr`,
    // not a ternary: the untaken branch would still need nonstatic_data_members_of to
    // type-check on a non-class FieldType (e.g. int).
    template <std::meta::info MemberInfo> consteval auto in_target_pk_info() -> std::optional<std::meta::info> {
        if constexpr (storm::meta::is_fk_field(MemberInfo)) {
            using RelatedType =
                    utilities::optional_inner_type_t<std::remove_cvref_t<typename[:std::meta::type_of(MemberInfo):]>>;
            for (const std::meta::info member : storm::meta::data_members_of_v<RelatedType>) {
                if (storm::meta::is_primary_member(member)) {
                    return std::meta::type_of(member);
                }
            }
            return std::nullopt;
        } else {
            // Unwrap std::optional exactly as the FK branch above already does (#625). A
            // nullable column's IN operands are its INNER type: ExpressionVariant has no
            // InExpression<std::optional<T>> arm for any T, so returning the declared type
            // verbatim named an arm that does not exist and hard-errored inside
            // make_shared<ExpressionVariant> instead of failing at in()'s requires-clause.
            // `^^` of a local alias, mirroring ^^RelatedType above — this must yield an info,
            // not a value.
            using FieldT =
                    utilities::optional_inner_type_t<std::remove_cvref_t<typename[:std::meta::type_of(MemberInfo):]>>;
            return ^^FieldT;
        }
    }

    // Gate checked BEFORE InTargetType is named, so a composite (or PK-less) FK target fails
    // in()'s requires-clause cleanly instead of substituting a splice of nullopt.
    template <std::meta::info MemberInfo>
    concept HasInTargetType = in_target_pk_info<MemberInfo>().has_value();

    template <std::meta::info MemberInfo> using InTargetType = typename[:in_target_pk_info<MemberInfo>().value():];

    // Per-operand gate for in(). `constructible_from` alone is a tight gate for every other
    // target type — constructible_from<int, const char*> and constructible_from<UUID, int> are
    // both FALSE — but bool is constructible from essentially every scalar and pointer, so
    // `.in("yes", nullptr, 3.7)` on a bool column would compile and make_in_expr's
    // static_cast<bool> would bind 1, 0, 1: the string's CONTENTS silently discarded, every
    // non-null pointer TRUE. Text and bind count stay internally consistent, so nothing
    // downstream can catch it — the query just selects a row set nobody wrote. Require a bool
    // operand to be spelled bool. Narrowing conversions between NUMERIC types stay allowed,
    // matching make_in_expr's documented truncate-on-overflow tradeoff (#610).
    template <std::meta::info MemberInfo, typename V>
    concept InOperandFor =
            std::constructible_from<InTargetType<MemberInfo>, V> &&
            (!std::same_as<InTargetType<MemberInfo>, bool> || std::same_as<std::remove_cvref_t<V>, bool>);

    // The column's own type for comparison purposes (#622): a plain member's declared type, or —
    // for a single-column FK member — the FK TARGET's primary-key type, reusing in_target_pk_info
    // above (#610's lookup for in()). Falls back to the member's own declared type when the
    // target has no resolvable single PK, so Field/CollatedField still instantiate for every
    // member (comparison methods stay gated by ComparableOperand separately).
    //
    // Since #625 the non-FK branch unwraps std::optional, so this now always yields an unwrapped
    // type. That is a no-op for both consumers — normalize_column_operand's `Unwrapped` alias and
    // ColumnIsUuid each apply optional_inner_type_t themselves, which is idempotent — so the
    // comparison path is unchanged. Note the FALLBACK branch below still returns the declared
    // type verbatim; it is reached only when HasInTargetType is false — an FK to a composite or
    // PK-less target, whose declared type may still be std::optional<Model>. Both consumers
    // unwrap that themselves, so do not add one that assumes an already-unwrapped type here.
    template <std::meta::info MemberInfo> consteval auto comparison_column_info() -> std::meta::info {
        if constexpr (HasInTargetType<MemberInfo>) {
            return in_target_pk_info<MemberInfo>().value();
        } else {
            return std::meta::type_of(MemberInfo);
        }
    }
    template <std::meta::info MemberInfo> using ComparisonColumnType = typename[:comparison_column_info<MemberInfo>():];

    // True when the column's own type (FK-target-aware) is a (nullable) storm::UUID — a direct
    // member or a single-column FK to a UUID-PK model alike (#622).
    template <std::meta::info MemberInfo>
    concept ColumnIsUuid = std::is_same_v<
            utilities::optional_inner_type_t<std::remove_cvref_t<ComparisonColumnType<MemberInfo>>>,
            utilities::UUID>;

} // namespace storm::orm::where
