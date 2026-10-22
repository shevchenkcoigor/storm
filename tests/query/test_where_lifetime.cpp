#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness,performance-unnecessary-value-param,performance-unnecessary-copy-initialization)

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_where;
import storm_orm_utilities;
import std;

#include "../../shared/models/people_25.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::orm::statements::SelectStatement;
using storm::orm::utilities::Collate;
using storm::orm::where::Expr;

// Lifetime safety for WHERE comparison operands (#352).
//
// QuerySet::where() is immutable/Django-style and deferred: an expression node can outlive the
// buffer a text operand points at, and .select() binds the operand only later. ComparisonExpr used
// to keep std::string_view / const char* operands by view, so a deferred bind read a dangling
// buffer (use-after-free under ASAN). Text operands are now normalized to an owning std::string at
// construction, so any deferred bind is safe.
// Run the deferred WHERE and assert it returns exactly one Bob — the operand must have been copied,
// not stored as a view into a buffer that has already been freed.
template <typename ConnType> auto expect_single_bob(const Expr& expr) -> void {
    auto result = (SelectStatement<Person, ConnType>{}).where(expr).execute();
    ASSERT_TRUE(result.has_value()) << "Deferred bind failed: " << result.error().message();
    ASSERT_EQ(result.value().size(), 1) << "Expected exactly 1 person named Bob";
    EXPECT_EQ(result.value().begin()->name, "Bob");
}

template <typename ConnType> class WhereLifetimeTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end())
        )));
    }
};

TYPED_TEST_SUITE(WhereLifetimeTest, DatabaseTypes);

// Test: operand built from a string_view into a heap temporary survives a deferred bind.
TYPED_TEST(WhereLifetimeTest, StringViewOperandSurvivesDeferredBind) {
    // Build the expression from a string_view into a heap temporary, then let the temporary die.
    Expr expr = [] {
        auto             owner = std::make_unique<std::string>("Bob");
        std::string_view view  = *owner;
        return fields::Person.name == view; // node must copy "Bob", not keep a view into *owner
    }();
    // `owner` is freed here; ASAN poisons its buffer.

    expect_single_bob<TypeParam>(expr);
}

// Test: const char* operand from a heap buffer survives a deferred bind (char* alternative).
TYPED_TEST(WhereLifetimeTest, CharPtrOperandSurvivesDeferredBind) {
    Expr expr = [] {
        auto        owner = std::make_unique<std::array<char, 4>>(std::array<char, 4>{'B', 'o', 'b', '\0'});
        const char* ptr   = owner->data();
        return fields::Person.name == ptr; // node must copy "Bob", not keep the char*
    }();

    expect_single_bob<TypeParam>(expr);
}

// Test: collated comparison operand from a temporary string_view also survives a deferred bind.
using SqliteConn = storm::db::sqlite::Connection;

class WhereLifetimeCollateTest : public StormTestFixture<Person, SqliteConn> {
  protected:
    auto on_after_setup(const std::shared_ptr<SqliteConn>&) -> void override {
        ASSERT_TRUE((storm::test::batch_insert<Person, SqliteConn>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end())
        )));
    }
};

TEST_F(WhereLifetimeCollateTest, CollatedStringViewOperandSurvivesDeferredBind) {
    Expr expr = [] {
        auto             owner = std::make_unique<std::string>("bob");
        std::string_view view  = *owner;
        return fields::Person.name.collate(Collate::NoCase) == view;
    }();

    // Case-insensitive match: "bob" finds "Bob".
    expect_single_bob<SqliteConn>(expr);
}

// Test: between() string-literal operands survive a deferred bind (#406).
// between() used to store text operands by value-decay (BetweenExpr<const char*>),
// bypassing the #352 normalize_operand text-copy. A string literal then dangles only
// if the literal's storage dies — but a literal lives for the whole program, so to
// prove the copy we route through a heap buffer that dies before the deferred bind.
TYPED_TEST(WhereLifetimeTest, BetweenStringOperandSurvivesDeferredBind) {
    // "Bob" BETWEEN "Bob" AND "Bob" matches exactly the one Bob.
    Expr expr = [] {
        auto        lo_owner = std::make_unique<std::array<char, 4>>(std::array<char, 4>{'B', 'o', 'b', '\0'});
        auto        hi_owner = std::make_unique<std::array<char, 4>>(std::array<char, 4>{'B', 'o', 'b', '\0'});
        const char* lo       = lo_owner->data();
        const char* hi       = hi_owner->data();
        return fields::Person.name.between(lo, hi); // node must copy, not keep the char*
    }();

    expect_single_bob<TypeParam>(expr);
}

// Test: between() with a literal compiles and binds correctly (#406 DoD).
TYPED_TEST(WhereLifetimeTest, BetweenStringLiteralCompilesAndBinds) {
    Expr expr = fields::Person.name.between("Bob", "Bob");
    expect_single_bob<TypeParam>(expr);
}

// Test: CollatedField::between() string operands survive a deferred bind (#406).
TEST_F(WhereLifetimeCollateTest, CollatedBetweenStringOperandSurvivesDeferredBind) {
    Expr expr = [] {
        auto        lo_owner = std::make_unique<std::array<char, 4>>(std::array<char, 4>{'b', 'o', 'b', '\0'});
        auto        hi_owner = std::make_unique<std::array<char, 4>>(std::array<char, 4>{'b', 'o', 'b', '\0'});
        const char* lo       = lo_owner->data();
        const char* hi       = hi_owner->data();
        return fields::Person.name.collate(Collate::NoCase).between(lo, hi);
    }();

    // Case-insensitive: "bob" BETWEEN-bounds finds "Bob".
    expect_single_bob<SqliteConn>(expr);
}

// NOLINTEND(misc-const-correctness,performance-unnecessary-value-param,performance-unnecessary-copy-initialization)
