#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954 — Person
#include "test_fixture.h"
using storm::field_specs_for;
using storm::primary;
using storm::primary_part;
using storm::UUID;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::ReturnId;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateStatement;

// ── #573: UPDATE/DELETE by an unset UUID primary key silently no-ops ─────────
//
// bind_one_pk_part (src/orm/statements/base.cppm) routed every PK part through
// bind_value_by_type -> bind_parameter_value -> utilities::bind_uuid, which
// AUTO-GENERATES a fresh random UUID when the value is empty. Correct for a
// non-PK UUID column, wrong for a KEY: the by-key WHERE clause bound a random
// UUID, matched zero rows, and reported success — a silent no-op write with no
// diagnostic. INSERT already guarded against this via bind_uuid_pk; the fix
// routes bind_one_pk_part through the same guard.
//
// Three shapes, each round-tripped when the key IS set and rejected with
// INSERT's own error message when it is not: a plain single UUID PK, a
// composite key with a UUID part, and an FK part whose target has a
// UUID key.
//
// #608 (INSERT-side, FK-part shape only): bind_field_at_index's is_fk_field
// branch (bind_fk_field_at_index) never routed the FK-target UUID PK value
// through bind_uuid_pk at all, so a composite key whose FK part targets a
// UUID key still auto-generated a fresh random UUID on INSERT — the one
// shape #573 left asymmetric. Tests are in the FK-part section below
// (InsertWithEmptyOwnerKeyIsRejected / InsertWithSetOwnerKeyInsertsTheRow).

// NOLINTBEGIN(readability-implicit-bool-conversion)

namespace {

    // Plain single UUID PK — the minimal shape the defect needs.
    struct UuidKeyedDoc {
        [[= primary]] UUID id{};
        std::string        title;
    };

    // Composite key with a UUID part alongside a plain int part.
    struct UuidComposite {
        [[= primary_part]] UUID         doc_id{};
        [[= primary_part]] std::int64_t version{};
        std::string                     content;
    };

    // FK target with a UUID key.
    struct UuidOwner {
        [[= primary]] UUID id{};
        std::string        name;
    };

    // Composite key whose FK part's TARGET has a UUID key. bind_one_pk_part
    // resolves an FK part to the referenced row's key (find_fk_primary_key), so
    // that referenced value can itself be a UUID — the same association-
    // table shape as StockEntry/Person (test_composite_pk_models.h), widened to a
    // UUID-keyed owner.
    struct UuidRefEntry {
        [[= primary_part]][[= storm::fk<>]] UuidOwner owner;
        [[= primary_part]] int                        sku{};
        int                                           qty{};
    };

    constexpr std::string_view kDocUuid    = "11111111-1111-4111-8111-111111111111";
    constexpr std::string_view kDocUuid2   = "22222222-2222-4222-8222-222222222222";
    constexpr std::string_view kOwnerUuid  = "33333333-3333-4333-8333-333333333333";
    constexpr std::string_view kOwnerUuid2 = "44444444-4444-4444-8444-444444444444";

    constexpr std::string_view kExpectedError =
            "Primary key UUID must be explicitly set; auto-generation not allowed for PKs";

    // Shared by every fixture below — avoids repeating the same three-line body
    // per model.
    template <typename Model, typename ConnType> auto count_rows() -> int {
        auto result = (SelectStatement<Model, ConnType>{}).all().count().execute();
        return result.has_value() ? static_cast<int>(result.value()) : -1;
    }

} // namespace

namespace fields {

    struct UuidKeyedDocT;
    consteval {
        std::meta::define_aggregate(^^UuidKeyedDocT, field_specs_for(^^UuidKeyedDoc));
    }
    inline constexpr UuidKeyedDocT UuidKeyedDoc{};

    struct UuidCompositeT;
    consteval {
        std::meta::define_aggregate(^^UuidCompositeT, field_specs_for(^^UuidComposite));
    }
    inline constexpr UuidCompositeT UuidComposite{};

} // namespace fields

// ============================================================================
// Plain single UUID PK
// ============================================================================

namespace {

    template <typename ConnType> class UuidKeyedDocTest : public StormTestFixture<UuidKeyedDoc, ConnType> {
      public:
        // Two rows, not one: a test that only checks "the row count dropped to
        // zero" or "the one row changed" would pass identically against a WHERE
        // clause that matched every row. Seeding a second, untouched row lets
        // every round-trip assertion below prove the UUID key is load-bearing in
        // the emitted predicate, not just present.
        auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
            ASSERT_TRUE((InsertStatement<UuidKeyedDoc, ConnType>{})
                                .template query<ReturnId::No>(UuidKeyedDoc{.id = UUID{kDocUuid}, .title = "Original"})
                                .execute()
                                .has_value());
            ASSERT_TRUE((InsertStatement<UuidKeyedDoc, ConnType>{})
                                .template query<ReturnId::No>(UuidKeyedDoc{.id = UUID{kDocUuid2}, .title = "Other"})
                                .execute()
                                .has_value());
        }

        static auto title_of(std::string_view id) -> std::string {
            auto rows =
                    (SelectStatement<UuidKeyedDoc, ConnType>{}).where(fields::UuidKeyedDoc.id == UUID{id}).execute();
            if (!rows.has_value() || rows.value().empty()) {
                return "<missing>";
            }
            return rows.value().begin()->title;
        }
    };

} // namespace
TYPED_TEST_SUITE(UuidKeyedDocTest, DatabaseTypes);

// The core defect: an unset key must ERROR, not silently match zero rows.
TYPED_TEST(UuidKeyedDocTest, UpdateWithEmptyKeyIsRejected) {
    auto result = (UpdateStatement<UuidKeyedDoc, TypeParam>{})
                          .query(UuidKeyedDoc{.id = UUID{}, .title = "changed"})
                          .execute();
    ASSERT_FALSE(result.has_value()) << "an unset UUID key must not silently no-op";
    EXPECT_EQ(result.error().message(), kExpectedError) << "must match INSERT's own guard message";
    EXPECT_EQ(this->title_of(kDocUuid), "Original") << "the existing row must be untouched";
}

TYPED_TEST(UuidKeyedDocTest, EraseWithEmptyKeyIsRejected) {
    auto result = (EraseStatement<UuidKeyedDoc, TypeParam>{})
                          .query(UuidKeyedDoc{.id = UUID{}, .title = "irrelevant"})
                          .execute();
    ASSERT_FALSE(result.has_value()) << "an unset UUID key must not silently delete a random row";
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ((count_rows<UuidKeyedDoc, TypeParam>()), 2) << "both existing rows must survive";
}

// A SET key still round-trips correctly, and touches ONLY the matching row.
TYPED_TEST(UuidKeyedDocTest, UpdateWithSetKeyUpdatesTheMatchingRow) {
    auto result = (UpdateStatement<UuidKeyedDoc, TypeParam>{})
                          .query(UuidKeyedDoc{.id = UUID{kDocUuid}, .title = "Updated"})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(this->title_of(kDocUuid), "Updated");
    EXPECT_EQ(this->title_of(kDocUuid2), "Other") << "the other row's key must discriminate it from the update";
}

TYPED_TEST(UuidKeyedDocTest, EraseWithSetKeyDeletesTheMatchingRow) {
    auto result = (EraseStatement<UuidKeyedDoc, TypeParam>{})
                          .query(UuidKeyedDoc{.id = UUID{kDocUuid}, .title = "irrelevant"})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ((count_rows<UuidKeyedDoc, TypeParam>()), 1);
    EXPECT_EQ(this->title_of(kDocUuid2), "Other") << "the other row's key must discriminate it from the delete";
}

// Batch UPDATE/DELETE route through the same bind_pk_values_impl fold as the
// single-row path; an empty key anywhere in the batch must reject the WHOLE
// call, not silently skip just that element.
//
// Batch UPDATE (update.cppm execute_batch) wraps every row in one
// TransactionGuard and returns as soon as one reset_bind_execute fails —
// without calling commit() — so an EARLIER successful row in the same batch
// must be rolled back too.
TYPED_TEST(UuidKeyedDocTest, BatchUpdateWithOneEmptyKeyRollsBackTheEarlierRow) {
    const std::vector<UuidKeyedDoc> updates{
            {.id = UUID{kDocUuid}, .title = "Updated"},
            {.id = UUID{}, .title = "Bad"},
    };
    auto result = (UpdateStatement<UuidKeyedDoc, TypeParam>{}).query(std::span<const UuidKeyedDoc>(updates)).execute();
    ASSERT_FALSE(result.has_value()) << "any empty key in the batch must be rejected";
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ(this->title_of(kDocUuid), "Original")
            << "the earlier successful UPDATE in the same batch must be rolled back";
}

// Batch DELETE (erase.cppm bind_pks_and_execute) binds every row's key into
// ONE bulk "IN" statement before calling execute() once — a bind failure on
// any row means the statement never runs at all, so nothing is deleted.
TYPED_TEST(UuidKeyedDocTest, BatchEraseWithOneEmptyKeyDeletesNothing) {
    const std::vector<UuidKeyedDoc> targets{
            {.id = UUID{kDocUuid}, .title = "irrelevant"},
            {.id = UUID{}, .title = "irrelevant"},
    };
    auto result = (EraseStatement<UuidKeyedDoc, TypeParam>{}).query(std::span<const UuidKeyedDoc>(targets)).execute();
    ASSERT_FALSE(result.has_value()) << "any empty key in the batch must be rejected";
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ((count_rows<UuidKeyedDoc, TypeParam>()), 2) << "the bulk DELETE must not have executed at all";
}

// ============================================================================
// Composite key with a UUID part
// ============================================================================

namespace {

    template <typename ConnType> class UuidCompositeTest : public StormTestFixture<UuidComposite, ConnType> {
      public:
        // The second row shares `version` with the first but differs in `doc_id`
        // — the only case that proves the UUID part itself discriminates in the
        // emitted predicate, rather than the int part alone doing all the work
        // (which PartialKeyMatchUpdatesNothing below already covers separately).
        auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
            ASSERT_TRUE((InsertStatement<UuidComposite, ConnType>{})
                                .query(UuidComposite{.doc_id = UUID{kDocUuid}, .version = 1, .content = "Original"})
                                .execute()
                                .has_value());
            ASSERT_TRUE((InsertStatement<UuidComposite, ConnType>{})
                                .query(UuidComposite{.doc_id = UUID{kDocUuid2}, .version = 1, .content = "Other doc"})
                                .execute()
                                .has_value());
        }

        static auto content_of(std::string_view doc_id, std::int64_t version) -> std::string {
            auto rows = (SelectStatement<UuidComposite, ConnType>{})
                                .where(fields::UuidComposite.doc_id == UUID{doc_id} &&
                                       fields::UuidComposite.version == version)
                                .execute();
            if (!rows.has_value() || rows.value().empty()) {
                return "<missing>";
            }
            return rows.value().begin()->content;
        }
    };

} // namespace
TYPED_TEST_SUITE(UuidCompositeTest, DatabaseTypes);

TYPED_TEST(UuidCompositeTest, UpdateWithEmptyUuidPartIsRejected) {
    auto result = (UpdateStatement<UuidComposite, TypeParam>{})
                          .query(UuidComposite{.doc_id = UUID{}, .version = 1, .content = "changed"})
                          .execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ(this->content_of(kDocUuid, 1), "Original");
}

TYPED_TEST(UuidCompositeTest, EraseWithEmptyUuidPartIsRejected) {
    auto result = (EraseStatement<UuidComposite, TypeParam>{})
                          .query(UuidComposite{.doc_id = UUID{}, .version = 1, .content = "irrelevant"})
                          .execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ((count_rows<UuidComposite, TypeParam>()), 2);
}

// The other row shares `version` — only the UUID part differs — so this also
// proves the UUID part is load-bearing in the emitted predicate.
TYPED_TEST(UuidCompositeTest, UpdateWithSetKeyUpdatesTheMatchingRow) {
    auto result = (UpdateStatement<UuidComposite, TypeParam>{})
                          .query(UuidComposite{.doc_id = UUID{kDocUuid}, .version = 1, .content = "Updated"})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(this->content_of(kDocUuid, 1), "Updated");
    EXPECT_EQ(this->content_of(kDocUuid2, 1), "Other doc") << "the other row's doc_id must discriminate it";
}

TYPED_TEST(UuidCompositeTest, EraseWithSetKeyDeletesTheMatchingRow) {
    auto result = (EraseStatement<UuidComposite, TypeParam>{})
                          .query(UuidComposite{.doc_id = UUID{kDocUuid}, .version = 1, .content = "irrelevant"})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ((count_rows<UuidComposite, TypeParam>()), 1);
    EXPECT_EQ(this->content_of(kDocUuid2, 1), "Other doc") << "the other row's doc_id must discriminate it";
}

// A mismatched second (int) part must not match — proves the UUID part alone
// isn't treated as the whole key.
TYPED_TEST(UuidCompositeTest, PartialKeyMatchUpdatesNothing) {
    auto result = (UpdateStatement<UuidComposite, TypeParam>{})
                          .query(UuidComposite{.doc_id = UUID{kDocUuid}, .version = 99, .content = "changed"})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(this->content_of(kDocUuid, 1), "Original") << "a different version part must not match";
}

// ============================================================================
// FK part whose target has a UUID key
// ============================================================================

namespace {

    template <typename ConnType> class UuidRefEntryTest : public StormTestFixture<UuidRefEntry, ConnType> {
      public:
        // The FK target table must exist before the referencing table.
        auto on_setup(const std::shared_ptr<ConnType>& conn) -> void override {
            ASSERT_TRUE((storm::test::ensure_tables<ConnType, UuidOwner>(conn))) << "Failed to create UuidOwner";
            StormTestFixture<UuidRefEntry, ConnType>::on_setup(conn);
        }

        // A second owner + row with the SAME sku proves the FK part's UUID value
        // itself discriminates in the emitted predicate, not just sku alone.
        auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
            ASSERT_TRUE((InsertStatement<UuidOwner, ConnType>{})
                                .template query<ReturnId::No>(UuidOwner{.id = UUID{kOwnerUuid}, .name = "Owner"})
                                .execute()
                                .has_value());
            ASSERT_TRUE((InsertStatement<UuidOwner, ConnType>{})
                                .template query<ReturnId::No>(UuidOwner{.id = UUID{kOwnerUuid2}, .name = "Other owner"})
                                .execute()
                                .has_value());

            ASSERT_TRUE((InsertStatement<UuidRefEntry, ConnType>{})
                                .query(UuidRefEntry{.owner = {.id = UUID{kOwnerUuid}}, .sku = 10, .qty = 5})
                                .execute()
                                .has_value());
            ASSERT_TRUE((InsertStatement<UuidRefEntry, ConnType>{})
                                .query(UuidRefEntry{.owner = {.id = UUID{kOwnerUuid2}}, .sku = 10, .qty = 7})
                                .execute()
                                .has_value());
        }

        // Selects every row and matches the key in C++ — filtering on the FK member
        // itself is out of scope here (see the identical rationale in
        // test_composite_pk_crud_body.h's StockEntryTest::qty_of).
        static auto qty_of(std::string_view owner_id, int sku) -> int {
            auto rows = (SelectStatement<UuidRefEntry, ConnType>{}).all().execute();
            if (!rows.has_value()) {
                return -1;
            }
            for (const UuidRefEntry& row : rows.value()) {
                if (row.owner.id.value == owner_id && row.sku == sku) {
                    return row.qty;
                }
            }
            return -1;
        }
    };

} // namespace
TYPED_TEST_SUITE(UuidRefEntryTest, DatabaseTypes);

TYPED_TEST(UuidRefEntryTest, UpdateWithEmptyOwnerKeyIsRejected) {
    auto result = (UpdateStatement<UuidRefEntry, TypeParam>{})
                          .query(UuidRefEntry{.owner = {.id = UUID{}}, .sku = 10, .qty = 900})
                          .execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ(this->qty_of(kOwnerUuid, 10), 5);
}

TYPED_TEST(UuidRefEntryTest, EraseWithEmptyOwnerKeyIsRejected) {
    auto result = (EraseStatement<UuidRefEntry, TypeParam>{})
                          .query(UuidRefEntry{.owner = {.id = UUID{}}, .sku = 10})
                          .execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ((count_rows<UuidRefEntry, TypeParam>()), 2);
}

// The other row shares `sku` — only the FK part's owner UUID differs — so
// this proves that value reaches the owner_id placeholder rather than some
// other slot.
TYPED_TEST(UuidRefEntryTest, UpdateWithSetOwnerKeyUpdatesTheMatchingRow) {
    auto result = (UpdateStatement<UuidRefEntry, TypeParam>{})
                          .query(UuidRefEntry{.owner = {.id = UUID{kOwnerUuid}}, .sku = 10, .qty = 900})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(this->qty_of(kOwnerUuid, 10), 900);
    EXPECT_EQ(this->qty_of(kOwnerUuid2, 10), 7) << "the other owner's row must be untouched";
}

TYPED_TEST(UuidRefEntryTest, EraseWithSetOwnerKeyDeletesTheMatchingRow) {
    auto result = (EraseStatement<UuidRefEntry, TypeParam>{})
                          .query(UuidRefEntry{.owner = {.id = UUID{kOwnerUuid}}, .sku = 10})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ((count_rows<UuidRefEntry, TypeParam>()), 1);
    EXPECT_EQ(this->qty_of(kOwnerUuid2, 10), 7) << "the other owner's row must survive";
}

// #608: INSERT of a composite key whose FK part targets a UUID key must
// reject an unset owner, not fabricate a random one that only fails later
// against the FOREIGN KEY constraint (and not at all if the connection
// doesn't enforce it).
TYPED_TEST(UuidRefEntryTest, InsertWithEmptyOwnerKeyIsRejected) {
    auto result = (InsertStatement<UuidRefEntry, TypeParam>{})
                          .query(UuidRefEntry{.owner = {.id = UUID{}}, .sku = 99, .qty = 1})
                          .execute();
    ASSERT_FALSE(result.has_value()) << "an unset owner UUID must not silently fabricate a foreign key";
    EXPECT_EQ(result.error().message(), kExpectedError) << "must match UPDATE/DELETE's own guard message (#573)";
    EXPECT_EQ((count_rows<UuidRefEntry, TypeParam>()), 2) << "the rejected INSERT must not have written a row";
}

// A SET owner key still round-trips correctly, proving the guard doesn't
// reject a genuinely valid FK-part value.
TYPED_TEST(UuidRefEntryTest, InsertWithSetOwnerKeyInsertsTheRow) {
    auto result = (InsertStatement<UuidRefEntry, TypeParam>{})
                          .query(UuidRefEntry{.owner = {.id = UUID{kOwnerUuid}}, .sku = 20, .qty = 3})
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(this->qty_of(kOwnerUuid, 20), 3);
    EXPECT_EQ((count_rows<UuidRefEntry, TypeParam>()), 3);
}

// Bulk INSERT (insert.cppm's std::span overload) binds every row inside one
// TransactionGuard and rolls back on the first bind failure (same shape as
// BatchUpdateWithOneEmptyKeyRollsBackTheEarlierRow above) — an empty owner
// key anywhere in the batch must undo an earlier, otherwise-valid row in the
// same call, not just skip the bad one.
TYPED_TEST(UuidRefEntryTest, BatchInsertWithOneEmptyOwnerKeyRollsBackTheEarlierRow) {
    const std::vector<UuidRefEntry> inserts{
            {.owner = {.id = UUID{kOwnerUuid}}, .sku = 30, .qty = 9},
            {.owner = {.id = UUID{}}, .sku = 40, .qty = 1},
    };
    auto result = (InsertStatement<UuidRefEntry, TypeParam>{}).query(std::span<const UuidRefEntry>(inserts)).execute();
    ASSERT_FALSE(result.has_value()) << "any empty owner key in the batch must be rejected";
    EXPECT_EQ(result.error().message(), kExpectedError);
    EXPECT_EQ((count_rows<UuidRefEntry, TypeParam>()), 2)
            << "the earlier successful INSERT in the same batch must be rolled back";
}

// NOLINTEND(readability-implicit-bool-conversion)
