#pragma once

// Shared test body for test_reverse_fk_sqlite.cpp / test_reverse_fk_pg.cpp — the two
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
#error "test_reverse_fk_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm_defaults;
import storm_db_default_connection;
import storm_db_sqlite;
import storm_model;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_statements_update_grammar; // UpdateGrammar — SET-target gate (#486) / SQL leak assert (#485); not re-exported by `storm`
import storm_orm_statements_base;
import storm_orm_statements_join;
import storm_orm_fields;
import storm_orm_statements_upsert_grammar;
import storm_db_postgresql;
import std;

#include "test_fixture.h" // NOSONAR cpp:S954

#include "test_reverse_fk_models.h" // NOSONAR cpp:S954
using storm::meta::FieldRef;
using storm::orm::schema::SchemaStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateStatement;

namespace stmt = storm::orm::statements;

// ============================================================================
// Compile-time: reverse_fk container fields are excluded from persisted members
// ============================================================================

static_assert(stmt::BaseStatement<RfPerson>::field_count_ == 3, "tasks (reverse_fk) is not a persisted column");
static_assert(stmt::BaseStatement<RfTask>::field_count_ == 3); // id, title, assignee
static_assert(stmt::BaseStatement<RfReporter>::field_count_ == 2);
static_assert(stmt::BaseStatement<RfBug>::field_count_ == 4); // id, summary, author, reviewer
static_assert(stmt::BaseStatement<RfBoard>::field_count_ == 2, "notes (reverse_fk) is not a persisted column");
static_assert(stmt::BaseStatement<RfPerson>::has_reverse_fk_field_);
static_assert(!stmt::BaseStatement<RfTask>::has_reverse_fk_field_);

// Pre-computed single-row/bulk UPDATE SQL must not name relation containers (#485).
constexpr std::string_view rf_person_update_sql{stmt::UpdateGrammar<RfPerson>::update_sql_array.data.data(),
                                                stmt::UpdateGrammar<RfPerson>::update_sql_array.len};
static_assert(!rf_person_update_sql.contains("tasks"), "reverse_fk container leaked into UPDATE SET clause");

// ============================================================================
// Compile-time: reverse-FK concepts
// ============================================================================

// Annotated container destination (select path)
static_assert(stmt::ReverseFKFieldOf<RfPerson, ^^RfPerson::tasks>);
static_assert(stmt::ReverseFKFieldOf<RfBoard, ^^RfBoard::notes>);
static_assert(!stmt::ReverseFKFieldOf<RfPerson, ^^RfPerson::name>);

// Cross-model FK selector (aggregate/filter path)
static_assert(stmt::ReverseFKSelector<RfPerson, ^^RfTask::assignee>);
static_assert(stmt::ReverseFKSelector<RfReporter, ^^RfBug::author>);
static_assert(stmt::ReverseFKSelector<RfReporter, ^^RfBug::reviewer>);
static_assert(!stmt::ReverseFKSelector<RfPerson, ^^RfTask::title>);   // not an FK
static_assert(!stmt::ReverseFKSelector<RfPerson, ^^RfPerson::tasks>); // same model, not cross-model FK

// JoinableFields accepts both reverse-FK forms
static_assert(stmt::JoinableFields<RfPerson, ^^RfPerson::tasks>);
static_assert(stmt::JoinableFields<RfPerson, ^^RfTask::assignee>);

// ============================================================================
// Compile-time: the column proxy rejects reverse_fk relation members (#408)
// A reverse_fk container is not a persisted column — a WHERE clause on it would
// reference a non-existent column, so FieldRef must fail at the call site.
// (#518: a relation member gets a RelationRef instead — joinable, not comparable.)
// ============================================================================

template <auto M>
concept FCallableRfk = requires { FieldRef<M>{}; };

static_assert(!FCallableRfk<^^RfPerson::tasks>, "reverse_fk member rejected by FieldRef");
static_assert(!FCallableRfk<^^RfBoard::notes>, "reverse_fk member rejected by FieldRef");
static_assert(FCallableRfk<^^RfPerson::name>, "persisted column still accepted");
static_assert(FCallableRfk<^^RfTask::assignee>, "FK column still accepted");

// ============================================================================
// Compile-time: SET-target gates reject reverse_fk relation members (#486)
// A reverse_fk container is not a persisted column, so passing it as a SET NTTP
// to update<>()/update_all<>() or upsert .update<>() must fail at the call site.
// ============================================================================

static_assert(!stmt::UpdateGrammar<RfPerson>::is_settable_member<^^RfPerson::tasks>(),
              "reverse_fk member rejected as a conditional-UPDATE SET target");
static_assert(!stmt::UpsertGrammar<RfPerson>::is_settable_member<^^RfPerson::tasks>(),
              "reverse_fk member rejected as an upsert DO UPDATE SET target");
static_assert(!stmt::UpsertSettable<RfPerson, ^^RfPerson::tasks>, "UpsertSettable rejects a reverse_fk SET target");
static_assert(stmt::UpdateGrammar<RfPerson>::is_settable_member<^^RfPerson::name>(), "persisted column still settable");

// ============================================================================
// Schema: reverse_fk creates NO junction table and is not a column
// ============================================================================

static_assert(!SchemaStatement<RfPerson>::has_m2m_junction_);

// ============================================================================
// Runtime fixtures
// ============================================================================

template <typename ConnType> class ReverseFKTest : public StormTestFixture<RfPerson, ConnType, RfTask> {
  public:
    void on_after_setup(const std::shared_ptr<ConnType> &conn) override {
        (void)conn;
        storm::test::seed_rf_people<ConnType>();
    }

    // Shared assertion: the first two rows are Alice (2 tasks) then Bob (1 task).
    // Used by both INNER and LEFT join tests (they agree on the present rows; LEFT
    // additionally keeps Carol, asserted separately).
    static void expect_alice_then_bob(plf::hive<RfPerson> &rows) {
        auto it = rows.begin();
        EXPECT_EQ(it->name, "Alice");
        EXPECT_EQ(it->tasks.size(), 2U);
        ++it;
        EXPECT_EQ(it->name, "Bob");
        EXPECT_EQ(it->tasks.size(), 1U);
    }
};
TYPED_TEST_SUITE(ReverseFKTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Plain CRUD ignores the reverse_fk container.
TYPED_TEST(ReverseFKTest, PlainSelectDoesNotPopulateReverseContainer) {
    auto rows = (SelectStatement<RfPerson, TypeParam>{}).all().template order_by<fields::RfPerson.id>().execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 3U);
    for (const auto &p : *rows) {
        EXPECT_TRUE(p.tasks.empty()) << "plain select must not eager-load reverse_fk";
    }
}

// Single-row UPDATE must skip the reverse_fk container (#485): the SET clause
// may only name persisted columns; the round-trip re-select verifies values and
// that the PK binds into the last placeholder.
TYPED_TEST(ReverseFKTest, SingleRowUpdateIgnoresReverseContainer) {
    RfPerson const updated{.id = 1, .name = "Alice Updated", .age = 33, .tasks{{.id = 99, .title = "ghost"}}};
    auto upd = (UpdateStatement<RfPerson, TypeParam>{}).query(updated).execute();
    ASSERT_TRUE(upd.has_value()) << upd.error().message();

    auto rows = (SelectStatement<RfPerson, TypeParam>{}).where(fields::RfPerson.id == 1).execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->begin()->name, "Alice Updated");
    EXPECT_EQ(rows->begin()->age, 33);
}

// Bulk UPDATE goes through the same pre-computed SQL (#485).
TYPED_TEST(ReverseFKTest, BulkUpdateIgnoresReverseContainer) {
    std::vector<RfPerson> const updated = {
        {.id = 1, .name = "Alice2", .age = 40, .tasks{{.id = 99, .title = "ghost"}}},
        {.id = 2, .name = "Bob2", .age = 41},
    };
    auto upd = (UpdateStatement<RfPerson, TypeParam>{}).query(std::span<const RfPerson>(updated)).execute();
    ASSERT_TRUE(upd.has_value()) << upd.error().message();

    auto rows =
        (SelectStatement<RfPerson, TypeParam>{}).all().template order_by<fields::RfPerson.id>().limit(2).execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 2U);
    auto it = rows->begin();
    EXPECT_EQ(it->name, "Alice2");
    EXPECT_EQ(it->age, 40);
    ++it;
    EXPECT_EQ(it->name, "Bob2");
    EXPECT_EQ(it->age, 41);
}

// LEFT join: every person, container filled (empty when none).
TYPED_TEST(ReverseFKTest, LeftJoinPopulatesAndKeepsEmpty) {
    auto rows = (SelectStatement<RfPerson, TypeParam>{})
                    .all()
                    .template left_join<fields::RfPerson.tasks>()
                    .template order_by<fields::RfPerson.id>()
                    .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 3U);
    TestFixture::expect_alice_then_bob(*rows);
    auto carol = rows->begin();
    ++carol;
    ++carol; // plf::hive: std::next is ambiguous (plf has its own), advance by hand
    EXPECT_EQ(carol->name, "Carol");
    EXPECT_TRUE(carol->tasks.empty()); // LEFT keeps zero-task person
}

// INNER join: drops persons with no tasks.
TYPED_TEST(ReverseFKTest, InnerJoinDropsZeroRelationEntities) {
    auto rows = (SelectStatement<RfPerson, TypeParam>{})
                    .all()
                    .template join<fields::RfPerson.tasks>()
                    .template order_by<fields::RfPerson.id>()
                    .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 2U); // Carol dropped
    TestFixture::expect_alice_then_bob(*rows);
}

// Owner rows carry their columns (title) and the FK pk.
TYPED_TEST(ReverseFKTest, OwnerColumnsExtracted) {
    auto rows = (SelectStatement<RfPerson, TypeParam>{})
                    .where(fields::RfPerson.id == 1)
                    .template left_join<fields::RfPerson.tasks>()
                    .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    const auto &alice = *rows->begin();
    ASSERT_EQ(alice.tasks.size(), 2U);
    std::vector<std::string> titles;
    for (const auto &t : alice.tasks) {
        titles.emplace_back(t.title);
        EXPECT_EQ(t.assignee.id, 1); // FK pk threaded back
    }
    std::ranges::sort(titles);
    EXPECT_EQ(titles, (std::vector<std::string>{"T1", "T2"}));
}

// WHERE bounds the BASE entities (the loaded persons), not the tasks.
TYPED_TEST(ReverseFKTest, WhereAppliesToBaseEntities) {
    auto rows = (SelectStatement<RfPerson, TypeParam>{})
                    .where(fields::RfPerson.age < 25)
                    .template left_join<fields::RfPerson.tasks>()
                    .template order_by<fields::RfPerson.id>()
                    .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 2U); // Alice + Bob (age < 25)
    EXPECT_EQ(rows->begin()->tasks.size(), 2U);
}

// LIMIT bounds base entities.
TYPED_TEST(ReverseFKTest, LimitBoundsBaseEntities) {
    auto rows = (SelectStatement<RfPerson, TypeParam>{})
                    .all()
                    .template left_join<fields::RfPerson.tasks>()
                    .template order_by<fields::RfPerson.id>()
                    .limit(1)
                    .execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    ASSERT_EQ(rows->size(), 1U);
    EXPECT_EQ(rows->begin()->name, "Alice");
    EXPECT_EQ(rows->begin()->tasks.size(), 2U);
}

// Aggregate over the cross-model FK selector: tasks-per-person including zeros.
TYPED_TEST(ReverseFKTest, LeftJoinAggregateCountIncludesZeroGroups) {
    auto total =
        (SelectStatement<RfPerson, TypeParam>{}).all().template left_join<fields::RfTask.assignee>().count().execute();
    ASSERT_TRUE(total.has_value()) << total.error().message();
    // 3 task rows + 1 NULL row for Carol (LEFT) = 4.
    EXPECT_EQ(*total, 4);
}

// ============================================================================
// Disambiguation fixture (RfReporter / RfBug)
// ============================================================================

template <typename ConnType> class ReverseFKDisambigTest : public StormTestFixture<RfReporter, ConnType, RfBug> {
  public:
    void on_after_setup(const std::shared_ptr<ConnType> &conn) override {
        std::vector<RfReporter> const reporters = {{.name = "Rita"}, {.name = "Sam"}};
        ASSERT_TRUE((InsertStatement<RfReporter, ConnType>{})
                        .query(std::span<const RfReporter>(reporters))
                        .execute()
                        .has_value());

        // Bug1 authored by Rita(1), reviewed by Sam(2); Bug2 authored by Sam(2), reviewed by Rita(1).
        std::vector<RfBug> const bugs = {{.summary = "B1", .author = {.id = 1}, .reviewer = {.id = 2}},
                                         {.summary = "B2", .author = {.id = 2}, .reviewer = {.id = 1}}};
        ASSERT_TRUE((InsertStatement<RfBug, ConnType>{}).query(std::span<const RfBug>(bugs)).execute().has_value());
        (void)conn;
    }
};

TYPED_TEST_SUITE(ReverseFKDisambigTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Cross-model FK selector disambiguates between the owner's two FKs back at the
// base: counting over RfBug::author vs RfBug::reviewer keys on different columns
// (here both have 2 rows, but the selector picks distinct FK columns in the SQL).
TYPED_TEST(ReverseFKDisambigTest, SelectorDisambiguatesFkInAggregate) {
    auto by_author =
        (SelectStatement<RfReporter, TypeParam>{}).all().template join<fields::RfBug.author>().count().execute();
    ASSERT_TRUE(by_author.has_value()) << by_author.error().message();
    EXPECT_EQ(*by_author, 2); // two bugs, each with an author

    auto by_reviewer =
        (SelectStatement<RfReporter, TypeParam>{}).all().template join<fields::RfBug.reviewer>().count().execute();
    ASSERT_TRUE(by_reviewer.has_value()) << by_reviewer.error().message();
    EXPECT_EQ(*by_reviewer, 2); // two bugs, each with a reviewer
}

// NOLINTEND(misc-const-correctness)
