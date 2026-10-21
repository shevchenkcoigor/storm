#include <gtest/gtest.h>
#include "test_db_helpers.h"

// This TU supplies the SQLite half of test_reverse_fk_body.h;
// test_reverse_fk_pg.cpp supplies the other half. See that header's own comment
// for the compile-time-TU-split rationale.
#define STORM_SPLIT_TYPES DatabaseTypesSqliteHalf
#define STORM_SPLIT_TYPE_NAMES DatabaseTypesSqliteHalfNames
#include "test_reverse_fk_body.h"
using storm::create_table_sql;
#undef STORM_SPLIT_TYPE_NAMES
#undef STORM_SPLIT_TYPES

// ReverseFKSchemaTest and ReverseFKDisambigSqlTest live only in this half:
// they assert on create_table_sql<>()/get_complete_sql() text (the latter
// hardcodes storm::db::sqlite::Connection regardless of ConnType) —
// duplicating them into both split TUs would link-error on a repeated
// GoogleTest symbol.

// NOLINTBEGIN(misc-const-correctness) — matches the suppression these tests
// sat under in the pre-split file (test_reverse_fk_body.h:19).

TEST(ReverseFKSchemaTest, BaseTableHasNoReverseContainerColumn) {
    const auto& sql = create_table_sql<RfPerson>();
    EXPECT_FALSE(sql.contains("tasks")) << sql;
    EXPECT_TRUE(sql.contains("name")) << sql;
    EXPECT_TRUE(sql.contains("age")) << sql;
}

TEST(ReverseFKSchemaTest, OwnerTableHasFkColumn) {
    const auto& sql = create_table_sql<RfTask>();
    EXPECT_TRUE(sql.contains("assignee_id")) << sql;
}

// The two selectors generate different ON columns (author_id vs reviewer_id).
TEST(ReverseFKDisambigSqlTest, DistinctFkColumnsInJoinSql) {
    using AuthorJS = stmt::
            ReverseFKJoinStatement<RfReporter, storm::db::sqlite::Connection, stmt::JoinType::Left, ^^RfBug::author>;
    using ReviewerJS = stmt::
            ReverseFKJoinStatement<RfReporter, storm::db::sqlite::Connection, stmt::JoinType::Left, ^^RfBug::reviewer>;
    EXPECT_TRUE(AuthorJS::get_complete_sql().contains("t2.author_id = t1.id")) << AuthorJS::get_complete_sql();
    EXPECT_TRUE(ReviewerJS::get_complete_sql().contains("t2.reviewer_id = t1.id")) << ReviewerJS::get_complete_sql();
}

// NOLINTEND(misc-const-correctness)
