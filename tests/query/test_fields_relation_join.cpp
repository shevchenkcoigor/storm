#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include "plf_hive/plf_hive.h"

import storm;
import std;

using storm::QuerySet;

#include "test_fixture.h"           // NOSONAR cpp:S954
#include "test_m2m_models.h"        // NOSONAR cpp:S954
#include "test_reverse_fk_models.h" // NOSONAR cpp:S954

namespace {

    using Conn = storm::db::sqlite::Connection;

    // A relation member is not a column, but IS a legal join target. These assert
    // the spelling `join<fields::Student.courses>()` exists and emits byte-identical
    // SQL to the ^^ form — the 65 in-tree m2m/reverse-FK join sites depend on it.
    class FieldsRelationJoin : public StormTestFixture<Student, Conn, Course, Pupil, RfPerson, RfTask> {
      public:
        [[nodiscard]] static auto student_qs() -> QuerySet<Student, Conn> {
            return QuerySet<Student, Conn>{};
        }
        [[nodiscard]] static auto pupil_qs() -> QuerySet<Pupil, Conn> {
            return QuerySet<Pupil, Conn>{};
        }
        [[nodiscard]] static auto rf_qs() -> QuerySet<RfPerson, Conn> {
            return QuerySet<RfPerson, Conn>{};
        }
    };

    // Pinned SQL text, not proxy-vs-^^ comparison: the ^^ spelling is gone from
    // these positions, and a comparison whose two sides were both migrated
    // degrades into EXPECT_EQ(x, x) — passing even if relation joins break
    // entirely. The eager load is TWO statements (Q1 base rows; Q2 the relation),
    // so the Q2 half is what actually proves the relation was resolved.
    constexpr std::string_view STUDENT_Q2 =
            "SELECT t2.Student_id, t3.id, t3.title FROM Student_Course t2 INNER JOIN Course t3 ON t2.Course_id = "
            "t3.id WHERE t2.Student_id IN (SELECT id FROM Student)";

    TEST_F(FieldsRelationJoin, AutoJunctionM2MJoin) {
        // INNER and LEFT render byte-identical SQL: the difference only shows in how the
        // stitched rows are filtered, not in the two statements.
        const std::string expected_sql = std::format("SELECT id, name, age FROM Student; {}", STUDENT_Q2);
        EXPECT_EQ(student_qs().join<fields::Student.courses>().select().sql(), expected_sql);
        EXPECT_EQ(student_qs().left_join<fields::Student.courses>().select().sql(), expected_sql);
    }

    TEST_F(FieldsRelationJoin, ThroughModelM2MJoin) {
        // many_to_many_through<Enrollment> — the junction is the explicit model,
        // so Q2 names Enrollment and its own FK columns rather than Student_Course.
        EXPECT_EQ(
                pupil_qs().join<fields::Pupil.courses>().select().sql(),
                "SELECT id, name, age FROM Pupil; SELECT t2.pupil_id, t3.id, t3.title FROM Enrollment t2 INNER JOIN "
                "Course t3 ON t2.course_id = t3.id WHERE t2.pupil_id IN (SELECT id FROM Pupil)"
        );
    }

    TEST_F(FieldsRelationJoin, ReverseFkJoin) {
        // reverse_fk goes through the same RelationRef path as m2m, but its Q2
        // hits the OWNER TABLE DIRECTLY — no junction table and no second JOIN.
        // That structural difference is what this pinned text asserts.
        constexpr std::string_view rfk =
                "SELECT id, name, age FROM RfPerson; SELECT t2.assignee_id, t2.id, t2.title, t2.assignee_id FROM "
                "RfTask t2 WHERE t2.assignee_id IN (SELECT id FROM RfPerson)";
        EXPECT_EQ(rf_qs().join<fields::RfPerson.tasks>().select().sql(), rfk);
        EXPECT_EQ(rf_qs().left_join<fields::RfPerson.tasks>().select().sql(), rfk);
    }

    TEST_F(FieldsRelationJoin, RelationJoinCombinedWithWhereAndOrderBy) {
        // A relation proxy must compose with column proxies in one chain, and the
        // WHERE/ORDER BY must reach BOTH queries — Q2's IN-subquery repeats them
        // so the relation is only loaded for the base rows that survived.
        EXPECT_EQ(
                student_qs()
                        .join<fields::Student.courses>()
                        .where(fields::Student.age > 20)
                        .order_by<fields::Student.name>()
                        .select()
                        .sql(),
                "SELECT id, name, age FROM Student WHERE age > ? ORDER BY name ASC; SELECT t2.Student_id, t3.id, "
                "t3.title FROM Student_Course t2 INNER JOIN Course t3 ON t2.Course_id = t3.id WHERE t2.Student_id IN "
                "(SELECT id FROM Student WHERE age > ? ORDER BY name ASC)"
        );
    }

} // namespace
