#pragma once

// Shared test body for test_many_to_many_sqlite.cpp / test_many_to_many_pg.cpp — the two
// single-backend TUs of a compile-time TU split (see test_db_helpers.h,
// DatabaseTypesSqliteHalf/DatabaseTypesPgHalf). Splitting a 2-backend TU into
// two lets ninja compile them in parallel instead of serially instantiating
// both backends in one TU; keeping the body here (instead of duplicating it
// into both .cpp files) removes the risk of the two halves silently drifting.
//
// The includer must #define STORM_SPLIT_TYPES / STORM_SPLIT_TYPE_NAMES to one
// backend's ::testing::Types<> alias / NameGenerator before #include-ing this
// file, and #undef both afterward. Never include this file directly.
#if !defined(STORM_SPLIT_TYPES) || !defined(STORM_SPLIT_TYPE_NAMES)
#error "test_many_to_many_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm_defaults;
import storm_db_default_connection;
import storm_db_sqlite;
import storm_model;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_transaction;
import storm_orm_utilities;
import storm_orm_statements_update_grammar; // UpdateGrammar — SET-target gate (#486) / SQL leak assert (#485); not re-exported by `storm`
import storm_orm_statements_base;
import storm_orm_fields;
import storm_orm_statements_upsert_grammar;
import storm_db_postgresql;
import std;

#include "test_fixture.h" // NOSONAR cpp:S954

#include "test_m2m_models.h" // NOSONAR cpp:S954
using storm::meta::FieldRef;
using storm::orm::schema::SchemaStatement;
using storm::orm::statements::BaseStatement;
using storm::orm::statements::ConflictTargetUnique;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateGrammar;
using storm::orm::statements::UpdateStatement;
using storm::orm::statements::UpsertGrammar;
using storm::orm::statements::UpsertSettable;

// ============================================================================
// Compile-time: m2m fields are excluded from persisted members (#203)
// ============================================================================

static_assert(BaseStatement<Student>::field_count_ == 3, "courses (m2m) must not count as a persisted column");
static_assert(BaseStatement<Course>::field_count_ == 2);

// Pre-computed single-row/bulk UPDATE SQL must not name relation containers (#485).
constexpr std::string_view student_update_sql{UpdateGrammar<Student>::update_sql_array.data.data(),
                                              UpdateGrammar<Student>::update_sql_array.len};
static_assert(!student_update_sql.contains("courses"), "m2m container leaked into UPDATE SET clause");
constexpr std::string_view pupil_update_sql{UpdateGrammar<Pupil>::update_sql_array.data.data(),
                                            UpdateGrammar<Pupil>::update_sql_array.len};
static_assert(!pupil_update_sql.contains("courses"), "m2m_through container leaked into UPDATE SET clause");

// ============================================================================
// Compile-time: M2MFieldOf concept
// ============================================================================

static_assert(storm::orm::statements::M2MFieldOf<Student, ^^Student::courses>);
static_assert(!storm::orm::statements::M2MFieldOf<Student, ^^Student::name>);
static_assert(!storm::orm::statements::FKFieldOf<Student, ^^Student::courses>);
static_assert(storm::orm::statements::M2MFieldOf<Pupil, ^^Pupil::courses>);

// ============================================================================
// Compile-time: f<>() rejects m2m relation members (#408)
// An m2m container is not a persisted column — a WHERE clause on it would
// reference a non-existent column, so f<>() must fail at the call site.
// ============================================================================

template <auto M>
concept FCallableM2M = requires { FieldRef<M>{}; };

static_assert(!FCallableM2M<^^Student::courses>, "m2m member rejected by FieldRef");
static_assert(!FCallableM2M<^^Pupil::courses>, "m2m_through member rejected by FieldRef");
static_assert(FCallableM2M<^^Student::name>, "persisted column still accepted");
static_assert(FCallableM2M<^^Student::age>, "persisted column still accepted");

// ============================================================================
// Compile-time: SET-target gates reject m2m relation members (#486)
// A relation container is not a persisted column, so passing it as a
// SET-column NTTP to update<>()/update_all<>() or upsert .update<>() must
// fail at the named gate, not at runtime with "no such column".
// ============================================================================

static_assert(!UpdateGrammar<Student>::is_settable_member<^^Student::courses>(),
              "m2m member rejected as a conditional-UPDATE SET target");
static_assert(!UpsertGrammar<Student>::is_settable_member<^^Student::courses>(),
              "m2m member rejected as an upsert DO UPDATE SET target");
static_assert(!UpsertSettable<Student, ^^Student::courses>, "UpsertSettable concept rejects an m2m SET target");
static_assert(!UpsertSettable<Pupil, ^^Pupil::courses>, "UpsertSettable concept rejects an m2m_through SET target");

// Persisted, non-PK columns are still accepted at the same gate.
static_assert(UpdateGrammar<Student>::is_settable_member<^^Student::name>());
static_assert(UpsertGrammar<Student>::is_settable_member<^^Student::age>());
static_assert(UpsertSettable<Student, ^^Student::name>);
// The primary key remains rejected (pre-existing gate behavior).
static_assert(!UpdateGrammar<Student>::is_settable_member<^^Student::id>());

// The conflict-target gate is already safe (#486 DoD): a relation member carries
// no unique annotation and cannot appear in a UniqueIndex, so
// ConflictTargetUnique rejects it without needing an is_relation_field check.
static_assert(!ConflictTargetUnique<Student, ^^Student::courses>, "an m2m member is not a valid conflict target");

// ============================================================================
// Compile-time: related-type extraction from containers via std::meta
// ============================================================================

static_assert(std::same_as<storm::orm::statements::meta::m2m_related_t<std::vector<Course>>, Course>);
static_assert(std::same_as<storm::orm::statements::meta::m2m_related_t<plf::hive<Track>>, Track>);
static_assert(std::same_as<storm::orm::statements::meta::m2m_related_t<std::vector<std::shared_ptr<Track>>>, Track>);
static_assert(std::same_as<storm::orm::statements::meta::m2m_related_t<std::deque<Course>>, Course>);
static_assert(std::same_as<storm::orm::statements::meta::m2m_related_t<std::vector<std::unique_ptr<Course>>>, Course>);

// ============================================================================
// Runtime: CRUD on a model WITH an m2m field ignores the container member
// ============================================================================

template <typename ConnType> class M2MBaseTest : public StormTestFixture<Student, ConnType, Course> {};
TYPED_TEST_SUITE(M2MBaseTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(M2MBaseTest, PlainCrudIgnoresM2MField) {
    Student const s{.name = "Alice", .age = 20, .courses = {{.id = 99, .title = "ghost"}}};
    auto ins = (InsertStatement<Student, TypeParam>{}).query(s).execute();
    ASSERT_TRUE(ins.has_value()) << ins.error().message();

    auto rows = (SelectStatement<Student, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->begin()->name, "Alice");
    EXPECT_EQ(rows->begin()->age, 20);
    EXPECT_TRUE(rows->begin()->courses.empty()); // plain select never populates m2m
}

// Single-row UPDATE must skip the m2m container (#485): the SET clause may only
// name persisted columns, and the PK must bind into the last placeholder —
// verified by round-tripping through a re-select.
TYPED_TEST(M2MBaseTest, SingleRowUpdateIgnoresM2MField) {
    Student const s{.name = "Alice", .age = 20};
    auto ins = (InsertStatement<Student, TypeParam>{}).query(s).execute();
    ASSERT_TRUE(ins.has_value()) << ins.error().message();

    Student const updated{.id = static_cast<int>(ins.value()),
                          .name = "Alice Updated",
                          .age = 21,
                          .courses{{.id = 99, .title = "ghost"}}};
    auto upd = (UpdateStatement<Student, TypeParam>{}).query(updated).execute();
    ASSERT_TRUE(upd.has_value()) << upd.error().message();

    auto rows = (SelectStatement<Student, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->begin()->id, static_cast<int>(ins.value()));
    EXPECT_EQ(rows->begin()->name, "Alice Updated");
    EXPECT_EQ(rows->begin()->age, 21);
}

// Bulk UPDATE goes through the same pre-computed SQL (#485).
TYPED_TEST(M2MBaseTest, BulkUpdateIgnoresM2MField) {
    std::vector<Student> const students = {{.name = "Alice", .age = 20}, {.name = "Bob", .age = 22}};
    ASSERT_TRUE(
        (InsertStatement<Student, TypeParam>{}).query(std::span<const Student>(students)).execute().has_value());

    std::vector<Student> const updated = {
        {.id = 1, .name = "Alice2", .age = 30, .courses{{.id = 99, .title = "ghost"}}},
        {.id = 2, .name = "Bob2", .age = 32},
    };
    auto upd = (UpdateStatement<Student, TypeParam>{}).query(std::span<const Student>(updated)).execute();
    ASSERT_TRUE(upd.has_value()) << upd.error().message();

    auto rows = (SelectStatement<Student, TypeParam>{}).all().template order_by<fields::Student.id>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 2U);
    auto it = rows->begin();
    EXPECT_EQ(it->name, "Alice2");
    EXPECT_EQ(it->age, 30);
    ++it;
    EXPECT_EQ(it->name, "Bob2");
    EXPECT_EQ(it->age, 32);
}

// A through-model m2m field must NOT produce an auto junction table.
static_assert(!SchemaStatement<Pupil>::has_m2m_junction_);
static_assert(SchemaStatement<Student>::has_m2m_junction_);

TYPED_TEST(M2MBaseTest, JunctionTableIsCreatedAndWritable) {
    auto conn = DefaultConnection<TypeParam>::get();

    // Seed the referenced rows — the junction FKs (#412) require a real Student and
    // Course before a link row may be inserted.
    auto student_id = (InsertStatement<Student, TypeParam>{}).query(Student{.name = "S", .age = 20}).execute();
    ASSERT_TRUE(student_id.has_value()) << student_id.error().message();
    auto course_id = (InsertStatement<Course, TypeParam>{}).query(Course{.title = "C"}).execute();
    ASSERT_TRUE(course_id.has_value()) << course_id.error().message();

    const std::string pair = std::format("INSERT INTO Student_Course (Student_id, Course_id) VALUES ({}, {})",
                                         student_id.value(), course_id.value());

    // The fixture ran create_table_if_not_exists<Student> — the junction must exist.
    auto ins = conn->execute(pair);
    ASSERT_TRUE(ins.has_value()) << ins.error().message();
    // Composite PK rejects duplicate pairs.
    auto dup = conn->execute(pair);
    EXPECT_FALSE(dup.has_value());

    // A junction row with a dangling FK is rejected (referential integrity, #412).
    auto dangling = conn->execute("INSERT INTO Student_Course (Student_id, Course_id) VALUES (99999, 99999)");
    EXPECT_FALSE(dangling.has_value()) << "Junction FK must reject a dangling owner/related id";
}

// ============================================================================
// SQL shape: two-query predicate-pushdown eager load (#391)
//   Q1 = base entities; Q2 = (owner_pk, related.*) filtered by the same base
//   subquery. .sql() joins them with "; ". The execution path runs them as two
//   separate prepared statements inside a transaction (see execute_m2m_2query).
// ============================================================================

TYPED_TEST(M2MBaseTest, M2MJoinSqlShape) {
    auto sql = (SelectStatement<Student, TypeParam>{}).all().template join<fields::Student.courses>().sql();
    // Q1 — base entities, a plain SELECT (no join, no sorter).
    EXPECT_TRUE(sql.contains("SELECT id, name, age FROM Student; ")) << sql;
    // Q2 — junction⋈related, related rows filtered by the same base subquery.
    EXPECT_TRUE(sql.contains("SELECT t2.Student_id, t3.id, t3.title")) << sql;
    EXPECT_TRUE(sql.contains("FROM Student_Course t2 INNER JOIN Course t3 ON t2.Course_id = t3.id")) << sql;
    EXPECT_TRUE(sql.contains("WHERE t2.Student_id IN (SELECT id FROM Student)")) << sql;
    // No outer ORDER BY / pk-adjacency contract — the stitch is a hash map.
    EXPECT_FALSE(sql.contains(" t1")) << sql;
}

TYPED_TEST(M2MBaseTest, M2MJoinSqlModifiersGoInsideSubquery) {
    auto sql = (SelectStatement<Student, TypeParam>{})
                   .all()
                   .template join<fields::Student.courses>()
                   .where(fields::Student.age > 18)
                   .template order_by<fields::Student.name, false>()
                   .limit(2)
                   .sql();
    // WHERE / ORDER BY / LIMIT select WHICH base entities load — they appear in
    // Q1 directly AND inside Q2's IN-subquery so both pick the same entities.
    // PG dialect adds NULLS LAST after DESC (matches SQLite NULL ordering).
    if constexpr (storm::test::is_postgresql<TypeParam>()) {
        EXPECT_TRUE(
            sql.contains("SELECT id, name, age FROM Student WHERE age > ? ORDER BY name DESC NULLS LAST LIMIT 2; "))
            << sql;
        EXPECT_TRUE(sql.contains("IN (SELECT id FROM Student WHERE age > ? ORDER BY name DESC NULLS LAST LIMIT 2)"))
            << sql;
    } else {
        EXPECT_TRUE(sql.contains("SELECT id, name, age FROM Student WHERE age > ? ORDER BY name DESC LIMIT 2; "))
            << sql;
        EXPECT_TRUE(sql.contains("IN (SELECT id FROM Student WHERE age > ? ORDER BY name DESC LIMIT 2)")) << sql;
    }
}

TYPED_TEST(M2MBaseTest, M2MJoinSqlMultiFieldOrderByQualifiesAllFields) {
    auto sql = (SelectStatement<Student, TypeParam>{})
                   .all()
                   .template join<fields::Student.courses>()
                   .template order_by<fields::Student.age, fields::Student.name>()
                   .sql();
    // Multi-field ORDER BY appears (unqualified — no t1. surgery) in Q1 and the
    // Q2 IN-subquery; no pk tiebreak (the stitch is a hash map, not adjacency).
    if constexpr (storm::test::is_postgresql<TypeParam>()) {
        EXPECT_TRUE(sql.contains("ORDER BY age ASC NULLS FIRST, name ASC NULLS FIRST")) << sql;
    } else {
        EXPECT_TRUE(sql.contains("SELECT id, name, age FROM Student ORDER BY age ASC, name ASC; ")) << sql;
        EXPECT_TRUE(sql.contains("IN (SELECT id FROM Student ORDER BY age ASC, name ASC)")) << sql;
    }
}

TYPED_TEST(M2MBaseTest, M2MLeftJoinSqlShape) {
    auto sql = (SelectStatement<Student, TypeParam>{}).all().template left_join<fields::Student.courses>().sql();
    // Q2 is always an INNER junction⋈related join — LEFT vs INNER is a post-stitch
    // filter (LEFT keeps zero-relation entities), never an SQL difference.
    EXPECT_TRUE(sql.contains("SELECT id, name, age FROM Student; ")) << sql;
    EXPECT_TRUE(sql.contains("FROM Student_Course t2 INNER JOIN Course t3")) << sql;
}

// ============================================================================
// Runtime: eager loading aggregates related objects (#203 Phase 1)
// ============================================================================

// Fixture with seeded data: Alice→[Math, Physics], Bob→[Math], Carol→[].
template <typename ConnType> class M2MSeededTest : public StormTestFixture<Student, ConnType, Course> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        storm::test::seed_students<ConnType>();
    }
};

TYPED_TEST_SUITE(M2MSeededTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Finds the student with the given name; fails the test if absent.
template <typename Hive> auto find_student(Hive &rows, std::string_view name) -> Student * {
    for (auto &s : rows) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

TYPED_TEST(M2MSeededTest, EagerLoadAggregatesCourses) {
    auto rows = (SelectStatement<Student, TypeParam>{}).all().template join<fields::Student.courses>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    // INNER join drops Carol (no courses); 3 joined rows aggregate into 2 students
    ASSERT_EQ(rows->size(), 2U);

    auto *alice = find_student(*rows, "Alice");
    ASSERT_NE(alice, nullptr);
    ASSERT_EQ(alice->courses.size(), 2U);
    EXPECT_EQ(alice->courses[0].title, "Math");
    EXPECT_EQ(alice->courses[1].title, "Physics");
    EXPECT_EQ(alice->age, 20);

    auto *bob = find_student(*rows, "Bob");
    ASSERT_NE(bob, nullptr);
    ASSERT_EQ(bob->courses.size(), 1U);
    EXPECT_EQ(bob->courses[0].title, "Math");
}

// Referential integrity (#412): deleting an owner cascades to its junction rows.
// Alice (id 1) owns two Student_Course links (Math, Physics); deleting her must
// leave no orphaned junction rows for Student_id = 1 (ON DELETE CASCADE).
TYPED_TEST(M2MSeededTest, DeletingOwnerCascadesJunctionRows) {
    auto conn = DefaultConnection<TypeParam>::get();

    auto before = conn->prepare("SELECT COUNT(*) FROM Student_Course WHERE Student_id = 1");
    ASSERT_TRUE(before.has_value());
    auto before_stmt = std::move(before.value());
    ASSERT_EQ(before_stmt.step_raw(), decltype(before_stmt)::ROW_AVAILABLE);
    ASSERT_EQ(before_stmt.extract_int64(0), 2) << "Alice should start with two junction rows";

    auto del = (EraseStatement<Student, TypeParam>{}).query_where(fields::Student.id == 1).execute();
    ASSERT_TRUE(del.has_value()) << "Deleting Alice failed: " << del.error().message();

    auto after = conn->prepare("SELECT COUNT(*) FROM Student_Course WHERE Student_id = 1");
    ASSERT_TRUE(after.has_value());
    auto after_stmt = std::move(after.value());
    ASSERT_EQ(after_stmt.step_raw(), decltype(after_stmt)::ROW_AVAILABLE);
    EXPECT_EQ(after_stmt.extract_int64(0), 0) << "Junction rows must cascade away when the owner is deleted";
}

// #414 / #9 — the m2m two-query prefetch opens its own TransactionGuard for
// snapshot consistency. Inside an outer storm::begin() scope it must not fail on
// a nested BEGIN; the prefetch goes passive and the outer guard owns the commit.
TYPED_TEST(M2MSeededTest, EagerLoadInsideOuterTransactionSucceeds) {
    auto txn = storm::begin(DefaultConnection<TypeParam>::get());
    ASSERT_TRUE(txn.has_value()) << "storm::begin should start a transaction";

    auto rows = (SelectStatement<Student, TypeParam>{}).all().template join<fields::Student.courses>().execute();
    ASSERT_TRUE(rows.has_value()) << "m2m prefetch inside outer txn must not fail on nested BEGIN: "
                                  << (rows ? "" : rows.error().message());
    ASSERT_EQ(rows->size(), 2U);

    ASSERT_TRUE(txn->commit().has_value()) << "outer commit should succeed";
}

TYPED_TEST(M2MSeededTest, LeftJoinKeepsStudentsWithoutCourses) {
    auto rows = (SelectStatement<Student, TypeParam>{}).all().template left_join<fields::Student.courses>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 3U);

    auto *carol = find_student(*rows, "Carol");
    ASSERT_NE(carol, nullptr);
    EXPECT_TRUE(carol->courses.empty());
    auto *alice = find_student(*rows, "Alice");
    ASSERT_NE(alice, nullptr);
    EXPECT_EQ(alice->courses.size(), 2U);
}

TYPED_TEST(M2MSeededTest, EmptyResultSet) {
    auto rows = (SelectStatement<Student, TypeParam>{})
                    .all()
                    .template join<fields::Student.courses>()
                    .where(fields::Student.age > 99)
                    .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    EXPECT_TRUE(rows->empty());
}

// Aggregates over an m2m join count (student, course) PAIRS — documented behavior.
TYPED_TEST(M2MSeededTest, CountOverM2MJoinCountsPairs) {
    auto count =
        (SelectStatement<Student, TypeParam>{}).all().template join<fields::Student.courses>().count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 3);
}

// ============================================================================
// Container coverage (#203 Phase 1 edge cases): plf::hive and shared_ptr elements
// ============================================================================

template <typename ConnType> class M2MContainerTest : public StormTestFixture<Playlist, ConnType, Track, Album> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> &conn) -> void override {
        ASSERT_TRUE((InsertStatement<Playlist, ConnType>{}).query(Playlist{.name = "Road trip"}).execute().has_value());
        ASSERT_TRUE((InsertStatement<Album, ConnType>{}).query(Album{.name = "Greatest hits"}).execute().has_value());
        std::vector<Track> const tracks = {{.title = "Intro"}, {.title = "Outro"}};
        ASSERT_TRUE((InsertStatement<Track, ConnType>{}).query(std::span<const Track>(tracks)).execute().has_value());
        for (const auto *sql : {"INSERT INTO Playlist_Track (Playlist_id, Track_id) VALUES (1, 1)",
                                "INSERT INTO Playlist_Track (Playlist_id, Track_id) VALUES (1, 2)",
                                "INSERT INTO Album_Track (Album_id, Track_id) VALUES (1, 2)"}) {
            ASSERT_TRUE(conn->execute(sql).has_value());
        }
    }
};

TYPED_TEST_SUITE(M2MContainerTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(M2MContainerTest, HiveContainerEagerLoad) {
    auto rows = (SelectStatement<Playlist, TypeParam>{}).all().template join<fields::Playlist.tracks>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    ASSERT_EQ(rows->begin()->tracks.size(), 2U); // appended via hive insert()
    auto track_it = rows->begin()->tracks.begin();
    EXPECT_EQ(track_it->title, "Intro");
}

TYPED_TEST(M2MContainerTest, SharedPtrElementsEagerLoad) {
    auto rows = (SelectStatement<Album, TypeParam>{}).all().template join<fields::Album.tracks>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    ASSERT_EQ(rows->begin()->tracks.size(), 1U);
    ASSERT_NE(rows->begin()->tracks[0], nullptr); // wrapped via make_shared
    EXPECT_EQ(rows->begin()->tracks[0]->title, "Outro");
}

// NOLINTEND(misc-const-correctness)
