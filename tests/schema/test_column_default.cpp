// Tests for issue #413: generalize the schema DEFAULT clause beyond bool. A
// non-nullable int/int64/double/float/string column that carries a C++ default
// member initializer must emit a SQL DEFAULT clause, so that
// `ALTER TABLE ... ADD COLUMN <x> NOT NULL DEFAULT <v>` is valid on a populated
// table (SQLite rejects `ADD COLUMN ... NOT NULL` with no default when rows
// already exist). Follow-up to #344 (which covered only bool).

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

// Local model exercising every covered scalar/text default. Defined after
// `import storm;` so the storm:: annotation binds to the imported object.
struct ColumnDefaultModel {
    [[= primary]] int id{};
    int               priority{1};
    std::int64_t      big{1000000000000};
    int               neg{-5};
    double            rate{0.0};
    double            pi{3.14};
    float             ratio{2.5F};
    std::string       status{"new"};
    std::string       tricky{"O'Brien"};
};

// A model whose non-bool columns carry NO default initializer at all (not even
// `{}`) — must keep the no-DEFAULT shape. Reflection cannot distinguish `int x{};`
// from `int x = 0;`, so the "no DEFAULT" guarantee only holds for members with no
// initializer token whatsoever.
struct NoDefaultModel {
    [[= primary]] int id{};
    int               count;
    std::string       label;
};

// ============================================================================
// SQLite dialect — integer / int64 / double / float / text DEFAULTs
// ============================================================================

TEST(ColumnDefaultSchema, SqliteIntDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("priority INTEGER NOT NULL DEFAULT 1"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, SqliteInt64Default) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("big INTEGER NOT NULL DEFAULT 1000000000000"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, SqliteNegativeIntDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("neg INTEGER NOT NULL DEFAULT -5"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, SqliteZeroDoubleDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("rate REAL NOT NULL DEFAULT 0.0"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, SqliteDoubleDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("pi REAL NOT NULL DEFAULT 3.14"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, SqliteFloatDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("ratio REAL NOT NULL DEFAULT 2.5"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, SqliteTextDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("status TEXT NOT NULL DEFAULT 'new'"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, SqliteTextDefaultEscapesQuote) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::SQLite>();
    EXPECT_NE(sql.find("tricky TEXT NOT NULL DEFAULT 'O''Brien'"), std::string::npos) << sql;
}

// ============================================================================
// PostgreSQL dialect — same values, dialect-specific column types
// ============================================================================

TEST(ColumnDefaultSchema, PgIntDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("priority INTEGER NOT NULL DEFAULT 1"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, PgDoubleDefault) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("pi DOUBLE PRECISION NOT NULL DEFAULT 3.14"), std::string::npos) << sql;
}

TEST(ColumnDefaultSchema, PgTextDefaultEscapesQuote) {
    const std::string& sql = SchemaStatement<ColumnDefaultModel>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("tricky TEXT NOT NULL DEFAULT 'O''Brien'"), std::string::npos) << sql;
}

// ============================================================================
// Columns WITHOUT a default initializer keep the no-DEFAULT shape (scope guard)
// ============================================================================

TEST(ColumnDefaultSchema, SqliteNoInitializerHasNoDefault) {
    const std::string& sql       = SchemaStatement<NoDefaultModel>::create_table_sql<Dialect::SQLite>();
    const std::size_t  count_pos = sql.find("count INTEGER NOT NULL");
    ASSERT_NE(count_pos, std::string::npos) << sql;
    EXPECT_EQ(sql.substr(count_pos, 32).find("DEFAULT"), std::string::npos) << sql.substr(count_pos, 32);
}

TEST(ColumnDefaultSchema, SqliteNoInitializerTextHasNoDefault) {
    const std::string& sql       = SchemaStatement<NoDefaultModel>::create_table_sql<Dialect::SQLite>();
    const std::size_t  label_pos = sql.find("label TEXT NOT NULL");
    ASSERT_NE(label_pos, std::string::npos) << sql;
    EXPECT_EQ(sql.substr(label_pos, 32).find("DEFAULT"), std::string::npos) << sql.substr(label_pos, 32);
}

// ============================================================================
// The actual break: ADD COLUMN <non-bool> NOT NULL on a populated table must
// succeed because the generator now emits a DEFAULT. Runs on both backends.
// ============================================================================

template <typename ConnType> class ColumnDefaultAlterTest : public StormTestFixture<Person, ConnType> {
  public:
    // Skip Person table creation — this fixture builds its own bare table.
    auto on_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {}
};

TYPED_TEST_SUITE(ColumnDefaultAlterTest, DatabaseTypes);

TYPED_TEST(ColumnDefaultAlterTest, AddIntNotNullColumnOnPopulatedTableSucceeds) {
    const auto& conn = DefaultConnection<TypeParam>::get();

    ASSERT_TRUE(conn->execute("CREATE TABLE t (id INTEGER PRIMARY KEY)").has_value());
    ASSERT_TRUE(conn->execute("INSERT INTO t (id) VALUES (1)").has_value());

    const std::string alter = storm::test::is_postgresql<TypeParam>()
                                      ? "ALTER TABLE t ADD COLUMN priority INTEGER NOT NULL DEFAULT 1"
                                      : "ALTER TABLE t ADD COLUMN priority INTEGER NOT NULL DEFAULT 1";

    auto result = conn->execute(alter);
    ASSERT_TRUE(result.has_value()) << "ADD COLUMN int NOT NULL DEFAULT on populated table failed: "
                                    << result.error().message();
}

TYPED_TEST(ColumnDefaultAlterTest, AddTextNotNullColumnOnPopulatedTableSucceeds) {
    const auto& conn = DefaultConnection<TypeParam>::get();

    ASSERT_TRUE(conn->execute("CREATE TABLE t2 (id INTEGER PRIMARY KEY)").has_value());
    ASSERT_TRUE(conn->execute("INSERT INTO t2 (id) VALUES (1)").has_value());

    auto result = conn->execute("ALTER TABLE t2 ADD COLUMN status TEXT NOT NULL DEFAULT 'new'");
    ASSERT_TRUE(result.has_value()) << "ADD COLUMN text NOT NULL DEFAULT on populated table failed: "
                                    << result.error().message();
}

// The DEFAULT clause tests above assert generated SQL text only. This exercises
// the same ColumnDefaultModel's CREATE TABLE through a live connection on both
// backends, so a DEFAULT value the dialect's parser actually rejects (#568 class)
// would fail here instead of shipping behind a passing text assertion.
TYPED_TEST(ColumnDefaultAlterTest, CreateTableWithDefaultsSucceeds) {
    const auto& conn   = DefaultConnection<TypeParam>::get();
    auto        result = SchemaStatement<ColumnDefaultModel>::create_table_if_not_exists(conn);
    ASSERT_TRUE(result.has_value()) << "ColumnDefaultModel DDL failed: " << result.error().message();
}
