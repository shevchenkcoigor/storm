#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import std;

#include "test_fixture.h"    // NOSONAR cpp:S954
#include "test_m2m_models.h" // NOSONAR cpp:S954
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateStatement;

// ============================================================================
// Phase 2 (#203): explicit junction model — many_to_many_through<Enrollment>
// Pupil ⟷ Course through Enrollment (with `grade` metadata).
// ============================================================================

template <typename ConnType> class M2MThroughTest : public StormTestFixture<Pupil, ConnType, Course, Enrollment> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
        std::vector<Pupil> const pupils = {{.name = "Dora", .age = 11}, {.name = "Eli", .age = 12}};
        ASSERT_TRUE((InsertStatement<Pupil, ConnType>{}).query(std::span<const Pupil>(pupils)).execute().has_value());

        std::vector<Course> const courses = {{.title = "Math"}, {.title = "Art"}};
        ASSERT_TRUE(
                (InsertStatement<Course, ConnType>{}).query(std::span<const Course>(courses)).execute().has_value()
        );

        // Junction rows are plain Enrollment inserts — the through model is a model.
        std::vector<Enrollment> const enrollments = {
                {.pupil = {.id = 1}, .course = {.id = 1}, .grade = "A"},
                {.pupil = {.id = 1}, .course = {.id = 2}, .grade = "B"},
                {.pupil = {.id = 2}, .course = {.id = 1}, .grade = "C"},
        };
        ASSERT_TRUE((InsertStatement<Enrollment, ConnType>{})
                            .query(std::span<const Enrollment>(enrollments))
                            .execute()
                            .has_value());
    }
};

TYPED_TEST_SUITE(M2MThroughTest, DatabaseTypes);

TYPED_TEST(M2MThroughTest, ThroughSqlUsesJunctionModel) {
    auto sql = (SelectStatement<Pupil, TypeParam>{}).all().template join<fields::Pupil.courses>().sql();
    // Two-query shape (#391): junction = the through model's table; FK columns
    // come from its field names. Q1 selects the base pupils; Q2 the junction⋈course.
    EXPECT_TRUE(sql.contains("SELECT id, name, age FROM Pupil; ")) << sql;
    EXPECT_TRUE(sql.contains("FROM Enrollment t2 INNER JOIN Course t3 ON t2.course_id = t3.id")) << sql;
    EXPECT_TRUE(sql.contains("WHERE t2.pupil_id IN (SELECT id FROM Pupil)")) << sql;
}

// Single-row and bulk UPDATE on a through-model owner must skip the m2m
// container (#485); round-trip re-select verifies values and PK bind order.
TYPED_TEST(M2MThroughTest, SingleRowUpdateIgnoresThroughField) {
    Pupil const updated{.id = 1, .name = "Dora Updated", .age = 13, .courses{{.id = 99, .title = "ghost"}}};
    auto        upd = (UpdateStatement<Pupil, TypeParam>{}).query(updated).execute();
    ASSERT_TRUE(upd.has_value()) << upd.error().message();

    auto rows = (SelectStatement<Pupil, TypeParam>{}).where(fields::Pupil.id == 1).execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->begin()->name, "Dora Updated");
    EXPECT_EQ(rows->begin()->age, 13);
}

TYPED_TEST(M2MThroughTest, BulkUpdateIgnoresThroughField) {
    std::vector<Pupil> const updated = {
            {.id = 1, .name = "Dora2", .age = 14, .courses{{.id = 99, .title = "ghost"}}},
            {.id = 2, .name = "Eli2", .age = 15},
    };
    auto upd = (UpdateStatement<Pupil, TypeParam>{}).query(std::span<const Pupil>(updated)).execute();
    ASSERT_TRUE(upd.has_value()) << upd.error().message();

    auto rows = (SelectStatement<Pupil, TypeParam>{}).all().template order_by<fields::Pupil.id>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 2U);
    auto it = rows->begin();
    EXPECT_EQ(it->name, "Dora2");
    EXPECT_EQ(it->age, 14);
    ++it;
    EXPECT_EQ(it->name, "Eli2");
    EXPECT_EQ(it->age, 15);
}

TYPED_TEST(M2MThroughTest, ThroughEagerLoadIgnoresMetadata) {
    auto rows = (SelectStatement<Pupil, TypeParam>{}).all().template join<fields::Pupil.courses>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 2U);

    for (const auto& pupil : *rows) {
        if (pupil.name == "Dora") {
            ASSERT_EQ(pupil.courses.size(), 2U);
            EXPECT_EQ(pupil.courses[0].title, "Math");
            EXPECT_EQ(pupil.courses[1].title, "Art");
        } else {
            EXPECT_EQ(pupil.name, "Eli");
            ASSERT_EQ(pupil.courses.size(), 1U);
            EXPECT_EQ(pupil.courses[0].title, "Math");
        }
    }
}

TYPED_TEST(M2MThroughTest, ThroughWithWhereOrderAndLimit) {
    auto rows = (SelectStatement<Pupil, TypeParam>{})
                        .all()
                        .template join<fields::Pupil.courses>()
                        .where(fields::Pupil.age >= 11)
                        .template order_by<fields::Pupil.name, false>()
                        .limit(1)
                        .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->begin()->name, "Eli"); // DESC → Eli first
    EXPECT_EQ(rows->begin()->courses.size(), 1U);
}

// Metadata access: query the junction model directly with the existing FK join API.
TYPED_TEST(M2MThroughTest, JunctionModelQueriedDirectlyForMetadata) {
    auto rows = (SelectStatement<Enrollment, TypeParam>{})
                        .all()
                        .template join<fields::Enrollment.pupil, fields::Enrollment.course>()
                        .where(fields::Enrollment.grade == "B")
                        .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->begin()->grade, "B");
    EXPECT_EQ(rows->begin()->pupil.name, "Dora");
    EXPECT_EQ(rows->begin()->course.title, "Art");
}

// NOLINTEND(misc-const-correctness)
