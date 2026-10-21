#pragma once

// Shared test body for test_composite_fk_join_sqlite.cpp / test_composite_fk_join_pg.cpp — the two
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
#error "test_composite_fk_join_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954 — Message/Person
#include "../../shared/models/person.h"
#include "test_fixture.h"

// Must follow the shared model headers: OrderLineWithShipments/Shipment name storm:: annotations.
#include "test_composite_pk_models.h" // NOSONAR cpp:S954
using storm::create_table_sql;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// ── #504: composite FK bind + extract (INSERT/SELECT plain FK column) ───────
// Shipment::line is a plain (non-JOIN) FK member whose target
// (OrderLineWithShipments) carries a 2-part composite PK (order_id, product_id).
// This proves the INSERT bind path writes both target-key parts into
// line_order_id/line_product_id, and the SELECT extraction path reads them back
// into a fully-populated `line` object — without ever touching the JOIN machinery
// (out of scope here, #504's later tasks).

namespace {

// SchemaStatement<Shipment>::create_table_if_not_exists creates the FK parent
// (OrderLineWithShipments) automatically via create_fk_parents (#412), so no
// ExtraModels are needed here — unlike StockEntryTest in test_composite_pk_crud.cpp,
// which creates Person itself because that fixture seeds through raw SQL before
// Shipment's own table exists.
template <typename ConnType> class CompositeFkJoinTest : public StormTestFixture<Shipment, ConnType> {};

} // namespace
TYPED_TEST_SUITE(CompositeFkJoinTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(CompositeFkJoinTest, InsertAndSelectRoundTripsCompositeFkColumns) {
    const OrderLineWithShipments line{.order_id = 5, .product_id = 12, .quantity = 3, .note = "first"};
    ASSERT_TRUE((InsertStatement<OrderLineWithShipments, TypeParam>{}).query(line).execute().has_value());

    const Shipment ship{.line = line, .carrier = "UPS"};
    auto insert_result = (InsertStatement<Shipment, TypeParam>{}).query(ship).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto rows = (SelectStatement<Shipment, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    const Shipment &loaded = *rows.value().begin();
    EXPECT_EQ(loaded.line.order_id, 5);
    EXPECT_EQ(loaded.line.product_id, 12);
    EXPECT_EQ(loaded.carrier, "UPS");
}

// Distinguishes the two composite-FK target parts from each other: swapping them
// would have this test pass if the bind/extract order matched a coincidental
// symmetry, but the two rows below share no value across order_id/product_id.
TYPED_TEST(CompositeFkJoinTest, MultipleShipmentsResolveDistinctCompositeFkTargets) {
    const OrderLineWithShipments line_a{.order_id = 1, .product_id = 100, .quantity = 1, .note = "a"};
    const OrderLineWithShipments line_b{.order_id = 2, .product_id = 200, .quantity = 2, .note = "b"};
    ASSERT_TRUE((InsertStatement<OrderLineWithShipments, TypeParam>{}).query(line_a).execute().has_value());
    ASSERT_TRUE((InsertStatement<OrderLineWithShipments, TypeParam>{}).query(line_b).execute().has_value());

    ASSERT_TRUE((InsertStatement<Shipment, TypeParam>{})
                    .query(Shipment{.line = line_a, .carrier = "DHL"})
                    .execute()
                    .has_value());
    ASSERT_TRUE((InsertStatement<Shipment, TypeParam>{})
                    .query(Shipment{.line = line_b, .carrier = "FedEx"})
                    .execute()
                    .has_value());

    auto rows = (SelectStatement<Shipment, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 2U);

    bool found_a = false;
    bool found_b = false;
    for (const Shipment &row : rows.value()) {
        if (row.carrier == "DHL") {
            EXPECT_EQ(row.line.order_id, 1);
            EXPECT_EQ(row.line.product_id, 100);
            found_a = true;
        } else if (row.carrier == "FedEx") {
            EXPECT_EQ(row.line.order_id, 2);
            EXPECT_EQ(row.line.product_id, 200);
            found_b = true;
        }
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
}

TYPED_TEST(CompositeFkJoinTest, SchemaEmitsTwoColumnsForCompositeFk) {
    // line_order_id, line_product_id must both exist as columns.
    const std::string &sql = create_table_sql<Shipment>();
    EXPECT_NE(sql.find("line_order_id"), std::string::npos) << sql;
    EXPECT_NE(sql.find("line_product_id"), std::string::npos) << sql;
}

// ── OptionalShipment plain (non-JOIN) bind/extract round trip ──────────────
// OptionalShipment::line is std::optional<OrderLineWithShipments> (a NULLABLE
// composite FK) — before this test, OptionalShipment was only ever exercised
// via create_table_sql<>() (pure DDL text, no bind/extract). These tests are
// the first to actually INSERT and plain-SELECT it, covering
// bind_fk_field_at_index's composite-optional branches (bind_null_run's
// has-no-value path, bind_fk_parts's has-a-value path) and
// extract_column_fast/extract_optional_fk_parts's composite-optional branches
// (both the NULL and the has-a-value path) on the PLAIN SELECT path — distinct
// from tests/query/test_composite_fk_join.cpp's JOIN-path coverage of the same
// model, which never touches extract_column_fast.
namespace {
template <typename ConnType>
class OptionalShipmentTest : public StormTestFixture<OptionalShipment, ConnType, OrderLineWithShipments> {};
} // namespace

TYPED_TEST_SUITE(OptionalShipmentTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(OptionalShipmentTest, PlainSelectRoundTripsNonNullCompositeFk) {
    const OrderLineWithShipments line{.order_id = 5, .product_id = 12, .quantity = 3, .note = "first"};
    ASSERT_TRUE((InsertStatement<OrderLineWithShipments, TypeParam>{}).query(line).execute().has_value());

    ASSERT_TRUE((InsertStatement<OptionalShipment, TypeParam>{})
                    .query(OptionalShipment{.line = line, .carrier = "UPS"})
                    .execute()
                    .has_value());

    auto rows = (SelectStatement<OptionalShipment, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    const OptionalShipment &loaded = *rows.value().begin();
    ASSERT_TRUE(loaded.line.has_value());
    EXPECT_EQ(loaded.line->order_id, 5);
    EXPECT_EQ(loaded.line->product_id, 12);
    EXPECT_EQ(loaded.carrier, "UPS");
}

TYPED_TEST(OptionalShipmentTest, PlainSelectRoundTripsNullCompositeFk) {
    ASSERT_TRUE((InsertStatement<OptionalShipment, TypeParam>{})
                    .query(OptionalShipment{.line = std::nullopt, .carrier = "FedEx"})
                    .execute()
                    .has_value());

    auto rows = (SelectStatement<OptionalShipment, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    const OptionalShipment &loaded = *rows.value().begin();
    EXPECT_FALSE(loaded.line.has_value());
    EXPECT_EQ(loaded.carrier, "FedEx");
}

// ── Review fix: composite-FK member before an optional single-column FK ────
// MixedFkOrder::composite (2 SQL columns) precedes MixedFkOrder::single (an
// optional single-column FK to Person). Before the fix, the optional
// single-column-FK extraction branch read column position = member index
// instead of the threaded col_idx, so `single` would read from the wrong
// column (composite's second column) once a composite-FK member preceded it.
namespace {
template <typename ConnType>
class MixedFkOrderTest : public StormTestFixture<MixedFkOrder, ConnType, OrderLineWithShipments, Person> {};

// Shared setup: insert and return an OrderLineWithShipments row used as
// MixedFkOrder::composite's target in both tests below.
template <typename ConnType>
auto insert_mixed_fk_order_line(int order_id, int product_id, std::string note) -> OrderLineWithShipments {
    const OrderLineWithShipments line{
        .order_id = order_id, .product_id = product_id, .quantity = 1, .note = std::move(note)};
    EXPECT_TRUE((InsertStatement<OrderLineWithShipments, ConnType>{}).query(line).execute().has_value());
    return line;
}

// Shared verification: insert `order`, select it back, and return the
// sole loaded row for the caller to assert on.
template <typename ConnType> auto insert_and_reload_mixed_fk_order(const MixedFkOrder &order) -> MixedFkOrder {
    EXPECT_TRUE((InsertStatement<MixedFkOrder, ConnType>{}).query(order).execute().has_value());

    auto rows = (SelectStatement<MixedFkOrder, ConnType>{}).all().execute();
    EXPECT_TRUE(rows.has_value());
    EXPECT_EQ(rows.value().size(), 1U);
    return *rows.value().begin();
}
} // namespace

TYPED_TEST_SUITE(MixedFkOrderTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(MixedFkOrderTest, ExtractsOptionalSingleColumnFkAfterPrecedingCompositeFk) {
    const OrderLineWithShipments line = insert_mixed_fk_order_line<TypeParam>(7, 42, "n");

    const Person owner{.name = "Carol", .age = 40};
    ASSERT_TRUE((InsertStatement<Person, TypeParam>{}).query(owner).execute().has_value());

    const MixedFkOrder order{.composite = line, .single = Person{.id = 1}};
    const MixedFkOrder loaded = insert_and_reload_mixed_fk_order<TypeParam>(order);
    EXPECT_EQ(loaded.composite.order_id, 7);
    EXPECT_EQ(loaded.composite.product_id, 42);
    ASSERT_TRUE(loaded.single.has_value());
    EXPECT_EQ(loaded.single->id, 1);
}

TYPED_TEST(MixedFkOrderTest, ExtractsNullOptionalSingleColumnFkAfterPrecedingCompositeFk) {
    const OrderLineWithShipments line = insert_mixed_fk_order_line<TypeParam>(8, 43, "n2");

    const MixedFkOrder order{.composite = line, .single = std::nullopt};
    const MixedFkOrder loaded = insert_and_reload_mixed_fk_order<TypeParam>(order);
    EXPECT_EQ(loaded.composite.order_id, 8);
    EXPECT_EQ(loaded.composite.product_id, 43);
    EXPECT_FALSE(loaded.single.has_value());
}

// ── Single-column FK regression ──────────────────────────────────────────────
// Message::sender (shared/models/message.h) is the long-standing single-column FK fixture.
// Its bind/extract behaviour must be byte-for-byte unchanged by the composite
// widening (the if constexpr branches key on fk_primary_key_count<FKType>()).

namespace {
template <typename ConnType> class SingleColumnFkRegressionTest : public StormTestFixture<Message, ConnType, Person> {};
} // namespace

TYPED_TEST_SUITE(SingleColumnFkRegressionTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(SingleColumnFkRegressionTest, InsertAndSelectRoundTripsSingleColumnFk) {
    const Person sender{.name = "Alice", .age = 30};
    ASSERT_TRUE((InsertStatement<Person, TypeParam>{}).query(sender).execute().has_value());

    const Message msg{.content = "hi", .sender = {.id = 1}};
    ASSERT_TRUE((InsertStatement<Message, TypeParam>{}).query(msg).execute().has_value());

    auto rows = (SelectStatement<Message, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_EQ(rows.value().begin()->sender.id, 1);
    EXPECT_EQ(rows.value().begin()->content, "hi");
}
