#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/people_25.h"
#include "../../shared/models/person.h"
#include "test_fixture.h"

template <typename ConnType> class SetOpTest : public StormTestFixture<Person, ConnType, Message> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
        QuerySet<Person, ConnType> qs;
        auto                       result = qs.insert(std::span<const Person>(storm::test::PEOPLE_25)).execute();
        ASSERT_TRUE(result.has_value()) << "Seed insert failed: " << result.error().message();
    }
};

TYPED_TEST_SUITE(SetOpTest, DatabaseTypes);

// ============================================================================
// Basic UNION
// ============================================================================

TYPED_TEST(SetOpTest, UnionBasic) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Left: 4 people with age above 40, right: 2 people with age below 24
    // No overlap, UNION yields 6
    auto result = qs_left.where(fields::Person.age > 40).union_(qs_right.where(fields::Person.age < 24)).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 6);
}

TYPED_TEST(SetOpTest, UnionDeduplicates) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // age >= 40: Eve(40), Frank(45), Leo(42), Olivia(48), Sam(40), Victor(45) = 6
    // age <= 45: all except Olivia(48) = 24
    // UNION deduplicates -> all 25
    auto result = qs_left.where(fields::Person.age >= 40).union_(qs_right.where(fields::Person.age <= 45)).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 25);
}

// ============================================================================
// Basic UNION ALL
// ============================================================================

TYPED_TEST(SetOpTest, UnionAllKeepsDuplicates) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // age >= 40: 6, age <= 45: 24, overlap: 5
    // UNION ALL = 6 + 24 = 30
    auto result = qs_left.where(fields::Person.age >= 40).union_all(qs_right.where(fields::Person.age <= 45)).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 30);
}

// ============================================================================
// Basic EXCEPT
// ============================================================================

TYPED_TEST(SetOpTest, ExceptBasic) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Active (16) EXCEPT (active AND age>35: Frank,Leo,Olivia,Sam,Victor = 5)
    // Result = 11
    auto result = qs_left.where(fields::Person.is_active == true)
                          .except_(qs_right.where(fields::Person.is_active == true && fields::Person.age > 35))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 11);
}

// ============================================================================
// Basic INTERSECT
// ============================================================================

TYPED_TEST(SetOpTest, IntersectBasic) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Active(16) INTERSECT age>35(8) = active AND age>35: Frank,Leo,Olivia,Sam,Victor = 5
    auto result = qs_left.where(fields::Person.is_active == true)
                          .intersect_(qs_right.where(fields::Person.age > 35))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 5);
}

// ============================================================================
// WHERE on both sides
// ============================================================================

TYPED_TEST(SetOpTest, UnionBothWithWhere) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // 6 Engineering + 5 Sales, no overlap, UNION yields 11
    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 11);
}

// ============================================================================
// WHERE on left only (right selects all via broad condition)
// ============================================================================

TYPED_TEST(SetOpTest, UnionLeftWhereOnly) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Engineering(6) UNION all(via age>0, 25) = 25
    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.age > 0))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 25);
}

// ============================================================================
// ORDER BY on combined result
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithOrderBy) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .template order_by<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 11);

    std::vector<std::string> names;
    for (const auto& p : *result) {
        names.push_back(p.name);
    }
    EXPECT_TRUE(std::ranges::is_sorted(names));
}

// ============================================================================
// ORDER BY + LIMIT
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithOrderByAndLimit) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .template order_by<fields::Person.name>()
                          .limit(3)
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    // Engineering + Sales by name: Alice Bob Eve Frank Ivy Karen Leo Olivia Quinn Tina Victor.
    std::vector<std::string> names;
    for (const auto& p : *result) {
        names.push_back(p.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Alice", "Bob", "Eve"}));
}

// ============================================================================
// ORDER BY + LIMIT + OFFSET
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithOrderByLimitOffset) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .template order_by<fields::Person.name>()
                          .limit(3)
                          .offset(2)
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    // Same ordering as above; OFFSET 2 skips Alice and Bob.
    std::vector<std::string> names;
    for (const auto& p : *result) {
        names.push_back(p.name);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Eve", "Frank", "Ivy"}));
}

// ============================================================================
// LIMIT without ORDER BY
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithLimitOnly) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .limit(5)
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 5);
}

// ============================================================================
// 3-way chaining: UNION via SetOpBuilder
// ============================================================================

TYPED_TEST(SetOpTest, ThreeWayUnion) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    QuerySet<Person, TypeParam> qs3;

    // 6 Engineering + 5 Sales + 4 HR, no overlap, UNION yields 15
    auto builder = qs1.where(fields::Person.department == "Engineering")
                           .union_(qs2.where(fields::Person.department == "Sales"));
    auto result = std::move(builder).union_(qs3.where(fields::Person.department == "HR").capture_operand()).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 15);
}

// ============================================================================
// Mixed ops chaining: UNION then EXCEPT
// ============================================================================

TYPED_TEST(SetOpTest, MixedOpsUnionThenExcept) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    QuerySet<Person, TypeParam> qs3;

    // (Engineering UNION Sales) EXCEPT age>40
    // Engineering(6) UNION Sales(5) = 11
    // age>40: Frank(45),Leo(42),Olivia(48),Victor(45) = 4
    // Result = 11 - 4 = 7
    auto builder = qs1.where(fields::Person.department == "Engineering")
                           .union_(qs2.where(fields::Person.department == "Sales"));
    auto result = std::move(builder).except_(qs3.where(fields::Person.age > 40).capture_operand()).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 7);
}

// ============================================================================
// Empty result: INTERSECT with no common rows
// ============================================================================

TYPED_TEST(SetOpTest, IntersectNoCommon) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Engineering INTERSECT Sales = empty
    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .intersect_(qs_right.where(fields::Person.department == "Sales"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 0);
}

// ============================================================================
// Empty result: Both sides empty
// ============================================================================

TYPED_TEST(SetOpTest, UnionBothEmpty) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.age > 100).union_(qs_right.where(fields::Person.age < 0)).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 0);
}

// ============================================================================
// EXCEPT: all match (empty result)
// ============================================================================

TYPED_TEST(SetOpTest, ExceptAllMatch) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Engineering EXCEPT Engineering = empty
    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .except_(qs_right.where(fields::Person.department == "Engineering"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 0);
}

// ============================================================================
// to_sql() verification
// ============================================================================

TYPED_TEST(SetOpTest, ToSqlUnion) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto sql = qs_left.where(fields::Person.age > 30).union_(qs_right.where(fields::Person.age < 25)).to_sql();
    ASSERT_TRUE(sql.has_value()) << sql.error().message();
    EXPECT_TRUE(sql->find("UNION") != std::string::npos) << "SQL should contain UNION: " << *sql;
    EXPECT_TRUE(sql->find("UNION ALL") == std::string::npos) << "SQL should not contain UNION ALL: " << *sql;
}

TYPED_TEST(SetOpTest, ToSqlUnionAll) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto sql = qs_left.where(fields::Person.age > 30).union_all(qs_right.where(fields::Person.age < 25)).to_sql();
    ASSERT_TRUE(sql.has_value()) << sql.error().message();
    EXPECT_TRUE(sql->find("UNION ALL") != std::string::npos) << "SQL should contain UNION ALL: " << *sql;
}

TYPED_TEST(SetOpTest, ToSqlExcept) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto sql = qs_left.where(fields::Person.age > 30).except_(qs_right.where(fields::Person.age < 25)).to_sql();
    ASSERT_TRUE(sql.has_value()) << sql.error().message();
    EXPECT_TRUE(sql->find("EXCEPT") != std::string::npos) << "SQL should contain EXCEPT: " << *sql;
}

TYPED_TEST(SetOpTest, ToSqlIntersect) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto sql = qs_left.where(fields::Person.age > 30).intersect_(qs_right.where(fields::Person.age < 40)).to_sql();
    ASSERT_TRUE(sql.has_value()) << sql.error().message();
    EXPECT_TRUE(sql->find("INTERSECT") != std::string::npos) << "SQL should contain INTERSECT: " << *sql;
}

// ============================================================================
// Self-union (same query)
// ============================================================================

TYPED_TEST(SetOpTest, SelfUnion) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // UNION deduplicates -> same as one side
    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Engineering"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 6);
}

// ============================================================================
// Self UNION ALL -> doubles
// ============================================================================

TYPED_TEST(SetOpTest, SelfUnionAll) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_all(qs_right.where(fields::Person.department == "Engineering"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 12);
}

// ============================================================================
// Single-row result
// ============================================================================

TYPED_TEST(SetOpTest, UnionSingleRow) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.name == "Alice")
                          .union_(qs_right.where(fields::Person.name == "Alice"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 1);
    EXPECT_EQ(result->begin()->name, "Alice");
}

// ============================================================================
// Large result set
// ============================================================================

TYPED_TEST(SetOpTest, UnionAllLargeResult) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // UNION ALL of all 25 with all 25 = 50
    auto result = qs_left.where(fields::Person.age > 0).union_all(qs_right.where(fields::Person.age > 0)).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 50);
}

// ============================================================================
// to_sql() with ORDER BY + LIMIT
// ============================================================================

TYPED_TEST(SetOpTest, ToSqlWithModifiers) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto sql = qs_left.where(fields::Person.age > 30)
                       .union_(qs_right.where(fields::Person.age < 25))
                       .template order_by<fields::Person.name>()
                       .limit(5)
                       .to_sql();
    ASSERT_TRUE(sql.has_value()) << sql.error().message();
    EXPECT_TRUE(sql->find("UNION") != std::string::npos);
    EXPECT_TRUE(sql->find("ORDER BY") != std::string::npos);
    EXPECT_TRUE(sql->find("LIMIT") != std::string::npos);
}

// ============================================================================
// Missing operator: !=
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithNotEquals) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // department != "Engineering" (19) UNION department == "Sales" (5)
    // Sales is subset of non-Engineering -> UNION = 19
    auto result = qs_left.where(fields::Person.department != "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 19);
}

// ============================================================================
// Double type coverage (salary)
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithDoubleWhere) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // salary >= 90000: Eve(92k),Leo(95k),Olivia(98k),Sam(90k),Victor(93k) = 5
    // salary < 40000: Paul(32k),Yara(35k) = 2
    // No overlap -> UNION = 7
    auto result = qs_left.where(fields::Person.salary >= 90000.0)
                          .union_(qs_right.where(fields::Person.salary < 40000.0))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 7);
}

TYPED_TEST(SetOpTest, ExceptWithDoubleWhere) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // salary >= 80000 (8): Jack,Leo,Frank,Olivia,Sam,Victor,Xander,Eve
    // salary >= 90000 (5): Eve,Leo,Olivia,Sam,Victor
    // EXCEPT = 3: Jack(85k),Frank(88k),Xander(82k)
    auto result = qs_left.where(fields::Person.salary >= 80000.0)
                          .except_(qs_right.where(fields::Person.salary >= 90000.0))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3);
}

// ============================================================================
// OR logical combination
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithOrWhere) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Left: age < 25 OR salary > 90000
    //   age<25: Paul(22),Yara(22) = 2
    //   salary>90000: Eve(92k),Leo(95k),Olivia(98k),Victor(93k) = 4
    //   No overlap -> 6
    // Right: department == "HR" -> Diana,Jack,Paul,Uma = 4
    // UNION: 6+4 - Paul(overlap) = 9
    auto result = qs_left.where(fields::Person.age < 25 || fields::Person.salary > 90000.0)
                          .union_(qs_right.where(fields::Person.department == "HR"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 9);
}

// ============================================================================
// Complex nested expression: (A && B) || C
// ============================================================================

TYPED_TEST(SetOpTest, IntersectWithComplexNested) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Left: (age > 40 && is_active) || department == "HR"
    //   age>40 && active: Frank(45),Leo(42),Olivia(48),Victor(45) = 4
    //   HR: Diana,Jack,Paul,Uma = 4
    //   No overlap -> 8
    // Right: salary > 50000
    //   All with salary > 50000 = 18 (excludes Paul(32k),Yara(35k),Tina(44k),Mia(46k),Diana(48k)) -> wait
    //   salary > 50000: 25 - 5 (Paul=32k,Yara=35k,Tina=44k,Mia=46k,Diana=48k) = 20
    // INTERSECT: rows in both = those from 8 that have salary>50k
    //   Frank(88k)✓,Leo(95k)✓,Olivia(98k)✓,Victor(93k)✓,Jack(85k)✓,Uma(69k)✓ = 6
    //   Diana(48k)✗,Paul(32k)✗ -> 6
    auto result = qs_left.where((fields::Person.age > 40 && fields::Person.is_active == true) ||
                                fields::Person.department == "HR")
                          .intersect_(qs_right.where(fields::Person.salary > 50000.0))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 6);
}

// ============================================================================
// Special expressions: LIKE
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithLike) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // name LIKE "A%" -> Alice = 1
    // name LIKE "B%" -> Bob = 1
    // UNION = 2
    auto result = qs_left.where(fields::Person.name.like("A%"))
                          .union_(qs_right.where(fields::Person.name.like("B%")))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 2);
}

// ============================================================================
// Special expressions: BETWEEN
// ============================================================================

TYPED_TEST(SetOpTest, ExceptWithBetween) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Left: age BETWEEN 30 AND 40 -> Bob,Charlie,Eve,Henry,Ivy,Jack,Nick,Quinn,Rachel,Sam,Uma,Xander = 12
    // Right: age BETWEEN 35 AND 40 -> Charlie,Eve,Jack,Nick,Rachel,Sam,Xander = 7
    // EXCEPT = 5: Bob(30),Henry(33),Ivy(30),Quinn(30),Uma(33)
    auto result = qs_left.where(fields::Person.age.between(30, 40))
                          .except_(qs_right.where(fields::Person.age.between(35, 40)))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 5);
}

// ============================================================================
// Special expressions: IN
// ============================================================================

TYPED_TEST(SetOpTest, IntersectWithIn) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Left: age IN (25, 30, 35) -> Alice,Bob,Charlie,Grace,Ivy,Karen,Nick,Quinn = 8
    // Right: department == "Engineering" -> Alice,Eve,Ivy,Leo,Quinn,Victor = 6
    // INTERSECT = Alice,Ivy,Quinn = 3
    auto result = qs_left.where(fields::Person.age.in(25, 30, 35))
                          .intersect_(qs_right.where(fields::Person.department == "Engineering"))
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3);
}

// ============================================================================
// ORDER BY DESC
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithOrderByDesc) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .template order_by<fields::Person.age, false>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 11);

    std::vector<int> ages;
    for (const auto& p : *result) {
        ages.push_back(p.age);
    }
    EXPECT_TRUE(std::ranges::is_sorted(ages, std::greater<>{}));
}

// ============================================================================
// OFFSET without ORDER BY
// ============================================================================

TYPED_TEST(SetOpTest, UnionWithOffsetOnly) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    // Engineering(6) UNION Sales(5) = 11, skip first 5 -> 6
    auto result = qs_left.where(fields::Person.department == "Engineering")
                          .union_(qs_right.where(fields::Person.department == "Sales"))
                          .offset(5)
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 6);
}

// ============================================================================
// Large result set (100+ rows via 5-way UNION ALL)
// ============================================================================

TYPED_TEST(SetOpTest, UnionAllLarge100Plus) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    QuerySet<Person, TypeParam> qs3;
    QuerySet<Person, TypeParam> qs4;
    QuerySet<Person, TypeParam> qs5;

    // 5x UNION ALL of all 25 = 125
    auto builder = qs1.where(fields::Person.age > 0).union_all(qs2.where(fields::Person.age > 0));
    auto result  = std::move(builder)
                          .union_all(qs3.where(fields::Person.age > 0).capture_operand())
                          .union_all(qs4.where(fields::Person.age > 0).capture_operand())
                          .union_all(qs5.where(fields::Person.age > 0).capture_operand())
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 125);
}

// ============================================================================
// Repeated execution (statement caching correctness)
// ============================================================================

TYPED_TEST(SetOpTest, RepeatedExecution) {
    QuerySet<Person, TypeParam> qs_left;
    QuerySet<Person, TypeParam> qs_right;

    auto builder = qs_left.where(fields::Person.department == "Engineering")
                           .union_(qs_right.where(fields::Person.department == "Sales"));

    auto result1 = builder.execute();
    ASSERT_TRUE(result1.has_value()) << result1.error().message();
    EXPECT_EQ(result1->size(), 11);

    // Second execution on same builder should use cached statement
    auto result2 = builder.execute();
    ASSERT_TRUE(result2.has_value()) << result2.error().message();
    EXPECT_EQ(result2->size(), 11);
}

// ============================================================================
// Additional chaining: EXCEPT then UNION
// ============================================================================

TYPED_TEST(SetOpTest, ExceptThenUnion) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    QuerySet<Person, TypeParam> qs3;

    // (Engineering EXCEPT age>40) UNION HR
    // Engineering(6) EXCEPT age>40(Frank45,Leo42,Victor45 = 3 in Eng) = 3 (Alice,Eve,Ivy,Quinn minus Eve? wait)
    // Engineering: Alice(25),Eve(40),Ivy(30),Leo(42),Quinn(30),Victor(45)
    // age>40: Frank(45-Sales),Leo(42),Olivia(48),Victor(45)
    // EXCEPT: Engineering rows NOT in age>40 -> Alice,Eve(40 not >40),Ivy,Quinn = 4
    // HR: Diana,Jack,Paul,Uma = 4
    // UNION = 8
    auto builder = qs1.where(fields::Person.department == "Engineering").except_(qs2.where(fields::Person.age > 40));
    auto result  = std::move(builder).union_(qs3.where(fields::Person.department == "HR").capture_operand()).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 8);
}

// ============================================================================
// Additional chaining: INTERSECT then UNION ALL
// ============================================================================

TYPED_TEST(SetOpTest, IntersectThenUnionAll) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    QuerySet<Person, TypeParam> qs3;

    // (active INTERSECT age>35) UNION ALL HR
    // Active(16) INTERSECT age>35(8) = active AND age>35: Frank,Leo,Olivia,Sam,Victor = 5
    // HR: Diana,Jack,Paul,Uma = 4
    // UNION ALL = 9
    auto builder = qs1.where(fields::Person.is_active == true).intersect_(qs2.where(fields::Person.age > 35));
    auto result =
            std::move(builder).union_all(qs3.where(fields::Person.department == "HR").capture_operand()).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 9);
}

// ============================================================================
// 4-way chaining: UNION x3
// ============================================================================

TYPED_TEST(SetOpTest, FourWayUnion) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    QuerySet<Person, TypeParam> qs3;
    QuerySet<Person, TypeParam> qs4;

    // 6 Engineering + 5 Sales + 4 HR + 5 Marketing, no overlap, UNION yields 20
    auto builder = qs1.where(fields::Person.department == "Engineering")
                           .union_(qs2.where(fields::Person.department == "Sales"));
    auto result = std::move(builder)
                          .union_(qs3.where(fields::Person.department == "HR").capture_operand())
                          .union_(qs4.where(fields::Person.department == "Marketing").capture_operand())
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 20);
}

// ============================================================================
// UNION ALL with EXCEPT (all 4 set ops in one chain)
// ============================================================================

TYPED_TEST(SetOpTest, AllFourOpsChained) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    QuerySet<Person, TypeParam> qs3;
    QuerySet<Person, TypeParam> qs4;
    QuerySet<Person, TypeParam> qs5;

    // Uses UNION ALL + EXCEPT + UNION (same precedence, left-to-right in all backends).
    // Avoids INTERSECT which has higher precedence in PostgreSQL vs left-to-right in SQLite.
    // Eng(6) UNION ALL Sales(5)=11 → EXCEPT age>40=7 → UNION HR(4)=11 → EXCEPT !active=7
    auto builder = qs1.where(fields::Person.department == "Engineering")
                           .union_all(qs2.where(fields::Person.department == "Sales"));
    auto result = std::move(builder)
                          .except_(qs3.where(fields::Person.age > 40).capture_operand())
                          .union_(qs4.where(fields::Person.department == "HR").capture_operand())
                          .except_(qs5.where(fields::Person.is_active == false).capture_operand())
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 7);
}

// ============================================================================
// Compile-time safety: Finalized QuerySet cannot be used as set-op operand
// ============================================================================

// is_finalized() compile-time checks
static_assert(!QuerySet<Person, storm::db::sqlite::Connection>::is_finalized());
static_assert(QuerySet<Person, storm::db::sqlite::Connection, true>::is_finalized());

// Concepts for compile-time constraint verification
// (template SFINAE via concepts avoids experimental Clang requires-expression bug)
template <typename QS, typename Other>
concept CanUnion = requires(QS qs, Other other) { qs.union_(other); };

template <typename QS, typename Other>
concept CanUnionAll = requires(QS qs, Other other) { qs.union_all(other); };

template <typename QS, typename Other>
concept CanExcept = requires(QS qs, Other other) { qs.except_(other); };

template <typename QS, typename Other>
concept CanIntersect = requires(QS qs, Other other) { qs.intersect_(other); };

template <typename QS>
concept CanCaptureOperand = requires(QS qs) { qs.capture_operand(); };

using NormalQS    = QuerySet<Person, storm::db::sqlite::Connection>;
using FinalizedQS = QuerySet<Person, storm::db::sqlite::Connection, true>;

// Finalized QuerySet must NOT have set-op methods
static_assert(!CanUnion<FinalizedQS, NormalQS>);
static_assert(!CanUnionAll<FinalizedQS, NormalQS>);
static_assert(!CanExcept<FinalizedQS, NormalQS>);
static_assert(!CanIntersect<FinalizedQS, NormalQS>);

// Set-op methods must NOT accept finalized operands
static_assert(!CanUnion<NormalQS, FinalizedQS>);
static_assert(!CanUnionAll<NormalQS, FinalizedQS>);
static_assert(!CanExcept<NormalQS, FinalizedQS>);
static_assert(!CanIntersect<NormalQS, FinalizedQS>);

// capture_operand() must NOT be callable on finalized QuerySet
static_assert(!CanCaptureOperand<FinalizedQS>);

// limit/offset/order_by return finalized QS (not void) — [[nodiscard]] makes discard a warning
static_assert(!std::is_void_v<decltype(std::declval<NormalQS>().limit(1))>);
static_assert(!std::is_void_v<decltype(std::declval<NormalQS>().offset(1))>);
static_assert(!std::is_void_v<decltype(std::declval<NormalQS>().template order_by<fields::Person.name>())>);

// Non-finalized QuerySet MUST still have set-op methods (positive checks)
static_assert(CanUnion<NormalQS, NormalQS>);
static_assert(CanUnionAll<NormalQS, NormalQS>);
static_assert(CanExcept<NormalQS, NormalQS>);
static_assert(CanIntersect<NormalQS, NormalQS>);
static_assert(CanCaptureOperand<NormalQS>);

// NOLINTEND(misc-const-correctness)
