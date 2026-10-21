#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_transaction;
import storm_orm_utilities;
import std;
import storm_orm_statements_base;

#include "test_fixture.h" // NOSONAR cpp:S954
using storm::full_unsigned;
using storm::primary;
using storm::primary_autoincrement;
using storm::primary_part;
using storm::signed_storage;
using storm::UUID;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::PrimaryKeyType;
using storm::orm::statements::SelectStatement;

// Tests for the PrimaryKeyType<T> concept (#505, widened for composite parts in #517):
// a compile-time gate on the type of a model's primary-key member(s). DECIDED
// 2026-07-22 for single PKs: every signed integral type except bool — short/int/long/
// long long and their fixed-width spellings. Rejected: bool, std::optional<T>, unsigned
// types (unless signed_storage), and text. UUID was admitted by #507.
// DECIDED 2026-07-29 for composite parts (#517): the same allowlist WIDENED WITH TEXT
// (std::string / std::string_view), because a composite key is never DB-generated (#502)
// — so #505's int64-identity rationale (RETURNING id / last_insert_rowid) does not reach
// parts. A TEXT SINGLE PK stays rejected. An FK part routes through valid_fk_key_target:
// it binds the TARGET's key, so the target's PK must itself be well-typed — the widening
// does not reach through an FK.

// ============================================================================
// Test models (local to this TU)
// ============================================================================

namespace {

    // ---- Accepted: signed integral widths ----
    struct PkShort {
        [[= primary]] short id{};
    };
    struct PkInt {
        [[= primary]] int id{};
    };
    struct PkLong {
        [[= primary]] long id{};
    };
    struct PkLongLong {
        [[= primary]] long long id{};
    };
    struct PkInt16 {
        [[= primary]] std::int16_t id{};
    };
    struct PkInt32 {
        [[= primary]] std::int32_t id{};
    };
    struct PkInt64 {
        [[= primary]] std::int64_t id{};
    };
    struct PkPrimaryAutoincrement {
        [[= primary_autoincrement]] int id{};
    };
    // A uint64_t PK explicitly annotated signed_storage is admitted — it binds and
    // extracts through the same int64 path as any other signed field (#436).
    struct PkUint64SignedStorage {
        [[ = primary, = signed_storage ]] std::uint64_t id{};
    };

    // ---- Rejected: bool ----
    struct PkBool {
        [[= primary]] bool id{};
    };

    // ---- Rejected: char (a 1-byte identity is pathological; not in the width list) ----
    struct PkChar {
        [[= primary]] char id{};
    };

    // ---- Rejected: nullable primary key ----
    struct PkOptionalInt {
        [[= primary]] std::optional<int> id{};
    };

    // ---- Rejected: unsigned without signed_storage ----
    // (unsigned short/int are not gated by ModelStorageAnnotated at all, so they
    // reach PrimaryKeyType — reject them here since they aren't in the decided set.)
    struct PkUnsignedInt {
        [[= primary]] unsigned int id{};
    };
    // full_unsigned stores as zero-padded 20-char TEXT, which breaks every
    // extract_int64 site a PK is read through — reject even though it is annotated.
    struct PkUint64FullUnsigned {
        [[ = primary, = full_unsigned ]] std::uint64_t id{};
    };

    // ---- Rejected: text ----
    struct PkText {
        [[= primary]] std::string id{};
    };

    // ---- Rejected: UUID (not yet — a "not never", see #507) ----
    struct PkUuid {
        [[= primary]] UUID id{};
    };

    // ---- Composite PK parts (#517) ----
    // Accepted: the #505 scalar allowlist, widened with TEXT for parts only.
    struct PartsAllInt {
        [[= primary_part]] int order_id{};
        [[= primary_part]] int product_id{};
    };
    struct PartsIntAndText {
        [[= primary_part]] int         warehouse{};
        [[= primary_part]] std::string sku;
    };
    struct PartsThreeMixed {
        [[= primary_part]] int          region{};
        [[= primary_part]] std::string  account;
        [[= primary_part]] std::int64_t period{};
    };
    struct PartsStringView {
        [[= primary_part]] int              id{};
        [[= primary_part]] std::string_view code;
    };
    struct PartsUuid {
        [[= primary_part]] UUID order_id{};
        [[= primary_part]] int  product_id{};
    };
    struct PartsUint64SignedStorage {
        [[= primary_part]] int                               id{};
        [[ = primary_part, = signed_storage ]] std::uint64_t big{};
    };
    // An FK part is validated through its TARGET's primary key, not its own declared
    // type — it binds the referenced row's key (bind_one_pk_part's is_fk_field branch).
    struct PartFkTarget {
        [[= primary]] int id{};
    };
    struct PartsFkAndInt {
        [[= primary_part]][[= storm::fk<>]] PartFkTarget warehouse;
        [[= primary_part]] int                           sku{};
    };

    // Rejected part types.
    struct PartsBool {
        [[= primary_part]] int  id{};
        [[= primary_part]] bool flag{};
    };
    struct PartsChar {
        [[= primary_part]] int  id{};
        [[= primary_part]] char code{};
    };
    struct PartsOptionalInt {
        [[= primary_part]] int                id{};
        [[= primary_part]] std::optional<int> maybe{};
    };
    // Guards the is_text_member optional-unwrap trap: is_text_member looks THROUGH
    // std::optional<>, so a naive TEXT check would accept this nullable part.
    struct PartsOptionalText {
        [[= primary_part]] int                        id{};
        [[= primary_part]] std::optional<std::string> maybe;
    };
    struct PartsUnsignedInt {
        [[= primary_part]] int          id{};
        [[= primary_part]] unsigned int u{};
    };
    struct PartsUint64FullUnsigned {
        [[= primary_part]] int                              id{};
        [[ = primary_part, = full_unsigned ]] std::uint64_t big{};
    };
    struct PartsDouble {
        [[= primary_part]] int    id{};
        [[= primary_part]] double amount{};
    };
    struct PartsBlob {
        [[= primary_part]] int                       id{};
        [[= primary_part]] std::vector<std::uint8_t> data;
    };
    // An FK part whose target has NO primary key — valid_fk_key_target must reject it.
    struct NoPkTarget {
        int value{};
    };
    struct PartsFkNoPkTarget {
        [[= primary_part]][[= storm::fk<>]] NoPkTarget bad;
        [[= primary_part]] int                         sku{};
    };
    // An FK part whose target HAS a PK, but of a type the policy rejects. The part binds
    // the TARGET's key (bind_one_pk_part splices find_fk_primary_key), so a double would
    // land in the composite key column — the policy must not be circumventable one hop
    // away. Checking PK presence alone (valid_fk_target) would wrongly accept this.
    struct BadPkTarget {
        [[= primary]] double id{};
    };
    struct PartsFkBadPkTarget {
        [[= primary_part]][[= storm::fk<>]] BadPkTarget bad;
        [[= primary_part]] int                          sku{};
    };
    // An FK part whose target is itself COMPOSITE-keyed. Not merely unsupported — #504
    // shipped composite FKs for ordinary FK fields — but bind_one_pk_part still splices
    // the single-column find_fk_primary_key, so such a part would bind ONE column for an
    // N-column key. Refused outright rather than validated on its first part alone. Also
    // guards the is_primary_member subsumption trap: is_primary_member matches
    // primary_part too, so testing it first would accept this model after checking only
    // `warehouse`, never seeing `sku`.
    struct CompositeKeyTarget {
        [[= primary_part]] int         warehouse{};
        [[= primary_part]] std::string sku;
    };
    struct PartsFkCompositeTarget {
        [[= primary_part]][[= storm::fk<>]] CompositeKeyTarget ref;
        [[= primary_part]] int                                 n{};
    };
    // The parts-only TEXT widening does NOT reach through an FK: what binds here is the
    // target's own single PK, and #505 rejects a std::string single PK. So an FK part
    // pointing at a TEXT-keyed model inherits that rejection.
    struct TextPkTarget {
        [[= primary]] std::string id;
    };
    struct PartsFkTextPkTarget {
        [[= primary_part]][[= storm::fk<>]] TextPkTarget ref;
        [[= primary_part]] int                           sku{};
    };

} // namespace

// ============================================================================
// Compile-time concept gate — PrimaryKeyType<T>
// ============================================================================

// PrimaryKeyType<T> is the model-boundary concept: true iff T's primary-key member
// has a type in the decided allowed set. It is one of the concepts BaseStatement<T>
// (and thus QuerySet<T>) requires, so a bad model fails to instantiate with a clear
// constraint violation. These static_asserts pin the contract on the GATE predicate
// itself (mirrors ModelMaxLengthValid in test_max_length.cpp), not on a downstream
// public call.

static_assert(PrimaryKeyType<PkShort>, "short PK must be accepted");
static_assert(PrimaryKeyType<PkInt>, "int PK must be accepted");
static_assert(PrimaryKeyType<PkLong>, "long PK must be accepted");
static_assert(PrimaryKeyType<PkLongLong>, "long long PK must be accepted");
static_assert(PrimaryKeyType<PkInt16>, "std::int16_t PK must be accepted");
static_assert(PrimaryKeyType<PkInt32>, "std::int32_t PK must be accepted");
static_assert(PrimaryKeyType<PkInt64>, "std::int64_t PK must be accepted");
static_assert(PrimaryKeyType<PkPrimaryAutoincrement>, "primary_autoincrement int PK must be accepted");
static_assert(PrimaryKeyType<PkUint64SignedStorage>, "uint64_t PK annotated signed_storage must be accepted");

static_assert(!PrimaryKeyType<PkBool>, "bool PK must be rejected");
static_assert(!PrimaryKeyType<PkChar>, "char PK must be rejected");
static_assert(!PrimaryKeyType<PkOptionalInt>, "std::optional<int> PK must be rejected");
static_assert(!PrimaryKeyType<PkUnsignedInt>, "unsigned int PK must be rejected");
static_assert(!PrimaryKeyType<PkUint64FullUnsigned>, "uint64_t PK annotated full_unsigned must be rejected");
static_assert(!PrimaryKeyType<PkText>, "std::string PK must be rejected");
// UUID PKs are now supported (#507) — caller must provide the UUID explicitly
static_assert(PrimaryKeyType<PkUuid>, "storm::UUID primary keys should be accepted");

// ---- Composite PK parts (#517) ----
// Accepted
static_assert(PrimaryKeyType<PartsAllInt>, "all-int composite parts must be accepted");
static_assert(PrimaryKeyType<PartsIntAndText>, "a TEXT composite part must be accepted");
static_assert(PrimaryKeyType<PartsThreeMixed>, "3-part mixed int/TEXT/int64 must be accepted");
static_assert(PrimaryKeyType<PartsStringView>, "a string_view part must be accepted");
static_assert(PrimaryKeyType<PartsUuid>, "a storm::UUID part must be accepted");
static_assert(PrimaryKeyType<PartsUint64SignedStorage>, "a signed_storage uint64 part must be accepted");
static_assert(PrimaryKeyType<PartsFkAndInt>, "an FK part with a PK'd target must be accepted");

// Rejected
static_assert(!PrimaryKeyType<PartsBool>, "a bool part must be rejected");
static_assert(!PrimaryKeyType<PartsChar>, "a char part must be rejected");
static_assert(!PrimaryKeyType<PartsOptionalInt>, "a nullable part must be rejected");
static_assert(
        !PrimaryKeyType<PartsOptionalText>, "optional<string> must be rejected — is_text_member looks through optional"
);
static_assert(!PrimaryKeyType<PartsUnsignedInt>, "an unsigned int part must be rejected");
static_assert(
        !PrimaryKeyType<PartsUint64FullUnsigned>,
        "a full_unsigned uint64 part must be rejected — it stores as zero-padded TEXT"
);
static_assert(!PrimaryKeyType<PartsDouble>, "a double part must be rejected");
static_assert(!PrimaryKeyType<PartsBlob>, "a blob part must be rejected");
static_assert(!PrimaryKeyType<PartsFkNoPkTarget>, "an FK part whose target has no PK must be rejected");
static_assert(
        !PrimaryKeyType<PartsFkBadPkTarget>,
        "an FK part whose target PK is a rejected type must be rejected — the part binds that key"
);
static_assert(
        !PrimaryKeyType<PartsFkTextPkTarget>,
        "the parts-only TEXT widening must not reach through an FK to a std::string single PK"
);
static_assert(
        !PrimaryKeyType<PartsFkCompositeTarget>,
        "an FK part pointing at a composite-keyed target needs a multi-column FK (#504) — refuse it"
);

// ============================================================================
// Positive path — accepted PK types remain queryable end to end
// ============================================================================

namespace {
    // A non-PK column keeps this out of the single-field (PK-only, non-autoincrement)
    // INSERT edge case — a separate, pre-existing issue unrelated to #505.
    struct PkIntWithColumn {
        [[= primary]] int id{};
        int               value{};
    };
} // namespace

template <typename ConnType> class PrimaryKeyTypeAcceptedTest : public StormTestFixture<PkIntWithColumn, ConnType> {};
TYPED_TEST_SUITE(PrimaryKeyTypeAcceptedTest, DatabaseTypes);

TYPED_TEST(PrimaryKeyTypeAcceptedTest, InsertAndSelectRoundTrip) {
    auto result =
            (InsertStatement<PkIntWithColumn, TypeParam>{}).query(PkIntWithColumn{.id = 1, .value = 42}).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = (SelectStatement<PkIntWithColumn, TypeParam>{}).all().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    ASSERT_EQ(selected.value().size(), 1U);
    EXPECT_EQ(selected.value().begin()->id, 1);
    EXPECT_EQ(selected.value().begin()->value, 42);
}

TEST(PrimaryKeyType, CompileTimeOnly) {
    // The static_asserts above are the real test; this keeps the TU a runnable
    // GTest target so the suite reports it.
    SUCCEED();
}
