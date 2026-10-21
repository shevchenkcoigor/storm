#pragma once

// Shared test body for test_composite_m2m_junction_sqlite.cpp / test_composite_m2m_junction_pg.cpp — the two
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
#error "test_composite_m2m_junction_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

import storm;
import std;

using storm::db::DefaultConnection;

#include "../../shared/models/person.h" // NOSONAR cpp:S954 — Person, via test_composite_pk_models.h's FK members
#include "test_fixture.h"

// Must follow the shared model headers: the composite-PK models name storm:: annotations.
#include "crud/test_composite_pk_models.h" // NOSONAR cpp:S954
using storm::QuerySet;

// ── #504 Task 9: junction DDL is genuinely ACCEPTED by both live backends ────
//
// A DDL text assertion proves the string we generate, not that a database will
// take it. This distinction is load-bearing here: the pre-fix junction DDL
// referenced a nonexistent "id" column on a composite-PK side, which SQLite
// silently accepts (it does not validate FK target columns at CREATE TABLE
// time) while PostgreSQL rejects outright. So the SQLite-only signal was
// actively misleading — these fixtures create the real tables on whichever
// backend the TYPED_TEST is running, and StormTestFixture's on_setup already
// ASSERTs that table creation succeeded. A junction whose FK clause names a
// nonexistent column, or whose column count disagrees with its PK clause, fails
// here on PG before a single test body runs.

namespace {

template <typename ConnType>
class CompositeOwnerJunctionCreateTest : public StormTestFixture<LedgerWithTags, ConnType, LedgerTag> {};

template <typename ConnType>
class CompositeRelatedJunctionCreateTest : public StormTestFixture<TagRegistry, ConnType, CatalogEntry> {};

template <typename ConnType>
class CompositeBothSidesJunctionCreateTest : public StormTestFixture<ShelfAssignment, ConnType, StorageBin> {};

template <typename ConnType>
class MultiRelationCompositeOwnerTest : public StormTestFixture<ShelfAssignment, ConnType, StorageBin, LedgerTag> {};

// ── Shared seeding ───────────────────────────────────────────────────────
// Each helper plants the SAME decoy shape its suite depends on: rows that
// agree on one PK part and differ in another, so a comparison that stops
// early conflates them. Sharing the seeding keeps every test in a suite
// agreeing on which pairs are linked — a per-test copy that drifted would
// weaken the decoy silently rather than fail.

// Two entries sharing catalog_id == 7, differing in entry_no (3 vs 4).
template <typename ConnType> auto seed_catalog_entries() -> void {
    QuerySet<CatalogEntry, ConnType> entry_qs;
    ASSERT_TRUE(entry_qs.insert(CatalogEntry{.catalog_id = 7, .entry_no = 3, .title = "widget"}).execute().has_value());
    ASSERT_TRUE(entry_qs.insert(CatalogEntry{.catalog_id = 7, .entry_no = 4, .title = "gadget"}).execute().has_value());
}

// Junction column lists, paired with the VALUES tuples each test links.
constexpr std::string_view REGISTRY_ENTRY_COLS =
    "TagRegistry_CatalogEntry (TagRegistry_id, CatalogEntry_catalog_id, CatalogEntry_entry_no)";
constexpr std::string_view SHELF_TAG_COLS =
    "ShelfAssignment_LedgerTag (ShelfAssignment_warehouse_no, ShelfAssignment_shelf_code, LedgerTag_id)";
constexpr std::string_view SHELF_BIN_COLS =
    "ShelfAssignment_StorageBin (ShelfAssignment_warehouse_no, ShelfAssignment_shelf_code, "
    "StorageBin_aisle, StorageBin_bin_code, StorageBin_revision)";

// Seeds LedgerTag rows 1..n ('audit', then 'review'). LedgerTag is
// autoincrement, so ids are positional — the junction tuples reference them
// by the 1-based order inserted here.
auto seed_tags(const auto &conn, int count) -> void {
    for (const auto *label : std::array{"audit", "review"} | std::views::take(count)) {
        ASSERT_TRUE(conn->execute(std::format("INSERT INTO LedgerTag (label) VALUES ('{}')", label)).has_value());
    }
}

// Inserts each "(...)" tuple into the named junction. Junction rows are written
// as raw SQL throughout the m2m suites — Storm has no junction write path.
auto link_junction_rows(const auto &conn, std::string_view table_and_cols, auto rows) -> void {
    for (const auto *row : rows) {
        ASSERT_TRUE(conn->execute(std::format("INSERT INTO {} VALUES {}", table_and_cols, row)).has_value());
    }
}

// Two bins sharing aisle=4 and bin_code="B12", differing only in `revision` —
// the LAST part, so a key comparison that stops early conflates them.
template <typename ConnType> auto seed_bins() -> void {
    QuerySet<StorageBin, ConnType> bin_qs;
    ASSERT_TRUE(
        bin_qs.insert(StorageBin{.aisle = 4, .bin_code = "B12", .revision = 9, .capacity = 2.5}).execute().has_value());
    ASSERT_TRUE(bin_qs.insert(StorageBin{.aisle = 4, .bin_code = "B12", .revision = 10, .capacity = 3.5})
                    .execute()
                    .has_value());
}

// Shelf S-1, and optionally S-2 sharing its warehouse_no. Tests that count
// junction PAIRS seed only S-1, so the second shelf is opt-in rather than
// baked in — it would change their expected count.
template <typename ConnType> auto seed_shelves(bool with_second = true) -> void {
    QuerySet<ShelfAssignment, ConnType> shelf_qs;
    ASSERT_TRUE(
        shelf_qs.insert(ShelfAssignment{.warehouse_no = 2, .shelf_code = "S-1", .priority = 5}).execute().has_value());
    if (with_second) {
        ASSERT_TRUE(shelf_qs.insert(ShelfAssignment{.warehouse_no = 2, .shelf_code = "S-2", .priority = 6})
                        .execute()
                        .has_value());
    }
}

} // namespace
TYPED_TEST_SUITE(CompositeOwnerJunctionCreateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(CompositeRelatedJunctionCreateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(CompositeBothSidesJunctionCreateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(MultiRelationCompositeOwnerTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Composite OWNER (3-part, mixed types) + single-PK related. Before this task
// the emitted FK clause was "REFERENCES LedgerWithTags(id)" — LedgerWithTags has
// no "id" column, so this could not be created on PostgreSQL at all.
// End-to-end m2m eager load over a COMPOSITE-PK owner (#504 Task 9). This is the
// test the compile-time gate stood in for: it exercises the whole vertical slice
// at once — the widened junction DDL creates the table, raw INSERTs populate it
// through the N owner-key columns, and join<>().select() runs the two-query
// stitch, whose Q2 must select N owner columns, compare them as an N-column row
// value, and key the stitch map on all N. A disagreement at ANY of those points
// produces wrong stitches rather than an error, so the assertions check WHICH
// tags landed on WHICH ledger, not merely how many.
TYPED_TEST(CompositeOwnerJunctionCreateTest, M2MEagerLoadStitchesOnFullCompositeOwnerKey) {
    using LedgerQs = storm::QuerySet<LedgerWithTags, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    // Two ledgers sharing the FIRST PK part (region == 1) and differing only in
    // later parts. If the stitch keyed on region alone — the single-column
    // fallback the old code would have had to use — both ledgers would receive
    // both tags.
    LedgerQs ledger_qs;
    const auto ins_a =
        ledger_qs.insert(LedgerWithTags{.region = 1, .account = "acct-a", .period = 202601, .balance = 1.0}).execute();
    ASSERT_TRUE(ins_a.has_value());
    const auto ins_b =
        ledger_qs.insert(LedgerWithTags{.region = 1, .account = "acct-b", .period = 202602, .balance = 2.0}).execute();
    ASSERT_TRUE(ins_b.has_value());

    ASSERT_TRUE(conn->execute("INSERT INTO LedgerTag (label) VALUES ('audit')").has_value());
    ASSERT_TRUE(conn->execute("INSERT INTO LedgerTag (label) VALUES ('review')").has_value());

    // acct-a gets BOTH tags; acct-b gets only 'review'.
    for (const auto *row : {"(1, 'acct-a', 202601, 1)", "(1, 'acct-a', 202601, 2)", "(1, 'acct-b', 202602, 2)"}) {
        const auto link = conn->execute(
            std::format("INSERT INTO LedgerWithTags_LedgerTag "
                        "(LedgerWithTags_region, LedgerWithTags_account, LedgerWithTags_period, LedgerTag_id) VALUES "
                        "{}",
                        row));
        ASSERT_TRUE(link.has_value());
    }

    const auto results = ledger_qs.template join<fields::LedgerWithTags.tags>().select().execute();
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 2U);

    for (const auto &ledger : *results) {
        if (ledger.account == "acct-a") {
            EXPECT_EQ(ledger.tags.size(), 2U) << "acct-a must receive exactly its own two tags";
        } else {
            EXPECT_EQ(ledger.account, "acct-b");
            ASSERT_EQ(ledger.tags.size(), 1U) << "acct-b must NOT inherit acct-a's tags via a partial-key stitch";
            EXPECT_EQ(ledger.tags.begin()->label, "review");
        }
    }
}

// LEFT join keeps a composite-owner entity that has no junction rows at all;
// INNER drops it. Same post-stitch filter semantics as the single-PK path.
TYPED_TEST(CompositeOwnerJunctionCreateTest, LeftJoinKeepsCompositeOwnerWithNoTags) {
    using LedgerQs = storm::QuerySet<LedgerWithTags, TypeParam>;
    LedgerQs ledger_qs;
    const auto inserted =
        ledger_qs.insert(LedgerWithTags{.region = 9, .account = "lonely", .period = 202609, .balance = 0.0}).execute();
    ASSERT_TRUE(inserted.has_value());

    const auto left = ledger_qs.template left_join<fields::LedgerWithTags.tags>().select().execute();
    ASSERT_TRUE(left.has_value());
    ASSERT_EQ(left->size(), 1U);
    EXPECT_TRUE(left->begin()->tags.empty());

    const auto inner = ledger_qs.template join<fields::LedgerWithTags.tags>().select().execute();
    ASSERT_TRUE(inner.has_value());
    EXPECT_EQ(inner->size(), 0U) << "INNER drops a composite owner with zero relation rows";
}

TYPED_TEST(CompositeOwnerJunctionCreateTest, JunctionTableIsCreatedOnLiveBackend) {
    using LedgerQs = storm::QuerySet<LedgerWithTags, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();
    // Round-trip a junction row through raw SQL: the ORM has no junction write
    // path (existing m2m tests populate junctions this way too), so this is the
    // only way to prove the emitted columns actually exist and accept data.
    ASSERT_TRUE(conn->execute("INSERT INTO LedgerTag (label) VALUES ('audit')").has_value());
    QuerySet<LedgerWithTags, TypeParam> ledger_qs;
    const auto ledger_inserted =
        ledger_qs.insert(LedgerWithTags{.region = 1, .account = "acct-a", .period = 202601, .balance = 10.5}).execute();
    ASSERT_TRUE(ledger_inserted.has_value());
    auto link = conn->execute("INSERT INTO LedgerWithTags_LedgerTag "
                              "(LedgerWithTags_region, LedgerWithTags_account, LedgerWithTags_period, LedgerTag_id) "
                              "VALUES (1, 'acct-a', 202601, 1)");
    EXPECT_TRUE(link.has_value()) << "composite-owner junction must accept a row keyed on all 3 owner parts";
}

// Single-PK owner + composite (2-part) RELATED — the mirror direction.
TYPED_TEST(CompositeRelatedJunctionCreateTest, JunctionTableIsCreatedOnLiveBackend) {
    using RegistryQs = storm::QuerySet<TagRegistry, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();
    QuerySet<CatalogEntry, TypeParam> entry_qs;
    const auto entry_inserted =
        entry_qs.insert(CatalogEntry{.catalog_id = 7, .entry_no = 3, .title = "widget"}).execute();
    ASSERT_TRUE(entry_inserted.has_value());
    QuerySet<TagRegistry, TypeParam> registry_qs;
    const auto registry_inserted = registry_qs.insert(TagRegistry{.label = "featured"}).execute();
    ASSERT_TRUE(registry_inserted.has_value());
    auto link = conn->execute("INSERT INTO TagRegistry_CatalogEntry "
                              "(TagRegistry_id, CatalogEntry_catalog_id, CatalogEntry_entry_no) VALUES (1, 7, 3)");
    EXPECT_TRUE(link.has_value()) << "composite-related junction must accept a row keyed on both related parts";
}

// Review fix — the RELATED side of the m2m query path was never widened.
//
// Task 9's junction DDL widened BOTH sides, but only the OWNER side of the query
// path followed: Q2's junction⋈related ON clause and the aggregate path's second
// ON clause both still spelled a single "<Related>_id" junction column matched
// against RelatedBase::pk_name_. For a composite RELATED model there IS no
// "<Related>_id" column — the junction carries "<Related>_<part>" per part — so
// the query failed outright ("no such column: t2.CatalogEntry_id" / PG "column
// t2.catalogentry_id does not exist"). Even with the column present, matching one
// column against the FIRST part alone would join related rows that share that
// part but differ in a later one.
//
// This was invisible to the tests that shipped with Task 9 because they only ran
// raw INSERTs into the junction — M2MJoinStatement instantiates lazily, so the
// broken code was never even compiled. The tests below therefore run
// join<>().select() and join<>().count() for real on both backends.
TYPED_TEST(CompositeRelatedJunctionCreateTest, M2MEagerLoadStitchesOverCompositeRelatedKey) {
    using RegistryQs = storm::QuerySet<TagRegistry, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    // Entries share catalog_id == 7 and differ only in entry_no — a partial-key
    // match on catalog_id alone would make either junction row resolve to both.
    seed_catalog_entries<TypeParam>();

    RegistryQs registry_qs;
    ASSERT_TRUE(registry_qs.insert(TagRegistry{.label = "featured"}).execute().has_value());

    // The registry links ONLY (7, 3) — never (7, 4).
    link_junction_rows(conn, REGISTRY_ENTRY_COLS, std::array{"(1, 7, 3)"});

    const auto results = registry_qs.template join<fields::TagRegistry.entries>().select().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 1U);
    const auto &registry = *results->begin();
    ASSERT_EQ(registry.entries.size(), 1U) << "a match on catalog_id alone would also pull in entry_no=4";
    EXPECT_EQ(registry.entries.begin()->catalog_id, 7);
    EXPECT_EQ(registry.entries.begin()->entry_no, 3);
    EXPECT_EQ(registry.entries.begin()->title, "widget");
}

// The aggregate path builds its own SQL (get_complete_sql, a chained junction
// join) rather than the two-query prefix, so its junction⋈related ON clause needs
// the same widening independently — count() exercises it end to end.
TYPED_TEST(CompositeRelatedJunctionCreateTest, CountOverCompositeRelatedM2MCountsPairs) {
    using RegistryQs = storm::QuerySet<TagRegistry, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    seed_catalog_entries<TypeParam>();

    RegistryQs registry_qs;
    ASSERT_TRUE(registry_qs.insert(TagRegistry{.label = "featured"}).execute().has_value());
    link_junction_rows(conn, REGISTRY_ENTRY_COLS, std::array{"(1, 7, 3)", "(1, 7, 4)"});

    const auto count = registry_qs.template join<fields::TagRegistry.entries>().count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 2); // one (registry, entry) pair per junction row
}

// Composite on BOTH sides: 5 junction columns, 5-column PK, two multi-column FK
// clauses. The widest junction shape the tree produces.
TYPED_TEST(CompositeBothSidesJunctionCreateTest, JunctionTableIsCreatedOnLiveBackend) {
    using ShelfQs = storm::QuerySet<ShelfAssignment, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();
    QuerySet<StorageBin, TypeParam> bin_qs;
    const auto bin_inserted =
        bin_qs.insert(StorageBin{.aisle = 4, .bin_code = "B12", .revision = 9, .capacity = 2.5}).execute();
    ASSERT_TRUE(bin_inserted.has_value());
    QuerySet<ShelfAssignment, TypeParam> shelf_qs;
    const auto shelf_inserted =
        shelf_qs.insert(ShelfAssignment{.warehouse_no = 2, .shelf_code = "S-1", .priority = 5}).execute();
    ASSERT_TRUE(shelf_inserted.has_value());
    auto link = conn->execute("INSERT INTO ShelfAssignment_StorageBin "
                              "(ShelfAssignment_warehouse_no, ShelfAssignment_shelf_code, "
                              "StorageBin_aisle, StorageBin_bin_code, StorageBin_revision) "
                              "VALUES (2, 'S-1', 4, 'B12', 9)");
    EXPECT_TRUE(link.has_value()) << "both-sides-composite junction must accept a row keyed on all 5 columns";
}

// Both sides composite: a 2-column owner key AND a 3-column related key in one
// query. Q2 must select 2 owner columns, compare them as a 2-column row value,
// AND-join 3 related columns in its junction⋈related ON clause, and start the
// related entity's own columns at offset 2. Decoys share a PK part on BOTH sides.
TYPED_TEST(CompositeBothSidesJunctionCreateTest, M2MEagerLoadStitchesOverBothCompositeKeys) {
    using ShelfQs = storm::QuerySet<ShelfAssignment, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    seed_bins<TypeParam>();
    seed_shelves<TypeParam>();
    ShelfQs shelf_qs;

    // S-1 -> revision 9 only; S-2 -> revision 10 only.
    link_junction_rows(conn, SHELF_BIN_COLS, std::array{"(2, 'S-1', 4, 'B12', 9)", "(2, 'S-2', 4, 'B12', 10)"});

    const auto results = shelf_qs.template join<fields::ShelfAssignment.bins>().select().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 2U);

    for (const auto &shelf : *results) {
        ASSERT_EQ(shelf.bins.size(), 1U) << "shelf " << shelf.shelf_code
                                         << " must receive exactly its own bin — a partial match on either side "
                                            "(warehouse_no alone, or aisle/bin_code without revision) gives it both";
        const auto &bin = *shelf.bins.begin();
        EXPECT_EQ(bin.aisle, 4);
        EXPECT_EQ(bin.bin_code, "B12");
        if (shelf.shelf_code == "S-1") {
            EXPECT_EQ(bin.revision, 9);
            EXPECT_DOUBLE_EQ(bin.capacity, 2.5);
        } else {
            EXPECT_EQ(shelf.shelf_code, "S-2");
            EXPECT_EQ(bin.revision, 10);
            EXPECT_DOUBLE_EQ(bin.capacity, 3.5);
        }
    }
}

TYPED_TEST(CompositeBothSidesJunctionCreateTest, CountOverBothCompositeM2MCountsPairs) {
    using ShelfQs = storm::QuerySet<ShelfAssignment, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    seed_bins<TypeParam>();
    seed_shelves<TypeParam>(/*with_second=*/false); // only S-1: the count is over PAIRS
    ShelfQs shelf_qs;
    link_junction_rows(conn, SHELF_BIN_COLS, std::array{"(2, 'S-1', 4, 'B12', 9)", "(2, 'S-1', 4, 'B12', 10)"});

    const auto count = shelf_qs.template join<fields::ShelfAssignment.bins>().count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 2);
}

// LEFT keeps a both-sides-composite owner with no junction rows; INNER drops it.
TYPED_TEST(CompositeBothSidesJunctionCreateTest, LeftJoinKeepsBothCompositeOwnerWithNoBins) {
    QuerySet<ShelfAssignment, TypeParam> shelf_qs;
    ASSERT_TRUE(shelf_qs.insert(ShelfAssignment{.warehouse_no = 8, .shelf_code = "lonely", .priority = 1})
                    .execute()
                    .has_value());

    const auto left = shelf_qs.template left_join<fields::ShelfAssignment.bins>().select().execute();
    ASSERT_TRUE(left.has_value()) << left.error().message();
    ASSERT_EQ(left->size(), 1U);
    EXPECT_TRUE(left->begin()->bins.empty());

    const auto inner = shelf_qs.template join<fields::ShelfAssignment.bins>().select().execute();
    ASSERT_TRUE(inner.has_value()) << inner.error().message();
    EXPECT_EQ(inner->size(), 0U);
}

// ── #504 Task 10: multi-relation join over a COMPOSITE owner ────────────────
//
// #392 chains several m2m fields in one call, assigning relation Is the junction
// alias 2+2*Is and the related alias 3+2*Is. Every prior composite m2m test uses
// a SINGLE relation, whose aliases (t2/t3) are exactly the ones the one-relation
// path already used — so none of them can catch an error in that arithmetic.
//
// With a composite owner each relation's ON clause now AND-joins one equality
// PER OWNER PK PART, so the per-relation alias and the per-relation part loop
// are nested. ShelfAssignment is the only composite owner with two relations,
// and they deliberately differ in related-side arity (StorageBin has a 3-part
// PK, LedgerTag a single-column one): a per-relation column count that leaked
// across relations would misalign the second relation's extraction rather than
// fail outright.
TYPED_TEST(MultiRelationCompositeOwnerTest, TwoRelationsOnACompositeOwnerStitchIndependently) {
    using ShelfQs = storm::QuerySet<ShelfAssignment, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    seed_bins<TypeParam>();
    seed_shelves<TypeParam>();
    seed_tags(conn, 2);

    // S-1 → bin revision 9 + tag 'audit'; S-2 → bin revision 10 + tag 'review'.
    // The two shelves share warehouse_no, so a stitch on the first PK part alone
    // would hand each shelf BOTH relations' rows.
    link_junction_rows(conn, SHELF_BIN_COLS, std::array{"(2, 'S-1', 4, 'B12', 9)", "(2, 'S-2', 4, 'B12', 10)"});
    link_junction_rows(conn, SHELF_TAG_COLS, std::array{"(2, 'S-1', 1)", "(2, 'S-2', 2)"});

    ShelfQs shelf_qs;
    const auto results =
        shelf_qs.template join<fields::ShelfAssignment.bins, fields::ShelfAssignment.tags>().select().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 2U);

    for (const auto &shelf : *results) {
        ASSERT_EQ(shelf.bins.size(), 1U) << "shelf " << shelf.shelf_code << " must get exactly its own bin";
        ASSERT_EQ(shelf.tags.size(), 1U) << "shelf " << shelf.shelf_code << " must get exactly its own tag";
        if (shelf.shelf_code == "S-1") {
            EXPECT_EQ(shelf.bins.begin()->revision, 9);
            EXPECT_EQ(shelf.tags.begin()->label, "audit");
        } else {
            EXPECT_EQ(shelf.shelf_code, "S-2");
            EXPECT_EQ(shelf.bins.begin()->revision, 10);
            EXPECT_EQ(shelf.tags.begin()->label, "review");
        }
    }
}

// LEFT fills each relation independently: a composite owner linked in ONE
// relation but not the other keeps the empty container rather than dropping.
TYPED_TEST(MultiRelationCompositeOwnerTest, LeftJoinFillsEachRelationIndependently) {
    using ShelfQs = storm::QuerySet<ShelfAssignment, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    seed_bins<TypeParam>();
    seed_shelves<TypeParam>(/*with_second=*/false); // S-1 only
    seed_tags(conn, 1);

    // S-1 gets a bin but NO tag.
    link_junction_rows(conn, SHELF_BIN_COLS, std::array{"(2, 'S-1', 4, 'B12', 9)"});

    ShelfQs shelf_qs;
    const auto results =
        shelf_qs.template left_join<fields::ShelfAssignment.bins, fields::ShelfAssignment.tags>().select().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 1U);
    const auto &shelf = *results->begin();
    EXPECT_EQ(shelf.bins.size(), 1U);
    EXPECT_TRUE(shelf.tags.empty()) << "an unlinked relation must stay empty, not borrow the other's rows";
}

// select() runs the two-query path; count() builds ONE chained-junction SQL via
// append_complete_join, which is where #392's alias arithmetic (2+2*Is / 3+2*Is)
// actually lives. The select() tests above pass even with that arithmetic broken,
// so this test is what guards it — with a composite owner each relation also
// AND-joins one equality per owner PK part, nesting the two loops.
TYPED_TEST(MultiRelationCompositeOwnerTest, CountOverTwoRelationsChainsBothJunctionsWithDistinctAliases) {
    using ShelfQs = storm::QuerySet<ShelfAssignment, TypeParam>;
    auto conn = DefaultConnection<TypeParam>::get();

    seed_bins<TypeParam>();
    seed_shelves<TypeParam>(/*with_second=*/false); // S-1 only
    seed_tags(conn, 2);

    // S-1 → 2 bins × 2 tags. The chained join yields the cross product, so a
    // correct alias assignment gives 4; reusing one alias for both junctions
    // collapses or corrupts the join instead.
    link_junction_rows(conn, SHELF_BIN_COLS, std::array{"(2, 'S-1', 4, 'B12', 9)", "(2, 'S-1', 4, 'B12', 10)"});
    link_junction_rows(conn, SHELF_TAG_COLS, std::array{"(2, 'S-1', 1)", "(2, 'S-1', 2)"});

    ShelfQs shelf_qs;
    const auto count =
        shelf_qs.template join<fields::ShelfAssignment.bins, fields::ShelfAssignment.tags>().count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 4) << "2 bins x 2 tags for S-1; a shared junction alias breaks the chain";
}
