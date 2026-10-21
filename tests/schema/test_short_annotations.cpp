#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

import storm_defaults;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_schema;
import std;
using storm::create_table_sql;
using storm::primary;
using storm::primary_autoincrement;
using storm::RefAction;

// ── #442: the short top-level annotation spelling ────────────────────────────
// The user-facing annotation names are re-exported into the top-level `storm`
// namespace so model declarations can drop the `meta::` level. This file proves:
//   1. the short names resolve to the SAME entities as storm::meta::* (compile-time)
//   2. a model annotated with the short spelling generates byte-identical DDL to
//      a structurally-identical model annotated the long way.

// The short name and the long name must be the very same entity, not a copy.
// The flag annotation objects (#492) are re-exported as the same inline constexpr
// objects; &short == &long proves they are one entity, not a copy. clang-tidy's
// misc-redundant-expression sees the two operands as identical *after* name
// resolution and flags them — but resolving `storm::x` and `storm::meta::x` to the
// same address is exactly what the assert verifies, so the check is a false positive.
// NOLINTBEGIN(misc-redundant-expression) — comparing the short and long spelling of the
// SAME re-exported object is the point; tidy sees the operands as identical after name lookup.
static_assert(&primary == &storm::meta::primary);
static_assert(&storm::unique == &storm::meta::unique);
static_assert(&primary_autoincrement == &storm::meta::primary_autoincrement);
// NOLINTEND(misc-redundant-expression)
static_assert(std::is_same_v<RefAction, storm::meta::RefAction>);
static_assert(std::is_same_v<decltype(storm::fk<>), decltype(storm::meta::fk<>)>);
static_assert(std::is_same_v<
              decltype(storm::fk<RefAction::Cascade>),
              decltype(storm::meta::fk<storm::meta::RefAction::Cascade>)>);

// One shared parent, referenced by both children so the REFERENCES target is
// identical — the only remaining difference between the two DDLs is the child
// table's own name prefix (Short vs Long).
struct SharedParent {
    [[= storm::meta::primary]] int        id{};
    [[= storm::meta::unique]] std::string name;
};

// A model annotated entirely through the short `storm::` spelling.
struct ShortChild {
    [[= primary]] int                                id{};
    [[= storm::fk<>]] SharedParent                   parent;
    [[= storm::fk<RefAction::Cascade>]] SharedParent owner;
    std::string                                      label;
};

// A structurally-identical model annotated the long `storm::meta::` way.
struct LongChild {
    [[= storm::meta::primary]] int                                      id{};
    [[= storm::meta::fk<>]] SharedParent                                parent;
    [[= storm::meta::fk<storm::meta::RefAction::Cascade>]] SharedParent owner;
    std::string                                                         label;
};

// Replace the child table name with a placeholder so the only difference between
// the two DDL strings is canonicalised away — everything else (columns,
// REFERENCES, ON DELETE) must match exactly if the two spellings are equivalent.
namespace {
    auto strip_table_name(std::string sql, std::string_view table) -> std::string {
        std::size_t pos = sql.find(table);
        while (pos != std::string::npos) {
            sql.replace(pos, table.size(), "T");
            pos = sql.find(table, pos + 1);
        }
        return sql;
    }

    // Assert the short-spelled ShortChild DDL equals the long-spelled LongChild DDL
    // for one dialect, after canonicalising away the differing table identifier.
    void expect_ddl_match(std::string_view short_sql, std::string_view long_sql) {
        const std::string diag = std::format("short:\n{}\nlong:\n{}", short_sql, long_sql);
        EXPECT_EQ(
                strip_table_name(std::string(short_sql), "ShortChild"),
                strip_table_name(std::string(long_sql), "LongChild")
        ) << diag;
    }
} // namespace

TEST(ShortAnnotationsTest, SqliteDdlMatchesLongSpelling) {
    expect_ddl_match(create_table_sql<ShortChild>(), create_table_sql<LongChild>());
}

TEST(ShortAnnotationsTest, PostgresDdlMatchesLongSpelling) {
    using orm = storm::orm::schema::Dialect;
    expect_ddl_match(create_table_sql<ShortChild, orm::PostgreSQL>(), create_table_sql<LongChild, orm::PostgreSQL>());
}

// The ON DELETE CASCADE policy carried through the short fk<> spelling must reach
// the DDL (proves the RefAction template arg survives the re-export).
TEST(ShortAnnotationsTest, ShortSpellingEmitsOnDeleteCascade) {
    const std::string& sql = create_table_sql<ShortChild>();
    EXPECT_TRUE(sql.contains("ON DELETE CASCADE")) << sql;
}

// NOLINTEND(misc-const-correctness)
