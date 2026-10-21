#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "../../shared/models/simple_record.h"
#include "test_fixture.h"
using storm::orm::statements::ReturnId;

// ============================================================================
// Helpers
// ============================================================================

// Returns "ASC" or "ASC NULLS FIRST" depending on backend
template <typename ConnType> constexpr auto order_asc() -> std::string_view {
    if constexpr (storm::test::is_postgresql<ConnType>()) {
        return "ASC NULLS FIRST";
    } else {
        return "ASC";
    }
}

template <typename ConnType> constexpr auto order_desc() -> std::string_view {
    if constexpr (storm::test::is_postgresql<ConnType>()) {
        return "DESC NULLS LAST";
    } else {
        return "DESC";
    }
}

// Returns "LIMIT ALL" for PostgreSQL, "LIMIT -1" for SQLite (unlimited with offset)
template <typename ConnType> constexpr auto limit_all() -> std::string_view {
    if constexpr (storm::test::is_postgresql<ConnType>()) {
        return "LIMIT ALL";
    } else {
        return "LIMIT -1";
    }
}

// ============================================================================
// Fixture — no data needed, SQL verification is structural only
// ============================================================================

template <typename ConnType> class SqlVerifyTest : public StormTestFixture<Person, ConnType, Message> {};

TYPED_TEST_SUITE(SqlVerifyTest, DatabaseTypes);

// ============================================================================
// SELECT sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, SelectSimple) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.select().sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar FROM Person"
    );
}

TYPED_TEST(SqlVerifyTest, SelectWithWhere) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.where(fields::Person.age > 30).select().sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age > ?"
    );
}

TYPED_TEST(SqlVerifyTest, SelectWithJoin) {
    QuerySet<Message, TypeParam> qs;
    auto                         sql = qs.template join<fields::Message.sender>().select().sql();
    EXPECT_EQ(
            sql,
            "SELECT t1.id, t1.content, t1.value, t2.id, t2.name, t2.age, t2.salary,"
            " t2.is_active, t2.years_experience, t2.department, t2.score, t2.nickname, t2.avatar"
            " FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
    );
}

TYPED_TEST(SqlVerifyTest, SelectWithJoinAndWhere) {
    QuerySet<Message, TypeParam> qs;
    auto sql = qs.template join<fields::Message.sender>().where(fields::Message.value > 10).select().sql();
    EXPECT_EQ(
            sql,
            "SELECT t1.id, t1.content, t1.value, t2.id, t2.name, t2.age, t2.salary,"
            " t2.is_active, t2.years_experience, t2.department, t2.score, t2.nickname, t2.avatar"
            " FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id WHERE value > ?"
    );
}

TYPED_TEST(SqlVerifyTest, SelectWithOrderBy) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template order_by<fields::Person.name>().select().sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
                    " FROM Person ORDER BY name {}",
                    order_asc<TypeParam>()
            )
    );
}

TYPED_TEST(SqlVerifyTest, SelectWithOrderByDesc) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template order_by<fields::Person.age, false>().select().sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
                    " FROM Person ORDER BY age {}",
                    order_desc<TypeParam>()
            )
    );
}

TYPED_TEST(SqlVerifyTest, SelectWithLimitOffset) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.limit(10).offset(5).select().sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person LIMIT 10 OFFSET 5"
    );
}

TYPED_TEST(SqlVerifyTest, SelectWithOffsetOnly) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.offset(5).select().sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
                    " FROM Person {} OFFSET 5",
                    limit_all<TypeParam>()
            )
    );
}

TYPED_TEST(SqlVerifyTest, SelectFullCombo) {
    QuerySet<Message, TypeParam> qs;
    auto                         sql = qs.template join<fields::Message.sender>()
                       .where(fields::Message.value > 10)
                       .template order_by<fields::Message.content>()
                       .limit(5)
                       .offset(2)
                       .select()
                       .sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT t1.id, t1.content, t1.value, t2.id, t2.name, t2.age, t2.salary,"
                    " t2.is_active, t2.years_experience, t2.department, t2.score, t2.nickname, t2.avatar"
                    " FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
                    " WHERE value > ? ORDER BY content {} LIMIT 5 OFFSET 2",
                    order_asc<TypeParam>()
            )
    );
}

// ============================================================================
// first().sql() and get().sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, FirstSimple) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.first().sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person LIMIT 1"
    );
}

TYPED_TEST(SqlVerifyTest, FirstWithWhere) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.where(fields::Person.age > 30).first().sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age > ? LIMIT 1"
    );
}

TYPED_TEST(SqlVerifyTest, GetSimple) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.get().sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person LIMIT 2"
    );
}

TYPED_TEST(SqlVerifyTest, GetWithWhere) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.where(fields::Person.id == 1).get().sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE id = ? LIMIT 2"
    );
}

// ============================================================================
// INSERT sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, InsertSingleReturning) {
    QuerySet<Person, TypeParam> qs;
    Person const                p{};
    auto                        sql = qs.insert(p).sql();
    EXPECT_EQ(
            sql,
            "INSERT INTO Person (name, age, salary, is_active, years_experience, department, score, nickname, avatar)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id"
    );
}

TYPED_TEST(SqlVerifyTest, InsertSingleNoReturning) {
    QuerySet<Person, TypeParam> qs;
    Person const                p{};
    auto                        sql = qs.template insert<ReturnId::No>(p).sql();
    EXPECT_EQ(
            sql,
            "INSERT INTO Person (name, age, salary, is_active, years_experience, department, score, nickname, avatar)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
}

TYPED_TEST(SqlVerifyTest, InsertBulk) {
    QuerySet<Person, TypeParam>   qs;
    std::array<Person, 3> const   people{};
    std::span<const Person> const span{people};
    auto                          sql = qs.insert(span).sql();
    EXPECT_EQ(
            sql,
            "INSERT INTO Person (name, age, salary, is_active, years_experience, department, score, nickname, avatar)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
}

TYPED_TEST(SqlVerifyTest, InsertBulkReturning) {
    QuerySet<Person, TypeParam>   qs;
    std::array<Person, 2> const   people{};
    std::span<const Person> const span{people};
    auto                          sql = qs.template insert<ReturnId::Yes>(span).sql();
    EXPECT_EQ(
            sql,
            "INSERT INTO Person (name, age, salary, is_active, years_experience, department, score, nickname, avatar)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?), (?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id"
    );
}

// ============================================================================
// INSERT sql() with SimpleRecord (fewer fields)
// ============================================================================

TYPED_TEST(SqlVerifyTest, InsertSimpleRecord) {
    QuerySet<SimpleRecord, TypeParam> qs;
    SimpleRecord const                r{};
    auto                              sql = qs.insert(r).sql();
    EXPECT_EQ(sql, "INSERT INTO SimpleRecord (name, value) VALUES (?, ?) RETURNING id");
}

// ============================================================================
// INSERT sql() with FK model
// ============================================================================

TYPED_TEST(SqlVerifyTest, InsertMessageWithFK) {
    QuerySet<Message, TypeParam> qs;
    Message const                m{};
    auto                         sql = qs.insert(m).sql();
    EXPECT_EQ(sql, "INSERT INTO Message (content, value, sender_id) VALUES (?, ?, ?) RETURNING id");
}

// ============================================================================
// UPDATE sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, UpdateSingle) {
    QuerySet<Person, TypeParam> qs;
    Person const                p{};
    auto                        sql = qs.update(p).sql();
    EXPECT_EQ(
            sql,
            "UPDATE Person SET name=?, age=?, salary=?, is_active=?, years_experience=?,"
            " department=?, score=?, nickname=?, avatar=? WHERE id = ?"
    );
}

TYPED_TEST(SqlVerifyTest, UpdateBulk) {
    QuerySet<Person, TypeParam>   qs;
    std::array<Person, 3> const   people{};
    std::span<const Person> const span{people};
    auto                          sql = qs.update(span).sql();
    EXPECT_EQ(
            sql,
            "UPDATE Person SET name=?, age=?, salary=?, is_active=?, years_experience=?,"
            " department=?, score=?, nickname=?, avatar=? WHERE id = ?"
    );
}

TYPED_TEST(SqlVerifyTest, UpdateMessageWithFK) {
    QuerySet<Message, TypeParam> qs;
    Message const                m{};
    auto                         sql = qs.update(m).sql();
    EXPECT_EQ(sql, "UPDATE Message SET content=?, value=?, sender_id=? WHERE id = ?");
}

// ============================================================================
// DELETE sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, DeleteSingle) {
    QuerySet<Person, TypeParam> qs;
    Person const                p{};
    auto                        sql = qs.erase(p).sql();
    EXPECT_EQ(sql, "DELETE FROM Person WHERE id = ?");
}

TYPED_TEST(SqlVerifyTest, DeleteBulk) {
    QuerySet<Person, TypeParam>   qs;
    std::array<Person, 3> const   people{};
    std::span<const Person> const span{people};
    auto                          sql = qs.erase(span).sql();
    EXPECT_EQ(sql, "DELETE FROM Person WHERE id IN (?,?,?)");
}

TYPED_TEST(SqlVerifyTest, DeleteAll) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.erase_all().sql();
    EXPECT_EQ(sql, "DELETE FROM Person");
}

TYPED_TEST(SqlVerifyTest, DeleteSimpleRecord) {
    QuerySet<SimpleRecord, TypeParam> qs;
    SimpleRecord const                r{};
    auto                              sql = qs.erase(r).sql();
    EXPECT_EQ(sql, "DELETE FROM SimpleRecord WHERE id = ?");
}

// ============================================================================
// AGGREGATE sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, AggregateCountSimple) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.count().sql();
    EXPECT_EQ(sql, "SELECT COUNT(*) FROM Person");
}

TYPED_TEST(SqlVerifyTest, AggregateCountWithWhere) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.where(fields::Person.age > 30).count().sql();
    EXPECT_EQ(sql, "SELECT COUNT(*) FROM Person WHERE age > ?");
}

TYPED_TEST(SqlVerifyTest, AggregateSumField) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template sum<fields::Person.salary>().sql();
    EXPECT_EQ(sql, "SELECT SUM(salary) FROM Person");
}

TYPED_TEST(SqlVerifyTest, AggregateAvgField) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template avg<fields::Person.age>().sql();
    EXPECT_EQ(sql, "SELECT AVG(age) FROM Person");
}

TYPED_TEST(SqlVerifyTest, AggregateMinField) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template min<fields::Person.age>().sql();
    EXPECT_EQ(sql, "SELECT MIN(age) FROM Person");
}

TYPED_TEST(SqlVerifyTest, AggregateMaxField) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template max<fields::Person.salary>().sql();
    EXPECT_EQ(sql, "SELECT MAX(salary) FROM Person");
}

TYPED_TEST(SqlVerifyTest, AggregateChainedOps) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template sum<fields::Person.age>().template count<>().sql();
    EXPECT_EQ(sql, "SELECT SUM(age), COUNT(*) FROM Person");
}

TYPED_TEST(SqlVerifyTest, AggregateCountWithJoin) {
    QuerySet<Message, TypeParam> qs;
    auto                         sql = qs.template join<fields::Message.sender>().count().sql();
    EXPECT_EQ(sql, "SELECT COUNT(*) FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id");
}

TYPED_TEST(SqlVerifyTest, AggregateCountWithJoinAndWhere) {
    QuerySet<Message, TypeParam> qs;
    auto sql = qs.template join<fields::Message.sender>().where(fields::Message.value > 10).count().sql();
    EXPECT_EQ(sql, "SELECT COUNT(*) FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id WHERE value > ?");
}

// ============================================================================
// GROUP BY sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, GroupByCount) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template group_by<fields::Person.department>().template count<>().sql();
    EXPECT_EQ(sql, "SELECT department, COUNT(*) FROM Person GROUP BY department");
}

TYPED_TEST(SqlVerifyTest, GroupBySum) {
    QuerySet<Person, TypeParam> qs;
    auto sql = qs.template group_by<fields::Person.department>().template sum<fields::Person.salary>().sql();
    EXPECT_EQ(sql, "SELECT department, SUM(salary) FROM Person GROUP BY department");
}

TYPED_TEST(SqlVerifyTest, GroupByWithWhere) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.where(fields::Person.is_active == true)
                       .template group_by<fields::Person.department>()
                       .template count<>()
                       .sql();
    EXPECT_EQ(sql, "SELECT department, COUNT(*) FROM Person WHERE is_active = ? GROUP BY department");
}

TYPED_TEST(SqlVerifyTest, GroupByWithHaving) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template group_by<fields::Person.department>()
                       .template count<>()
                       .having(fields::Person.department == "Engineering")
                       .sql();
    EXPECT_EQ(sql, "SELECT department, COUNT(*) FROM Person GROUP BY department HAVING department = ?");
}

TYPED_TEST(SqlVerifyTest, GroupByWithWhereAndHaving) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.where(fields::Person.age > 25)
                       .template group_by<fields::Person.department>()
                       .template count<>()
                       .having(fields::Person.department == "Engineering")
                       .sql();
    EXPECT_EQ(sql, "SELECT department, COUNT(*) FROM Person WHERE age > ? GROUP BY department HAVING department = ?");
}

TYPED_TEST(SqlVerifyTest, GroupByWithOrderBy) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template order_by<fields::Person.department>()
                       .template group_by<fields::Person.department>()
                       .template count<>()
                       .sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT department, COUNT(*) FROM Person GROUP BY department ORDER BY department {}",
                    order_asc<TypeParam>()
            )
    );
}

TYPED_TEST(SqlVerifyTest, GroupByWithLimitOffset) {
    QuerySet<Person, TypeParam> qs;
    auto sql = qs.limit(3).offset(1).template group_by<fields::Person.department>().template count<>().sql();
    EXPECT_EQ(sql, "SELECT department, COUNT(*) FROM Person GROUP BY department LIMIT 3 OFFSET 1");
}

TYPED_TEST(SqlVerifyTest, GroupByMultiField) {
    QuerySet<Person, TypeParam> qs;
    auto sql = qs.template group_by<fields::Person.department, fields::Person.is_active>().template count<>().sql();
    EXPECT_EQ(sql, "SELECT department, is_active, COUNT(*) FROM Person GROUP BY department, is_active");
}

TYPED_TEST(SqlVerifyTest, GroupByWithJoin) {
    QuerySet<Message, TypeParam> qs;
    auto                         sql = qs.template join<fields::Message.sender>()
                       .template group_by<fields::Message.content>()
                       .template count<>()
                       .sql();
    EXPECT_EQ(
            sql,
            "SELECT content, COUNT(*) FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
            " GROUP BY content"
    );
}

// ============================================================================
// SET OPERATION sql()
// ============================================================================

TYPED_TEST(SqlVerifyTest, UnionSimple) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    auto sql = qs1.where(fields::Person.age > 40).union_(qs2.where(fields::Person.age < 25)).sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age > ? UNION "
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age < ?"
    );
}

TYPED_TEST(SqlVerifyTest, UnionAllSimple) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    auto sql = qs1.where(fields::Person.age > 40).union_all(qs2.where(fields::Person.age < 25)).sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age > ? UNION ALL "
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age < ?"
    );
}

TYPED_TEST(SqlVerifyTest, ExceptSimple) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    auto                        sql = qs1.except_(qs2.where(fields::Person.age < 25)).sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person EXCEPT "
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age < ?"
    );
}

TYPED_TEST(SqlVerifyTest, IntersectSimple) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    auto sql = qs1.where(fields::Person.age > 30).intersect_(qs2.where(fields::Person.is_active == true)).sql();
    EXPECT_EQ(
            sql,
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE age > ? INTERSECT "
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
            " FROM Person WHERE is_active = ?"
    );
}

TYPED_TEST(SqlVerifyTest, SetOpWithOrderByAndLimit) {
    QuerySet<Person, TypeParam> qs1;
    QuerySet<Person, TypeParam> qs2;
    auto                        sql = qs1.where(fields::Person.age > 40)
                       .union_(qs2.where(fields::Person.age < 25))
                       .template order_by<fields::Person.name>()
                       .limit(10)
                       .sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
                    " FROM Person WHERE age > ? UNION "
                    "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
                    " FROM Person WHERE age < ? ORDER BY name {} LIMIT 10",
                    order_asc<TypeParam>()
            )
    );
}

// NOLINTEND(misc-const-correctness)
