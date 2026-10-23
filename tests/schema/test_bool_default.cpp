// Tests for issue #344: the schema generator must emit a SQL DEFAULT clause for
// non-nullable bool columns that carry a C++ default member initializer, so that
// `ALTER TABLE ... ADD COLUMN <x> NOT NULL DEFAULT <v>` is valid on a populated
// table (SQLite rejects `ADD COLUMN ... NOT NULL` with no default when rows
// already exist).

#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_schema;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::primary;
using storm::db::DefaultConnection;

using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;

// Local models exercising both default values. Defined after `import storm;`
// so the storm:: annotation binds to the imported object.
struct BoolDefaultModel {
    [[= primary]] int id{};
    bool              flag_false{false};
    bool              flag_true{true};
};

// ============================================================================
// SQLite dialect — bool NOT NULL must carry DEFAULT 0 / DEFAULT 1
// ============================================================================

TEST(BoolDefaultSchema, SqliteFalseDefaultIsZero) {
    const std::string& sql = SchemaStatement<BoolDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("flag_false INTEGER NOT NULL DEFAULT 0"), std::string::npos)
            << "Expected 'flag_false INTEGER NOT NULL DEFAULT 0' in: " << sql;
}

TEST(BoolDefaultSchema, SqliteTrueDefaultIsOne) {
    const std::string& sql = SchemaStatement<BoolDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("flag_true INTEGER NOT NULL DEFAULT 1"), std::string::npos)
            << "Expected 'flag_true INTEGER NOT NULL DEFAULT 1' in: " << sql;
}

// Person.is_active{} value-initializes to false → DEFAULT 0 (regression guard).
TEST(BoolDefaultSchema, SqlitePersonIsActiveHasDefaultZero) {
    const std::string& sql = SchemaStatement<Person>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("is_active INTEGER NOT NULL DEFAULT 0"), std::string::npos)
            << "Expected 'is_active INTEGER NOT NULL DEFAULT 0' in: " << sql;
}

// ============================================================================
// PostgreSQL dialect — bool NOT NULL must carry DEFAULT FALSE / DEFAULT TRUE
// ============================================================================

TEST(BoolDefaultSchema, PgFalseDefaultIsFalse) {
    const std::string& sql = SchemaStatement<BoolDefaultModel>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("flag_false BOOLEAN NOT NULL DEFAULT FALSE"), std::string::npos)
            << "Expected 'flag_false BOOLEAN NOT NULL DEFAULT FALSE' in: " << sql;
}

TEST(BoolDefaultSchema, PgTrueDefaultIsTrue) {
    const std::string& sql = SchemaStatement<BoolDefaultModel>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("flag_true BOOLEAN NOT NULL DEFAULT TRUE"), std::string::npos)
            << "Expected 'flag_true BOOLEAN NOT NULL DEFAULT TRUE' in: " << sql;
}

TEST(BoolDefaultSchema, PgPersonIsActiveHasDefaultFalse) {
    const std::string& sql = SchemaStatement<Person>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("is_active BOOLEAN NOT NULL DEFAULT FALSE"), std::string::npos)
            << "Expected 'is_active BOOLEAN NOT NULL DEFAULT FALSE' in: " << sql;
}

// ============================================================================
// Since #413, a non-bool NOT NULL column with a default initializer (including
// `int age{};` value-init) DOES gain a DEFAULT — reflection cannot distinguish
// `{}` from `= 0`. This was the bool-only scope guard in #344; the general
// DEFAULT coverage lives in test_column_default.cpp.
// ============================================================================

TEST(BoolDefaultSchema, IntColumnGainsDefault) {
    const std::string& sql     = SchemaStatement<Person>::create_table_sql<Dialect::SQLite>();
    const std::size_t  age_pos = sql.find("age INTEGER NOT NULL DEFAULT 0");
    EXPECT_NE(age_pos, std::string::npos) << "int 'age' column should carry DEFAULT 0: " << sql;
}

// ============================================================================
// The actual break: ADD COLUMN bool NOT NULL on a populated table must succeed.
//
// This integration test runs against both backends. It creates a one-column
// table, inserts a row, then issues the ALTER produced for a bool NOT NULL
// column (with the DEFAULT the generator now emits) and asserts it succeeds.
// ============================================================================

template <typename ConnType> class BoolDefaultAlterTest : public StormTestFixture<Person, ConnType> {
  protected:
    // Skip Person table creation — this fixture builds its own bare table.
    auto on_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {}
};

TYPED_TEST_SUITE(BoolDefaultAlterTest, DatabaseTypes);

TYPED_TEST(BoolDefaultAlterTest, AddBoolNotNullColumnOnPopulatedTableSucceeds) {
    const auto& conn = DefaultConnection<TypeParam>::get();

    // Bare table with a single row already present.
    ASSERT_TRUE(conn->execute("CREATE TABLE t (id INTEGER PRIMARY KEY)").has_value());
    ASSERT_TRUE(conn->execute("INSERT INTO t (id) VALUES (1)").has_value());

    // The generator-shaped ALTER for a bool NOT NULL column carries a DEFAULT,
    // so this must succeed on a populated table.
    const std::string alter = storm::test::is_postgresql<TypeParam>()
                                      ? "ALTER TABLE t ADD COLUMN is_raw BOOLEAN NOT NULL DEFAULT FALSE"
                                      : "ALTER TABLE t ADD COLUMN is_raw INTEGER NOT NULL DEFAULT 0";

    auto result = conn->execute(alter);
    ASSERT_TRUE(result.has_value()) << "ADD COLUMN NOT NULL DEFAULT on populated table failed: "
                                    << result.error().message();
}
