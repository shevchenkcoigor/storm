// test_collate.cpp - Comprehensive tests for COLLATE support (ORDER BY + WHERE)
#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness,performance-unnecessary-copy-initialization)

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

#include "../../shared/models/color.h" // NOSONAR cpp:S954
#include "../../shared/models/extended_types.h"
#include "../../shared/models/person.h"
#include "test_fixture.h"
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;
using storm::orm::utilities::Collate;

// SQLite-only: COLLATE NOCASE/BINARY/RTRIM are SQLite-specific collation sequences.
// PostgreSQL uses different syntax (COLLATE "C", COLLATE "en_US").
using SqliteTypes = ::testing::Types<storm::db::sqlite::Connection>;

// ============================================================================
// Test Fixture — inserts mixed-case names for COLLATE testing
// ============================================================================

template <typename ConnType> class CollateTest : public StormTestFixture<Person, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<Person> const test_data = {
                {.id = 1, .name = "alice", .age = 25, .department = "eng"},
                {.id = 2, .name = "Alice", .age = 30, .department = "Eng"},
                {.id = 3, .name = "BOB", .age = 35, .department = "HR"},
                {.id = 4, .name = "bob", .age = 40, .department = "hr"},
                {.id = 5, .name = "Charlie", .age = 22, .department = "Sales"},
                {.id = 6, .name = "charlie", .age = 28, .department = "sales"},
                {.id = 7, .name = "ALICE", .age = 33, .department = "ENG"},
                {.id = 8, .name = "  bob", .age = 45, .department = "HR  "},
        };

        auto result = (InsertStatement<Person, ConnType>{}).query(test_data).execute();
        ASSERT_TRUE(result.has_value()) << "Failed to insert COLLATE test data";
    }
};

TYPED_TEST_SUITE(CollateTest, SqliteTypes);

// ============================================================================
// ORDER BY with COLLATE Tests
// ============================================================================

TYPED_TEST(CollateTest, OrderByNoCaseAsc) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, Collate::NoCase>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);

    // With COLLATE NOCASE, "  bob" sorts first (space < 'A'),
    // then alice/Alice/ALICE grouped together, then bob/BOB, then charlie/Charlie
    auto it = items.begin();
    EXPECT_EQ(it->name, "  bob"); // space sorts before letters
}

TYPED_TEST(CollateTest, OrderByNoCaseDesc) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, Collate::NoCase, false>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);

    // DESC + NOCASE: charlie/Charlie first, then bob/BOB, then alice/Alice/ALICE, then "  bob"
    auto it = items.begin();
    // First item should be a charlie variant (case-insensitive DESC)
    EXPECT_TRUE(it->name == "charlie" || it->name == "Charlie");
}

TYPED_TEST(CollateTest, OrderByBinary) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, Collate::Binary>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);

    // COLLATE BINARY: strict byte order. Uppercase letters (A=65) come before lowercase (a=97)
    // "  bob" < "ALICE" < "Alice" < "BOB" < "Charlie" < "alice" < "bob" < "charlie"
    auto it = items.begin();
    EXPECT_EQ(it->name, "  bob"); // Space (32) < uppercase
    ++it;
    EXPECT_EQ(it->name, "ALICE"); // 'A'=65
    ++it;
    EXPECT_EQ(it->name, "Alice"); // 'A'=65, 'l'=108 > 'L'=76
    ++it;
    EXPECT_EQ(it->name, "BOB");
    ++it;
    EXPECT_EQ(it->name, "Charlie");
    ++it;
    EXPECT_EQ(it->name, "alice");
    ++it;
    EXPECT_EQ(it->name, "bob");
    ++it;
    EXPECT_EQ(it->name, "charlie");
}

TYPED_TEST(CollateTest, OrderByRTrim) {
    // COLLATE RTRIM trims trailing spaces before comparison
    auto result =
            (SelectStatement<Person, TypeParam>{})
                    .all()
                    .template order_by<fields::Person.department, Collate::RTrim, true, fields::Person.name, true>()
                    .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);
}

TYPED_TEST(CollateTest, OrderByCollateWithBoolDirection) {
    // Test: field, Collate, bool — both modifiers after field
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, Collate::NoCase, false>()
                          .execute();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 8);
}

TYPED_TEST(CollateTest, OrderByBoolThenCollate) {
    // Test: field, bool, Collate — reversed modifier order
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, false, Collate::NoCase>()
                          .execute();
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().size(), 8);
}

TYPED_TEST(CollateTest, OrderByMultipleFieldsWithCollate) {
    // ORDER BY name COLLATE NOCASE ASC, age DESC
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, Collate::NoCase, fields::Person.age, false>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    ASSERT_EQ(items.size(), 8);
}

// ============================================================================
// WHERE with COLLATE Tests — all 6 comparison operators
// ============================================================================

TYPED_TEST(CollateTest, WhereEqualNoCase) {
    // Case-insensitive equality: "alice" should match "alice", "Alice", "ALICE"
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) == "alice")
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 3);
}

TYPED_TEST(CollateTest, WhereNotEqualNoCase) {
    // Case-insensitive not-equal: exclude all "alice" variants → 5 remaining
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) != "alice")
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 5);
}

TYPED_TEST(CollateTest, WhereGreaterNoCase) {
    // Case-insensitive greater: names > "bob" → charlie variants + leading-space bob
    // Actually "  bob" < "bob" even with NOCASE since space < 'b'
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) > "bob")
                          .execute();
    ASSERT_TRUE(result.has_value());
    // "charlie", "Charlie" are > "bob" (case-insensitive)
    EXPECT_EQ(result.value().size(), 2);
}

TYPED_TEST(CollateTest, WhereGreaterEqualNoCase) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) >= "bob")
                          .execute();
    ASSERT_TRUE(result.has_value());
    // "bob", "BOB", "charlie", "Charlie" = 4
    EXPECT_EQ(result.value().size(), 4);
}

TYPED_TEST(CollateTest, WhereLessNoCase) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) < "bob")
                          .execute();
    ASSERT_TRUE(result.has_value());
    // "alice", "Alice", "ALICE", "  bob" = 4
    EXPECT_EQ(result.value().size(), 4);
}

TYPED_TEST(CollateTest, WhereLessEqualNoCase) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) <= "bob")
                          .execute();
    ASSERT_TRUE(result.has_value());
    // "alice" x3, "bob" x2, "  bob" = 6
    EXPECT_EQ(result.value().size(), 6);
}

// ============================================================================
// WHERE COLLATE with LIKE
// ============================================================================

TYPED_TEST(CollateTest, WhereLikeNoCase) {
    // LIKE with NOCASE: "A%" should match alice, Alice, ALICE
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase).like("a%"))
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 3);
}

// ============================================================================
// WHERE COLLATE with BETWEEN
// ============================================================================

TYPED_TEST(CollateTest, WhereBetweenNoCase) {
    // BETWEEN with NOCASE: names between "alice" and "bob" (inclusive)
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase)
                                         .between(std::string("alice"), std::string("bob")))
                          .execute();
    ASSERT_TRUE(result.has_value());
    // "alice" x3, "bob" x2 = 5 (not "  bob" — space < 'a')
    EXPECT_EQ(result.value().size(), 5);
}

// ============================================================================
// WHERE COLLATE with IN
// ============================================================================

TYPED_TEST(CollateTest, WhereInNoCase) {
    // IN with NOCASE: match "alice" (case-insensitive) and "bob" (case-insensitive)
    // Note: IN comparisons in SQLite still use the collation of the left-hand operand
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase).in("alice", "bob"))
                          .execute();
    ASSERT_TRUE(result.has_value());
    // "alice" x3, "bob" x2 = 5 (not "  bob" — it's "  bob" not "bob")
    EXPECT_EQ(result.value().size(), 5);
}

// ============================================================================
// WHERE COLLATE Binary (exact comparison)
// ============================================================================

TYPED_TEST(CollateTest, WhereEqualBinary) {
    // COLLATE BINARY: exact case match
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::Binary) == "alice")
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 1); // Only lowercase "alice"
}

// ============================================================================
// Combined COLLATE in WHERE + ORDER BY
// ============================================================================

TYPED_TEST(CollateTest, WhereCollateWithOrderByCollate) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) >= "bob")
                          .template order_by<fields::Person.name, Collate::NoCase>()
                          .execute();
    ASSERT_TRUE(result.has_value());

    auto items = result.value();
    EXPECT_EQ(items.size(), 4); // bob, BOB, charlie, Charlie
}

// ============================================================================
// COLLATE with LIMIT/OFFSET
// ============================================================================

TYPED_TEST(CollateTest, OrderByCollateWithLimit) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, Collate::NoCase>()
                          .limit(3)
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 3);
}

TYPED_TEST(CollateTest, OrderByCollateWithLimitOffset) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .all()
                          .template order_by<fields::Person.name, Collate::NoCase>()
                          .limit(2)
                          .offset(3)
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 2);
}

// ============================================================================
// WHERE COLLATE combined with AND/OR logic
// ============================================================================

TYPED_TEST(CollateTest, WhereCollateWithAndLogic) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) == "alice" && fields::Person.age > 30)
                          .execute();
    ASSERT_TRUE(result.has_value());
    // "ALICE" age=33 is the only alice variant with age > 30
    EXPECT_EQ(result.value().size(), 1);
}

TYPED_TEST(CollateTest, WhereCollateWithOrLogic) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) == "alice" ||
                                 fields::Person.name.collate(Collate::NoCase) == "charlie")
                          .execute();
    ASSERT_TRUE(result.has_value());
    // alice x3, charlie x2 = 5
    EXPECT_EQ(result.value().size(), 5);
}

// ============================================================================
// Repeated queries (statement caching correctness)
// ============================================================================

TYPED_TEST(CollateTest, RepeatedCollateQueries) {
    // Same query twice — should get same result (caching correctness)
    auto r1 = (SelectStatement<Person, TypeParam>{})
                      .where(fields::Person.name.collate(Collate::NoCase) == "bob")
                      .execute();
    auto r2 = (SelectStatement<Person, TypeParam>{})
                      .where(fields::Person.name.collate(Collate::NoCase) == "bob")
                      .execute();

    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1.value().size(), r2.value().size());
}

TYPED_TEST(CollateTest, DifferentCollateOnSameField) {
    // NOCASE vs BINARY on same field — different results (cache invalidation)
    auto r_nocase = (SelectStatement<Person, TypeParam>{})
                            .where(fields::Person.name.collate(Collate::NoCase) == "alice")
                            .execute();
    auto r_binary = (SelectStatement<Person, TypeParam>{})
                            .where(fields::Person.name.collate(Collate::Binary) == "alice")
                            .execute();

    ASSERT_TRUE(r_nocase.has_value());
    ASSERT_TRUE(r_binary.has_value());

    EXPECT_EQ(r_nocase.value().size(), 3); // Case-insensitive: 3 matches
    EXPECT_EQ(r_binary.value().size(), 1); // Exact: 1 match
}

// ============================================================================
// Empty result set
// ============================================================================

TYPED_TEST(CollateTest, WhereCollateNoMatch) {
    auto result = (SelectStatement<Person, TypeParam>{})
                          .where(fields::Person.name.collate(Collate::NoCase) == "nonexistent")
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 0);
}

// ============================================================================
// SQL generation verification
// ============================================================================

TYPED_TEST(CollateTest, OrderBySqlGeneration) {
    auto sql_result = (SelectStatement<Person, TypeParam>{})
                              .all()
                              .template order_by<fields::Person.name, Collate::NoCase>()
                              .to_sql();
    ASSERT_TRUE(sql_result.has_value());
    const auto& sql = sql_result.value();
    EXPECT_NE(sql.find("COLLATE NOCASE"), std::string::npos) << "SQL should contain COLLATE NOCASE: " << sql;
    EXPECT_NE(sql.find("ORDER BY"), std::string::npos) << "SQL should contain ORDER BY: " << sql;
}

TYPED_TEST(CollateTest, WhereCollateSqlGeneration) {
    auto sql_result = (SelectStatement<Person, TypeParam>{})
                              .where(fields::Person.name.collate(Collate::NoCase) == "test")
                              .to_sql();
    ASSERT_TRUE(sql_result.has_value());
    const auto& sql = sql_result.value();
    EXPECT_NE(sql.find("name COLLATE NOCASE"), std::string::npos)
            << "SQL should contain 'name COLLATE NOCASE': " << sql;
}

// ============================================================================
// COLLATE .in() on type-folding fields (#578)
//
// CollatedField::in() used to construct InExpression<FieldType> directly from
// the declared field type, which fails to compile whenever FieldType needs
// normalize_operand's fold (enum -> int, narrow/unsigned int -> int/int64_t,
// text-like -> std::string) to reach an actual ExpressionVariant arm.
// Field::in() already routed through normalize_operand; these tests cross
// .collate() with each folding category so the two paths cannot silently
// diverge again.
// ============================================================================

template <typename ConnType> class CollateInFoldTest : public StormTestFixture<ExtendedTypes, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<ExtendedTypes> const test_data = {
                {.id = 1, .label = "alpha", .tiny_signed = 1, .color = Color::Red},
                {.id = 2, .label = "beta", .tiny_signed = 2, .color = Color::Green},
                {.id = 3, .label = "GAMMA", .tiny_signed = 3, .color = Color::Blue},
        };

        auto result = (InsertStatement<ExtendedTypes, ConnType>{}).query(test_data).execute();
        ASSERT_TRUE(result.has_value()) << "Failed to insert CollateInFoldTest data";
    }
};

TYPED_TEST_SUITE(CollateInFoldTest, SqliteTypes);

TYPED_TEST(CollateInFoldTest, EnumField) {
    // Enum folds to int in the variant — used to fail to compile via CollatedField::in().
    auto result = (SelectStatement<ExtendedTypes, TypeParam>{})
                          .where(fields::ExtendedTypes.color.collate(Collate::Binary).in(Color::Red, Color::Blue))
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 2);
}

TYPED_TEST(CollateInFoldTest, NarrowIntField) {
    // signed char folds to int in the variant.
    auto result = (SelectStatement<ExtendedTypes, TypeParam>{})
                          .where(fields::ExtendedTypes.tiny_signed.collate(Collate::Binary)
                                         .in(static_cast<signed char>(1), static_cast<signed char>(3)))
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 2);
}

TYPED_TEST(CollateInFoldTest, StringViewOperand) {
    // string_view folds to an owning std::string; NOCASE makes "gamma" match the stored "GAMMA".
    std::string_view const alpha  = "alpha";
    auto                   result = (SelectStatement<ExtendedTypes, TypeParam>{})
                          .where(fields::ExtendedTypes.label.collate(Collate::NoCase).in(alpha, "gamma"))
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().size(), 2);
}

TYPED_TEST(CollateInFoldTest, SqlGenerationPreservesCollate) {
    auto sql_result =
            (SelectStatement<ExtendedTypes, TypeParam>{})
                    .where(fields::ExtendedTypes.tiny_signed.collate(Collate::Binary).in(static_cast<signed char>(1)))
                    .to_sql();
    ASSERT_TRUE(sql_result.has_value());
    const auto& sql = sql_result.value();
    EXPECT_NE(sql.find("tiny_signed COLLATE BINARY IN"), std::string::npos)
            << "SQL should contain 'tiny_signed COLLATE BINARY IN': " << sql;
}

// NOLINTEND(misc-const-correctness,performance-unnecessary-copy-initialization)
