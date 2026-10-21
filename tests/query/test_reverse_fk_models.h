#ifndef TESTS_QUERY_TEST_REVERSE_FK_MODELS_H
#define TESTS_QUERY_TEST_REVERSE_FK_MODELS_H

// Reverse-FK test models (#398). Include AFTER `import storm;` — the
// [[= storm::reverse_fk<...>]] annotations need the storm module, and
// model structs stay textual because clang-p2996 loses annotations across
// BMI boundaries (#262).
//
// "All Persons, each with the Tasks that point at them": RfTask has an FK to
// RfPerson, and RfPerson declares a reverse_fk container that the eager load
// fills. The Base⟷Owner reference cycle is broken by:
//   - RfPerson's container is vector<RfTask> (RfTask forward-declared at the
//     annotation site — vector tolerates an incomplete value_type until its
//     members are instantiated), and
//   - the annotation carries the OWNER TYPE (^^RfTask), not a member splice,
//     so RfTask need not be complete where RfPerson is defined. The unique FK
//     back at RfPerson is resolved when the join is instantiated.
//
// Multi-FK disambiguation lives on the AGGREGATE/SELECTOR path (no container,
// no cycle): RfBug has two FKs to RfReporter (author, reviewer), and the cross-
// model FK selector ^^RfBug::author / ^^RfBug::reviewer names which to reverse.

#include <memory>
#include <meta>
#include <plf_hive/plf_hive.h>
#include <string>
#include <vector>
using storm::field_specs_for;
using storm::primary;
using storm::orm::statements::InsertStatement;

struct RfTask; // forward declaration breaks the Base⟷Owner cycle

// Base model: owns a reverse_fk container of the tasks pointing at it. The
// annotation names the OWNER TYPE; the unique FK back at RfPerson is resolved.
struct RfPerson {
    [[= primary]] int id{};
    std::string name;
    int age{};
    // Not a column — invisible to CRUD, filled on eager load.
    [[= storm::reverse_fk<^^RfTask>]] std::vector<RfTask> tasks;
};

struct RfTask {
    [[= primary]] int id{};
    std::string title;
    [[= storm::fk<>]] RfPerson assignee;
};

// shared_ptr container coverage: RfBoard collects the notes pinned to it.
struct RfNote;

struct RfBoard {
    [[= primary]] int id{};
    std::string name;
    [[= storm::reverse_fk<^^RfNote>]] std::vector<std::shared_ptr<RfNote>> notes;
};

struct RfNote {
    [[= primary]] int id{};
    std::string body;
    [[= storm::fk<>]] RfBoard board;
};

// Multi-FK disambiguation (aggregate/selector path — no reverse container, no
// cycle, so RfReporter is complete before RfBug): two FKs back at RfReporter.
struct RfReporter {
    [[= primary]] int id{};
    std::string name;
};

struct RfBug {
    [[= primary]] int id{};
    std::string summary;
    [[= storm::fk<>]] RfReporter author;
    [[= storm::fk<>]] RfReporter reviewer;
};

namespace storm::test {

// Seeds: Alice(20)→[T1,T2], Bob(22)→[T3], Carol(25)→[] (no tasks).
template <typename ConnType> inline auto seed_rf_people() -> void {
    std::vector<RfPerson> const people = {
        {.name = "Alice", .age = 20}, {.name = "Bob", .age = 22}, {.name = "Carol", .age = 25}};
    ASSERT_TRUE((InsertStatement<RfPerson, ConnType>{}).query(std::span<const RfPerson>(people)).execute().has_value());

    std::vector<RfTask> const tasks = {{.title = "T1", .assignee = {.id = 1}},
                                       {.title = "T2", .assignee = {.id = 1}},
                                       {.title = "T3", .assignee = {.id = 2}}};
    ASSERT_TRUE((InsertStatement<RfTask, ConnType>{}).query(std::span<const RfTask>(tasks)).execute().has_value());
}

} // namespace storm::test

// fields:: selector proxies (#518).
namespace fields {

struct RfPersonT;
consteval { std::meta::define_aggregate(^^RfPersonT, field_specs_for(^^RfPerson)); }
inline constexpr RfPersonT RfPerson{};

struct RfTaskT;
consteval { std::meta::define_aggregate(^^RfTaskT, field_specs_for(^^RfTask)); }
inline constexpr RfTaskT RfTask{};

struct RfBugT;
consteval { std::meta::define_aggregate(^^RfBugT, field_specs_for(^^RfBug)); }
inline constexpr RfBugT RfBug{};
} // namespace fields

#endif // TESTS_QUERY_TEST_REVERSE_FK_MODELS_H
