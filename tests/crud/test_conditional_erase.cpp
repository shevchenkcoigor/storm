#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// clang-tidy's readability-implicit-bool-conversion mis-fires on the GTest
// streamed-message idiom `EXPECT_*(...) << "literal"` (the const char* message,
// not the asserted condition). Band the whole file like other test files do.
// NOLINTBEGIN(readability-implicit-bool-conversion)

// Conditional bulk DELETE: (EraseStatement<Person, TypeParam>{}).query_where(cond)  (#198)
// Seeds a richer dataset (varied age/salary/department/is_active/score) so
// every comparison operator, IN/BETWEEN/LIKE/NULL, and AND/OR can be
// exercised against real rows.
template <typename ConnType> class ConditionalEraseTest : public StormTestFixture<Person, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<Person> seeded = {
                {.id = 1, .name = "Alice", .age = 30, .salary = 50000.0, .is_active = true, .department = "Eng"},
                {.id = 2, .name = "Bob", .age = 25, .salary = 40000.0, .is_active = false, .department = "Eng"},
                {.id = 3, .name = "Charlie", .age = 35, .salary = 60000.0, .is_active = true, .department = "Sales"},
                {.id = 4, .name = "Dave", .age = 40, .salary = 70000.0, .is_active = false, .department = "Sales"},
                {.id = 5, .name = "Eve", .age = 45, .salary = 80000.0, .is_active = true, .department = "Legacy"},
        };
        // Give two rows a non-NULL score to test IS NULL / IS NOT NULL.
        seeded[0].score = 10;
        seeded[2].score = 30;
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(seeded)));
    }

    static auto countPersons() -> int {
        auto result = (SelectStatement<Person, ConnType>{}).all().count().execute();
        return result.has_value() ? static_cast<int>(result.value()) : -1;
    }

    static auto personExists(int person_id) -> bool {
        auto result = (SelectStatement<Person, ConnType>{}).where(fields::Person.id == person_id).execute();
        return result.has_value() && !result.value().empty();
    }
};

TYPED_TEST_SUITE(ConditionalEraseTest, DatabaseTypes);

TYPED_TEST(ConditionalEraseTest, Setup) {
    EXPECT_EQ(this->countPersons(), 5) << "Should start with 5 persons";
}

// --- All six comparison operators ---------------------------------------
TYPED_TEST(ConditionalEraseTest, Equal) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age == 30).execute().has_value());
    EXPECT_FALSE(this->personExists(1));
    EXPECT_EQ(this->countPersons(), 4);
}

TYPED_TEST(ConditionalEraseTest, NotEqual) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age != 30).execute().has_value());
    EXPECT_TRUE(this->personExists(1)) << "age==30 row (Alice) survives";
    EXPECT_EQ(this->countPersons(), 1);
}

TYPED_TEST(ConditionalEraseTest, GreaterThan) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age > 35).execute().has_value());
    EXPECT_FALSE(this->personExists(4));
    EXPECT_FALSE(this->personExists(5));
    EXPECT_EQ(this->countPersons(), 3);
}

TYPED_TEST(ConditionalEraseTest, GreaterEqual) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age >= 35).execute().has_value());
    EXPECT_EQ(this->countPersons(), 2) << "ages 35,40,45 removed";
}

TYPED_TEST(ConditionalEraseTest, LessThan) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age < 30).execute().has_value());
    EXPECT_FALSE(this->personExists(2)) << "Bob age 25 removed";
    EXPECT_EQ(this->countPersons(), 4);
}

TYPED_TEST(ConditionalEraseTest, LessEqual) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age <= 30).execute().has_value());
    EXPECT_EQ(this->countPersons(), 3) << "ages 25,30 removed";
}

// --- Special expressions: IN / BETWEEN / LIKE / IS NULL -----------------
TYPED_TEST(ConditionalEraseTest, InClause) {
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age.in(25, 35, 45)).execute().has_value()
    );
    EXPECT_EQ(this->countPersons(), 2) << "Bob(25), Charlie(35), Eve(45) removed";
}

TYPED_TEST(ConditionalEraseTest, BetweenClause) {
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age.between(30, 40)).execute().has_value()
    );
    EXPECT_EQ(this->countPersons(), 2) << "ages 30,35,40 removed";
}

TYPED_TEST(ConditionalEraseTest, LikeClause) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{})
                        .query_where(fields::Person.department.like("Eng%"))
                        .execute()
                        .has_value());
    EXPECT_EQ(this->countPersons(), 3) << "Alice & Bob in Eng removed";
}

TYPED_TEST(ConditionalEraseTest, IsNull) {
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.score.is_null()).execute().has_value()
    );
    EXPECT_EQ(this->countPersons(), 2) << "rows with NULL score removed (3 of 5)";
}

TYPED_TEST(ConditionalEraseTest, IsNotNull) {
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.score.is_not_null()).execute().has_value()
    );
    EXPECT_EQ(this->countPersons(), 3) << "rows with non-NULL score removed (2 of 5)";
}

// --- String / double / bool types ---------------------------------------
TYPED_TEST(ConditionalEraseTest, StringEqual) {
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.name == "Alice").execute().has_value()
    );
    EXPECT_FALSE(this->personExists(1));
    EXPECT_EQ(this->countPersons(), 4);
}

TYPED_TEST(ConditionalEraseTest, DoubleGreater) {
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.salary > 55000.0).execute().has_value()
    );
    EXPECT_EQ(this->countPersons(), 2) << "salaries 60k,70k,80k removed";
}

TYPED_TEST(ConditionalEraseTest, BoolEqual) {
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.is_active == false).execute().has_value()
    );
    EXPECT_EQ(this->countPersons(), 3) << "Bob & Dave (inactive) removed";
}

// --- Logical combinations: AND / OR / nested ----------------------------
TYPED_TEST(ConditionalEraseTest, AndCombination) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{})
                        .query_where(fields::Person.department == "Sales" && fields::Person.is_active == false)
                        .execute()
                        .has_value());
    EXPECT_FALSE(this->personExists(4)) << "Dave (Sales, inactive) removed";
    EXPECT_TRUE(this->personExists(3)) << "Charlie (Sales, active) survives";
    EXPECT_EQ(this->countPersons(), 4);
}

TYPED_TEST(ConditionalEraseTest, OrCombination) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{})
                        .query_where(fields::Person.age < 26 || fields::Person.age > 44)
                        .execute()
                        .has_value());
    EXPECT_FALSE(this->personExists(2)) << "Bob 25 removed";
    EXPECT_FALSE(this->personExists(5)) << "Eve 45 removed";
    EXPECT_EQ(this->countPersons(), 3);
}

TYPED_TEST(ConditionalEraseTest, NestedCombination) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{})
                        .query_where(
                                (fields::Person.department == "Eng" && fields::Person.age < 28) ||
                                fields::Person.salary > 75000.0
                        )
                        .execute()
                        .has_value());
    EXPECT_FALSE(this->personExists(2)) << "Bob (Eng, 25) removed";
    EXPECT_FALSE(this->personExists(5)) << "Eve (salary 80k) removed";
    EXPECT_EQ(this->countPersons(), 3);
}

// --- Chained where() (AND-combined) -------------------------------------
TYPED_TEST(ConditionalEraseTest, ChainedWhere) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{})
                        .query_where(fields::Person.department == "Eng" && fields::Person.is_active == true)
                        .execute()
                        .has_value());
    EXPECT_FALSE(this->personExists(1)) << "Alice (Eng, active) removed";
    EXPECT_TRUE(this->personExists(2)) << "Bob (Eng, inactive) survives";
    EXPECT_EQ(this->countPersons(), 4);
}

// --- Result-set sizes ----------------------------------------------------
TYPED_TEST(ConditionalEraseTest, EmptyResultNoOp) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age > 1000).execute().has_value())
            << "Matching nothing is a successful no-op";
    EXPECT_EQ(this->countPersons(), 5) << "No rows deleted";
}

TYPED_TEST(ConditionalEraseTest, SingleRowMatch) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.id == 3).execute().has_value());
    EXPECT_FALSE(this->personExists(3));
    EXPECT_EQ(this->countPersons(), 4);
}

TYPED_TEST(ConditionalEraseTest, LargeResultSet) {
    // Add 150 more persons, all in department "Bulk", then delete them at once.
    std::vector<Person> batch;
    batch.reserve(150);
    for (int i = 100; i < 250; ++i) {
        batch.emplace_back(Person{.id = i, .name = std::format("Bulk{}", i), .age = 50, .department = "Bulk"});
    }
    ASSERT_TRUE((InsertStatement<Person, TypeParam>{}).query(std::span<const Person>(batch)).execute().has_value());
    EXPECT_EQ(this->countPersons(), 155);
    ASSERT_TRUE(
            (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.department == "Bulk").execute().has_value()
    );
    EXPECT_EQ(this->countPersons(), 5) << "All 150 bulk rows deleted in one statement";
}

// --- Safety: empty WHERE refuses the full-table wipe --------------------
TYPED_TEST(ConditionalEraseTest, EmptyWhereIsRefused) {
    auto result = (EraseStatement<Person, TypeParam>{}).query_where(nullptr).execute();
    ASSERT_FALSE(result.has_value()) << "erase() with no where() must refuse to wipe the table";
    EXPECT_EQ(this->countPersons(), 5) << "Table must be untouched";
}

TYPED_TEST(ConditionalEraseTest, EraseAllStillWipes) {
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_all().execute().has_value())
            << "erase_all() remains the explicit full wipe";
    EXPECT_EQ(this->countPersons(), 0);
}

// --- to_sql() shape ------------------------------------------------------
TYPED_TEST(ConditionalEraseTest, ToSqlShape) {
    auto sql = (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.age > 30).to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("DELETE FROM")) << sql.value();
    EXPECT_TRUE(sql.value().contains("WHERE")) << sql.value();
}

TYPED_TEST(ConditionalEraseTest, ToSqlEmptyWhereRefused) {
    auto sql = (EraseStatement<Person, TypeParam>{}).query_where(nullptr).to_sql();
    EXPECT_FALSE(sql.has_value()) << "to_sql() on an unfiltered erase() must also refuse";
}

// NOLINTEND(readability-implicit-bool-conversion)
