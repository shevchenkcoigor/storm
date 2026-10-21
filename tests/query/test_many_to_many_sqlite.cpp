#include <gtest/gtest.h>
#include "test_db_helpers.h"

// This TU supplies the SQLite half of test_many_to_many_body.h;
// test_many_to_many_pg.cpp supplies the other half. See that header's own comment
// for the compile-time-TU-split rationale.
#define STORM_SPLIT_TYPES DatabaseTypesSqliteHalf
#define STORM_SPLIT_TYPE_NAMES DatabaseTypesSqliteHalfNames
#include "test_many_to_many_body.h"
using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;
#undef STORM_SPLIT_TYPE_NAMES
#undef STORM_SPLIT_TYPES

// M2MSchemaTest (junction-table DDL text, both dialects) lives only in
// this half: it asserts on SchemaStatement<>::junction_table_sql<Dialect>()
// text, which takes no ConnType — duplicating it into both split TUs would
// link-error on a repeated GoogleTest symbol.

// NOLINTBEGIN(misc-const-correctness) — matches the suppression these tests
// sat under in the pre-split file (test_many_to_many_body.h:19).

// ============================================================================
// Schema: auto-generated junction table DDL (#203 Phase 1)
// ============================================================================

TEST(M2MSchemaTest, JunctionTableSqlSQLite) {
    const auto& sql = SchemaStatement<Student>::junction_table_sql<Dialect::SQLite>();
    EXPECT_TRUE(sql.contains("CREATE TABLE Student_Course")) << sql;
    EXPECT_TRUE(sql.contains("Student_id INTEGER NOT NULL")) << sql;
    EXPECT_TRUE(sql.contains("Course_id INTEGER NOT NULL")) << sql;
    EXPECT_TRUE(sql.contains("PRIMARY KEY (Student_id, Course_id)")) << sql;
}

TEST(M2MSchemaTest, JunctionTableSqlPostgreSQL) {
    const auto& sql = SchemaStatement<Student>::junction_table_sql<Dialect::PostgreSQL>();
    EXPECT_TRUE(sql.contains("CREATE TABLE Student_Course")) << sql;
    EXPECT_TRUE(sql.contains("Student_id BIGINT NOT NULL")) << sql;
    EXPECT_TRUE(sql.contains("Course_id BIGINT NOT NULL")) << sql;
}

// ── Referential integrity on the auto-junction (#412) ───────────────────────
// Each junction column carries a FOREIGN KEY ... REFERENCES ... ON DELETE
// CASCADE so deleting an owner/related row removes its junction rows instead of
// leaving orphans. The composite PK still rejects duplicate pairs.

// Both sides must carry FOREIGN KEY ... REFERENCES ... ON DELETE CASCADE; shared
// by the SQLite and PostgreSQL cases (the junction FK DDL is dialect-independent).
namespace {
    void expect_junction_cascade_fks(std::string_view sql) {
        EXPECT_TRUE(sql.contains("FOREIGN KEY (Student_id) REFERENCES Student(id) ON DELETE CASCADE")) << sql;
        EXPECT_TRUE(sql.contains("FOREIGN KEY (Course_id) REFERENCES Course(id) ON DELETE CASCADE")) << sql;
    }
} // namespace

TEST(M2MSchemaTest, JunctionEmitsForeignKeysSqlite) {
    expect_junction_cascade_fks(SchemaStatement<Student>::junction_table_sql<Dialect::SQLite>());
}

TEST(M2MSchemaTest, JunctionEmitsForeignKeysPostgreSQL) {
    expect_junction_cascade_fks(SchemaStatement<Student>::junction_table_sql<Dialect::PostgreSQL>());
}

// Junction ON DELETE override (#431): many_to_many<RefAction::Restrict>
// on the m2m field flips BOTH junction sides from the CASCADE default to RESTRICT.
namespace {
    void expect_junction_restrict_fks(std::string_view sql) {
        EXPECT_TRUE(
                sql.contains("FOREIGN KEY (RestrictedMember_id) REFERENCES RestrictedMember(id) ON DELETE RESTRICT")
        ) << sql;
        EXPECT_TRUE(sql.contains("FOREIGN KEY (RestrictedClub_id) REFERENCES RestrictedClub(id) ON DELETE RESTRICT"))
                << sql;
        EXPECT_FALSE(sql.contains("ON DELETE CASCADE")) << "override must replace CASCADE on both sides: " << sql;
    }
} // namespace

TEST(M2MSchemaTest, JunctionOnDeleteOverrideSqlite) {
    expect_junction_restrict_fks(SchemaStatement<RestrictedMember>::junction_table_sql<Dialect::SQLite>());
}

TEST(M2MSchemaTest, JunctionOnDeleteOverridePostgreSQL) {
    expect_junction_restrict_fks(SchemaStatement<RestrictedMember>::junction_table_sql<Dialect::PostgreSQL>());
}

// NOLINTEND(misc-const-correctness)
