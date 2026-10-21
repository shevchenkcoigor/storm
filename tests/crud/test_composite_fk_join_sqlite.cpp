#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

// This TU supplies the SQLite half of test_composite_fk_join_body.h;
// test_composite_fk_join_pg.cpp supplies the other half. See that header's own comment
// for the compile-time-TU-split rationale.
#define STORM_SPLIT_TYPES DatabaseTypesSqliteHalf
#define STORM_SPLIT_TYPE_NAMES DatabaseTypesSqliteHalfNames
#include "test_composite_fk_join_body.h"
using storm::create_table_sql;
#undef STORM_SPLIT_TYPE_NAMES
#undef STORM_SPLIT_TYPES

// CompositeFkSchemaTest lives only in this half: it asserts on
// create_table_sql<>() text, which takes no ConnType — duplicating it
// into both split TUs would link-error on a repeated GoogleTest symbol.

// ── Review fix: nullable composite-FK DDL must not force NOT NULL ───────────
// OptionalShipment::line is std::optional<OrderLineWithShipments> — the
// generated columns (line_order_id, line_product_id) must NOT carry NOT NULL,
// since the FK member itself is nullable. Also proves the non-optional case
// (Shipment above) never emitted a doubled "NOT NULL NOT NULL" suffix.
TEST(CompositeFkSchemaTest, NullableCompositeFkColumnsOmitNotNull) {
    const std::string& sql = create_table_sql<OptionalShipment>();
    EXPECT_NE(sql.find("line_order_id INTEGER,"), std::string::npos) << sql;
    EXPECT_NE(sql.find("line_product_id INTEGER,"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("line_order_id INTEGER NOT NULL"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("line_product_id INTEGER NOT NULL"), std::string::npos) << sql;
}

TEST(CompositeFkSchemaTest, NonOptionalCompositeFkColumnsHaveExactlyOneNotNull) {
    const std::string& sql = create_table_sql<Shipment>();
    EXPECT_EQ(sql.find("NOT NULL NOT NULL"), std::string::npos) << sql;
}
