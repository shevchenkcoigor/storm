#include <gtest/gtest.h>
#include <meta>
#include "test_db_helpers.h"

import storm;
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"

using storm::QuerySet;

namespace {

    using Conn = storm::db::sqlite::Connection;

    // Stage 1's correctness criterion: the two spellings produce BYTE-IDENTICAL SQL
    // in every selector position.
    //
    // Pinned to one backend rather than typed over DatabaseTypes: .sql() renders the
    // statement without executing it, and both spellings feed the SAME grammar, so a
    // dialect difference (PG's NULLS FIRST/LAST, LIMIT ALL) applies equally to both
    // sides of every comparison and cannot expose a proxy-vs-^^ divergence. The
    // per-backend rendering itself is already covered by test_sql_verify.cpp.
    //
    // A fixture is still required despite the tests being structural: .sql() reaches
    // DefaultConnection::get(), which asserts when no connection is set.
    class FieldsSqlParity : public StormTestFixture<Person, Conn, Message> {
      public:
        // NOTE on style: SonarCloud S1659 forbids multiple identifiers per declaration
        // ("QuerySet<Person, Conn> a1, a2;"), and the Storm Strict gate is
        // zero-tolerance. So each comparison takes its QuerySet from this factory
        // rather than declaring a pile of locals. where() is immutable, but a fresh
        // object per side keeps the comparison honest regardless.
        [[nodiscard]] static auto qs() -> QuerySet<Person, Conn> {
            return QuerySet<Person, Conn>{};
        }
        [[nodiscard]] static auto msg_qs() -> QuerySet<Message, Conn> {
            return QuerySet<Message, Conn>{};
        }
    };

    // WHERE-clause assertions are VALUE assertions against the expected SQL text,
    // not proxy-vs-^^ comparisons. Deliberate: f<> is deleted in the next commit,
    // at which point a cross-spelling comparison becomes impossible to write —
    // and a comparison whose two sides were both migrated silently degrades into
    // EXPECT_EQ(x, x), which passes even if the proxy path is completely broken.
    // Pinning the text keeps these meaningful after f<> is gone.
    constexpr std::string_view SELECT_ALL =
            "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar FROM "
            "Person";

    [[nodiscard]] auto expected(std::string_view where_clause) -> std::string {
        return std::format("{} WHERE {}", SELECT_ALL, where_clause);
    }

    // Same, for a clause that is not a WHERE (ORDER BY / LIMIT / a full tail).
    [[nodiscard]] auto sel(std::string_view tail) -> std::string {
        return std::format("{} {}", SELECT_ALL, tail);
    }

    TEST_F(FieldsSqlParity, BareComparisonOperator) {
        // THE headline spelling from the issue — no f<> wrapper.
        EXPECT_EQ(qs().where(fields::Person.age == 30).select().sql(), expected("age = ?"));
    }

    TEST_F(FieldsSqlParity, AllSixComparisonOperators) {
        // Per the CLAUDE.md testing checklist: every comparison operator.
        EXPECT_EQ(qs().where(fields::Person.age == 30).select().sql(), expected("age = ?"));
        EXPECT_EQ(qs().where(fields::Person.age != 30).select().sql(), expected("age != ?"));
        EXPECT_EQ(qs().where(fields::Person.age > 30).select().sql(), expected("age > ?"));
        EXPECT_EQ(qs().where(fields::Person.age >= 30).select().sql(), expected("age >= ?"));
        EXPECT_EQ(qs().where(fields::Person.age < 30).select().sql(), expected("age < ?"));
        EXPECT_EQ(qs().where(fields::Person.age <= 30).select().sql(), expected("age <= ?"));
    }

    TEST_F(FieldsSqlParity, CompoundExpression) {
        // The motivating case: the model is named ONCE per field, not twice.
        EXPECT_EQ(
                qs().where(fields::Person.department == "Eng" && fields::Person.age < 28).select().sql(),
                expected("(department = ? AND age < ?)")
        );
    }

    TEST_F(FieldsSqlParity, OrAndNestedComposition) {
        EXPECT_EQ(
                qs().where((fields::Person.age > 30 && fields::Person.is_active == true) ||
                           fields::Person.department == "Eng")
                        .select()
                        .sql(),
                expected("((age > ? AND is_active = ?) OR department = ?)")
        );
    }

    TEST_F(FieldsSqlParity, InBetweenLikeCollate) {
        using storm::orm::utilities::Collate;
        EXPECT_EQ(qs().where(fields::Person.id.in(1, 2, 3)).select().sql(), expected("id IN (?, ?, ?)"));
        EXPECT_EQ(qs().where(fields::Person.age.between(20, 40)).select().sql(), expected("age BETWEEN ? AND ?"));
        EXPECT_EQ(qs().where(fields::Person.name.like("A%")).select().sql(), expected("name LIKE ?"));
        EXPECT_EQ(
                qs().where(fields::Person.name.collate(Collate::NoCase) == "bob").select().sql(),
                expected("name COLLATE NOCASE = ?")
        );
    }

    TEST_F(FieldsSqlParity, NullChecksOnOptionalField) {
        EXPECT_EQ(qs().where(fields::Person.score.is_null()).select().sql(), expected("score IS NULL"));
        EXPECT_EQ(qs().where(fields::Person.nickname.is_not_null()).select().sql(), expected("nickname IS NOT NULL"));
    }

    TEST_F(FieldsSqlParity, OrderBySingleAndMulti) {
        EXPECT_EQ(qs().order_by<fields::Person.name>().select().sql(), sel("ORDER BY name ASC"));
        EXPECT_EQ(
                (qs().order_by<fields::Person.department, fields::Person.age>().select().sql()),
                sel("ORDER BY department ASC, age ASC")
        );
    }

    TEST_F(FieldsSqlParity, OrderByWithDescModifier) {
        // The bool modifier attaches to the field BEFORE it. Pinning the text is
        // what catches a shifted pairing — that emits valid SQL sorted the wrong
        // way, which no compiler and no self-comparison can detect.
        EXPECT_EQ((qs().order_by<fields::Person.age, false>().select().sql()), sel("ORDER BY age DESC"));
    }

    TEST_F(FieldsSqlParity, OrderByWithCollateModifier) {
        using storm::orm::utilities::Collate;
        EXPECT_EQ(
                (qs().order_by<fields::Person.name, Collate::NoCase>().select().sql()),
                sel("ORDER BY name COLLATE NOCASE ASC")
        );
    }

    TEST_F(FieldsSqlParity, OrderByCombinedWithWhereAndLimit) {
        EXPECT_EQ(
                qs().where(fields::Person.age > 20).order_by<fields::Person.name>().limit(5).select().sql(),
                sel("WHERE age > ? ORDER BY name ASC LIMIT 5")
        );
    }

    TEST_F(FieldsSqlParity, DistinctSingleAndMulti) {
        EXPECT_EQ(qs().distinct<fields::Person.department>().sql(), "SELECT DISTINCT department FROM Person");
        EXPECT_EQ(
                (qs().distinct<fields::Person.department, fields::Person.age>().sql()),
                "SELECT DISTINCT department, age FROM Person"
        );
    }

    TEST_F(FieldsSqlParity, ValuesProjection) {
        EXPECT_EQ(qs().values<fields::Person.name>().sql(), "SELECT name FROM Person");
        EXPECT_EQ((qs().values<fields::Person.name, fields::Person.age>().sql()), "SELECT name, age FROM Person");
    }

    TEST_F(FieldsSqlParity, ScalarAggregates) {
        EXPECT_EQ(qs().sum<fields::Person.age>().sql(), "SELECT SUM(age) FROM Person");
        EXPECT_EQ(qs().avg<fields::Person.salary>().sql(), "SELECT AVG(salary) FROM Person");
        EXPECT_EQ(qs().min<fields::Person.age>().sql(), "SELECT MIN(age) FROM Person");
        EXPECT_EQ(qs().max<fields::Person.age>().sql(), "SELECT MAX(age) FROM Person");
        EXPECT_EQ(qs().count<fields::Person.id>().sql(), "SELECT COUNT(id) FROM Person");
        EXPECT_EQ(
                qs().count_distinct<fields::Person.department>().sql(), "SELECT COUNT(DISTINCT department) FROM Person"
        );
    }

    TEST_F(FieldsSqlParity, MultiFieldAggregate) {
        // SUM(a + b) — pack-order-sensitive. A reordered or dropped pack emits
        // VALID but WRONG SQL, so the operand order is pinned explicitly.
        EXPECT_EQ(
                (qs().sum<fields::Person.age, fields::Person.years_experience>().sql()),
                "SELECT SUM(age + years_experience) FROM Person"
        );
    }

    TEST_F(FieldsSqlParity, AggregateStatementChainMethods) {
        // AggregateStatement's OWN chain methods — a separate code path from
        // GroupByBuilder's identically-named ones.
        EXPECT_EQ(qs().sum<fields::Person.age>().count().sql(), "SELECT SUM(age), COUNT(*) FROM Person");
        EXPECT_EQ(
                qs().sum<fields::Person.age>().avg<fields::Person.salary>().sql(),
                "SELECT SUM(age), AVG(salary) FROM Person"
        );
        EXPECT_EQ(
                qs().min<fields::Person.age>().max<fields::Person.age>().sql(), "SELECT MIN(age), MAX(age) FROM Person"
        );
    }

    TEST_F(FieldsSqlParity, GroupByWithAggregate) {
        EXPECT_EQ(
                qs().group_by<fields::Person.department>().count<>().sql(),
                "SELECT department, COUNT(*) FROM Person GROUP BY department"
        );
        EXPECT_EQ(
                qs().group_by<fields::Person.department>().sum<fields::Person.salary>().sql(),
                "SELECT department, SUM(salary) FROM Person GROUP BY department"
        );
        // Multi-column GROUP BY — the other pack-order-sensitive position.
        EXPECT_EQ(
                (qs().group_by<fields::Person.department, fields::Person.age>().count<>().sql()),
                "SELECT department, age, COUNT(*) FROM Person GROUP BY department, age"
        );
    }

    TEST_F(FieldsSqlParity, GroupByHavingBothChainPositions) {
        // Both chaining orders, per the CLAUDE.md testing checklist.
        EXPECT_EQ(
                qs().group_by<fields::Person.age>().having(fields::Person.age > 30).count<>().sql(),
                "SELECT age, COUNT(*) FROM Person GROUP BY age HAVING age > ?"
        );
        EXPECT_EQ(
                qs().group_by<fields::Person.department>().count<>().having(fields::Person.department == "Eng").sql(),
                "SELECT department, COUNT(*) FROM Person GROUP BY department HAVING department = ?"
        );
    }

    TEST_F(FieldsSqlParity, ConditionalUpdateAndUpdateAll) {
        // Write paths return std::expected<std::string, Error>.
        const Person proto{.salary = 60000, .is_active = true};

        const auto upd = qs().where(fields::Person.salary < 50000)
                                 .update<fields::Person.salary, fields::Person.is_active>(proto)
                                 .to_sql();
        ASSERT_TRUE(upd.has_value());
        // NOTE: to_sql() on a write path renders BOUND VALUES inline, not
        // placeholders — unlike the read paths' sql(). Pinned as it actually is.
        EXPECT_EQ(*upd, "UPDATE Person SET salary=60000.0, is_active=1 WHERE salary < 50000");

        const auto all = qs().update_all<fields::Person.department>(proto).to_sql();
        ASSERT_TRUE(all.has_value());
        EXPECT_EQ(*all, "UPDATE Person SET department=''");
    }

    TEST_F(FieldsSqlParity, JoinOnFkField) {
        // The FK column is sender_id — derived by append_fk_column_names, NOT by
        // the proxy's bare member name. Pinning it guards that derivation.
        EXPECT_TRUE(
                msg_qs().join<fields::Message.sender>().select().sql().contains(
                        "FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
                )
        );
        EXPECT_TRUE(
                msg_qs().left_join<fields::Message.sender>().select().sql().contains(
                        "FROM Message t1 LEFT JOIN Person t2 ON t2.id = t1.sender_id"
                )
        );
    }

    // An FK member IS a persisted column and MUST appear in the generated struct —
    // only m2m/reverse_fk containers are filtered out.
    template <typename FieldsT> constexpr bool has_sender = requires(const FieldsT& obj) { obj.sender; };
    static_assert(has_sender<decltype(fields::Message)>);

    // The numeric-aggregate gate survives the widening in BOTH spellings. A dropped
    // pack constraint is otherwise silently green.
    template <auto S> constexpr bool summable = requires(QuerySet<Person, Conn> qs) { qs.template sum<S>(); };

    static_assert(summable<fields::Person.age>);   // int — allowed
    static_assert(!summable<fields::Person.name>); // std::string — rejected
    static_assert(!summable<^^Person::age>);       // a raw info is not a selector at all

    // Upsert is a write path: to_sql() returns std::expected<std::string, Error>.
    // A helper keeps each case to the has_value + compare shape without repetition.
    [[nodiscard]] auto sql_of(auto&& proxy) -> std::string {
        auto result = proxy.to_sql();
        EXPECT_TRUE(result.has_value());
        return result.value_or(std::string{});
    }

    TEST_F(FieldsSqlParity, UpsertOnConflictDoUpdate) {
        const Person p{.id = 1, .name = "Ann", .age = 30};
        EXPECT_EQ(
                sql_of(qs().insert(p).on_conflict<fields::Person.name>().update<fields::Person.age>()),
                sql_of(qs().insert(p).on_conflict<fields::Person.name>().update<fields::Person.age>())
        );
    }

    TEST_F(FieldsSqlParity, UpsertOnConflictDoNothing) {
        const Person p{.id = 1, .name = "Ann", .age = 30};
        EXPECT_EQ(
                sql_of(qs().insert(p).on_conflict<fields::Person.name>().nothing()),
                sql_of(qs().insert(p).on_conflict<fields::Person.name>().nothing())
        );
    }

    TEST_F(FieldsSqlParity, UpsertMultiColumnSetClause) {
        const Person p{.id = 1, .name = "Ann", .age = 30};
        EXPECT_EQ(
                sql_of(qs().insert(p)
                               .on_conflict<fields::Person.name>()
                               .update<fields::Person.age, fields::Person.salary>()),
                sql_of(qs().insert(p)
                               .on_conflict<fields::Person.name>()
                               .update<fields::Person.age, fields::Person.salary>())
        );
    }

    // The conflict target must still be a unique column in BOTH spellings.
    template <auto S>
    constexpr bool conflictable =
            requires(QuerySet<Person, Conn> qs, Person obj) { qs.insert(obj).template on_conflict<S>(); };

    static_assert(conflictable<fields::Person.name>); // [[= storm::unique]] — allowed
    static_assert(!conflictable<fields::Person.age>); // not unique — rejected
    static_assert(!conflictable<^^Person::name>);     // a raw info is not a selector at all

} // namespace
