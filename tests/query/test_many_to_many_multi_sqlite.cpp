#include <gtest/gtest.h>
#include <meta>
#include "test_db_helpers.h"

// This TU supplies the SQLite half of test_many_to_many_multi_body.h;
// test_many_to_many_multi_pg.cpp supplies the other half. See that header's own comment
// for the compile-time-TU-split rationale.
#define STORM_SPLIT_TYPES DatabaseTypesSqliteHalf
#define STORM_SPLIT_TYPE_NAMES DatabaseTypesSqliteHalfNames
#include "test_many_to_many_multi_body.h"
using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;
#undef STORM_SPLIT_TYPE_NAMES
#undef STORM_SPLIT_TYPES

// MultiM2MSchemaTest.OneJunctionTablePerM2MField lives only in this half:
// it asserts on SchemaStatement<>::junction_table_sqls<Dialect>() text,
// which takes no ConnType — duplicating it into both split TUs would
// link-error on a repeated GoogleTest symbol.

// NOLINTBEGIN(misc-const-correctness) — matches the suppression this test sat
// under in the pre-split file (test_many_to_many_multi_body.h:19).

// ============================================================================
// Schema: one auto junction table per m2m field (#392)
// ============================================================================

TEST(MultiM2MSchemaTest, OneJunctionTablePerM2MField) {
    const auto& sqls = SchemaStatement<Member>::junction_table_sqls<Dialect::SQLite>();
    ASSERT_EQ(sqls.size(), 2U);
    EXPECT_TRUE(sqls[0].contains("CREATE TABLE Member_Course")) << sqls[0];
    EXPECT_TRUE(sqls[0].contains("PRIMARY KEY (Member_id, Course_id)")) << sqls[0];
    EXPECT_TRUE(sqls[1].contains("CREATE TABLE Member_Club")) << sqls[1];
    EXPECT_TRUE(sqls[1].contains("PRIMARY KEY (Member_id, Club_id)")) << sqls[1];
}

// NOLINTEND(misc-const-correctness)
