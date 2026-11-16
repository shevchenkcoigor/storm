#include <gtest/gtest.h>
#include "test_db_helpers.h"

// readability-implicit-bool-conversion: false positive from GTest's
// EXPECT_TRUE(cond) << "msg" — clang-tidy misreads the streamed message literal.
// NOLINTBEGIN(misc-const-correctness,misc-use-anonymous-namespace,readability-implicit-bool-conversion)

import storm;
import std;

#include "../../shared/models/person.h"
#include "test_fixture.h"

using storm::QuerySet;

// ============================================================================
// Test fixture
// ============================================================================

template <typename ConnType> class SqlInspectionTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(SqlInspectionTest, DatabaseTypes);

// ============================================================================
// Helper: check that string contains a substring
// ============================================================================

static auto contains(std::string_view str, std::string_view substr) -> bool {
    return str.contains(substr);
}

// ============================================================================
// SELECT .to_sql() tests
// ============================================================================

TYPED_TEST(SqlInspectionTest, SelectBareToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_FALSE(sql.empty()) << "SQL should not be empty";
    EXPECT_TRUE(contains(sql, "SELECT")) << "Should contain SELECT";
    EXPECT_TRUE(contains(sql, R"("person")")) << "Should contain table name";
    EXPECT_TRUE(contains(sql, "name")) << "Should contain field 'name'";
    EXPECT_TRUE(contains(sql, "age")) << "Should contain field 'age'";
    // No WHERE clause
    EXPECT_FALSE(contains(sql, "WHERE")) << "Bare select should have no WHERE";
}

TYPED_TEST(SqlInspectionTest, SelectWithWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age > 30).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "SELECT")) << "Should contain SELECT";
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE";
    EXPECT_TRUE(contains(sql, "age")) << "Should contain field 'age'";
    // Value 30 should appear in the SQL (either as 30 or '30')
    EXPECT_TRUE(contains(sql, "30")) << "Should contain value 30";
}

TYPED_TEST(SqlInspectionTest, SelectWithStringWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.name == "Alice").select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE";
    EXPECT_TRUE(contains(sql, "Alice")) << "Should contain string value 'Alice'";
}

TYPED_TEST(SqlInspectionTest, SelectWithLimitToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.limit(10).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "LIMIT")) << "Should contain LIMIT";
    EXPECT_TRUE(contains(sql, "10")) << "Should contain limit value";
}

TYPED_TEST(SqlInspectionTest, SelectWithOffsetToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.limit(5).offset(10).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "LIMIT")) << "Should contain LIMIT";
    EXPECT_TRUE(contains(sql, "OFFSET")) << "Should contain OFFSET";
    EXPECT_TRUE(contains(sql, "10")) << "Should contain offset value";
}

TYPED_TEST(SqlInspectionTest, SelectWithOrderByToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.template order_by<fields::Person.age>().select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "ORDER BY")) << "Should contain ORDER BY";
    EXPECT_TRUE(contains(sql, "age")) << "Should contain sorted field";
}

TYPED_TEST(SqlInspectionTest, SelectWithWhereAndLimitToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age > 25).limit(5).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE";
    EXPECT_TRUE(contains(sql, "LIMIT")) << "Should contain LIMIT";
    EXPECT_TRUE(contains(sql, "25")) << "Should contain WHERE value";
    EXPECT_TRUE(contains(sql, "5")) << "Should contain LIMIT value";
}

TYPED_TEST(SqlInspectionTest, SelectOperatorsToSql) {
    QuerySet<Person, TypeParam> qs;

    // Test all comparison operators generate proper SQL
    auto r_eq = qs.where(fields::Person.age == 30).select().to_sql();
    auto r_ne = qs.where(fields::Person.age != 30).select().to_sql();
    auto r_gt = qs.where(fields::Person.age > 30).select().to_sql();
    auto r_ge = qs.where(fields::Person.age >= 30).select().to_sql();
    auto r_lt = qs.where(fields::Person.age < 30).select().to_sql();
    auto r_le = qs.where(fields::Person.age <= 30).select().to_sql();

    ASSERT_TRUE(r_eq.has_value());
    ASSERT_TRUE(r_ne.has_value());
    ASSERT_TRUE(r_gt.has_value());
    ASSERT_TRUE(r_ge.has_value());
    ASSERT_TRUE(r_lt.has_value());
    ASSERT_TRUE(r_le.has_value());

    EXPECT_TRUE(contains(r_eq.value(), "=")) << "EQ should use =";
    EXPECT_TRUE(contains(r_ne.value(), "!=") || contains(r_ne.value(), "<>")) << "NE should use != or <>";
    EXPECT_TRUE(contains(r_gt.value(), ">")) << "GT should use >";
    EXPECT_TRUE(contains(r_ge.value(), ">=")) << "GE should use >=";
    EXPECT_TRUE(contains(r_lt.value(), "<")) << "LT should use <";
    EXPECT_TRUE(contains(r_le.value(), "<=")) << "LE should use <=";
}

TYPED_TEST(SqlInspectionTest, SelectWithInToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age.in(20, 25, 30)).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "IN")) << "Should contain IN";
    EXPECT_TRUE(contains(sql, "20")) << "Should contain first value";
    EXPECT_TRUE(contains(sql, "25")) << "Should contain second value";
    EXPECT_TRUE(contains(sql, "30")) << "Should contain third value";
}

TYPED_TEST(SqlInspectionTest, SelectWithBetweenToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age.between(20, 40)).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "BETWEEN")) << "Should contain BETWEEN";
    EXPECT_TRUE(contains(sql, "20")) << "Should contain lower bound";
    EXPECT_TRUE(contains(sql, "40")) << "Should contain upper bound";
}

TYPED_TEST(SqlInspectionTest, SelectWithLikeToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.name.like("A%")).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "LIKE")) << "Should contain LIKE";
    EXPECT_TRUE(contains(sql, "A%")) << "Should contain LIKE pattern";
}

TYPED_TEST(SqlInspectionTest, SelectWithAndOrToSql) {
    QuerySet<Person, TypeParam> qs;
    auto result = qs.where((fields::Person.age > 25) && (fields::Person.age < 50)).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "AND")) << "Should contain AND";
    EXPECT_TRUE(contains(sql, "25")) << "Should contain lower bound value";
    EXPECT_TRUE(contains(sql, "50")) << "Should contain upper bound value";
}

// ============================================================================
// FIRST .to_sql() tests
// ============================================================================

TYPED_TEST(SqlInspectionTest, FirstBareToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.first().to_sql();
    ASSERT_TRUE(result.has_value()) << "first().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "SELECT")) << "Should contain SELECT";
    EXPECT_TRUE(contains(sql, "LIMIT 1")) << "first() should use LIMIT 1";
}

TYPED_TEST(SqlInspectionTest, FirstWithWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age > 25).first().to_sql();
    ASSERT_TRUE(result.has_value()) << "first().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE";
    EXPECT_TRUE(contains(sql, "LIMIT 1")) << "first() should use LIMIT 1";
    EXPECT_TRUE(contains(sql, "25")) << "Should contain value";
}

TYPED_TEST(SqlInspectionTest, FirstWithOrderByToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.template order_by<fields::Person.age>().first().to_sql();
    ASSERT_TRUE(result.has_value()) << "first().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "ORDER BY")) << "Should contain ORDER BY";
    EXPECT_TRUE(contains(sql, "LIMIT 1")) << "first() should use LIMIT 1";
}

// ============================================================================
// GET .to_sql() tests
// ============================================================================

TYPED_TEST(SqlInspectionTest, GetBareToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.get().to_sql();
    ASSERT_TRUE(result.has_value()) << "get().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "SELECT")) << "Should contain SELECT";
    EXPECT_TRUE(contains(sql, "LIMIT 2")) << "get() should use LIMIT 2";
}

TYPED_TEST(SqlInspectionTest, GetWithWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.id == 42).get().to_sql();
    ASSERT_TRUE(result.has_value()) << "get().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE";
    EXPECT_TRUE(contains(sql, "LIMIT 2")) << "get() should use LIMIT 2";
    EXPECT_TRUE(contains(sql, "42")) << "Should contain value";
}

// ============================================================================
// INSERT .to_sql() tests
// ============================================================================

TYPED_TEST(SqlInspectionTest, InsertSingleToSql) {
    QuerySet<Person, TypeParam> qs;
    Person                      alice{.id = 0, .name = "Alice", .age = 30};

    auto result = qs.insert(alice).to_sql();
    ASSERT_TRUE(result.has_value()) << "insert().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "INSERT INTO")) << "Should contain INSERT INTO";
    EXPECT_TRUE(contains(sql, R"("person")")) << "Should contain table name";
    EXPECT_TRUE(contains(sql, "VALUES")) << "Should contain VALUES";
    EXPECT_TRUE(contains(sql, "Alice")) << "Should contain name value";
    EXPECT_TRUE(contains(sql, "30")) << "Should contain age value";
    EXPECT_TRUE(contains(sql, "RETURNING")) << "Both SQLite 3.35+ and PostgreSQL support RETURNING";
}

TYPED_TEST(SqlInspectionTest, InsertSingleToSqlDoesNotExecute) {
    QuerySet<Person, TypeParam> qs;
    Person                      alice{.id = 0, .name = "Alice", .age = 30};

    // Calling to_sql() should NOT insert into the database
    auto sql_result = qs.insert(alice).to_sql();
    ASSERT_TRUE(sql_result.has_value());

    // Table should still be empty
    auto select_result = qs.select().execute();
    ASSERT_TRUE(select_result.has_value());
    EXPECT_TRUE(select_result.value().empty()) << "to_sql() should not insert data";
}

TYPED_TEST(SqlInspectionTest, InsertBulkToSql) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         people = {
            Person{.id = 0, .name = "Alice", .age = 30},
            Person{.id = 0, .name = "Bob", .age = 25},
    };

    auto result = qs.insert(std::span<const Person>(people)).to_sql();
    ASSERT_TRUE(result.has_value()) << "bulk insert().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "INSERT INTO")) << "Should contain INSERT INTO";
    EXPECT_TRUE(contains(sql, "VALUES")) << "Should contain VALUES";
    EXPECT_TRUE(contains(sql, "Alice")) << "Should contain first name";
    EXPECT_TRUE(contains(sql, "Bob")) << "Should contain second name";
}

TYPED_TEST(SqlInspectionTest, InsertWithSpecialCharsToSql) {
    QuerySet<Person, TypeParam> qs;
    Person                      tricky{.id = 0, .name = "O'Brien", .age = 35};

    auto result = qs.insert(tricky).to_sql();
    ASSERT_TRUE(result.has_value()) << "insert().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_FALSE(sql.empty());
    EXPECT_TRUE(contains(sql, "O")) << "Should contain part of name";
    EXPECT_TRUE(contains(sql, "Brien")) << "Should contain part of name";
}

// ============================================================================
// UPDATE .to_sql() tests
// ============================================================================

TYPED_TEST(SqlInspectionTest, UpdateSingleToSql) {
    QuerySet<Person, TypeParam> qs;
    // First insert to get a valid ID
    Person alice{.id = 0, .name = "Alice", .age = 30};
    auto   insert_result = qs.insert(alice).execute();
    ASSERT_TRUE(insert_result.has_value());
    alice.id = static_cast<int>(insert_result.value());

    alice.age   = 31;
    auto result = qs.update(alice).to_sql();
    ASSERT_TRUE(result.has_value()) << "update().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "UPDATE")) << "Should contain UPDATE";
    EXPECT_TRUE(contains(sql, R"("person")")) << "Should contain table name";
    EXPECT_TRUE(contains(sql, "SET")) << "Should contain SET";
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE";
    EXPECT_TRUE(contains(sql, "31")) << "Should contain updated age value";
}

TYPED_TEST(SqlInspectionTest, UpdateToSqlDoesNotExecute) {
    QuerySet<Person, TypeParam> qs;
    Person                      alice{.id = 0, .name = "Alice", .age = 30};
    auto                        insert_result = qs.insert(alice).execute();
    ASSERT_TRUE(insert_result.has_value());
    alice.id = static_cast<int>(insert_result.value());

    // Get SQL without executing
    Person modified = alice;
    modified.age    = 99;
    auto sql_result = qs.update(modified).to_sql();
    ASSERT_TRUE(sql_result.has_value());

    // Check original value unchanged
    auto select_result = qs.select().execute();
    ASSERT_TRUE(select_result.has_value());
    ASSERT_EQ(select_result.value().size(), 1U);
    EXPECT_EQ(select_result.value().begin()->age, 30) << "to_sql() should not update data";
}

// ============================================================================
// REMOVE .to_sql() tests
// ============================================================================

TYPED_TEST(SqlInspectionTest, RemoveSingleToSql) {
    QuerySet<Person, TypeParam> qs;
    Person                      alice{.id = 0, .name = "Alice", .age = 30};
    auto                        insert_result = qs.insert(alice).execute();
    ASSERT_TRUE(insert_result.has_value());
    alice.id = static_cast<int>(insert_result.value());

    auto result = qs.erase(alice).to_sql();
    ASSERT_TRUE(result.has_value()) << "erase().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "DELETE FROM")) << "Should contain DELETE FROM";
    EXPECT_TRUE(contains(sql, R"("person")")) << "Should contain table name";
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE (id condition)";
}

TYPED_TEST(SqlInspectionTest, RemoveToSqlDoesNotExecute) {
    QuerySet<Person, TypeParam> qs;
    Person                      alice{.id = 0, .name = "Alice", .age = 30};
    auto                        insert_result = qs.insert(alice).execute();
    ASSERT_TRUE(insert_result.has_value());
    alice.id = static_cast<int>(insert_result.value());

    // Get SQL without executing
    auto sql_result = qs.erase(alice).to_sql();
    ASSERT_TRUE(sql_result.has_value());

    // Row should still be there
    auto select_result = qs.select().execute();
    ASSERT_TRUE(select_result.has_value());
    EXPECT_EQ(select_result.value().size(), 1U) << "to_sql() should not delete data";
}

TYPED_TEST(SqlInspectionTest, RemoveBulkToSql) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         people = {
            Person{.id = 0, .name = "Alice", .age = 30},
            Person{.id = 0, .name = "Bob", .age = 25},
    };

    // Insert and get IDs
    for (auto& p : people) {
        auto r = qs.insert(p).execute();
        ASSERT_TRUE(r.has_value());
        p.id = static_cast<int>(r.value());
    }

    auto result = qs.erase(std::span<const Person>(people)).to_sql();
    ASSERT_TRUE(result.has_value()) << "bulk erase().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "DELETE FROM")) << "Should contain DELETE FROM";
    EXPECT_FALSE(sql.empty()) << "Bulk delete SQL should not be empty";
}

// ============================================================================
// No side effects: execute() still works after to_sql()
// ============================================================================

TYPED_TEST(SqlInspectionTest, ToSqlAndExecuteAreIndependent) {
    QuerySet<Person, TypeParam> qs;
    Person                      alice{.id = 0, .name = "Alice", .age = 30};

    // Call to_sql() first
    auto sql_result = qs.insert(alice).to_sql();
    ASSERT_TRUE(sql_result.has_value());
    EXPECT_FALSE(sql_result.value().empty());

    // Then call execute() — should succeed
    auto exec_result = qs.insert(alice).execute();
    ASSERT_TRUE(exec_result.has_value());

    // Data should now be in the database
    auto select_result = qs.select().execute();
    ASSERT_TRUE(select_result.has_value());
    EXPECT_EQ(select_result.value().size(), 1U);
}

TYPED_TEST(SqlInspectionTest, SelectToSqlAndExecuteAreIndependent) {
    QuerySet<Person, TypeParam> qs;
    Person                      alice{.id = 0, .name = "Alice", .age = 30};
    ASSERT_TRUE(qs.insert(alice).execute().has_value());

    // Call to_sql()
    auto sql_result = qs.select().to_sql();
    ASSERT_TRUE(sql_result.has_value());
    EXPECT_FALSE(sql_result.value().empty());

    // Call execute() — should return data
    auto exec_result = qs.select().execute();
    ASSERT_TRUE(exec_result.has_value());
    EXPECT_EQ(exec_result.value().size(), 1U);
}

// ============================================================================
// Empty span edge case
// ============================================================================

TYPED_TEST(SqlInspectionTest, InsertEmptySpanToSql) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         empty;

    auto result = qs.insert(std::span<const Person>(empty)).to_sql();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().empty()) << "Empty span should return empty SQL";
}

TYPED_TEST(SqlInspectionTest, RemoveEmptySpanToSql) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         empty;

    auto result = qs.erase(std::span<const Person>(empty)).to_sql();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().empty()) << "Empty span should return empty SQL";
}

// ============================================================================
// Cross-backend to_sql() parity (#411)
//
// to_sql() is a debug/inspection aid only — execution always binds ? params.
// SQLite produces it via the engine-native sqlite3_expanded_sql(); PostgreSQL
// hand-rolls ?-placeholder substitution into the stored original SQL.
//
// These tests pin the current behavior:
//   * "Parity" tests assert the same rendered token on BOTH backends (one
//     expectation string, same for each TypeParam instantiation).
//   * "ByBackend" tests pin the KNOWN divergences (BLOB encoding, double
//     formatting) per backend via if constexpr.
// ============================================================================

// int operand DIVERGES in quoting: SQLite renders a bare integer; PostgreSQL
// stores every bound param as text and wraps it in quotes. The numeric value is
// the same; only the quoting differs. Pin each backend's output.
TYPED_TEST(SqlInspectionTest, ToSqlIntByBackend) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age == 30).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    if constexpr (std::is_same_v<TypeParam, storm::db::postgresql::Connection>) {
        EXPECT_TRUE(contains(sql, "= '30'")) << "PG renders int operands quoted: " << sql;
    } else {
        EXPECT_TRUE(contains(sql, "= 30")) << "SQLite renders int operands bare: " << sql;
        EXPECT_FALSE(contains(sql, "'30'")) << "SQLite int operand must not be quoted: " << sql;
    }
}

// bool operand DIVERGES the same way: SQLite renders bare 1; PG renders '1'.
TYPED_TEST(SqlInspectionTest, ToSqlBoolByBackend) {
    QuerySet<Person, TypeParam> qs;
    const bool                  active = true; // bound value (avoid bare literal in the predicate)
    auto                        result = qs.where(fields::Person.is_active == active).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    if constexpr (std::is_same_v<TypeParam, storm::db::postgresql::Connection>) {
        EXPECT_TRUE(contains(sql, "= '1'")) << "PG renders bool true as quoted '1': " << sql;
    } else {
        EXPECT_TRUE(contains(sql, "= 1")) << "SQLite renders bool true as bare 1: " << sql;
    }
}

// NULL (empty std::optional) renders as the keyword NULL on both backends.
TYPED_TEST(SqlInspectionTest, ToSqlNullParity) {
    QuerySet<Person, TypeParam> qs;
    Person                      p{.id = 0, .name = "NullScore", .age = 40};
    p.score = std::nullopt; // explicit: score is empty

    auto result = qs.insert(p).to_sql();
    ASSERT_TRUE(result.has_value()) << "insert().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "NULL")) << "empty optional should render as NULL on both backends: " << sql;
}

// Embedded single quote is escaped by doubling on both backends ('O''Brien').
TYPED_TEST(SqlInspectionTest, ToSqlEmbeddedQuoteParity) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.name == "O'Brien").select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "'O''Brien'")) << "embedded quote should be doubled on both backends: " << sql;
}

// A literal ? inside a quoted string is NOT treated as a placeholder (the core
// escaping check from #411). Both backends preserve it verbatim.
TYPED_TEST(SqlInspectionTest, ToSqlLiteralQuestionMarkParity) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.name == "a?b").select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "'a?b'")) << "literal ? inside a string must be preserved, not substituted: " << sql;
}

// BLOB encoding DIVERGES: SQLite emits an x'..' hex literal; PostgreSQL emits the
// raw bytes inside a quoted string. Pin each backend's actual output.
TYPED_TEST(SqlInspectionTest, ToSqlBlobDivergesByBackend) {
    QuerySet<Person, TypeParam> qs;
    Person                      p{.id = 0, .name = "Blobby", .age = 33};
    p.avatar = {0x48, 0x49}; // "HI"

    auto result = qs.insert(p).to_sql();
    ASSERT_TRUE(result.has_value()) << "insert().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    if constexpr (std::is_same_v<TypeParam, storm::db::postgresql::Connection>) {
        // PG: raw bytes 0x48 0x49 = "HI" inside a quoted string literal.
        EXPECT_TRUE(contains(sql, "'HI'")) << "PG should render BLOB as raw quoted bytes: " << sql;
    } else {
        // SQLite: native x'4849' hex literal.
        EXPECT_TRUE(contains(sql, "x'4849'") || contains(sql, "X'4849'"))
                << "SQLite should render BLOB as an x'..' hex literal: " << sql;
    }
}

// double formatting DIVERGES: SQLite uses the engine's float->text; PostgreSQL
// uses %.17g. Pin that the value is present in each backend's rendering.
TYPED_TEST(SqlInspectionTest, ToSqlDoubleByBackend) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.salary == 1234.5).select().to_sql();
    ASSERT_TRUE(result.has_value()) << "to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    // Both backends render the significant digits "1234.5"; exact trailing
    // formatting may differ, which is the documented divergence.
    EXPECT_TRUE(contains(sql, "1234.5")) << "double value should appear in the rendered SQL: " << sql;
}

// ============================================================================
// DISTINCT / VALUES .to_sql() tests (#197 — parity with SELECT)
// ============================================================================

TYPED_TEST(SqlInspectionTest, DistinctBareToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.template distinct<fields::Person.name>().to_sql();
    ASSERT_TRUE(result.has_value()) << "distinct().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "SELECT DISTINCT")) << "Should contain SELECT DISTINCT: " << sql;
    EXPECT_TRUE(contains(sql, "name")) << "Should contain projected field 'name': " << sql;
    EXPECT_TRUE(contains(sql, R"("person")")) << "Should contain table name: " << sql;
    EXPECT_FALSE(contains(sql, "WHERE")) << "Bare distinct should have no WHERE: " << sql;
}

TYPED_TEST(SqlInspectionTest, DistinctWithWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto result = qs.where(fields::Person.age > 30).template distinct<fields::Person.name>().to_sql();
    ASSERT_TRUE(result.has_value()) << "distinct().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "SELECT DISTINCT")) << "Should contain SELECT DISTINCT: " << sql;
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE: " << sql;
    EXPECT_TRUE(contains(sql, "30")) << "WHERE value 30 should be bound into the SQL: " << sql;
}

TYPED_TEST(SqlInspectionTest, ValuesBareToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.template values<fields::Person.name, fields::Person.age>().to_sql();
    ASSERT_TRUE(result.has_value()) << "values().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "SELECT")) << "Should contain SELECT: " << sql;
    EXPECT_FALSE(contains(sql, "DISTINCT")) << "values() must not emit DISTINCT: " << sql;
    EXPECT_TRUE(contains(sql, "name")) << "Should contain field 'name': " << sql;
    EXPECT_TRUE(contains(sql, "age")) << "Should contain field 'age': " << sql;
}

TYPED_TEST(SqlInspectionTest, ValuesWithWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto result = qs.where(fields::Person.name == "Alice").template values<fields::Person.age>().to_sql();
    ASSERT_TRUE(result.has_value()) << "values().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE: " << sql;
    EXPECT_TRUE(contains(sql, "Alice")) << "WHERE value 'Alice' should be bound into the SQL: " << sql;
}

// ============================================================================
// AGGREGATE .to_sql() tests (#197 — parity with SELECT)
// ============================================================================

TYPED_TEST(SqlInspectionTest, CountBareToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.count().to_sql();
    ASSERT_TRUE(result.has_value()) << "count().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "COUNT")) << "Should contain COUNT: " << sql;
    EXPECT_TRUE(contains(sql, R"("person")")) << "Should contain table name: " << sql;
    EXPECT_FALSE(contains(sql, "WHERE")) << "Bare count should have no WHERE: " << sql;
}

TYPED_TEST(SqlInspectionTest, CountWithWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age > 30).count().to_sql();
    ASSERT_TRUE(result.has_value()) << "count().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "COUNT")) << "Should contain COUNT: " << sql;
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE: " << sql;
    EXPECT_TRUE(contains(sql, "30")) << "WHERE value 30 should be bound into the SQL: " << sql;
}

TYPED_TEST(SqlInspectionTest, SumWithWhereToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.age >= 25).template sum<fields::Person.age>().to_sql();
    ASSERT_TRUE(result.has_value()) << "sum().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "SUM")) << "Should contain SUM: " << sql;
    EXPECT_TRUE(contains(sql, "WHERE")) << "Should contain WHERE: " << sql;
    EXPECT_TRUE(contains(sql, "25")) << "WHERE value 25 should be bound into the SQL: " << sql;
}

TYPED_TEST(SqlInspectionTest, GroupByCountToSql) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.template group_by<fields::Person.age>().count().to_sql();
    ASSERT_TRUE(result.has_value()) << "group_by().count().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "COUNT")) << "Should contain COUNT: " << sql;
    EXPECT_TRUE(contains(sql, "GROUP BY")) << "Should contain GROUP BY: " << sql;
    EXPECT_TRUE(contains(sql, "age")) << "Should contain group field 'age': " << sql;
}

TYPED_TEST(SqlInspectionTest, GroupByHavingCountToSql) {
    QuerySet<Person, TypeParam> qs;
    auto result = qs.template group_by<fields::Person.age>().having(fields::Person.age > 30).count().to_sql();
    ASSERT_TRUE(result.has_value()) << "group_by().having().count().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "GROUP BY")) << "Should contain GROUP BY: " << sql;
    EXPECT_TRUE(contains(sql, "HAVING")) << "Should contain HAVING: " << sql;
    EXPECT_TRUE(contains(sql, "30")) << "HAVING value 30 should be bound into the SQL: " << sql;
}

// Other chaining position: count() before having().
TYPED_TEST(SqlInspectionTest, GroupByCountHavingToSql) {
    QuerySet<Person, TypeParam> qs;
    auto result = qs.template group_by<fields::Person.age>().count().having(fields::Person.age > 30).to_sql();
    ASSERT_TRUE(result.has_value()) << "group_by().count().having().to_sql() failed: " << result.error().message();

    const std::string& sql = result.value();
    EXPECT_TRUE(contains(sql, "GROUP BY")) << "Should contain GROUP BY: " << sql;
    EXPECT_TRUE(contains(sql, "HAVING")) << "Should contain HAVING: " << sql;
    EXPECT_TRUE(contains(sql, "30")) << "HAVING value 30 should be bound into the SQL: " << sql;
}

// NOLINTEND(misc-const-correctness,misc-use-anonymous-namespace,readability-implicit-bool-conversion)
