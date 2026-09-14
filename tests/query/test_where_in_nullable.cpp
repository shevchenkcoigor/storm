#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm;
import std;

#include "../../shared/models/extended_types.h" // NOSONAR cpp:S954 — BLOB/path rejection probes
#include "test_fixture.h"

// ── #625: .in() on a nullable NON-FK column ────────────────────────────────────────────────────
//
// in_target_pk_info<MemberInfo>() (where_operand.cppm) unwrapped std::optional for the FK branch
// but returned std::meta::type_of(MemberInfo) verbatim in the non-FK else branch. So for
// `std::optional<std::string> nickname`, InTargetType was std::optional<std::string>,
// `constructible_from<optional<string>, const char*>` passed, normalize_operand had no matching
// fold, and InExpression<std::optional<std::string>> named a variant arm that does not exist —
// a hard error inside std::make_shared<ExpressionVariant>(...) instead of a clean rejection.
//
// Never hit in-tree before: the only nullable members tested with .in() were FK members, which
// unwrap correctly via the FK branch (test_fk_column_name_orderby_aggregate_body.h).
//
// This is the third instance of ONE invariant being violated (#578, #610, #625): the normalized
// target type must name an InExpression arm that exists. InStorableTarget now states it, so the
// failure mode is structurally impossible rather than patched a third time.
//
// WHERE_CLAUSES.md already promised this worked — its std::optional<T> row reads "Plus any
// operator T itself supports". The fix makes the code match its own documented contract.

// NOLINTBEGIN(readability-implicit-bool-conversion)

namespace {

    // Every nullable shape that resolves to a DIFFERENT InExpression arm, plus a bool column
    // (whose arm this change adds — see the variant note below).
    struct InNullableRow {
        [[= storm::primary]] int   id{};
        std::optional<std::string> nickname;
        std::optional<int>         score;
        std::optional<storm::UUID> opt_uuid;
        bool                       is_active{};
    };

    constexpr std::string_view kUuidA = "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view kUuidB = "22222222-2222-4222-8222-222222222222";

    constexpr std::string_view kExpectedFilterError =
            "UUID comparison value must be explicitly set; auto-generation not allowed in a WHERE/HAVING clause";

} // namespace

namespace fields {

    struct InNullableRowT;
    consteval {
        std::meta::define_aggregate(^^InNullableRowT, storm::field_specs_for(^^InNullableRow));
    }
    inline constexpr InNullableRowT InNullableRow{};

} // namespace fields

// ── Compile-time surface ───────────────────────────────────────────────────────────────────────
// The reason #625 wants a `requires` constraint rather than a body static_assert: a constraint
// can be TESTED. A static_assert inside in() would only break the build, and would be invisible
// to query_builder.hpp's requires-based probing.

template <typename V>
concept CanInNickname = requires(V v) { fields::InNullableRow.nickname.in(v); };
static_assert(CanInNickname<const char*>, ".in() on a nullable string column must compile (#625)");
static_assert(CanInNickname<std::string>, ".in() on a nullable string column must accept std::string");

template <typename V>
concept CanInScore = requires(V v) { fields::InNullableRow.score.in(v); };
static_assert(CanInScore<int>, ".in() on a nullable int column must compile (#625)");

template <typename V>
concept CanInOptUuid = requires(V v) { fields::InNullableRow.opt_uuid.in(v); };
static_assert(CanInOptUuid<storm::UUID>, ".in() on a nullable UUID column must compile (#625)");
static_assert(CanInOptUuid<std::string_view>, "a string operand converts to UUID via make_in_expr's static_cast");
static_assert(!CanInOptUuid<int>, ".in(5) on a UUID column must be rejected — UUID is not int-constructible");

// std::nullopt as an IN operand: SQL's `x IN (1, NULL)` never matches NULL rows (it is
// `x = 1 OR x = NULL` -> NULL), so this can only ever be a query that silently matches nothing.
// Before #625 it was worse than useless — constructible_from<optional<string>, nullopt_t> is
// TRUE, so it passed the constraint and then hard-errored in the variant. Unwrapping the target
// makes it a clean rejection. To match NULL rows, compose is_null() || in(...) — tested below.
static_assert(!CanInNickname<std::nullopt_t>, ".in(std::nullopt) must be rejected, not hard-error (#625)");
static_assert(!CanInScore<std::nullopt_t>, ".in(std::nullopt) must be rejected on a nullable int column too");

// bool gained an InExpression arm in this change. Investigation found NO recorded rationale
// anywhere in src/ or docs/ for its absence — ComparisonExpr<bool> was an arm and
// InExpression<bool> simply was not, so `.in()` on a bool column hard-errored in the same place
// as #625's nullable columns. (Contrast UUID, whose ordering exclusion IS deliberate and
// documented — #609/#407.)
template <typename V>
concept CanInIsActive = requires(V v) { fields::InNullableRow.is_active.in(v); };
static_assert(CanInIsActive<bool>, ".in() on a bool column must compile");
// bool is constructible from every scalar and pointer, unlike every other target type
// (constructible_from<int, const char*> is false), so constructible_from alone would let
// `.in("yes", nullptr, 3.7)` compile and bind 1, 0, 1 — the string's contents silently dropped.
// InOperandFor requires a bool operand to be spelled bool.
static_assert(!CanInIsActive<const char*>, ".in(\"yes\") on a bool column must not coerce to true");
static_assert(!CanInIsActive<std::nullptr_t>, ".in(nullptr) on a bool column must be rejected");
static_assert(!CanInIsActive<double>, ".in(3.7) on a bool column must be rejected");
// ...and the carve-out must not leak to other targets: numeric narrowing stays allowed, matching
// make_in_expr's documented truncate-on-overflow tradeoff (#610).
static_assert(CanInScore<bool>, "an int column still accepts a bool operand — InOperandFor gates bool TARGETS only");

// Both in() requires-clauses changed, and #578 was exactly a Field/CollatedField body drift, so
// pin the collated twin on a nullable column rather than trusting the two to stay in step.
template <typename V>
concept CanCollateInNickname =
        requires(V v) { fields::InNullableRow.nickname.collate(storm::orm::utilities::Collate::NoCase).in(v); };
static_assert(CanCollateInNickname<const char*>, "CollatedField::in must accept a nullable string column too (#625)");
static_assert(
        !CanCollateInNickname<std::nullopt_t>, "CollatedField::in must reject std::nullopt the same as Field::in"
);

// Still rejected, but now AT THE CONSTRAINT rather than inside the variant construction.
// ExpressionVariant has no InExpression arm for either, and normalize_operand folds neither to
// a type that does. Both are documented non-filterable in WHERE_CLAUSES.md; path is an
// accidental gap tracked separately, BLOB needs cross-backend comparison semantics first.
template <typename V>
concept CanInRawData = requires(V v) { fields::ExtendedTypes.raw_data.in(v); };
static_assert(!CanInRawData<std::vector<std::byte>>, ".in() on a BLOB column must be rejected cleanly");

template <typename V>
concept CanInFilePath = requires(V v) { fields::ExtendedTypes.file_path.in(v); };
static_assert(!CanInFilePath<std::filesystem::path>, ".in() on a path column must be rejected cleanly");

// ── Runtime behaviour, both backends ───────────────────────────────────────────────────────────

namespace {

    template <typename ConnType> class InNullableTest : public StormTestFixture<InNullableRow, ConnType> {
      public:
        // Row 3 has every nullable member NULL. Its absence from the .in() results below is the
        // point: SQL three-valued logic, asserted rather than reasoned about. `NULL IN (...)`
        // evaluates to NULL, which is not TRUE, so WHERE excludes the row — on SQLite and PG
        // alike. Nothing in Storm guards this and nothing needs to: the emitted text is
        // `col IN (?, ?)` (nullability leaves no trace) and the bound params are the OPERANDS,
        // never the column.
        auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
            storm::QuerySet<InNullableRow, ConnType> qs;
            ASSERT_TRUE(qs.insert(InNullableRow{
                                          .id        = 1,
                                          .nickname  = "alice",
                                          .score     = 10,
                                          .opt_uuid  = storm::UUID{kUuidA},
                                          .is_active = true
                                  })
                                .execute()
                                .has_value());
            ASSERT_TRUE(qs.insert(InNullableRow{
                                          .id        = 2,
                                          .nickname  = "bob",
                                          .score     = 20,
                                          .opt_uuid  = storm::UUID{kUuidB},
                                          .is_active = false
                                  })
                                .execute()
                                .has_value());
            ASSERT_TRUE(qs.insert(InNullableRow{.id = 3, .is_active = true}).execute().has_value());
        }
    };

} // namespace
TYPED_TEST_SUITE(InNullableTest, DatabaseTypes);

TYPED_TEST(InNullableTest, InOnNullableStringMatchesAndOmitsNullRow) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.nickname.in("alice", "carol")).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value().begin()->id, 1);
}

TYPED_TEST(InNullableTest, InOnNullableIntMatchesBothAndOmitsNullRow) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.score.in(10, 20)).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value().size(), 2U);
    for (const auto& row : result.value()) {
        EXPECT_NE(row.id, 3) << "the NULL-score row must not match IN";
    }
}

TYPED_TEST(InNullableTest, InOnNullableUuidWithUuidOperand) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.opt_uuid.in(storm::UUID{kUuidA})).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1U);
    EXPECT_EQ(result.value().begin()->id, 1);
}

// make_in_expr static_casts each operand to TargetType before normalizing, so a string operand
// becomes UUID(str) by construction — which is why .in() never needed #622's comparison-side fix.
TYPED_TEST(InNullableTest, InOnNullableUuidWithStringOperand) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.opt_uuid.in(kUuidA, kUuidB)).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value().size(), 2U);
}

// ── #609/#622's UUID guard on a NULLABLE UUID column ──────────────────────────────────────────
// std::optional<storm::UUID> is a shape no in-tree model had before: UuidFilterDoc's columns are
// non-nullable, so the existing rejection tests (test_where_uuid_filter.cpp) cannot cover it. The
// unwrap is what makes the guard REACHABLE here — before #625 this shape hard-errored — so the
// three tests below assert that rather than argue it. #609's whole point was that a silently
// auto-generated UUID matches zero rows and reports success.

TYPED_TEST(InNullableTest, InOnNullableUuidWithEmptyUuidIsRejected) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.opt_uuid.in(storm::UUID{kUuidA}, storm::UUID{})).select().execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), kExpectedFilterError);
}

TYPED_TEST(InNullableTest, InOnNullableUuidWithMalformedTextIsRejected) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.opt_uuid.in(storm::UUID{"not-a-uuid"})).select().execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), "Invalid UUID format: 'not-a-uuid'");
}

// Pins the comparison side too: comparison_column_info now returns an UNWRAPPED type for this
// member, so ColumnIsUuid must still resolve it as a UUID column.
TYPED_TEST(InNullableTest, EqualsOnNullableUuidWithEmptyUuidIsRejected) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.opt_uuid == storm::UUID{}).select().execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), kExpectedFilterError);
}

TYPED_TEST(InNullableTest, InOnBoolColumn) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.is_active.in(true)).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value().size(), 2U);
}

// The documented way to include NULL rows: IN alone never can.
TYPED_TEST(InNullableTest, IsNullOrInIncludesTheNullRow) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto result = qs.where(fields::InNullableRow.nickname.is_null() || fields::InNullableRow.nickname.in("alice"))
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value().size(), 2U);
}

// Empty list -> `1 = 0` (where.cppm InExpression::to_sql). Matches nothing, NULL row included.
TYPED_TEST(InNullableTest, EmptyInMatchesNothing) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    auto                                      result = qs.where(fields::InNullableRow.nickname.in()).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value().size(), 0U);
}

// Nullability must leave no trace in the emitted text — the column's NULL-ness is row data the
// DB evaluates, never something Storm binds or spells.
TYPED_TEST(InNullableTest, NullableColumnEmitsPlainInText) {
    storm::QuerySet<InNullableRow, TypeParam> qs;
    const auto sql = qs.where(fields::InNullableRow.nickname.in("alice", "bob")).select().sql();
    EXPECT_NE(sql.find("nickname IN (?, ?)"), std::string::npos) << sql;
}

// NOLINTEND(readability-implicit-bool-conversion)
