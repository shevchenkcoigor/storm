#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import std;
import storm_orm_model_concepts;

#include "test_fixture.h"
using storm::create_table_sql;
using storm::max_length;
using storm::primary;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::ModelMaxLengthValid;
using storm::orm::statements::SelectStatement;

// Tests for the max_length<N> field annotation (#493): a DB-enforced text-length
// bound. SQLite emits TEXT ... CHECK(length(col) <= N); PostgreSQL emits VARCHAR(N).
// Both dialects genuinely enforce the bound on every write path. A compile-time
// concept rejects max_length on non-text fields.
using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;

// ============================================================================
// Test models (local to this TU)
// ============================================================================

namespace {

    // A plain bounded text field, NOT NULL.
    struct BoundedName {
        [[= primary]] int                id{};
        [[= max_length<50>]] std::string name;
    };

    // A nullable bounded text field: CHECK passes on NULL (standard SQL); PG omits NOT NULL.
    struct BoundedOptional {
        [[= primary]] int                               id{};
        [[= max_length<20>]] std::optional<std::string> nickname;
    };

    // max_length combined with unique.
    struct BoundedUnique {
        [[= primary]] int                                   id{};
        [[ = max_length<30>, = storm::unique ]] std::string code;
    };

    // max_length combined with a C++ default member initializer (#413).
    struct BoundedDefault {
        [[= primary]] int                id{};
        [[= max_length<10>]] std::string status = "new";
    };

    // string_view text field also accepts the annotation.
    struct BoundedView {
        [[= primary]] int                     id{};
        [[= max_length<15>]] std::string_view label;
    };

} // namespace

// ============================================================================
// Compile-time concept gate — max_length on non-text fields is a hard error
// ============================================================================

// ModelMaxLengthValid<T> is the model-boundary concept: true iff every max_length
// annotation in T sits on a text field. It is one of the concepts BaseStatement<T>
// (and thus QuerySet<T>) requires, so a bad model fails to instantiate with a clear
// constraint violation. These static_asserts pin the contract without a compile-fail
// harness (mirrors the full_unsigned / SET NULL negative tests in test_types.cpp).
namespace {
    struct MaxLengthOnInt {
        [[= primary]] int       id{};
        [[= max_length<5>]] int age{}; // non-text → invalid
    };
    struct MaxLengthOnDouble {
        [[= primary]] int          id{};
        [[= max_length<5>]] double score{}; // non-text → invalid
    };
    struct MaxLengthOnText {
        [[= primary]] int               id{};
        [[= max_length<5>]] std::string name;
    };
    struct MaxLengthOnOptionalText {
        [[= primary]] int                              id{};
        [[= max_length<5>]] std::optional<std::string> name;
    };

    static_assert(
            !ModelMaxLengthValid<MaxLengthOnInt>, "max_length on an int field must NOT satisfy ModelMaxLengthValid"
    );
    static_assert(
            !ModelMaxLengthValid<MaxLengthOnDouble>, "max_length on a double field must NOT satisfy ModelMaxLengthValid"
    );
    static_assert(
            ModelMaxLengthValid<MaxLengthOnText>, "max_length on a std::string field must satisfy ModelMaxLengthValid"
    );
    static_assert(
            ModelMaxLengthValid<MaxLengthOnOptionalText>,
            "max_length on an optional<std::string> field must satisfy ModelMaxLengthValid"
    );
    // A model with no max_length annotation at all trivially satisfies the concept.
    static_assert(
            ModelMaxLengthValid<BoundedName>, "a model with a text max_length field must satisfy ModelMaxLengthValid"
    );
} // namespace

// ============================================================================
// SQLite DDL — CHECK(length(col) <= N)
// ============================================================================

TEST(MaxLengthSchema, SqliteNotNullEmitsCheck) {
    const auto& sql = create_table_sql<BoundedName>();
    EXPECT_NE(sql.find("name TEXT NOT NULL CHECK(length(name) <= 50)"), std::string::npos) << sql;
}

TEST(MaxLengthSchema, SqliteOptionalOmitsNotNullKeepsCheck) {
    const auto& sql = create_table_sql<BoundedOptional>();
    // Nullable: no NOT NULL, but the CHECK is still emitted (passes on NULL).
    EXPECT_NE(sql.find("nickname TEXT CHECK(length(nickname) <= 20)"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("nickname TEXT NOT NULL"), std::string::npos) << sql;
}

TEST(MaxLengthSchema, SqliteUniqueAppendsAfterCheck) {
    const auto& sql = create_table_sql<BoundedUnique>();
    EXPECT_NE(sql.find("code TEXT NOT NULL CHECK(length(code) <= 30) UNIQUE"), std::string::npos) << sql;
}

TEST(MaxLengthSchema, SqliteDefaultPrecedesCheck) {
    const auto& sql = create_table_sql<BoundedDefault>();
    EXPECT_NE(sql.find("status TEXT NOT NULL DEFAULT 'new' CHECK(length(status) <= 10)"), std::string::npos) << sql;
}

TEST(MaxLengthSchema, SqliteStringViewEmitsCheck) {
    const auto& sql = create_table_sql<BoundedView>();
    EXPECT_NE(sql.find("label TEXT NOT NULL CHECK(length(label) <= 15)"), std::string::npos) << sql;
}

// ============================================================================
// PostgreSQL DDL — VARCHAR(N)
// ============================================================================

TEST(MaxLengthSchema, PgNotNullEmitsVarchar) {
    const std::string& sql = SchemaStatement<BoundedName>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("name VARCHAR(50) NOT NULL"), std::string::npos) << sql;
}

TEST(MaxLengthSchema, PgOptionalOmitsNotNull) {
    const std::string& sql = SchemaStatement<BoundedOptional>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("nickname VARCHAR(20)"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("nickname VARCHAR(20) NOT NULL"), std::string::npos) << sql;
}

TEST(MaxLengthSchema, PgUniqueAppendsAfterVarchar) {
    const std::string& sql = SchemaStatement<BoundedUnique>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("code VARCHAR(30) NOT NULL UNIQUE"), std::string::npos) << sql;
}

TEST(MaxLengthSchema, PgDefaultAfterVarchar) {
    const std::string& sql = SchemaStatement<BoundedDefault>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("status VARCHAR(10) NOT NULL DEFAULT 'new'"), std::string::npos) << sql;
}

// ============================================================================
// Runtime enforcement — INSERT under / at / over the limit
// ============================================================================

template <typename ConnType> class MaxLengthEnforceTest : public StormTestFixture<BoundedName, ConnType> {};
TYPED_TEST_SUITE(MaxLengthEnforceTest, DatabaseTypes);

TYPED_TEST(MaxLengthEnforceTest, InsertUnderLimitSucceeds) {
    auto result =
            (InsertStatement<BoundedName, TypeParam>{}).query(BoundedName{.name = "short"}).execute(); // 5 chars <= 50
    ASSERT_TRUE(result.has_value()) << result.error().message();
}

TYPED_TEST(MaxLengthEnforceTest, InsertAtLimitSucceeds) {
    const std::string at_limit(50, 'x'); // exactly 50 chars
    auto result = (InsertStatement<BoundedName, TypeParam>{}).query(BoundedName{.name = at_limit}).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = (SelectStatement<BoundedName, TypeParam>{}).all().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    ASSERT_EQ(selected.value().size(), 1U);
    EXPECT_EQ(selected.value().begin()->name, at_limit);
}

TYPED_TEST(MaxLengthEnforceTest, InsertOverLimitRejected) {
    const std::string over_limit(51, 'x'); // 51 chars > 50 — DB must reject
    auto result = (InsertStatement<BoundedName, TypeParam>{}).query(BoundedName{.name = over_limit}).execute();
    EXPECT_FALSE(result.has_value()) << "DB should reject a value over max_length";

    // The row must NOT have been written.
    auto selected = (SelectStatement<BoundedName, TypeParam>{}).all().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    EXPECT_EQ(selected.value().size(), 0U);
}

// Nullable bounded field: NULL is allowed, over-limit non-NULL is rejected.
template <typename ConnType> class MaxLengthOptionalEnforceTest : public StormTestFixture<BoundedOptional, ConnType> {};
TYPED_TEST_SUITE(MaxLengthOptionalEnforceTest, DatabaseTypes);

TYPED_TEST(MaxLengthOptionalEnforceTest, NullAllowed) {
    auto result =
            (InsertStatement<BoundedOptional, TypeParam>{}).query(BoundedOptional{.nickname = std::nullopt}).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
}

TYPED_TEST(MaxLengthOptionalEnforceTest, OverLimitRejected) {
    const std::string over_limit(21, 'x'); // 21 > 20
    auto              result =
            (InsertStatement<BoundedOptional, TypeParam>{}).query(BoundedOptional{.nickname = over_limit}).execute();
    EXPECT_FALSE(result.has_value()) << "DB should reject an over-limit non-NULL value";
}
