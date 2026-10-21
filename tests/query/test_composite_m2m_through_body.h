#pragma once

// Shared test body for test_composite_m2m_through_sqlite.cpp / test_composite_m2m_through_pg.cpp — the two
// single-backend TUs of a compile-time TU split (see test_db_helpers.h,
// DatabaseTypesSqliteHalf/DatabaseTypesPgHalf). Splitting a 2-backend TU into
// two lets ninja compile them in parallel instead of serially instantiating
// both backends in one TU; keeping the body here (instead of duplicating it
// into both .cpp files) removes the risk of the two halves silently drifting.
//
// The includer must #define STORM_SPLIT_TYPES / STORM_SPLIT_TYPE_NAMES to one
// backend's ::testing::Types<> alias / NameGenerator before #include-ing this
// file, and #undef both afterward. Never include this file directly.
#if !defined(STORM_SPLIT_TYPES) || !defined(STORM_SPLIT_TYPE_NAMES)
#error "test_composite_m2m_through_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

import storm_db_default_connection;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_db_postgresql;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954 — Person
#include "test_fixture.h"

// Must follow the shared model headers: the composite-PK models name storm:: annotations.
#include "crud/test_composite_pk_models.h" // NOSONAR cpp:S954
// Pupil/Course/Enrollment — the single-PK through model, used as the
// byte-identity anchor below.
#include "test_m2m_models.h" // NOSONAR cpp:S954
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// ── #536: many_to_many_through<> over a composite primary key ────────────────
//
// A through model is a REAL user-declared model, so its junction columns are
// ordinary composite-FK columns of an ordinary table: the FK rule names them
// "<member>_<part>" with the target part's BARE identifier. Storm's synthetic
// auto-junction is a different table with a different owner — the junction rule
// routes each part through append_column_name, so an FK part gains "_id".
//
// The two rules agree for a single-column PK and for a composite key of plain
// columns; they DIVERGE the moment a PK part is itself an FK. The m2m query
// path applied the junction rule to both kinds of junction, so a through model
// over such a key asked for "t2.entry_warehouse_id" while its real column is
// "entry_warehouse" — "no such column", and the eager load failed outright.
//
// These are execution tests, not SQL-text tests, and they run on both backends:
// the failure is a column that does not exist, which only a live query surfaces.
// StormTestFixture creates every listed model's table before the body runs, so
// a through model whose DDL and query path disagree fails here on SQLite and
// PostgreSQL alike.

namespace {

template <typename ConnType>
class ThroughCompositeOwnerTest : public StormTestFixture<RegionEntry, ConnType, ThroughTopic, RegionEntryLink> {};

template <typename ConnType>
class ThroughCompositeRelatedTest : public StormTestFixture<ShelfOwner, ConnType, ShelfCode, ShelfCodeLink> {};

// Person is the FK part's target, so it must exist before FkPartEntry.
template <typename ConnType>
class ThroughFkPartTest : public StormTestFixture<Person, ConnType, ThroughTopic, FkPartEntry, FkPartLink> {};

template <typename ConnType>
class ThroughBothSidesTest : public StormTestFixture<Person, ConnType, ShelfCode, BothSidesOwner, BothSidesLink> {};

// Two topics, 'audit' then 'review' — autoincrement, so ids are 1 and 2.
template <typename ConnType> auto seed_topics() -> void {
    ASSERT_TRUE(
        (InsertStatement<ThroughTopic, ConnType>{}).query(ThroughTopic{.label = "audit"}).execute().has_value());
    ASSERT_TRUE(
        (InsertStatement<ThroughTopic, ConnType>{}).query(ThroughTopic{.label = "review"}).execute().has_value());
}

// Shared seeding for the FK-part suite: topics, one depot Person (the FK
// part's target), and entry sku 10 linked to BOTH topics. `with_decoy` adds
// entry sku 20 — sharing the FK part (warehouse == 1) and linked to 'review'
// only — which is what makes a stitch keyed on the FK part alone observable.
// The aggregate test omits it so its COUNT has one unambiguous expected value.
template <typename ConnType> auto seed_fk_part_entries(bool with_decoy) -> void {
    seed_topics<ConnType>();

    ASSERT_TRUE(
        (InsertStatement<Person, ConnType>{}).query(Person{.name = "depot-1", .age = 30}).execute().has_value());

    ASSERT_TRUE((InsertStatement<FkPartEntry, ConnType>{})
                    .query(FkPartEntry{.warehouse = {.id = 1}, .sku = 10, .title = "ten"})
                    .execute()
                    .has_value());

    ASSERT_TRUE((InsertStatement<FkPartLink, ConnType>{})
                    .query(FkPartLink{.entry = {.warehouse = {.id = 1}, .sku = 10}, .topic = {.id = 1}})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<FkPartLink, ConnType>{})
                    .query(FkPartLink{.entry = {.warehouse = {.id = 1}, .sku = 10}, .topic = {.id = 2}})
                    .execute()
                    .has_value());

    if (with_decoy) {
        ASSERT_TRUE((InsertStatement<FkPartEntry, ConnType>{})
                        .query(FkPartEntry{.warehouse = {.id = 1}, .sku = 20, .title = "twenty"})
                        .execute()
                        .has_value());
        ASSERT_TRUE((InsertStatement<FkPartLink, ConnType>{})
                        .query(FkPartLink{.entry = {.warehouse = {.id = 1}, .sku = 20}, .topic = {.id = 2}})
                        .execute()
                        .has_value());
    }
}

} // namespace
TYPED_TEST_SUITE(ThroughCompositeOwnerTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(ThroughCompositeRelatedTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(ThroughFkPartTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(ThroughBothSidesTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Composite (2-part) OWNER through an explicit junction model. Both parts are
// plain columns, so the FK and junction rules still agree here — this is the
// control that proves the through path handles a multi-column key at all,
// independently of the FK-part divergence below.
//
// The two entries share their FIRST PK part (region == 1) and differ only in
// `code`, so a stitch that keyed on the first part alone would give both
// entries both topics.
TYPED_TEST(ThroughCompositeOwnerTest, StitchesOnFullCompositeOwnerKey) {
    seed_topics<TypeParam>();

    ASSERT_TRUE((InsertStatement<RegionEntry, TypeParam>{})
                    .query(RegionEntry{.region = 1, .code = "A", .title = "alpha"})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<RegionEntry, TypeParam>{})
                    .query(RegionEntry{.region = 1, .code = "B", .title = "bravo"})
                    .execute()
                    .has_value());

    // The junction is a model — its rows are plain Storm inserts, no raw SQL.
    // A/audit, A/review, B/review.
    ASSERT_TRUE((InsertStatement<RegionEntryLink, TypeParam>{})
                    .query(RegionEntryLink{.entry = {.region = 1, .code = "A"}, .topic = {.id = 1}, .note = "n1"})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<RegionEntryLink, TypeParam>{})
                    .query(RegionEntryLink{.entry = {.region = 1, .code = "A"}, .topic = {.id = 2}, .note = "n2"})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<RegionEntryLink, TypeParam>{})
                    .query(RegionEntryLink{.entry = {.region = 1, .code = "B"}, .topic = {.id = 2}, .note = "n3"})
                    .execute()
                    .has_value());

    const auto results =
        (SelectStatement<RegionEntry, TypeParam>{}).all().template join<fields::RegionEntry.topics>().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 2U);

    for (const auto &entry : *results) {
        if (entry.code == "A") {
            EXPECT_EQ(entry.topics.size(), 2U) << "entry A must receive exactly its own two topics";
        } else {
            EXPECT_EQ(entry.code, "B");
            ASSERT_EQ(entry.topics.size(), 1U) << "entry B must not inherit A's topics via a partial-key stitch";
            EXPECT_EQ(entry.topics.begin()->label, "review");
        }
    }
}

// LEFT keeps a composite through-owner with no junction rows; INNER drops it.
TYPED_TEST(ThroughCompositeOwnerTest, LeftJoinKeepsCompositeOwnerWithNoTopics) {
    ASSERT_TRUE((InsertStatement<RegionEntry, TypeParam>{})
                    .query(RegionEntry{.region = 9, .code = "Z", .title = "lonely"})
                    .execute()
                    .has_value());

    const auto left =
        (SelectStatement<RegionEntry, TypeParam>{}).all().template left_join<fields::RegionEntry.topics>().execute();
    ASSERT_TRUE(left.has_value()) << left.error().message();
    ASSERT_EQ(left->size(), 1U);
    EXPECT_TRUE(left->begin()->topics.empty());

    const auto inner =
        (SelectStatement<RegionEntry, TypeParam>{}).all().template join<fields::RegionEntry.topics>().execute();
    ASSERT_TRUE(inner.has_value()) << inner.error().message();
    EXPECT_EQ(inner->size(), 0U) << "INNER drops a through-owner with zero junction rows";
}

// Single-PK OWNER, composite (2-part) RELATED — the mirror direction. The Q2
// junction⟷related ON clause must AND-join one equality per related PK part;
// matching only the first would attach both codes to the owner linked to one.
TYPED_TEST(ThroughCompositeRelatedTest, JoinsRelatedOnFullCompositeKey) {
    ASSERT_TRUE((InsertStatement<ShelfCode, TypeParam>{})
                    .query(ShelfCode{.aisle = 4, .code = "C1", .label = "first"})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<ShelfCode, TypeParam>{})
                    .query(ShelfCode{.aisle = 4, .code = "C2", .label = "second"})
                    .execute()
                    .has_value());

    ASSERT_TRUE((InsertStatement<ShelfOwner, TypeParam>{}).query(ShelfOwner{.name = "owner-1"}).execute().has_value());

    // Link the owner to C1 only. The two codes share aisle == 4, so a related-side
    // comparison that stops after the first part would also attach C2.
    ASSERT_TRUE((InsertStatement<ShelfCodeLink, TypeParam>{})
                    .query(ShelfCodeLink{.owner = {.id = 1}, .code = {.aisle = 4, .code = "C1"}})
                    .execute()
                    .has_value());

    const auto results =
        (SelectStatement<ShelfOwner, TypeParam>{}).all().template join<fields::ShelfOwner.codes>().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 1U);
    ASSERT_EQ(results->begin()->codes.size(), 1U) << "related side must match on BOTH parts, not just `aisle`";
    EXPECT_EQ(results->begin()->codes.begin()->label, "first");
}

// Byte-identity anchor for the SHARED branch. The #536 fix routes composite
// junction columns through a JunctionNaming switch, but that switch sits INSIDE
// the `has_composite_pk_` gate, so a single-PK side and an auto-junction reach
// the same instruction path they always did. Every other test here exercises the
// NEW branch; this one pins the OLD text, so a future refactor that lifts the
// Naming switch above the composite gate fails loudly instead of silently
// re-spelling every pre-#536 junction.
//
// Pupil/Course/Enrollment is the canonical single-PK through model from #203.
TYPED_TEST(ThroughCompositeOwnerTest, SinglePkThroughSqlIsUnchanged) {
    const auto sql = (SelectStatement<Pupil, TypeParam>{}).all().template join<fields::Pupil.courses>().sql();
    EXPECT_TRUE(sql.contains("FROM Enrollment t2 INNER JOIN Course t3 ON t2.course_id = t3.id")) << sql;
    EXPECT_TRUE(sql.contains("WHERE t2.pupil_id IN (SELECT id FROM Pupil)")) << sql;
}

// Same anchor for an AUTO junction over a COMPOSITE side — the branch the fix
// deliberately left alone. LedgerWithTags has a 3-part key, so this pins the
// "<Side>_<part>" spelling (and the `_id` an FK part would gain) against the
// Through branch bleeding into it.
TYPED_TEST(ThroughCompositeOwnerTest, AutoJunctionCompositeSqlIsUnchanged) {
    const auto sql =
        (SelectStatement<LedgerWithTags, TypeParam>{}).all().template join<fields::LedgerWithTags.tags>().sql();
    EXPECT_TRUE(sql.contains("t2.LedgerWithTags_region, t2.LedgerWithTags_account, t2.LedgerWithTags_period")) << sql;
    EXPECT_TRUE(sql.contains("ON t2.LedgerTag_id = t3.id")) << sql;
}

// THE divergence case: the owner's composite key has a part that is itself an
// FK. The through model's real column is "entry_warehouse" (FK rule, bare part
// identifier); the junction rule would ask for "entry_warehouse_id". Before the
// fix this failed with "no such column: t2.entry_warehouse_id" — so the first
// assertion below is the regression guard, and the stitch assertions prove the
// FK part is also carried correctly through the key comparison.
TYPED_TEST(ThroughFkPartTest, StitchesOnCompositeKeyWithForeignKeyPart) {
    // With the decoy: sku 20 shares the FK part (warehouse == 1) with sku 10, so
    // a stitch keyed on that part alone hands sku 20 both of sku 10's topics.
    seed_fk_part_entries<TypeParam>(/*with_decoy=*/true);

    const auto results =
        (SelectStatement<FkPartEntry, TypeParam>{}).all().template join<fields::FkPartEntry.topics>().execute();
    // The regression guard: before the fix this errored rather than returning rows.
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 2U);

    for (const auto &entry : *results) {
        if (entry.sku == 10) {
            EXPECT_EQ(entry.topics.size(), 2U) << "sku 10 must receive exactly its own two topics";
        } else {
            EXPECT_EQ(entry.sku, 20);
            ASSERT_EQ(entry.topics.size(), 1U)
                << "sku 20 must not inherit sku 10's topics via a stitch keyed on the FK part alone";
            EXPECT_EQ(entry.topics.begin()->label, "review");
        }
    }
}

// The aggregate / complete-SQL path is assembled from DIFFERENT fragments than
// the two-query select path — `append_side_join_names` renders both the model
// column and the junction column per PK part into one static buffer, and it is
// the third site the #536 fix rewires. The select tests above instantiate that
// buffer (so its SIZE is proven) but never execute a query through it. This does.
TYPED_TEST(ThroughFkPartTest, AggregateOverCompositeThroughJunction) {
    // No decoy: exactly one entry linked to exactly two topics, so the COUNT has
    // one unambiguous expected value.
    seed_fk_part_entries<TypeParam>(/*with_decoy=*/false);

    const auto total =
        (SelectStatement<FkPartEntry, TypeParam>{}).all().template join<fields::FkPartEntry.topics>().count().execute();
    ASSERT_TRUE(total.has_value()) << total.error().message();
    EXPECT_EQ(*total, 2) << "COUNT over the composite through junction must see both linked topics";
}

// Both sides composite AND the owner's key has an FK part — the widest through
// junction. Owner contributes 2 columns (owner_depot, owner_slot), related
// contributes 2 more (code_aisle, code_code); every one of the four must be
// named by the FK rule.
TYPED_TEST(ThroughBothSidesTest, JoinsWhenBothSidesAreComposite) {
    ASSERT_TRUE(
        (InsertStatement<Person, TypeParam>{}).query(Person{.name = "depot-1", .age = 40}).execute().has_value());

    ASSERT_TRUE((InsertStatement<ShelfCode, TypeParam>{})
                    .query(ShelfCode{.aisle = 7, .code = "D1", .label = "dee-one"})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<ShelfCode, TypeParam>{})
                    .query(ShelfCode{.aisle = 7, .code = "D2", .label = "dee-two"})
                    .execute()
                    .has_value());

    // Two owners sharing the FK part (depot == 1) and differing in `slot`.
    ASSERT_TRUE((InsertStatement<BothSidesOwner, TypeParam>{})
                    .query(BothSidesOwner{.depot = {.id = 1}, .slot = 1, .name = "slot-one"})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<BothSidesOwner, TypeParam>{})
                    .query(BothSidesOwner{.depot = {.id = 1}, .slot = 2, .name = "slot-two"})
                    .execute()
                    .has_value());

    // slot 1 → D1 and D2; slot 2 → D2 only.
    ASSERT_TRUE((InsertStatement<BothSidesLink, TypeParam>{})
                    .query(BothSidesLink{.owner = {.depot = {.id = 1}, .slot = 1}, .code = {.aisle = 7, .code = "D1"}})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<BothSidesLink, TypeParam>{})
                    .query(BothSidesLink{.owner = {.depot = {.id = 1}, .slot = 1}, .code = {.aisle = 7, .code = "D2"}})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<BothSidesLink, TypeParam>{})
                    .query(BothSidesLink{.owner = {.depot = {.id = 1}, .slot = 2}, .code = {.aisle = 7, .code = "D2"}})
                    .execute()
                    .has_value());

    const auto results =
        (SelectStatement<BothSidesOwner, TypeParam>{}).all().template join<fields::BothSidesOwner.codes>().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 2U);

    for (const auto &owner : *results) {
        if (owner.slot == 1) {
            EXPECT_EQ(owner.codes.size(), 2U) << "slot 1 must receive exactly its own two codes";
        } else {
            EXPECT_EQ(owner.slot, 2);
            ASSERT_EQ(owner.codes.size(), 1U) << "slot 2 must not inherit slot 1's codes";
            EXPECT_EQ(owner.codes.begin()->label, "dee-two");
        }
    }
}
