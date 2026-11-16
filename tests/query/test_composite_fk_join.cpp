#include <gtest/gtest.h>
#include <meta>
#include <plf_hive/plf_hive.h> // NOSONAR cpp:S954 — must precede `import std;` (see test_m2m_models.h)

#include "test_db_helpers.h"

import storm;
import storm_orm_statements_join; // JoinStatement / M2MJoinStatement / JoinType — not re-exported by `storm`
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954 — Message/Person
#include "../../shared/models/task.h"
#include "test_fixture.h"

// Must follow the shared model headers: OrderLineWithShipments/Shipment name storm:: annotations.
#include "crud/test_composite_pk_models.h" // NOSONAR cpp:S954
#include "test_m2m_models.h"               // NOSONAR cpp:S954 — Student/Course (single-PK m2m regression baseline)
#include "test_reverse_fk_models.h"        // NOSONAR cpp:S954 — RfPerson/RfTask (single-PK reverse-FK baseline)
using storm::QuerySet;

// ── #504 Task 7: regular FK JoinStatement — multi-column AND-joined ON clause ──
// Shipment::line is a plain (non-m2m, non-reverse-FK) FK member whose target
// (OrderLineWithShipments) carries a 2-part composite PK (order_id, product_id).
// join<fields::Shipment.line>() must emit an ON clause that AND-joins BOTH target PK
// parts against their respective local <fk>_<part>_id columns — using only ONE
// part (as the pre-fix code did, via FKBase_at<Is>::pk_name_) would silently
// match on order_id alone, potentially joining to the WRONG OrderLineWithShipments
// row when several rows share an order_id but differ in product_id.

namespace stmt = storm::orm::statements;

namespace {

    template <typename ConnType> class CompositeFkRegularJoinTest : public StormTestFixture<Shipment, ConnType> {};

} // namespace

TYPED_TEST_SUITE(CompositeFkRegularJoinTest, DatabaseTypes);

// Disambiguation case (mandated by the task brief): two OrderLineWithShipments
// rows share ONE PK part (order_id == 5) but differ in the OTHER (product_id).
// Each is referenced by a different Shipment. If the ON clause only matched on
// order_id (the pre-fix bug), BOTH Shipments would join to whichever row the
// query planner picked first — this test proves each Shipment resolves to its
// OWN OrderLineWithShipments, not the other one sharing the partial key.
TYPED_TEST(CompositeFkRegularJoinTest, InnerJoinDoesNotCrossMatchOnSharedPkPart) {
    QuerySet<OrderLineWithShipments, TypeParam> line_qs;
    const OrderLineWithShipments                line_a{.order_id = 5, .product_id = 12, .quantity = 3, .note = "a"};
    const OrderLineWithShipments                line_b{.order_id = 5, .product_id = 99, .quantity = 7, .note = "b"};
    ASSERT_TRUE(line_qs.insert(line_a).execute().has_value());
    ASSERT_TRUE(line_qs.insert(line_b).execute().has_value());

    QuerySet<Shipment, TypeParam> ship_qs;
    ASSERT_TRUE(ship_qs.insert(Shipment{.line = line_a, .carrier = "UPS"}).execute().has_value());
    ASSERT_TRUE(ship_qs.insert(Shipment{.line = line_b, .carrier = "FedEx"}).execute().has_value());

    auto results = ship_qs.template join<fields::Shipment.line>().select().execute();
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 2U);

    bool found_ups   = false;
    bool found_fedex = false;
    for (auto& shipment : *results) {
        if (shipment.carrier == "UPS") {
            EXPECT_EQ(shipment.line.order_id, 5);
            EXPECT_EQ(shipment.line.product_id, 12);
            EXPECT_EQ(shipment.line.quantity, 3);
            found_ups = true;
        } else if (shipment.carrier == "FedEx") {
            EXPECT_EQ(shipment.line.order_id, 5);
            EXPECT_EQ(shipment.line.product_id, 99);
            EXPECT_EQ(shipment.line.quantity, 7);
            found_fedex = true;
        }
    }
    EXPECT_TRUE(found_ups);
    EXPECT_TRUE(found_fedex);
}

// Shipment::line is a non-optional FK, so SQLite's FK enforcement (RESTRICT,
// the default policy) rejects both a dangling INSERT and a DELETE of a
// referenced parent — there is no way to produce a "no match" row through it.
// OptionalShipment::line (std::optional<OrderLineWithShipments>) is the
// composite-FK fixture built for exactly this: a NULL FK, mirroring the
// established convention for single-column optional-FK LEFT JOIN tests
// (test_fk_fields.cpp's NullableFKTest).
template <typename ConnType> class OptionalCompositeFkJoinTest : public StormTestFixture<OptionalShipment, ConnType> {};
TYPED_TEST_SUITE(OptionalCompositeFkJoinTest, DatabaseTypes);

TYPED_TEST(OptionalCompositeFkJoinTest, LeftJoinKeepsRowWithNullCompositeFk) {
    QuerySet<OptionalShipment, TypeParam> ship_qs;
    ASSERT_TRUE(ship_qs.insert(OptionalShipment{.line = std::nullopt, .carrier = "FedEx"}).execute().has_value());

    auto results = ship_qs.template left_join<fields::OptionalShipment.line>().select().execute();
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1U); // LEFT keeps the shipment even with a NULL composite FK
    EXPECT_FALSE(results->begin()->line.has_value());
}

// ── #744: m2m aggregate "complete SQL" columns for composite-FK members ─────────────
// A composite FK spells one column per target PK part ("<member>_<part>"). The aggregate
// path's base-column list and related-column list must do the same; "<member>_id" names a
// column the table does not have.
TEST(CompositeFkM2MAggregateSqlTest, BaseColsExpandCompositeFkMember) {
    using MS = stmt::M2MJoinStatement<Parcel, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^Parcel::labels>;
    EXPECT_EQ(MS::base_cols_view(), R"(SELECT t1."id", t1."line_order_id", t1."line_product_id")");
}

TEST(CompositeFkM2MAggregateSqlTest, RelatedColsExpandCompositeFkMember) {
    using MS = stmt::M2MJoinStatement<Parcel, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^Parcel::labels>;
    std::string sql;
    MS::append_complete_cols(sql, 3);
    EXPECT_EQ(sql, R"(, t3."id", t3."line_order_id", t3."line_product_id", t3."text")");
}

TEST(CompositeFkM2MAggregateSqlTest, ReverseFkBaseColsExpandCompositeFkMember) {
    using RS = stmt::ReverseFKJoinStatement<
            ParcelHolder,
            storm::db::sqlite::Connection,
            stmt::JoinType::Inner,
            ^^ParcelNote::holder>;
    EXPECT_TRUE(
            RS::get_complete_sql().starts_with(
                    R"(SELECT t1."id", t1."line_order_id", t1."line_product_id", t1."tag" FROM "parcelholder" t1)"
            )
    ) << RS::get_complete_sql();
}

// ── #744: a regular FK JOIN whose TARGET itself carries a composite-FK member ──────
// ParcelNote::holder joins ParcelHolder, which has its own composite FK ("line" ->
// OrderLine's 2-part key). The joined target's select list must spell that member as one
// column per key part, not as a bare "line" column the table does not have; and the
// column-offset bookkeeping must advance by the real column count so later columns still
// line up with what the extractor reads.
TEST(CompositeFkJoinSqlTest, JoinedTargetCompositeFkMemberExpandsInSelectList) {
    using JS =
            stmt::JoinStatement<ParcelNote, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^ParcelNote::holder>;
    EXPECT_TRUE(
            JS::get_complete_sql().starts_with(
                    R"(SELECT t1."id", t1."text", t2."id", t2."line_order_id", t2."line_product_id", t2."tag" FROM)"
            )
    ) << JS::get_complete_sql();
}

namespace {
    template <typename ConnType> class JoinedTargetCompositeFkTest : public StormTestFixture<ParcelNote, ConnType> {};
} // namespace
TYPED_TEST_SUITE(JoinedTargetCompositeFkTest, DatabaseTypes);

TYPED_TEST(JoinedTargetCompositeFkTest, JoinHydratesTargetCompositeFkKeyAndLaterColumns) {
    QuerySet<OrderLine, TypeParam> line_qs;
    const OrderLine                line{.order_id = 5, .product_id = 12, .quantity = 3, .note = "n"};
    ASSERT_TRUE(line_qs.insert(line).execute().has_value());

    QuerySet<ParcelHolder, TypeParam> holder_qs;
    ASSERT_TRUE(holder_qs.insert(ParcelHolder{.id = 1, .line = line, .tag = "cold"}).execute().has_value());

    QuerySet<ParcelNote, TypeParam> note_qs;
    ASSERT_TRUE(note_qs.insert(ParcelNote{.holder = ParcelHolder{.id = 1}, .text = "fragile"}).execute().has_value());

    auto results = note_qs.template join<fields::ParcelNote.holder>().select().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 1U);
    const auto& note = *results->begin();
    EXPECT_EQ(note.text, "fragile");
    EXPECT_EQ(note.holder.id, 1);
    EXPECT_EQ(note.holder.line.order_id, 5);
    EXPECT_EQ(note.holder.line.product_id, 12);
    EXPECT_EQ(note.holder.tag, "cold"); // the column AFTER the widened composite-FK block
}

namespace {
    template <typename ConnType> class TwoJoinedTargetsTest : public StormTestFixture<ParcelPair, ConnType> {};
} // namespace
TYPED_TEST_SUITE(TwoJoinedTargetsTest, DatabaseTypes);

// The second joined table's columns begin after the first's WIDENED block (3 key columns +
// tag, not 2 members + tag). Offsets counted per member would read the second target from
// the wrong columns — a silent misread, not an error.
TYPED_TEST(TwoJoinedTargetsTest, SecondJoinedTargetReadsFromItsOwnColumns) {
    QuerySet<OrderLine, TypeParam> line_qs;
    const OrderLine                line_a{.order_id = 5, .product_id = 12, .quantity = 1, .note = "a"};
    const OrderLine                line_b{.order_id = 6, .product_id = 34, .quantity = 2, .note = "b"};
    ASSERT_TRUE(line_qs.insert(line_a).execute().has_value());
    ASSERT_TRUE(line_qs.insert(line_b).execute().has_value());

    QuerySet<ParcelHolder, TypeParam> holder_qs;
    ASSERT_TRUE(holder_qs.insert(ParcelHolder{.id = 1, .line = line_a, .tag = "first"}).execute().has_value());
    ASSERT_TRUE(holder_qs.insert(ParcelHolder{.id = 2, .line = line_b, .tag = "second"}).execute().has_value());

    QuerySet<ParcelPair, TypeParam> pair_qs;
    ASSERT_TRUE(pair_qs.insert(ParcelPair{.first = ParcelHolder{.id = 1}, .second = ParcelHolder{.id = 2}})
                        .execute()
                        .has_value());

    auto results = pair_qs.template join<fields::ParcelPair.first, fields::ParcelPair.second>().select().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results->size(), 1U);
    const auto& pair = *results->begin();
    EXPECT_EQ(pair.first.id, 1);
    EXPECT_EQ(pair.first.line.product_id, 12);
    EXPECT_EQ(pair.first.tag, "first");
    EXPECT_EQ(pair.second.id, 2);
    EXPECT_EQ(pair.second.line.order_id, 6);
    EXPECT_EQ(pair.second.line.product_id, 34);
    EXPECT_EQ(pair.second.tag, "second");
}

// Single-PK / single-FK models must render EXACTLY as before: one "<fk>_id" per FK member.
TEST(CompositeFkM2MAggregateSqlTest, SingleColumnFkAndPkStayByteIdentical) {
    using MS = stmt::M2MJoinStatement<CardBox, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^CardBox::cards>;
    EXPECT_EQ(MS::base_cols_view(), R"(SELECT t1."id", t1."owner_id")");
    std::string sql;
    MS::append_complete_cols(sql, 3);
    EXPECT_EQ(sql, R"(, t3."id", t3."owner_id", t3."text")");

    using PS =
            stmt::M2MJoinStatement<Student, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^Student::courses>;
    EXPECT_EQ(PS::base_cols_view(), R"(SELECT t1."id", t1."name", t1."age")");
}

TEST(CompositeFkM2MAggregateSqlTest, ReverseFkSinglePkStaysByteIdentical) {
    using RS = stmt::
            ReverseFKJoinStatement<RfPerson, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^RfTask::assignee>;
    EXPECT_TRUE(RS::get_complete_sql().starts_with(R"(SELECT t1."id", t1."name", t1."age" FROM "rfperson" t1)"))
            << RS::get_complete_sql();
}

namespace {
    template <typename ConnType>
    class ParcelM2MAggregateTest : public StormTestFixture<Parcel, ConnType, ParcelLabel> {};
    template <typename ConnType> class ParcelHolderAggregateTest : public StormTestFixture<ParcelNote, ConnType> {};
} // namespace
TYPED_TEST_SUITE(ParcelM2MAggregateTest, DatabaseTypes);
TYPED_TEST_SUITE(ParcelHolderAggregateTest, DatabaseTypes);

// The broken path was the aggregate one: its SELECT named a "line_id" column that a
// composite FK does not have, so executing it failed ("no such column") on both backends.
TYPED_TEST(ParcelM2MAggregateTest, CountOverM2MWithCompositeFkMembersExecutes) {
    auto                           conn = storm::db::DefaultConnection<TypeParam>::get();
    QuerySet<OrderLine, TypeParam> line_qs;
    const OrderLine                line{.order_id = 5, .product_id = 12, .quantity = 1, .note = "n"};
    ASSERT_TRUE(line_qs.insert(line).execute().has_value());

    QuerySet<Parcel, TypeParam>      parcel_qs;
    QuerySet<ParcelLabel, TypeParam> label_qs;
    ASSERT_TRUE(parcel_qs.insert(Parcel{.id = 1, .line = line}).execute().has_value());
    ASSERT_TRUE(label_qs.insert(ParcelLabel{.id = 1, .line = line, .text = "x"}).execute().has_value());
    ASSERT_TRUE(conn->execute("INSERT INTO Parcel_ParcelLabel (Parcel_id, ParcelLabel_id) VALUES (1, 1)").has_value());

    const auto count = parcel_qs.template join<fields::Parcel.labels>().count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 1);
}

TYPED_TEST(ParcelHolderAggregateTest, CountOverReverseFkWithCompositeFkBaseExecutes) {
    QuerySet<OrderLine, TypeParam> line_qs;
    const OrderLine                line{.order_id = 5, .product_id = 12, .quantity = 1, .note = "n"};
    ASSERT_TRUE(line_qs.insert(line).execute().has_value());

    QuerySet<ParcelHolder, TypeParam> holder_qs;
    ASSERT_TRUE(holder_qs.insert(ParcelHolder{.id = 1, .line = line, .tag = "t"}).execute().has_value());
    QuerySet<ParcelNote, TypeParam> note_qs;
    ASSERT_TRUE(note_qs.insert(ParcelNote{.holder = ParcelHolder{.id = 1}, .text = "n"}).execute().has_value());

    const auto count = holder_qs.template join<fields::ParcelNote.holder>().count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 1);
}

TEST(CompositeFkJoinSqlTest, JoinOnClauseAndJoinsBothCompositeKeyParts) {
    using JS = stmt::JoinStatement<Shipment, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^Shipment::line>;
    const std::string& sql = JS::get_complete_sql();
    EXPECT_NE(sql.find(R"(t2."order_id" = t1."line_order_id")"), std::string::npos) << sql;
    EXPECT_NE(sql.find(R"(t2."product_id" = t1."line_product_id")"), std::string::npos) << sql;
    EXPECT_NE(sql.find(" AND "), std::string::npos) << sql;
}

// Regression guard: Message::sender (single-column PK target) JOIN SQL must be
// EXACTLY what it was before #504 — no AND, plain "t2.id = t1.sender_id".
TEST(CompositeFkJoinSqlTest, SinglePkJoinSqlStaysByteIdentical) {
    using JS = stmt::JoinStatement<Message, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^Message::sender>;
    const std::string& sql = JS::get_complete_sql();
    EXPECT_NE(sql.find(R"(ON t2."id" = t1."sender_id")"), std::string::npos) << sql;
    EXPECT_EQ(sql.find(" AND "), std::string::npos) << sql; // no AND for a single-column key
}

// ── #504 Task 8: StitchKey-based m2m/reverse-FK stitch for a COMPOSITE owner ──
//
// The two-query eager-load path (m2m #391, reverse-FK #398) stitches Q2 rows to
// their Q1 owner through a hash map keyed on the owner's primary key. Before
// this task the key was a bare std::int64_t built from ONLY the first PK part
// (Base::primary_key_), which for LedgerWithTags/OrderLineWithShipments (both
// composite: 3 and 2 parts respectively) collides whenever two owners share
// their first part but differ in a later one — exactly what the tests below
// construct on purpose.

// ---- Reverse-FK composite-owner case (no junction table needed) -----------
// OrderLineWithShipments' PK is (order_id, product_id); Shipment::line is the
// FK back-reference (reverse_fk<^^Shipment>::shipments). The fan-out test below
// uses a decoy sharing order_id but differing in product_id — proves the stitch
// keys on BOTH parts.
template <typename ConnType>
class CompositeReverseFkOwnerTest : public StormTestFixture<OrderLineWithShipments, ConnType, Shipment> {
  protected:
    // Insert a lone OrderLineWithShipments with no Shipment pointing at it —
    // shared setup for the empty-relation INNER/LEFT pair below.
    static auto insert_solo_line(QuerySet<OrderLineWithShipments, ConnType>& line_qs) -> void {
        ASSERT_TRUE(
                line_qs.insert(OrderLineWithShipments{.order_id = 9, .product_id = 1, .quantity = 1, .note = "solo"})
                        .execute()
                        .has_value()
        );
    }
};
TYPED_TEST_SUITE(CompositeReverseFkOwnerTest, DatabaseTypes);

TYPED_TEST(CompositeReverseFkOwnerTest, EmptyRelationOnCompositeOwnerReturnsNoInnerJoinRows) {
    QuerySet<OrderLineWithShipments, TypeParam> line_qs;
    this->insert_solo_line(line_qs);
    auto results = line_qs.template join<fields::OrderLineWithShipments.shipments>().select().execute(); // INNER
    ASSERT_TRUE(results.has_value());
    EXPECT_TRUE(results->empty());
}

TYPED_TEST(CompositeReverseFkOwnerTest, LeftJoinKeepsEmptyRelationOnCompositeOwner) {
    QuerySet<OrderLineWithShipments, TypeParam> line_qs;
    this->insert_solo_line(line_qs);
    auto results = line_qs.template left_join<fields::OrderLineWithShipments.shipments>().select().execute();
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1U);
    EXPECT_TRUE(results->begin()->shipments.empty());
}

// Fan-out >= 10: one composite owner with 10 shipments — proves the stitch loop
// appends every related row into the SAME owner's container (no drops, no
// cross-contamination into a different owner sharing a PK part).
TYPED_TEST(CompositeReverseFkOwnerTest, FanOutTenShipmentsAllStitchToSameCompositeOwner) {
    QuerySet<OrderLineWithShipments, TypeParam> line_qs;
    const OrderLineWithShipments                owner{.order_id = 3, .product_id = 7, .quantity = 1, .note = "fanout"};
    ASSERT_TRUE(line_qs.insert(owner).execute().has_value());
    // A decoy sharing order_id=3 but a different product_id — must stay unaffected.
    const OrderLineWithShipments decoy{.order_id = 3, .product_id = 8, .quantity = 1, .note = "decoy"};
    ASSERT_TRUE(line_qs.insert(decoy).execute().has_value());

    QuerySet<Shipment, TypeParam> ship_qs;
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(
                ship_qs.insert(Shipment{.line = owner, .carrier = std::format("carrier-{}", i)}).execute().has_value()
        );
    }

    auto results = line_qs.template join<fields::OrderLineWithShipments.shipments>().select().execute();
    ASSERT_TRUE(results.has_value());
    ASSERT_EQ(results->size(), 1U); // decoy has zero shipments — dropped by INNER
    EXPECT_EQ(results->begin()->product_id, 7);
    EXPECT_EQ(results->begin()->shipments.size(), 10U);
}

// ---- m2m composite-owner case ----------------------------------------------
// LedgerWithTags' PK is (region, account, period); `tags` is a many_to_many<>
// container of LedgerTag via an auto-junction table.
//
// This case was a COMPILE-TIME rejection until #504 Task 9 (M2MOwnerPkSupported,
// now removed). The junction table used to carry exactly one "<Base>_id" column
// regardless of the owner's PK arity, and referenced "LedgerWithTags(id)" — a
// column a composite-PK model does not have. There was no valid single-column
// substitute (Base::pk_name_ is merely the FIRST declared part, "region"), so
// the shape was unrepresentable rather than merely unimplemented.
//
// Task 9 widened the junction to one owner column PER PK part, which makes the
// Q2 owner key a real N-column row value and the whole path expressible. The
// static_asserts that pinned the rejection are replaced by the end-to-end
// round-trip tests below — the strongest available evidence that the DDL, the
// Q2 SQL, and the stitch all agree on the same N.
static_assert(
        stmt::M2MJoinStatement<
                LedgerWithTags,
                storm::db::sqlite::Connection,
                stmt::JoinType::Inner,
                ^^LedgerWithTags::tags>::owner_key_column_count_ == 3,
        "m2m owner-key column count must match the composite owner's PK arity (3 for LedgerWithTags) "
        "— the junction carries one owner column per part since #504 Task 9"
);
static_assert(
        stmt::M2MJoinStatement<Student, storm::db::sqlite::Connection, stmt::JoinType::Inner, ^^Student::courses>::
                        owner_key_column_count_ == 1,
        "a single-PK owner still resolves to exactly one junction owner column"
);
static_assert(
        stmt::ReverseFKJoinStatement<
                OrderLineWithShipments,
                storm::db::sqlite::Connection,
                stmt::JoinType::Inner,
                ^^Shipment::line>::owner_key_column_count_ == 2,
        "reverse-FK owner-key column count must match the composite base's own PK arity (2 for "
        "OrderLineWithShipments) — the owning table carries a REAL N-column FK"
);

// ── Reverse-FK Q2 SQL shape over a composite owner (#504 Task 8) ────────────
// Documents (and regression-guards) the exact widened SQL: the owner-key
// column list widens to N parts, each individually re-aliased ("t2.<part>",
// not a bare comma list — that would alias only the first part); the WHERE
// clause becomes the row-value-IN-subquery form on BOTH sides (outer
// "(t2.a, t2.b)" wrapped in parens, inner subquery SELECT list a PLAIN
// comma-joined column list, NOT itself parenthesized — parenthesizing the
// inner list was a real bug this task's own testing caught: SQLite parses
// "(a, b)" in a SELECT list as one parenthesized scalar expression, not a row
// value, so the sub-select silently returns 1 column instead of 2 (an error,
// not a silent miscompare, so it fails loudly here — but a subtler variant of
// the same mistake could easily have gone unnoticed without an exact-string
// assertion). Owner columns include Shipment::line, itself a composite-FK
// member — its SELECT columns must ALSO widen (t2.line_order_id,
// t2.line_product_id), not collapse to a single bogus t2.line_id.
TEST(CompositeFkJoinSqlTest, ReverseFkQ2SqlShapeIsExactOverCompositeOwner) {
    using JS = stmt::ReverseFKJoinStatement<
            OrderLineWithShipments,
            storm::db::sqlite::Connection,
            stmt::JoinType::Inner,
            ^^Shipment::line>;
    const std::string sql = JS::build_q2_sql(nullptr, std::nullopt, std::nullopt, std::nullopt);
    EXPECT_EQ(
            sql,
            R"(SELECT t2."line_order_id", t2."line_product_id", t2."id", t2."line_order_id", t2."line_product_id", t2."carrier" )"
            R"(FROM "shipment" t2 WHERE (t2."line_order_id", t2."line_product_id") IN (SELECT "order_id", "product_id" FROM )"
            R"("orderlinewithshipments"))"
    );
}

// ── Review fix: fk_on_clause_body_size undercounted parts after the first ──
// The sizer reserved only " AND " (5 bytes) per composite-FK part after the
// first, but append_fk_on_clause_body actually writes " AND t<alias>." (7 +
// digits_of(alias) bytes) — a 2-3 byte-per-part deficit that ConstexprString
// silently swallows by truncating the tail of the buffer (ConstexprString::
// append drops bytes past capacity with no diagnostic). A `.contains(...)`
// substring check on a handful of expected fragments would still pass even
// with the tail chopped off, so the test below asserts EXACT equality
// against the full, hand-computed SQL string instead — the only check exact
// enough to catch a deficit regardless of whether SMALL_BUFFER's slack
// happens to absorb it for a given arity.

// LedgerEntryRefWide joins the 3-part Ledger target through SIX separate FK
// fields (ledger1..ledger6). Each ON-clause body has two after-first parts, a
// 6-byte deficit under the pre-fix sizer, so the six together lose 36 bytes —
// which DOES exceed the ~30-byte stacked SMALL_BUFFER slack (calculate_complete_
// sql_size / calculate_join_sql_size / calculate_select_fields_size each pad by
// 10 bytes). A single 3-part join (6 bytes) is absorbed by that slack and passed
// even under the pre-fix sizer. Confirmed to fail (truncated/wrong-length SQL)
// against the pre-fix sizer, and to pass now only because the sizer reserves the
// writer's exact byte count.
TEST(CompositeFkJoinSqlTest, SixCompositeFkJoinsToSameTargetSqlIsExact) {
    using JS = stmt::JoinStatement<
            LedgerEntryRefWide,
            storm::db::sqlite::Connection,
            stmt::JoinType::Inner,
            ^^LedgerEntryRefWide::ledger1,
            ^^LedgerEntryRefWide::ledger2,
            ^^LedgerEntryRefWide::ledger3,
            ^^LedgerEntryRefWide::ledger4,
            ^^LedgerEntryRefWide::ledger5,
            ^^LedgerEntryRefWide::ledger6>;
    const std::string& sql = JS::get_complete_sql();

    std::string expected = R"(SELECT t1."id")";
    for (int i = 2; i <= 7; ++i) {
        expected += std::format(R"(, t{0}."region", t{0}."account", t{0}."period", t{0}."balance")", i);
    }
    expected += R"( FROM "ledgerentryrefwide" t1)";
    for (int i = 2, ledger_n = 1; i <= 7; ++i, ++ledger_n) {
        expected += std::format(
                R"( INNER JOIN "ledger" t{0} ON t{0}."region" = t1."ledger{1}_region")"
                R"( AND t{0}."account" = t1."ledger{1}_account")"
                R"( AND t{0}."period" = t1."ledger{1}_period")",
                i,
                ledger_n
        );
    }
    EXPECT_EQ(sql, expected);
}
