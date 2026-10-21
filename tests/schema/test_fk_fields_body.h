#pragma once

// Shared test body for test_fk_fields_sqlite.cpp / test_fk_fields_pg.cpp — the two
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
#error "test_fk_fields_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

// NOLINTBEGIN(misc-const-correctness)

#include <meta>

import storm;
import std;

#include "../../shared/models/message.h"
#include "../../shared/models/person.h"
#include "../../shared/models/task.h"
#include "test_fixture.h"

#include "test_seed_helpers.h"
using storm::field_specs_for;
using storm::primary;
using storm::db::DefaultConnection;
using storm::orm::schema::SchemaStatement;

// ── Local structs (used only in this file) ──────────────────────────────────
struct NullableFKMessage {
    [[= primary]] int id{};
    [[= storm::fk<>]] std::optional<Person> sender;
    [[= storm::fk<>]] Person receiver;
    std::string text;
};
struct Project {
    [[= primary]] int id{};
    [[= storm::fk<>]] Person manager;
    std::string title;
    double budget{};
};
struct Measurement {
    [[= primary]] int id{};
    std::string sensor_name;
    float temperature{};
    long long timestamp{};
};
struct Counter {
    [[= primary]] int id{};
    std::string name;
    long count{};
};
struct Reading {
    [[= primary]] int id{};
    [[= storm::fk<>]] Measurement measurement;
    std::string reading_type;
    float value{};
};
struct Summary {
    [[= primary]] int id{};
    [[= storm::fk<>]] Counter counter;
    std::string report_type;
};

// fields:: selector proxies (#518).
namespace fields {

struct NullableFKMessageT;
consteval { std::meta::define_aggregate(^^NullableFKMessageT, field_specs_for(^^NullableFKMessage)); }
inline constexpr NullableFKMessageT NullableFKMessage{};

struct ProjectT;
consteval { std::meta::define_aggregate(^^ProjectT, field_specs_for(^^Project)); }
inline constexpr ProjectT Project{};

struct ReadingT;
consteval { std::meta::define_aggregate(^^ReadingT, field_specs_for(^^Reading)); }
inline constexpr ReadingT Reading{};

struct SummaryT;
consteval { std::meta::define_aggregate(^^SummaryT, field_specs_for(^^Summary)); }
inline constexpr SummaryT Summary{};

} // namespace fields

using storm::QuerySet;

// ── Compile-time API contract (#388) ─────────────────────────────────────────
// join/left_join take fields:: selector proxies (#518). Member-pointer syntax,
// raw ^^ reflections, non-FK fields, and other models' fields are all rejected —
// by FKFieldOf for the wrong field, and by selector_info's own constraint for a
// non-selector. The template parameter makes the call dependent so a rejected
// argument is a substitution failure (= false), not a hard error.
template <auto... FKs> constexpr bool join_accepts = requires(QuerySet<Task> qs) { qs.template join<FKs...>(); };

static_assert(join_accepts<fields::Task.assignee>);
static_assert(join_accepts<fields::Task.assignee, fields::Task.reviewer>);
static_assert(!join_accepts<^^Task::assignee>); // the raw ^^ spelling is gone (#518)
static_assert(requires(QuerySet<Task> qs) { qs.template left_join<fields::Task.assignee>(); });
// right_join was removed in #397 — its only distinguishing output was junk
// defaulted base entities; see #398 for the reverse-relation replacement.
// The QuerySet type must be the template parameter (detection idiom): with a
// concrete type the missing member is a hard error at parse, not 'false'.
template <typename QS>
constexpr bool has_right_join = requires(QS qs) { qs.template right_join<fields::Task.assignee>(); };
static_assert(!has_right_join<QuerySet<Task>>);
static_assert(!join_accepts<&Task::assignee>); // old pointer syntax removed
// These three now fail at selector_info's constraint (not a proxy) rather than
// at FKFieldOf. The FKFieldOf rejections that still MATTER are asserted through
// the proxy spelling below, where the argument IS a valid selector.
static_assert(!join_accepts<^^Task::description>);
static_assert(!join_accepts<^^Task>);
static_assert(!join_accepts<^^Person::name>);

// The real FK gate, probed with genuine selectors: a non-FK column of the right
// model, and an FK-shaped member of a DIFFERENT model, are both rejected.
static_assert(!join_accepts<fields::Task.description>); // non-FK field
static_assert(!join_accepts<fields::Person.name>);      // member of another model

// Test fixture for FK field operations — templated on database backend
template <typename ConnType> class FKFieldTest : public StormTestFixture<Person, ConnType, Task> {};
TYPED_TEST_SUITE(FKFieldTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Test: INSERT with FK field binds only the PK value
TYPED_TEST(FKFieldTest, InsertWithFKField) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users first
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(alice_result.has_value()) << "Alice INSERT failed: " << alice_result.error().message();
    ASSERT_TRUE(bob_result.has_value()) << "Bob INSERT failed: " << bob_result.error().message();

    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert a task with FK to Alice (assignee) and Bob (reviewer)
    // Only assignee.id and reviewer.id are used, other fields are ignored
    Task const msg{.id = 0,
                   .assignee = Person{.id = static_cast<int>(alice_id), .name = "ignored", .age = 0},
                   .reviewer = Person{.id = static_cast<int>(bob_id), .name = "ignored", .age = 0},
                   .description = "Hello World"};

    auto msg_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(msg_result.has_value()) << "Task INSERT failed: " << msg_result.error().message();

    std::int64_t const msg_id = msg_result.value();
    EXPECT_GT(msg_id, 0) << "Expected valid message ID";

    // Verify FKs were stored correctly by querying database directly
    const auto &conn = DefaultConnection<TypeParam>::get();
    auto stmt_res = conn->prepare("SELECT assignee_id, reviewer_id FROM Task WHERE id = ?");
    ASSERT_TRUE(stmt_res.has_value());

    auto stmt = std::move(stmt_res.value());
    ASSERT_TRUE(stmt.bind_int(1, msg_id).has_value());

    int const step_result = stmt.step_raw();
    ASSERT_EQ(step_result, decltype(stmt)::ROW_AVAILABLE);

    std::int64_t const stored_assignee_id = stmt.extract_int64(0);
    std::int64_t const stored_reviewer_id = stmt.extract_int64(1);
    EXPECT_EQ(stored_assignee_id, alice_id) << "Assignee FK value should match Alice ID";
    EXPECT_EQ(stored_reviewer_id, bob_id) << "Reviewer FK value should match Bob ID";
}

// Test: SELECT with FK field populates only the PK
TYPED_TEST(FKFieldTest, SelectWithFKFieldPartialPopulation) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    Person const charlie{.id = 0, .name = "Charlie", .age = 35};
    auto bob_result = user_qs.insert(bob).execute();
    auto charlie_result = user_qs.insert(charlie).execute();
    ASSERT_TRUE(bob_result.has_value());
    ASSERT_TRUE(charlie_result.has_value());
    std::int64_t const bob_id = bob_result.value();
    std::int64_t const charlie_id = charlie_result.value();

    // Insert a task from Bob to Charlie
    Task const msg{.id = 0,
                   .assignee = Person{.id = static_cast<int>(bob_id), .name = "", .age = 0},
                   .reviewer = Person{.id = static_cast<int>(charlie_id), .name = "", .age = 0},
                   .description = "Test message"};
    auto msg_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(msg_result.has_value());

    // SELECT tasks
    auto select_result = message_qs.select().execute();
    ASSERT_TRUE(select_result.has_value()) << "SELECT failed: " << select_result.error().message();

    const auto &messages = select_result.value();
    ASSERT_EQ(messages.size(), 1) << "Expected exactly one task";

    // Verify task fields
    auto it = messages.begin();
    EXPECT_EQ(it->description, "Test message");

    // Verify FK fields: only PK should be populated
    EXPECT_EQ(it->assignee.id, bob_id) << "Assignee FK PK should be populated";
    EXPECT_EQ(it->assignee.name, "") << "Assignee FK non-PK fields should remain default (empty string)";
    EXPECT_EQ(it->assignee.age, 0) << "Assignee FK non-PK fields should remain default (0)";

    EXPECT_EQ(it->reviewer.id, charlie_id) << "Reviewer FK PK should be populated";
    EXPECT_EQ(it->reviewer.name, "") << "Reviewer FK non-PK fields should remain default (empty string)";
    EXPECT_EQ(it->reviewer.age, 0) << "Reviewer FK non-PK fields should remain default (0)";
}

// Test: Batch INSERT with FK fields
TYPED_TEST(FKFieldTest, BatchInsertWithFKFields) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users (IDs will be auto-generated)
    std::vector<Person> users = {Person{.name = "Alice", .age = 30}, Person{.name = "Bob", .age = 25},
                                 Person{.name = "Charlie", .age = 35}, Person{.name = "Dave", .age = 40}};
    auto user_result = user_qs.insert(users).execute();
    ASSERT_TRUE(user_result.has_value());

    // SELECT to get auto-generated user IDs
    auto user_select = user_qs.select().execute();
    ASSERT_TRUE(user_select.has_value());
    ASSERT_EQ(user_select.value().size(), 4);

    // Get first user's ID for FK references
    int const first_user_id = user_select.value().begin()->id;

    // Insert tasks with FK references
    std::vector<Task> tasks = {Task{.assignee = Person{.id = first_user_id},
                                    .reviewer = Person{.id = first_user_id},
                                    .description = "Task from Alice to Bob"},
                               Task{.assignee = Person{.id = first_user_id},
                                    .reviewer = Person{.id = first_user_id},
                                    .description = "Task from Charlie to Dave"}};

    auto msg_result = message_qs.insert(tasks).execute();
    ASSERT_TRUE(msg_result.has_value()) << "Batch INSERT failed: " << msg_result.error().message();

    // Verify tasks were stored
    auto select_result = message_qs.select().execute();
    ASSERT_TRUE(select_result.has_value());

    const auto &retrieved_messages = select_result.value();
    ASSERT_EQ(retrieved_messages.size(), 2);

    // Verify FK values
    auto it = retrieved_messages.begin();
    EXPECT_EQ(it->assignee.id, first_user_id);
    EXPECT_EQ(it->reviewer.id, first_user_id);
    ++it;
    EXPECT_EQ(it->assignee.id, first_user_id);
    EXPECT_EQ(it->reviewer.id, first_user_id);
}

// Test: UPDATE with FK field
TYPED_TEST(FKFieldTest, UpdateWithFKField) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    Person const charlie{.id = 0, .name = "Charlie", .age = 35};
    Person const dave{.id = 0, .name = "Dave", .age = 40};

    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();
    auto charlie_result = user_qs.insert(charlie).execute();
    auto dave_result = user_qs.insert(dave).execute();

    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());
    ASSERT_TRUE(charlie_result.has_value());
    ASSERT_TRUE(dave_result.has_value());

    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();
    std::int64_t const charlie_id = charlie_result.value();
    std::int64_t const dave_id = dave_result.value();

    // Insert task assigned to Alice, reviewed by Bob
    Task const msg{.id = 0,
                   .assignee = Person{.id = static_cast<int>(alice_id), .name = "", .age = 0},
                   .reviewer = Person{.id = static_cast<int>(bob_id), .name = "", .age = 0},
                   .description = "Original message"};
    auto msg_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(msg_result.has_value());

    std::int64_t const msg_id = msg_result.value();

    // Update task: change assignee to Charlie, reviewer to Dave, and description
    Task const updated_msg{.id = static_cast<int>(msg_id),
                           .assignee = Person{.id = static_cast<int>(charlie_id), .name = "", .age = 0},
                           .reviewer = Person{.id = static_cast<int>(dave_id), .name = "", .age = 0},
                           .description = "Updated message"};

    auto update_result = message_qs.update(updated_msg).execute();
    ASSERT_TRUE(update_result.has_value()) << "UPDATE failed: " << update_result.error().message();

    // Verify update
    auto select_result = message_qs.select().execute();
    ASSERT_TRUE(select_result.has_value());

    const auto &messages = select_result.value();
    ASSERT_EQ(messages.size(), 1);

    EXPECT_EQ(messages.begin()->id, msg_id);
    EXPECT_EQ(messages.begin()->assignee.id, charlie_id) << "Assignee FK should be updated to Charlie";
    EXPECT_EQ(messages.begin()->reviewer.id, dave_id) << "Reviewer FK should be updated to Dave";
    EXPECT_EQ(messages.begin()->description, "Updated message");
}

// Test: DELETE with FK field
TYPED_TEST(FKFieldTest, DeleteWithFKField) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());
    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert tasks assigned to Alice, reviewed by Bob
    std::vector<Task> tasks = {Task{.assignee = Person{.id = static_cast<int>(alice_id)},
                                    .reviewer = Person{.id = static_cast<int>(bob_id)},
                                    .description = "Task 1"},
                               Task{.assignee = Person{.id = static_cast<int>(alice_id)},
                                    .reviewer = Person{.id = static_cast<int>(bob_id)},
                                    .description = "Task 2"}};

    auto msg_result = message_qs.insert(tasks).execute();
    ASSERT_TRUE(msg_result.has_value());

    // SELECT to get auto-generated task IDs
    auto msg_select = message_qs.select().execute();
    ASSERT_TRUE(msg_select.has_value());
    ASSERT_EQ(msg_select.value().size(), 2);

    // Get the first task's ID to delete
    auto it = msg_select.value().begin();
    int const first_msg_id = it->id;
    ++it;
    int const second_msg_id = it->id;

    // Delete first task
    Task const to_delete{.id = first_msg_id,
                         .assignee = Person{.id = static_cast<int>(alice_id)},
                         .reviewer = Person{.id = static_cast<int>(bob_id)},
                         .description = ""};

    auto delete_result = message_qs.erase(to_delete).execute();
    ASSERT_TRUE(delete_result.has_value()) << "DELETE failed: " << delete_result.error().message();

    // Verify only one task remains
    auto select_result = message_qs.select().execute();
    ASSERT_TRUE(select_result.has_value());

    const auto &remaining_messages = select_result.value();
    ASSERT_EQ(remaining_messages.size(), 1);
    EXPECT_EQ(remaining_messages.begin()->id, second_msg_id);
    EXPECT_EQ(remaining_messages.begin()->description, "Task 2");
}

// Test: Multiple FK fields to same type
TYPED_TEST(FKFieldTest, MultipleFKFieldsToSameType) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> conv_qs;

    auto create_conv_result = SchemaStatement<Task>::create_table_if_not_exists<TypeParam>();
    ASSERT_TRUE(create_conv_result.has_value());

    // Insert users
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};

    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();

    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());

    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert task assigned to Alice, reviewed by Bob
    Task const conv{.id = 0,
                    .assignee = Person{.id = static_cast<int>(alice_id)},
                    .reviewer = Person{.id = static_cast<int>(bob_id)},
                    .description = "Hello Bob!"};

    auto conv_result = conv_qs.insert(conv).execute();
    ASSERT_TRUE(conv_result.has_value()) << "Task INSERT failed: " << conv_result.error().message();

    // SELECT and verify both FK fields are populated
    auto select_result = conv_qs.select().execute();
    ASSERT_TRUE(select_result.has_value());

    const auto &conversations = select_result.value();
    ASSERT_EQ(conversations.size(), 1);

    EXPECT_EQ(conversations.begin()->assignee.id, alice_id) << "Assignee FK should be Alice";
    EXPECT_EQ(conversations.begin()->reviewer.id, bob_id) << "Reviewer FK should be Bob";
    EXPECT_EQ(conversations.begin()->description, "Hello Bob!");
}

// Test: Phase 2 - JOIN populates FK object fully
TYPED_TEST(FKFieldTest, JoinFullyPopulatesFKObject) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());
    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert task assigned to Alice, reviewed by Bob
    Task const msg{.id = 0,
                   .assignee = Person{.id = static_cast<int>(alice_id)},
                   .reviewer = Person{.id = static_cast<int>(bob_id)},
                   .description = "Hello from JOIN!"};
    auto msg_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(msg_result.has_value());

    // Phase 2: JOIN to get fully populated assignee
    auto join_result = message_qs.template join<fields::Task.assignee>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "JOIN failed: " << join_result.error().message();

    const auto &messages = join_result.value();
    ASSERT_EQ(messages.size(), 1);

    // Verify task fields
    auto it = messages.begin();
    EXPECT_EQ(it->description, "Hello from JOIN!");

    // Verify assignee FK object is FULLY populated (not just PK!)
    EXPECT_EQ(it->assignee.id, alice_id);
    EXPECT_EQ(it->assignee.name, "Alice") << "JOIN should populate assignee FK object's name!";
    EXPECT_EQ(it->assignee.age, 30) << "JOIN should populate assignee FK object's age!";

    // Verify reviewer FK object is NOT populated (current JOIN limitation)
    EXPECT_EQ(it->reviewer.id, 0) << "Non-JOINed FK fields are not populated (current limitation)";
    EXPECT_EQ(it->reviewer.name, "") << "Without JOIN, reviewer name should be empty";
    EXPECT_EQ(it->reviewer.age, 0) << "Without JOIN, reviewer age should be 0";
}

// Test: Phase 3 - Multi-JOIN populates multiple FK objects fully
TYPED_TEST(FKFieldTest, JoinMultipleFKFields) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());
    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert task assigned to Alice, reviewed by Bob
    Task const msg{.id = 0,
                   .assignee = Person{.id = static_cast<int>(alice_id)},
                   .reviewer = Person{.id = static_cast<int>(bob_id)},
                   .description = "Hello from multi-JOIN!"};
    auto msg_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(msg_result.has_value());

    // Phase 3: Multi-JOIN to get BOTH assignee and reviewer fully populated
    auto join_result = message_qs.template join<fields::Task.assignee, fields::Task.reviewer>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "Multi-JOIN failed: " << join_result.error().message();

    const auto &messages = join_result.value();
    ASSERT_EQ(messages.size(), 1);

    // Verify task fields
    auto it = messages.begin();
    EXPECT_EQ(it->description, "Hello from multi-JOIN!");

    // Verify assignee FK object is FULLY populated
    EXPECT_EQ(it->assignee.id, alice_id);
    EXPECT_EQ(it->assignee.name, "Alice") << "Multi-JOIN should populate assignee FK object's name!";
    EXPECT_EQ(it->assignee.age, 30) << "Multi-JOIN should populate assignee FK object's age!";

    // Verify reviewer FK object is ALSO FULLY populated (Phase 3 improvement!)
    EXPECT_EQ(it->reviewer.id, bob_id);
    EXPECT_EQ(it->reviewer.name, "Bob") << "Multi-JOIN should populate reviewer FK object's name!";
    EXPECT_EQ(it->reviewer.age, 25) << "Multi-JOIN should populate reviewer FK object's age!";
}

// Test: LEFT JOIN returns all messages even when FK doesn't match
TYPED_TEST(FKFieldTest, LeftJoinReturnsAllMessages) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert only one user (Alice)
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    auto alice_result = user_qs.insert(alice).execute();
    ASSERT_TRUE(alice_result.has_value());
    std::int64_t const alice_id = alice_result.value();

    // Insert a task whose reviewer FK points at a real Person (Alice) but is NOT
    // joined in the query below. With referential integrity always on (#412) the
    // reviewer_id must reference an existing row; the point of this test is that a
    // non-JOINed FK stays default in the result, not that a dangling FK is stored.
    Task const msg{.id = 0,
                   .assignee = Person{.id = static_cast<int>(alice_id)},
                   .reviewer = Person{.id = static_cast<int>(alice_id)},
                   .description = "Reviewer not joined"};
    ASSERT_TRUE(message_qs.insert(msg).execute().has_value());

    // LEFT JOIN on assignee only — reviewer is not fetched, so it stays default.
    auto join_result = message_qs.template left_join<fields::Task.assignee>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "LEFT JOIN failed: " << join_result.error().message();

    const auto &messages = join_result.value();
    ASSERT_EQ(messages.size(), 1) << "LEFT JOIN should return all tasks";

    // Verify assignee is fully populated (exists)
    auto it = messages.begin();
    EXPECT_EQ(it->assignee.id, alice_id);
    EXPECT_EQ(it->assignee.name, "Alice") << "LEFT JOIN should populate existing FK";
    EXPECT_EQ(it->assignee.age, 30);

    // Verify reviewer is not populated (not part of the JOIN)
    EXPECT_EQ(it->reviewer.id, 0) << "Non-JOINed FK should remain default";

    // Verify task description
    EXPECT_EQ(it->description, "Reviewer not joined");
}

// Test: LEFT JOIN with multiple FK fields
TYPED_TEST(FKFieldTest, LeftJoinMultipleFKFields) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<Task, TypeParam> message_qs;

    // Insert users
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());
    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert task assigned to Alice, reviewed by Bob
    Task const msg{.id = 0,
                   .assignee = Person{.id = static_cast<int>(alice_id)},
                   .reviewer = Person{.id = static_cast<int>(bob_id)},
                   .description = "Hello with LEFT JOIN"};
    auto msg_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(msg_result.has_value());

    // LEFT JOIN on both assignee and reviewer
    auto join_result = message_qs.template left_join<fields::Task.assignee, fields::Task.reviewer>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "Multi LEFT JOIN failed: " << join_result.error().message();

    const auto &messages = join_result.value();
    ASSERT_EQ(messages.size(), 1);

    // Verify both FK objects are fully populated
    auto it = messages.begin();
    EXPECT_EQ(it->assignee.id, alice_id);
    EXPECT_EQ(it->assignee.name, "Alice");
    EXPECT_EQ(it->assignee.age, 30);

    EXPECT_EQ(it->reviewer.id, bob_id);
    EXPECT_EQ(it->reviewer.name, "Bob");
    EXPECT_EQ(it->reviewer.age, 25);

    EXPECT_EQ(it->description, "Hello with LEFT JOIN");
}

// Test fixture for nullable FK fields — templated on database backend
template <typename ConnType> class NullableFKTest : public StormTestFixture<Person, ConnType, NullableFKMessage> {};

TYPED_TEST_SUITE(NullableFKTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Test: SELECT with NULL FK values
TYPED_TEST(NullableFKTest, SelectWithNullFKField) {
    QuerySet<NullableFKMessage, TypeParam> message_qs;

    // Insert a receiver user so the NOT NULL receiver_id is satisfied
    QuerySet<Person, TypeParam> user_qs;
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(bob_result.has_value());
    Person const receiver{.id = static_cast<int>(bob_result.value())};

    // Insert message with NULL sender (std::nullopt) via ORM
    NullableFKMessage const msg{.id = 0, .sender = std::nullopt, .receiver = receiver, .text = "no sender"};
    auto insert_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(insert_result.has_value()) << "INSERT with nullopt FK failed: " << insert_result.error().message();

    // SELECT should return message with sender == nullopt
    auto select_result = message_qs.select().execute();
    ASSERT_TRUE(select_result.has_value()) << "SELECT with NULL FK failed: " << select_result.error().message();

    const auto &messages = select_result.value();
    ASSERT_EQ(messages.size(), 1);

    auto it = messages.begin();
    EXPECT_FALSE(it->sender.has_value()) << "NULL FK should be std::nullopt";
    EXPECT_EQ(it->text, "no sender");
}

// Test: SELECT with non-null optional FK — covers extract_column_fast non-null branch (base.cppm lines 439-441)
TYPED_TEST(NullableFKTest, SelectWithNonNullFKField) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<NullableFKMessage, TypeParam> message_qs;

    // Insert a user to serve as sender and receiver
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(bob_result.has_value());
    Person const bob_fk{.id = static_cast<int>(bob_result.value())};

    // Insert message with non-null sender (optional FK with value)
    NullableFKMessage const msg{.id = 0, .sender = bob_fk, .receiver = bob_fk, .text = "with sender"};
    auto insert_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(insert_result.has_value())
        << "INSERT with non-null optional FK failed: " << insert_result.error().message();

    // Plain SELECT (no join) triggers extract_column_fast for optional FK — non-null path
    auto select_result = message_qs.select().execute();
    ASSERT_TRUE(select_result.has_value()) << "SELECT failed: " << select_result.error().message();

    const auto &messages = select_result.value();
    ASSERT_EQ(messages.size(), 1);

    auto it = messages.begin();
    ASSERT_TRUE(it->sender.has_value()) << "Non-null optional FK should have value";
    EXPECT_EQ(it->sender->id, bob_fk.id);
    EXPECT_EQ(it->text, "with sender");
}

// Test: LEFT JOIN with NULL FK values
TYPED_TEST(NullableFKTest, LeftJoinWithNullFKField) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<NullableFKMessage, TypeParam> message_qs;

    // Insert a receiver user
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    auto alice_result = user_qs.insert(alice).execute();
    ASSERT_TRUE(alice_result.has_value());
    Person const receiver{.id = static_cast<int>(alice_result.value())};

    // Insert message with NULL sender via ORM
    NullableFKMessage const msg{.id = 0, .sender = std::nullopt, .receiver = receiver, .text = "NULL sender message"};
    auto insert_result = message_qs.insert(msg).execute();
    ASSERT_TRUE(insert_result.has_value());

    // LEFT JOIN on sender - should return message even with NULL sender_id
    auto join_result = message_qs.template left_join<fields::NullableFKMessage.sender>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "LEFT JOIN with NULL FK failed: " << join_result.error().message();

    const auto &messages = join_result.value();
    ASSERT_EQ(messages.size(), 1) << "LEFT JOIN should return message with NULL FK";

    auto it = messages.begin();
    EXPECT_FALSE(it->sender.has_value()) << "NULL FK should be std::nullopt after left join";
    EXPECT_EQ(it->text, "NULL sender message");
}

// Test: LEFT JOIN with mix of NULL and valid FKs
TYPED_TEST(NullableFKTest, LeftJoinWithMixedNullAndValidFKs) {
    QuerySet<Person, TypeParam> user_qs;
    QuerySet<NullableFKMessage, TypeParam> message_qs;

    // Insert users
    Person const alice{.id = 0, .name = "Alice", .age = 30};
    Person const bob{.id = 0, .name = "Bob", .age = 25};
    auto alice_result = user_qs.insert(alice).execute();
    auto bob_result = user_qs.insert(bob).execute();
    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());
    Person const alice_fk{.id = static_cast<int>(alice_result.value())};
    Person const bob_fk{.id = static_cast<int>(bob_result.value())};

    // Message 1: valid sender (Alice) → receiver (Bob)
    NullableFKMessage const msg1{.id = 0, .sender = alice_fk, .receiver = bob_fk, .text = "From Alice"};
    ASSERT_TRUE(message_qs.insert(msg1).execute().has_value());

    // Message 2: NULL sender → receiver (Bob)
    NullableFKMessage const msg2{.id = 0, .sender = std::nullopt, .receiver = bob_fk, .text = "Anonymous"};
    ASSERT_TRUE(message_qs.insert(msg2).execute().has_value());

    // LEFT JOIN should return both messages
    auto join_result = message_qs.template left_join<fields::NullableFKMessage.sender>().select().execute();
    ASSERT_TRUE(join_result.has_value());

    const auto &messages = join_result.value();
    ASSERT_EQ(messages.size(), 2) << "LEFT JOIN should return all messages";

    bool found_alice = false;
    bool found_null = false;
    for (const auto &m : messages) {
        if (m.text == "From Alice") {
            found_alice = true;
            ASSERT_TRUE(m.sender.has_value()) << "Alice's message should have a sender";
            EXPECT_EQ(m.sender->id, alice_fk.id);
            EXPECT_EQ(m.sender->name, "Alice");
            EXPECT_EQ(m.sender->age, 30);
        } else if (m.text == "Anonymous") {
            found_null = true;
            EXPECT_FALSE(m.sender.has_value()) << "Anonymous message sender should be nullopt";
        }
    }
    EXPECT_TRUE(found_alice) << "Should find Alice's message";
    EXPECT_TRUE(found_null) << "Should find anonymous message";
}

// Test fixture for extended type support in JOINs — templated on database backend
template <typename ConnType> class ExtendedTypesJoinTest : public StormTestFixture<Person, ConnType, Project> {};

TYPED_TEST_SUITE(ExtendedTypesJoinTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Test: JOIN with extended types (double, bool, optional)
TYPED_TEST(ExtendedTypesJoinTest, JoinWithExtendedTypes) {
    QuerySet<Person, TypeParam> employee_qs;
    QuerySet<Project, TypeParam> project_qs;

    // Insert employees with extended types
    Person const alice{.id = 0, .name = "Alice Smith", .salary = 95000.50, .is_active = true, .nickname = "Ally"};
    Person const bob{.id = 0, .name = "Bob Johnson", .salary = 87500.75, .is_active = false, .nickname = std::nullopt};

    auto alice_result = employee_qs.insert(alice).execute();
    auto bob_result = employee_qs.insert(bob).execute();

    ASSERT_TRUE(alice_result.has_value()) << "Failed to insert Alice: " << alice_result.error().message();
    ASSERT_TRUE(bob_result.has_value()) << "Failed to insert Bob: " << bob_result.error().message();

    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert projects managed by Alice and Bob
    Project const proj1{.id = 0,
                        .manager = Person{.id = static_cast<int>(alice_id),
                                          .name = "",
                                          .salary = 0.0,
                                          .is_active = false,
                                          .nickname = std::nullopt},
                        .title = "Web Redesign",
                        .budget = 50000.0};
    Project const proj2{.id = 0,
                        .manager = Person{.id = static_cast<int>(bob_id),
                                          .name = "",
                                          .salary = 0.0,
                                          .is_active = false,
                                          .nickname = std::nullopt},
                        .title = "Mobile App",
                        .budget = 75000.0};

    auto proj1_result = project_qs.insert(proj1).execute();
    auto proj2_result = project_qs.insert(proj2).execute();

    ASSERT_TRUE(proj1_result.has_value()) << "Failed to insert project 1: " << proj1_result.error().message();
    ASSERT_TRUE(proj2_result.has_value()) << "Failed to insert project 2: " << proj2_result.error().message();

    // JOIN to get projects with fully populated manager (Person) objects
    auto join_result = project_qs.template join<fields::Project.manager>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "JOIN failed: " << join_result.error().message();

    const auto &projects = join_result.value();
    ASSERT_EQ(projects.size(), 2) << "Should retrieve both projects";

    // Find Alice's project and verify all extended types
    bool found_alice_project = false;
    bool found_bob_project = false;

    for (const auto &proj : projects) {
        if (proj.title == "Web Redesign") {
            found_alice_project = true;

            // Verify manager FK is fully populated with all extended types
            EXPECT_EQ(proj.manager.id, alice_id) << "Manager ID should be Alice";
            EXPECT_EQ(proj.manager.name, "Alice Smith") << "Manager name should be populated";

            // Test double type
            EXPECT_DOUBLE_EQ(proj.manager.salary, 95000.50) << "Double field (salary) should be populated correctly";

            // Test bool type
            EXPECT_TRUE(proj.manager.is_active) << "Bool field (is_active) should be true for Alice";

            // Test optional<string> with value
            ASSERT_TRUE(proj.manager.nickname.has_value()) << "Optional nickname should have value for Alice";
            EXPECT_EQ(proj.manager.nickname.value(), "Ally") << "Optional nickname value should be 'Ally'";

            // Verify project fields
            EXPECT_DOUBLE_EQ(proj.budget, 50000.0) << "Project budget should be correct";
        } else if (proj.title == "Mobile App") {
            found_bob_project = true;

            // Verify Bob's extended types
            EXPECT_EQ(proj.manager.id, bob_id) << "Manager ID should be Bob";
            EXPECT_EQ(proj.manager.name, "Bob Johnson") << "Manager name should be populated";

            // Test double type
            EXPECT_DOUBLE_EQ(proj.manager.salary, 87500.75) << "Double field should be populated correctly for Bob";

            EXPECT_FALSE(proj.manager.is_active) << "Bool field should be false for Bob";

            // Test optional<string> without value (NULL in DB)
            EXPECT_FALSE(proj.manager.nickname.has_value())
                << "Optional nickname should be empty (nullopt) for Bob when NULL in DB";

            // Verify project fields
            EXPECT_DOUBLE_EQ(proj.budget, 75000.0) << "Project budget should be correct";
        }
    }

    EXPECT_TRUE(found_alice_project) << "Should find Alice's project with extended types";
    EXPECT_TRUE(found_bob_project) << "Should find Bob's project with extended types";
}

// Test: Multi-JOIN with extended types
TYPED_TEST(ExtendedTypesJoinTest, MultiJoinWithExtendedTypes) {
    QuerySet<Person, TypeParam> employee_qs;
    QuerySet<Task, TypeParam> task_qs;

    auto create_task_result = SchemaStatement<Task>::create_table_if_not_exists<TypeParam>();
    ASSERT_TRUE(create_task_result.has_value());

    // Insert employees
    Person const alice{.id = 0, .name = "Alice", .salary = 95000.0, .is_active = true, .nickname = "Ally"};
    Person const bob{.id = 0, .name = "Bob", .salary = 87500.0, .is_active = false, .nickname = std::nullopt};

    auto alice_result = employee_qs.insert(alice).execute();
    auto bob_result = employee_qs.insert(bob).execute();

    ASSERT_TRUE(alice_result.has_value());
    ASSERT_TRUE(bob_result.has_value());

    std::int64_t const alice_id = alice_result.value();
    std::int64_t const bob_id = bob_result.value();

    // Insert task: Alice assigned, Bob reviewing
    Task const task{.id = 0,
                    .assignee = Person{.id = static_cast<int>(alice_id),
                                       .name = "",
                                       .salary = 0.0,
                                       .is_active = false,
                                       .nickname = std::nullopt},
                    .reviewer = Person{.id = static_cast<int>(bob_id),
                                       .name = "",
                                       .salary = 0.0,
                                       .is_active = false,
                                       .nickname = std::nullopt},
                    .description = "Implement feature X"};

    auto task_result = task_qs.insert(task).execute();
    ASSERT_TRUE(task_result.has_value());

    // Multi-JOIN to populate both assignee and reviewer
    // NOLINTNEXTLINE(readability-isolate-declaration) - false positive with template
    auto join_result = task_qs.template join<fields::Task.assignee, fields::Task.reviewer>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "Multi-JOIN failed: " << join_result.error().message();

    const auto &tasks = join_result.value();
    ASSERT_EQ(tasks.size(), 1);

    // Verify assignee (Alice) - all extended types
    EXPECT_EQ(tasks.begin()->assignee.id, alice_id);
    EXPECT_EQ(tasks.begin()->assignee.name, "Alice");
    EXPECT_DOUBLE_EQ(tasks.begin()->assignee.salary, 95000.0);
    EXPECT_TRUE(tasks.begin()->assignee.is_active);
    ASSERT_TRUE(tasks.begin()->assignee.nickname.has_value());
    EXPECT_EQ(tasks.begin()->assignee.nickname.value(), "Ally");

    // Verify reviewer (Bob) - all extended types
    EXPECT_EQ(tasks.begin()->reviewer.id, bob_id);
    EXPECT_EQ(tasks.begin()->reviewer.name, "Bob");
    EXPECT_DOUBLE_EQ(tasks.begin()->reviewer.salary, 87500.0);
    EXPECT_FALSE(tasks.begin()->reviewer.is_active);
    EXPECT_FALSE(tasks.begin()->reviewer.nickname.has_value()) << "Bob's nickname should be nullopt";

    EXPECT_EQ(tasks.begin()->description, "Implement feature X");
}

// Test: JOIN with float and long long types (coverage for extract_typed_field)
TYPED_TEST(ExtendedTypesJoinTest, JoinWithFloatAndLongLongTypes) {
    QuerySet<Measurement, TypeParam> measurement_qs;
    QuerySet<Reading, TypeParam> reading_qs;

    auto create_measurement_result = SchemaStatement<Measurement>::create_table_if_not_exists<TypeParam>();
    ASSERT_TRUE(create_measurement_result.has_value())
        << "Failed to create Measurement table: " << create_measurement_result.error().message();

    auto create_reading_result = SchemaStatement<Reading>::create_table_if_not_exists<TypeParam>();
    ASSERT_TRUE(create_reading_result.has_value())
        << "Failed to create Reading table: " << create_reading_result.error().message();

    // Insert measurement with float and long long
    Measurement const meas{
        .id = 0,
        .sensor_name = "TempSensor1",
        .temperature = 23.5F,
        .timestamp = 1700000000000LL // Large timestamp
    };

    auto meas_result = measurement_qs.insert(meas).execute();
    ASSERT_TRUE(meas_result.has_value()) << "Failed to insert measurement: " << meas_result.error().message();

    std::int64_t const meas_id = meas_result.value();

    // Insert reading referencing the measurement
    Reading const reading{
        .id = 0,
        .measurement =
            Measurement{.id = static_cast<int>(meas_id), .sensor_name = "", .temperature = 0.0F, .timestamp = 0LL},
        .reading_type = "Humidity",
        .value = 65.3F};

    auto reading_result = reading_qs.insert(reading).execute();
    ASSERT_TRUE(reading_result.has_value()) << "Failed to insert reading: " << reading_result.error().message();

    // JOIN to get readings with fully populated measurement
    auto join_result = reading_qs.template join<fields::Reading.measurement>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "JOIN failed: " << join_result.error().message();

    const auto &readings = join_result.value();
    ASSERT_EQ(readings.size(), 1) << "Should retrieve one reading";

    // Verify float type extraction
    EXPECT_FLOAT_EQ(readings.begin()->measurement.temperature, 23.5F) << "Float field (temperature) should be correct";
    EXPECT_FLOAT_EQ(readings.begin()->value, 65.3F) << "Float field (value) should be correct";

    // Verify long long type extraction
    EXPECT_EQ(readings.begin()->measurement.timestamp, 1700000000000LL)
        << "Long long field (timestamp) should be correct";

    // Verify other fields
    EXPECT_EQ(readings.begin()->measurement.id, meas_id);
    EXPECT_EQ(readings.begin()->measurement.sensor_name, "TempSensor1");
    EXPECT_EQ(readings.begin()->reading_type, "Humidity");
}

// Test: JOIN with long type (tests separate code path from int64_t)
TYPED_TEST(ExtendedTypesJoinTest, JoinWithLongType) {
    QuerySet<Counter, TypeParam> counter_qs;
    QuerySet<Summary, TypeParam> summary_qs;

    auto create_counter_result = SchemaStatement<Counter>::create_table_if_not_exists<TypeParam>();
    ASSERT_TRUE(create_counter_result.has_value())
        << "Failed to create Counter table: " << create_counter_result.error().message();

    auto create_summary_result = SchemaStatement<Summary>::create_table_if_not_exists<TypeParam>();
    ASSERT_TRUE(create_summary_result.has_value())
        << "Failed to create Summary table: " << create_summary_result.error().message();

    // Insert counter with long value
    Counter const cnt{.id = 0, .name = "PageViews", .count = 9876543210L};

    auto cnt_result = counter_qs.insert(cnt).execute();
    ASSERT_TRUE(cnt_result.has_value()) << "Failed to insert counter: " << cnt_result.error().message();

    std::int64_t const cnt_id = cnt_result.value();

    // Insert summary referencing the counter
    Summary const sum{
        .id = 0, .counter = Counter{.id = static_cast<int>(cnt_id), .name = "", .count = 0L}, .report_type = "Daily"};

    auto sum_result = summary_qs.insert(sum).execute();
    ASSERT_TRUE(sum_result.has_value()) << "Failed to insert summary: " << sum_result.error().message();

    // JOIN to get summaries with fully populated counter
    auto join_result = summary_qs.template join<fields::Summary.counter>().select().execute();
    ASSERT_TRUE(join_result.has_value()) << "JOIN failed: " << join_result.error().message();

    const auto &summaries = join_result.value();
    ASSERT_EQ(summaries.size(), 1) << "Should retrieve one summary";

    // Verify long type extraction
    EXPECT_EQ(summaries.begin()->counter.count, 9876543210L) << "Long field (count) should be correct";

    // Verify other fields
    EXPECT_EQ(summaries.begin()->counter.id, cnt_id);
    EXPECT_EQ(summaries.begin()->counter.name, "PageViews");
    EXPECT_EQ(summaries.begin()->report_type, "Daily");
}

// =============================================================================
// JOIN Type Extraction Tests (from test_coverage_additional.cpp)
// =============================================================================

template <typename ConnType> class JoinTypeExtractionTest : public StormTestFixture<Person, ConnType, Message> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        person_qs = std::make_unique<QuerySet<Person, ConnType>>();
        msg_qs = std::make_unique<QuerySet<Message, ConnType>>();

        std::vector<Person> const people = {
            Person{.name = "Alice",
                   .salary = 4.5,
                   .is_active = true,
                   .score = std::optional<int>(100),
                   .nickname = std::optional<std::string>("Alice bio")},
            Person{.name = "Bob", .salary = 3.2},
            Person{.name = "Charlie", .salary = 4.9, .is_active = true, .score = std::optional<int>(95)},
        };
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(people)));

        std::vector<Message> const messages = {
            {.content = "Post by Alice", .value = 100, .sender = {.id = 1}},
            {.content = "Another Alice post", .value = 200, .sender = {.id = 1}},
            {.content = "Post by Bob", .value = 50, .sender = {.id = 2}},
            {.content = "Charlie post", .value = 150, .sender = {.id = 3}},
        };
        ASSERT_TRUE((storm::test::batch_insert<Message, ConnType>(messages)));
    }

    auto TearDown() -> void override {
        person_qs = nullptr;
        msg_qs = nullptr;
        StormTestFixture<Person, ConnType, Message>::TearDown();
    }

    std::unique_ptr<QuerySet<Person, ConnType>> person_qs;
    std::unique_ptr<QuerySet<Message, ConnType>> msg_qs;
};

TYPED_TEST_SUITE(JoinTypeExtractionTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(JoinTypeExtractionTest, JoinExtractsFloatField) {
    auto result = this->msg_qs->template join<fields::Message.sender>().select().execute();

    ASSERT_TRUE(result.has_value()) << "JOIN with float field should succeed";
    ASSERT_FALSE(result.value().empty());

    for (const auto &msg : result.value()) {
        if (msg.sender.name == "Alice") {
            EXPECT_NEAR(msg.sender.salary, 4.5, 0.01);
        } else if (msg.sender.name == "Bob") {
            EXPECT_NEAR(msg.sender.salary, 3.2, 0.01);
        } else if (msg.sender.name == "Charlie") {
            EXPECT_NEAR(msg.sender.salary, 4.9, 0.01);
        }
    }
}

TYPED_TEST(JoinTypeExtractionTest, JoinExtractsBoolField) {
    auto result = this->msg_qs->template join<fields::Message.sender>().select().execute();

    ASSERT_TRUE(result.has_value()) << "JOIN with bool field should succeed";
    ASSERT_FALSE(result.value().empty());

    for (const auto &msg : result.value()) {
        if (msg.sender.name == "Alice") {
            EXPECT_TRUE(msg.sender.is_active);
        } else if (msg.sender.name == "Bob") {
            EXPECT_FALSE(msg.sender.is_active);
        } else if (msg.sender.name == "Charlie") {
            EXPECT_TRUE(msg.sender.is_active);
        }
    }
}

TYPED_TEST(JoinTypeExtractionTest, JoinExtractsOptionalIntWithValue) {
    auto result = this->msg_qs->template join<fields::Message.sender>().select().execute();

    ASSERT_TRUE(result.has_value()) << "JOIN with optional int should succeed";
    ASSERT_FALSE(result.value().empty());

    for (const auto &msg : result.value()) {
        if (msg.sender.name == "Alice") {
            ASSERT_TRUE(msg.sender.score.has_value());
            EXPECT_EQ(msg.sender.score.value(), 100);
        } else if (msg.sender.name == "Charlie") {
            ASSERT_TRUE(msg.sender.score.has_value());
            EXPECT_EQ(msg.sender.score.value(), 95);
        }
    }
}

TYPED_TEST(JoinTypeExtractionTest, JoinExtractsOptionalIntNull) {
    auto result = this->msg_qs->template join<fields::Message.sender>().select().execute();

    ASSERT_TRUE(result.has_value()) << "JOIN with NULL optional should succeed";
    ASSERT_FALSE(result.value().empty());

    for (const auto &msg : result.value()) {
        if (msg.sender.name == "Bob") {
            EXPECT_FALSE(msg.sender.score.has_value());
        }
    }
}

TYPED_TEST(JoinTypeExtractionTest, JoinExtractsOptionalStringWithValue) {
    auto result = this->msg_qs->template join<fields::Message.sender>().select().execute();

    ASSERT_TRUE(result.has_value()) << "JOIN with optional string should succeed";
    ASSERT_FALSE(result.value().empty());

    for (const auto &msg : result.value()) {
        if (msg.sender.name == "Alice") {
            ASSERT_TRUE(msg.sender.nickname.has_value());
            EXPECT_EQ(msg.sender.nickname.value(), "Alice bio");
        }
    }
}

TYPED_TEST(JoinTypeExtractionTest, JoinExtractsOptionalStringNull) {
    auto result = this->msg_qs->template join<fields::Message.sender>().select().execute();

    ASSERT_TRUE(result.has_value()) << "JOIN with NULL optional string should succeed";
    ASSERT_FALSE(result.value().empty());

    for (const auto &msg : result.value()) {
        if (msg.sender.name == "Bob" || msg.sender.name == "Charlie") {
            EXPECT_FALSE(msg.sender.nickname.has_value());
        }
    }
}

TYPED_TEST(JoinTypeExtractionTest, JoinWithOrderBy) {
    auto result = this->msg_qs->template join<fields::Message.sender>()
                      .template order_by<fields::Message.value>()
                      .select()
                      .execute();

    ASSERT_TRUE(result.has_value()) << "JOIN with ORDER BY should succeed";
    EXPECT_EQ(result.value().size(), 4);

    auto it = result.value().begin();
    int prev_value = 0;
    while (it != result.value().end()) {
        EXPECT_GE(it->value, prev_value);
        prev_value = it->value;
        ++it;
    }
}

// NOLINTEND(misc-const-correctness)
