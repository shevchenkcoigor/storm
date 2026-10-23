#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm;
import storm_orm_statements_insert;         // default_return_id / ReturnIdSupported — not re-exported by `storm`
import storm_orm_statements_upsert_grammar; // UpsertGrammar / ConflictTargetUnique — not re-exported by `storm`
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954 — Person, the single-PK regression anchor
#include "test_fixture.h"
using storm::field_specs_for;
using storm::primary;
using storm::primary_autoincrement;
using storm::primary_part;
using storm::QuerySet;
using storm::UUID;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::ReturnId;

// ── #572: a UUID primary key is never DB-generated, so nothing is RETURNed ────
//
// default_return_id<T>() resolved to ReturnId::Yes for every non-composite
// model, so plain insert(uuid_model) emitted "INSERT ... RETURNING <uuid_pk>"
// and extracted that TEXT/UUID column with extract_int64(0). The caller got a
// std::expected<int64_t, Error> whose value can never identify the row — 0 on
// SQLite, garbage or an error on PostgreSQL.
//
// This is #502's composite-key reasoning one shape wider: AUTOINCREMENT and
// GENERATED ... AS IDENTITY are single-INTEGER-column features, so a UUID key
// is always caller data and RETURNING could only echo the input back.
//
// The UPSERT path carried the identical defect through the same entry point —
// UpsertGrammar's nothing_sql()/update_sql() gated RETURNING on
// has_composite_pk_ alone — and is fixed here in the same pass.
//
// Two things must be proven, and the TEXT assertions alone do not prove the
// second: SQLite tolerates a lossy extract_int64 on a TEXT column silently
// (yielding 0), PostgreSQL errors — so every round-trip below is a TYPED_TEST
// over both backends.

// NOLINTBEGIN(readability-implicit-bool-conversion)

namespace {

    using storm::orm::statements::default_return_id;
    using storm::orm::statements::ReturnId;
    using storm::orm::statements::ReturnIdSupported;

    // Single UUID PK, no other relation machinery — the minimal shape the defect
    // needs. A distinct model from test_junction_column_type.cpp's UuidDoc so
    // this TU owns its own tables.
    struct UuidNoteRec {
        [[= primary]] UUID id{};
        std::string        title;
    };

    // UUID PK whose member is NOT named "id": RETURNING would name the pk
    // column, so a hardcoded "id" would sit dead behind the same branch.
    struct UuidTicketRec {
        [[= primary]] UUID              ticket_uuid{};
        [[= storm::unique]] std::string code;
    };

    // The regression anchor: a plain single-INTEGER PK must keep the RETURNING
    // path byte-for-byte, since that is the one shape a DB really does generate.
    // `title` is unique so this model can also anchor the UPSERT text, whose two
    // RETURNING clauses this issue rewrote — deliberately the SAME shape as
    // UuidTicketRec above (key + one unique text column) so the two goldens differ
    // in exactly the key type and nothing else.
    struct IntNoteRec {
        [[= primary_autoincrement]] std::int64_t id{};
        [[= storm::unique]] std::string          title;
    };

    // The OTHER non-generated shape. This TU is about UUID keys, but the fix's
    // thesis is that ONE predicate now serves both — so without a composite model
    // here, narrowing pk_is_db_generated_() back to `!has_uuid_pk_()` would leave
    // every assertion below green while silently regressing composite INSERT and
    // upsert (#502/#503). Asserted, not round-tripped: the composite behaviour
    // itself is owned by test_composite_pk_sql.cpp / test_composite_pk_crud.cpp,
    // and what needs pinning HERE is only that both shapes share the one gate.
    struct CompositeNoteRec {
        [[= primary_part]] std::int64_t region{};
        [[= primary_part]] std::int64_t slot{};
        std::string                     title;
    };

    constexpr std::string_view kNoteUuid   = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    constexpr std::string_view kNoteUuid2  = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    constexpr std::string_view kTicketUuid = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";

    // SingleInsertQuery<R>::sql() is static — the text derives purely
    // from the model, so no connection is needed (same helper as #502's TU).
    template <typename T, ReturnId R = default_return_id<T>()> auto insert_sql() -> std::string {
        return InsertStatement<T, storm::db::sqlite::Connection>::template SingleInsertQuery<R>::sql();
    }

} // namespace

// fields:: selector proxies (#518). At global scope, NOT inside the anonymous
// namespace above: a nested `namespace fields` makes every unqualified
// `fields::` reference in this TU ambiguous against the global one.
namespace fields {

    struct UuidNoteRecT;
    consteval {
        std::meta::define_aggregate(^^UuidNoteRecT, field_specs_for(^^UuidNoteRec));
    }
    inline constexpr UuidNoteRecT UuidNoteRec{};

    struct UuidTicketRecT;
    consteval {
        std::meta::define_aggregate(^^UuidTicketRecT, field_specs_for(^^UuidTicketRec));
    }
    inline constexpr UuidTicketRecT UuidTicketRec{};

    struct IntNoteRecT;
    consteval {
        std::meta::define_aggregate(^^IntNoteRecT, field_specs_for(^^IntNoteRec));
    }
    inline constexpr IntNoteRecT IntNoteRec{};

} // namespace fields

// ============================================================================
// Compile-time gates
// ============================================================================

namespace {

    // (1) The default plain insert() resolves to.
    static_assert(
            default_return_id<UuidNoteRec>() == ReturnId::No,
            "plain insert() on a UUID-PK model resolves to the void path — nothing is DB-generated"
    );
    static_assert(
            default_return_id<UuidTicketRec>() == ReturnId::No,
            "the default is driven by the key TYPE, not by the member being named 'id'"
    );
    static_assert(
            default_return_id<IntNoteRec>() == ReturnId::Yes,
            "plain insert() on a single-integer-PK model still returns the generated id"
    );
    // The shared-gate assertions: pk_is_db_generated_() must answer for BOTH
    // non-generated shapes. Narrowing it to `!has_uuid_pk_()` passes every other
    // assertion in this TU and fails only these.
    static_assert(
            default_return_id<CompositeNoteRec>() == ReturnId::No,
            "the predicate this issue introduced must still cover composite keys (#502)"
    );
    static_assert(
            !ReturnIdSupported<CompositeNoteRec, ReturnId::Yes>,
            "the gate is shared between the UUID and composite shapes, not UUID-only"
    );
    static_assert(
            ReturnIdSupported<CompositeNoteRec, ReturnId::No>,
            "ReturnId::No stays valid on every model shape (composite)"
    );

    // (2) The gate. Asserted on the concept AND, below, on the public call site
    // it constrains — a static_assert on the concept alone survives deleting the
    // requires-clause that wires it to insert().
    static_assert(
            !ReturnIdSupported<UuidNoteRec, ReturnId::Yes>,
            "an explicit ReturnId::Yes on a UUID-PK model must be rejected, not silently lossy"
    );
    static_assert(
            !ReturnIdSupported<UuidTicketRec, ReturnId::Yes>, "the gate does not depend on the key member's name"
    );
    static_assert(ReturnIdSupported<UuidNoteRec, ReturnId::No>, "ReturnId::No stays valid on every model shape (UUID)");
    static_assert(
            ReturnIdSupported<IntNoteRec, ReturnId::No>, "ReturnId::No stays valid on every model shape (single int)"
    );
    static_assert(ReturnIdSupported<IntNoteRec, ReturnId::Yes>, "a single-integer-PK model keeps the RETURNING path");

    // (3) The gate is WIRED to the public entry points. Written as `!requires`
    // in an unevaluated context because a TU cannot contain the ill-formed call
    // it asserts about. Available here (unlike #502's is_settable_member) only
    // because insert() is genuinely constrained by ReturnIdSupported.
    template <typename Model, typename ConnType>
    concept SingleInsertYesCallable =
            requires(QuerySet<Model, ConnType>& qs, const Model& obj) { qs.template insert<ReturnId::Yes>(obj); };
    template <typename Model, typename ConnType>
    concept BulkInsertYesCallable = requires(QuerySet<Model, ConnType>& qs, std::span<const Model> objs) {
        qs.template insert<ReturnId::Yes>(objs);
    };

    using Sqlite = storm::db::sqlite::Connection;

    static_assert(
            !SingleInsertYesCallable<UuidNoteRec, Sqlite>,
            "qs.insert<ReturnId::Yes>(uuid_model) must fail at the CALL SITE, not deep in the statement"
    );
    static_assert(
            !BulkInsertYesCallable<UuidNoteRec, Sqlite>,
            "the bulk overload routes to BulkInsertQuery<ReturnId::Yes> — it must be gated too"
    );
    static_assert(
            SingleInsertYesCallable<IntNoteRec, Sqlite>, "a single-integer-PK model's insert<Yes> still compiles"
    );
    static_assert(
            BulkInsertYesCallable<IntNoteRec, Sqlite>, "a single-integer-PK model's bulk insert<Yes> still compiles"
    );
    static_assert(SingleInsertYesCallable<Person, Sqlite>, "the in-tree default model is untouched");

    // ── #585: a key INSERT EMITS can be an ON CONFLICT target ────────────────
    //
    // ConflictTargetUnique gated the PK case on has_composite_pk_, whose stated
    // reason was "INSERT omits a single-column PK, so it can never conflict".
    // True for a DB-generated integer key; false for a UUID key ever
    // since #565 put that column back in the INSERT. So the natural upsert on a
    // UUID key was a compile error, workaround-able only by declaring a
    // redundant UniqueIndex over the primary key.
    //
    // The gate is now the same "does INSERT emit this key" question #572 named,
    // which keeps the integer case rejected for the reason that actually applies
    // to it rather than by accident of arity.
    using storm::orm::statements::ConflictTargetUnique;

    static_assert(
            ConflictTargetUnique<UuidNoteRec, ^^UuidNoteRec::id>,
            "a caller-supplied UUID key IS in the INSERT, so it can conflict (#585)"
    );
    static_assert(
            ConflictTargetUnique<UuidTicketRec, ^^UuidTicketRec::ticket_uuid>,
            "the UUID key is a valid target whatever the member is named"
    );
    static_assert(
            !ConflictTargetUnique<IntNoteRec, ^^IntNoteRec::id>,
            "a DB-generated integer key is absent from the INSERT — it can never conflict"
    );
    static_assert(!ConflictTargetUnique<Person, ^^Person::id>, "the in-tree default model's PK stays rejected");
    // The composite case (#503) is unchanged: full set accepted in declaration
    // order, strict subset rejected. Asserted here because it now shares the gate.
    static_assert(
            ConflictTargetUnique<CompositeNoteRec, ^^CompositeNoteRec::region, ^^CompositeNoteRec::slot>,
            "the full composite-PK set is still a valid conflict target (#503)"
    );
    static_assert(
            !ConflictTargetUnique<CompositeNoteRec, ^^CompositeNoteRec::region>,
            "a STRICT SUBSET of a composite key is not unique on its own"
    );
    // A non-key, non-unique column is still not a conflict target either way.
    static_assert(
            !ConflictTargetUnique<UuidNoteRec, ^^UuidNoteRec::title>,
            "widening the PK case must not make an ordinary column a valid target"
    );

} // namespace

// ============================================================================
// Generated SQL text
// ============================================================================

// The whole defect, in one line of text: no RETURNING, and the caller's key
// column IS in the INSERT (it is data, per #565).
TEST(UuidPkReturningSql, PlainInsertEmitsNoReturningAndIncludesTheKey) {
    const std::string sql = insert_sql<UuidNoteRec>();
    EXPECT_EQ(sql, "INSERT INTO UuidNoteRec (id, title) VALUES (?, ?)");
    EXPECT_FALSE(sql.contains("RETURNING")) << sql;
}

TEST(UuidPkReturningSql, KeyMemberNotNamedIdIsAlsoCovered) {
    const std::string sql = insert_sql<UuidTicketRec>();
    EXPECT_EQ(sql, "INSERT INTO UuidTicketRec (ticket_uuid, code) VALUES (?, ?)");
    EXPECT_FALSE(sql.contains("RETURNING")) << sql;
}

// Explicit ReturnId::No was already valid on a UUID model and its text is
// unchanged — the default now simply resolves to the same statement.
TEST(UuidPkReturningSql, ExplicitNoMatchesTheNewDefault) {
    EXPECT_EQ((insert_sql<UuidNoteRec, ReturnId::No>()), insert_sql<UuidNoteRec>());
}

// Regression anchor: the shape a DB really does generate keeps RETURNING.
TEST(UuidPkReturningSql, SingleIntegerPkTextIsByteIdentical) {
    EXPECT_EQ((insert_sql<IntNoteRec, ReturnId::No>()), "INSERT INTO IntNoteRec (title) VALUES (?)");
    EXPECT_EQ(insert_sql<IntNoteRec>(), "INSERT INTO IntNoteRec (title) VALUES (?) RETURNING id");
}

// The upsert half of the same defect: DO UPDATE / DO NOTHING both echoed the
// caller's UUID back through RETURNING and extracted it as an int64.
//
// Asserted as whole-string goldens, not `contains("RETURNING")` substring
// checks: this issue REWROTE both of these clauses, so the anchor has to pin the
// entire statement or a change to the rest of it passes unnoticed. IntNoteRec and
// UuidTicketRec have the same shape (key + one unique text column), so each pair
// of goldens differs in exactly the key type — the variable under test.
TEST(UuidPkReturningSql, UpsertOmitsReturningOnAUuidKey) {
    using storm::orm::statements::UpsertGrammar;
    EXPECT_EQ(
            UpsertGrammar<UuidTicketRec>::nothing_sql<^^UuidTicketRec::code>(),
            "INSERT INTO UuidTicketRec (ticket_uuid, code) VALUES (?, ?) ON CONFLICT (code) DO NOTHING"
    );
    EXPECT_EQ(
            UpsertGrammar<UuidTicketRec>::update_sql<^^UuidTicketRec::code>(
                    UpsertGrammar<UuidTicketRec>::build_excluded_set_clause<^^UuidTicketRec::code>()
            ),
            "INSERT INTO UuidTicketRec (ticket_uuid, code) VALUES (?, ?) "
            "ON CONFLICT (code) DO UPDATE SET code=excluded.code"
    );
}

// Regression anchor for the upsert half — the shape a DB really does generate
// keeps its RETURNING on BOTH terminals. Covering only DO NOTHING would leave
// update_sql's rewritten clause unanchored on the unchanged path.
TEST(UuidPkReturningSql, UpsertKeepsReturningOnASingleIntegerKey) {
    using storm::orm::statements::UpsertGrammar;
    EXPECT_EQ(
            UpsertGrammar<IntNoteRec>::nothing_sql<^^IntNoteRec::title>(),
            "INSERT INTO IntNoteRec (title) VALUES (?) ON CONFLICT (title) DO NOTHING RETURNING id"
    );
    EXPECT_EQ(
            UpsertGrammar<IntNoteRec>::update_sql<^^IntNoteRec::title>(
                    UpsertGrammar<IntNoteRec>::build_excluded_set_clause<^^IntNoteRec::title>()
            ),
            "INSERT INTO IntNoteRec (title) VALUES (?) "
            "ON CONFLICT (title) DO UPDATE SET title=excluded.title RETURNING id"
    );
}

// ── #585: the UUID key itself as the conflict target ────────────────────────
// The target column must come from the canonical column-name writer, so a key
// member NOT named "id" is the case that would expose a hardcode.
TEST(UuidPkConflictTargetSql, UuidKeyIsAValidConflictTarget) {
    using storm::orm::statements::UpsertGrammar;
    EXPECT_EQ(
            UpsertGrammar<UuidTicketRec>::nothing_sql<^^UuidTicketRec::ticket_uuid>(),
            "INSERT INTO UuidTicketRec (ticket_uuid, code) VALUES (?, ?) ON CONFLICT (ticket_uuid) DO NOTHING"
    );
    EXPECT_EQ(
            UpsertGrammar<UuidTicketRec>::update_sql<^^UuidTicketRec::ticket_uuid>(
                    UpsertGrammar<UuidTicketRec>::build_excluded_set_clause<^^UuidTicketRec::code>()
            ),
            "INSERT INTO UuidTicketRec (ticket_uuid, code) VALUES (?, ?) "
            "ON CONFLICT (ticket_uuid) DO UPDATE SET code=excluded.code"
    );
}

// ============================================================================
// Executable round-trip — both backends
//
// PostgreSQL is the backend that genuinely ERRORS on the pre-fix statement
// (extracting a UUID column as int64), while SQLite quietly returns 0. Text
// assertions alone would not have caught either half.
// ============================================================================

template <typename ConnType> class UuidPkReturningTest : public StormTestFixture<UuidNoteRec, ConnType> {};
TYPED_TEST_SUITE(UuidPkReturningTest, DatabaseTypes);

// Plain insert() — no explicit ReturnId — now yields expected<void>, and the
// caller's key is what actually landed in the row.
TYPED_TEST(UuidPkReturningTest, PlainInsertReturnsVoidAndPersistsTheCallerKey) {
    QuerySet<UuidNoteRec, TypeParam> qs;

    auto result = qs.insert(UuidNoteRec{.id = UUID{kNoteUuid}, .title = "Spec"}).execute();
    static_assert(
            std::is_same_v<decltype(result), std::expected<void, typename TypeParam::Error>>,
            "plain insert() on a UUID-PK model returns std::expected<void, Error> (#572, BREAKING)"
    );
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto rows = qs.where(fields::UuidNoteRec.title == "Spec").select().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_EQ(rows.value().begin()->id.value, std::string(kNoteUuid)) << "the caller's key must be the stored key";
}

// Explicit ReturnId::No is unchanged — the pre-#572 spelling keeps working.
TYPED_TEST(UuidPkReturningTest, ExplicitReturnIdNoStillWorks) {
    QuerySet<UuidNoteRec, TypeParam> qs;
    auto result = qs.template insert<ReturnId::No>(UuidNoteRec{.id = UUID{kNoteUuid}, .title = "Spec"}).execute();
    static_assert(std::is_same_v<decltype(result), std::expected<void, typename TypeParam::Error>>);
    ASSERT_TRUE(result.has_value()) << result.error().message();
}

// The bulk overload's default was already ReturnId::No; this drives the batch
// bind path with caller-supplied UUID keys so it is executed, not merely gated.
TYPED_TEST(UuidPkReturningTest, BulkInsertPersistsEveryCallerKey) {
    QuerySet<UuidNoteRec, TypeParam> qs;
    const std::vector<UuidNoteRec>   batch{
            UuidNoteRec{.id = UUID{kNoteUuid}, .title = "First"},
            UuidNoteRec{.id = UUID{kNoteUuid2}, .title = "Second"},
    };

    auto result = qs.insert(std::span<const UuidNoteRec>(batch)).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto first = qs.where(fields::UuidNoteRec.title == "First").select().execute();
    ASSERT_TRUE(first.has_value()) << first.error().message();
    ASSERT_EQ(first.value().size(), 1U);
    EXPECT_EQ(first.value().begin()->id.value, std::string(kNoteUuid));

    auto second = qs.where(fields::UuidNoteRec.title == "Second").select().execute();
    ASSERT_TRUE(second.has_value()) << second.error().message();
    ASSERT_EQ(second.value().size(), 1U);
    EXPECT_EQ(second.value().begin()->id.value, std::string(kNoteUuid2));
}

// An empty UUID key is still refused — the fix removes RETURNING, not the
// "a key is never auto-generated" guard (#565).
TYPED_TEST(UuidPkReturningTest, EmptyUuidKeyIsStillRejected) {
    QuerySet<UuidNoteRec, TypeParam> qs;
    auto                             result = qs.insert(UuidNoteRec{.id = UUID{}, .title = "No key"}).execute();
    EXPECT_FALSE(result.has_value()) << "an empty UUID primary key must be refused, not auto-generated";

    // Refused must mean "no row", not "reported an error after writing one" — the
    // error is raised at BIND time, so a row reaching the table would mean the
    // guard moved behind the write.
    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 0) << "a refused insert must leave the table empty";
}

// ── Upsert round-trip on a UUID key ─────────────────────────────────────────

template <typename ConnType> class UuidPkUpsertReturningTest : public StormTestFixture<UuidTicketRec, ConnType> {};
TYPED_TEST_SUITE(UuidPkUpsertReturningTest, DatabaseTypes);

TYPED_TEST(UuidPkUpsertReturningTest, DoUpdateReturnsVoidAndKeepsTheKey) {
    QuerySet<UuidTicketRec, TypeParam> qs;

    auto inserted = qs.insert(UuidTicketRec{.ticket_uuid = UUID{kTicketUuid}, .code = "T-1"})
                            .template on_conflict<fields::UuidTicketRec.code>()
                            .template update<fields::UuidTicketRec.code>()
                            .execute();
    static_assert(
            std::is_same_v<decltype(inserted), std::expected<void, typename TypeParam::Error>>,
            "DO UPDATE on a UUID-PK model has no generated id to return (#572)"
    );
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    // Second pass conflicts on `code` and takes the DO UPDATE branch.
    auto updated = qs.insert(UuidTicketRec{.ticket_uuid = UUID{kTicketUuid}, .code = "T-1"})
                           .template on_conflict<fields::UuidTicketRec.code>()
                           .template update<fields::UuidTicketRec.code>()
                           .execute();
    ASSERT_TRUE(updated.has_value()) << updated.error().message();

    auto rows = qs.where(fields::UuidTicketRec.code == "T-1").select().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows.value().size(), 1U) << "DO UPDATE must not have inserted a second row";
    EXPECT_EQ(rows.value().begin()->ticket_uuid.value, std::string(kTicketUuid));
}

TYPED_TEST(UuidPkUpsertReturningTest, DoNothingReturnsVoidAndSkipsTheConflict) {
    QuerySet<UuidTicketRec, TypeParam> qs;

    auto inserted = qs.insert(UuidTicketRec{.ticket_uuid = UUID{kTicketUuid}, .code = "T-1"})
                            .template on_conflict<fields::UuidTicketRec.code>()
                            .nothing()
                            .execute();
    static_assert(
            std::is_same_v<decltype(inserted), std::expected<void, typename TypeParam::Error>>,
            "DO NOTHING on a UUID-PK model cannot report an id, so it resolves to void (#572)"
    );
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    // A conflicting row with a DIFFERENT key is skipped, not inserted.
    auto skipped = qs.insert(UuidTicketRec{.ticket_uuid = UUID{kNoteUuid}, .code = "T-1"})
                           .template on_conflict<fields::UuidTicketRec.code>()
                           .nothing()
                           .execute();
    ASSERT_TRUE(skipped.has_value()) << skipped.error().message();

    auto rows = qs.where(fields::UuidTicketRec.code == "T-1").select().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_EQ(rows.value().begin()->ticket_uuid.value, std::string(kTicketUuid)) << "the ORIGINAL key must survive";
}

// ── #585: upserting ON the UUID key, executed ───────────────────────────────
//
// The point the SQL text cannot make: the DB must actually accept
// "ON CONFLICT (ticket_uuid)". PostgreSQL requires the target to be backed by a
// real unique constraint and errors otherwise ("there is no unique or exclusion
// constraint matching the ON CONFLICT specification"), so this fails loudly
// there if the PK were not genuinely the constraint being named — a check
// SQLite alone would not give.

TYPED_TEST(UuidPkUpsertReturningTest, DoUpdateOnTheUuidKeyItself) {
    QuerySet<UuidTicketRec, TypeParam> qs;
    const UUID                         key{kTicketUuid};

    auto inserted = qs.insert(UuidTicketRec{.ticket_uuid = key, .code = "first"})
                            .template on_conflict<fields::UuidTicketRec.ticket_uuid>()
                            .template update<fields::UuidTicketRec.code>()
                            .execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    // Same KEY, different payload: the conflict is on the key itself, so this
    // must UPDATE the existing row rather than insert or error.
    auto updated = qs.insert(UuidTicketRec{.ticket_uuid = key, .code = "second"})
                           .template on_conflict<fields::UuidTicketRec.ticket_uuid>()
                           .template update<fields::UuidTicketRec.code>()
                           .execute();
    ASSERT_TRUE(updated.has_value()) << updated.error().message();

    auto rows = qs.select().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows.value().size(), 1U) << "conflicting on the key must UPDATE, not insert a second row";
    EXPECT_EQ(rows.value().begin()->ticket_uuid.value, std::string(kTicketUuid));
    EXPECT_EQ(rows.value().begin()->code, "second") << "the DO UPDATE SET column must have been written";
}

TYPED_TEST(UuidPkUpsertReturningTest, DoNothingOnTheUuidKeyItself) {
    QuerySet<UuidTicketRec, TypeParam> qs;
    const UUID                         key{kTicketUuid};

    auto inserted = qs.insert(UuidTicketRec{.ticket_uuid = key, .code = "first"})
                            .template on_conflict<fields::UuidTicketRec.ticket_uuid>()
                            .nothing()
                            .execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    auto skipped = qs.insert(UuidTicketRec{.ticket_uuid = key, .code = "second"})
                           .template on_conflict<fields::UuidTicketRec.ticket_uuid>()
                           .nothing()
                           .execute();
    ASSERT_TRUE(skipped.has_value()) << skipped.error().message();

    auto rows = qs.select().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_EQ(rows.value().begin()->code, "first") << "DO NOTHING must leave the original row untouched";
}

// ── Single-integer-PK regression, executed ──────────────────────────────────

template <typename ConnType> class IntPkReturningTest : public StormTestFixture<IntNoteRec, ConnType> {};
TYPED_TEST_SUITE(IntPkReturningTest, DatabaseTypes);

TYPED_TEST(IntPkReturningTest, PlainInsertStillReturnsTheGeneratedId) {
    QuerySet<IntNoteRec, TypeParam> qs;
    auto                            result = qs.insert(IntNoteRec{.title = "Generated"}).execute();
    static_assert(
            std::is_same_v<decltype(result), std::expected<std::int64_t, typename TypeParam::Error>>,
            "a single-integer PK is DB-generated — its return type is unchanged"
    );
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_GT(result.value(), 0) << "the DB-generated id must be a real row id";
}

// NOLINTEND(readability-implicit-bool-conversion)
