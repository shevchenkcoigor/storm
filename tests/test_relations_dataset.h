#pragma once

/**
 * @file test_relations_dataset.h
 * @brief Dataset `relations` of the unified YAML corpus (#869): the relation models of
 *        tests/test_relation_models.h, their tables, and the rows tests/test_cases/join.yaml is written against.
 *
 * Rows are inserted through Storm; only the auto-junction rows are raw SQL (Storm has no write API for an
 * auto junction, JOIN_OPERATIONS.md). Ids are looked up by name rather than assumed 1..n: a PostgreSQL
 * IDENTITY does not restart when the test schema is recreated.
 *
 * Include AFTER `import storm;`.
 */

#include <gtest/gtest.h>
#include <memory>
#include <meta>
#include <string>
#include <string_view>

#include "test_fixture.h"
#include "test_seed_helpers.h"
#include "test_yaml_models.h"

namespace storm::test::relations {

namespace m2m = storm::test::relation_models;
namespace bench_m2m = storm::benchmark::m2m; // BCourse / BStudent, named directly where reflected

// The tables of dataset `relations`, referenced tables first (PostgreSQL checks a REFERENCES target exists).
// Each test starts on an empty database (SQLite `:memory:`, PostgreSQL's schema recreated by
// rollback_test_txn), so the tables need creating, not clearing.
template <typename ConnType> auto create_tables(const std::shared_ptr<ConnType> &conn) -> bool {
    return storm::test::ensure_tables<ConnType, m2m::BCourse, m2m::BClub, m2m::BStudent, m2m::BMember, m2m::BPupil,
                                      m2m::BEnrollment, m2m::BAuthor, m2m::BPost, m2m::BReporter, m2m::BBug>(conn);
}

// The id of the one row whose `Key` member is `value`.
template <typename Model, std::meta::info Key, typename ConnType> auto id_where(std::string_view value) -> int {
    auto row =
        storm::QuerySet<Model, ConnType>{}.where(storm::meta::FieldRef<Key>{} == std::string{value}).get().execute();
    EXPECT_TRUE(row.has_value()) << "relations dataset: no single row named " << value;
    return row.has_value() ? row->id : 0;
}

// One auto-junction row: `<junction>` ("<owner>_id", "<related>_id"), the names Storm derives (#828).
template <typename ConnType>
auto link(std::string_view owner, std::string_view related, int owner_id, int related_id) -> void {
    std::string sql{"INSERT INTO \""};
    sql += owner;
    sql += '_';
    sql += related;
    sql += "\" (\"";
    sql += owner;
    sql += "_id\", \"";
    sql += related;
    sql += "_id\") VALUES (";
    sql += std::to_string(owner_id);
    sql += ", ";
    sql += std::to_string(related_id);
    sql += ')';
    auto inserted = storm::db::DefaultConnection<ConnType>::get()->execute(sql);
    ASSERT_TRUE(inserted.has_value()) << sql << ": " << inserted.error().message();
}

// The rows of tests/test_cases/join.yaml (its header lists them).
template <typename ConnType> auto seed() -> void {
    ASSERT_TRUE(
        (batch_insert<m2m::BCourse, ConnType>({{.title = "Math"}, {.title = "Physics"}, {.title = "Chemistry"}})));
    ASSERT_TRUE((batch_insert<m2m::BClub, ConnType>({{.name = "Chess"}, {.name = "Robotics"}})));
    ASSERT_TRUE((batch_insert<m2m::BStudent, ConnType>(
        {{.name = "Alice", .age = 20}, {.name = "Bob", .age = 22}, {.name = "Carol", .age = 25}})));
    ASSERT_TRUE(
        (batch_insert<m2m::BMember, ConnType>({{.name = "Ann"}, {.name = "Ben"}, {.name = "Cat"}, {.name = "Dan"}})));
    ASSERT_TRUE((batch_insert<m2m::BPupil, ConnType>({{.name = "Pia"}, {.name = "Quinn"}, {.name = "Rex"}})));
    ASSERT_TRUE((batch_insert<m2m::BAuthor, ConnType>({{.name = "Austen"}, {.name = "Bronte"}, {.name = "Carver"}})));
    ASSERT_TRUE((batch_insert<m2m::BReporter, ConnType>({{.name = "Rita"}, {.name = "Sam"}, {.name = "Tom"}})));

    const auto course = [](std::string_view title) {
        return id_where<m2m::BCourse, ^^bench_m2m::BCourse::title, ConnType>(title);
    };
    const auto club = [](std::string_view name) { return id_where<m2m::BClub, ^^m2m::BClub::name, ConnType>(name); };
    const auto student = [](std::string_view name) {
        return id_where<m2m::BStudent, ^^bench_m2m::BStudent::name, ConnType>(name);
    };
    const auto member = [](std::string_view name) {
        return id_where<m2m::BMember, ^^m2m::BMember::name, ConnType>(name);
    };
    const auto pupil = [](std::string_view name) { return id_where<m2m::BPupil, ^^m2m::BPupil::name, ConnType>(name); };
    const auto author = [](std::string_view name) {
        return id_where<m2m::BAuthor, ^^m2m::BAuthor::name, ConnType>(name);
    };
    const auto reporter = [](std::string_view name) {
        return id_where<m2m::BReporter, ^^m2m::BReporter::name, ConnType>(name);
    };

    link<ConnType>("bstudent", "bcourse", student("Alice"), course("Math"));
    link<ConnType>("bstudent", "bcourse", student("Alice"), course("Physics"));
    link<ConnType>("bstudent", "bcourse", student("Bob"), course("Math"));

    link<ConnType>("bmember", "bcourse", member("Ann"), course("Math"));
    link<ConnType>("bmember", "bcourse", member("Ann"), course("Physics"));
    link<ConnType>("bmember", "bcourse", member("Ben"), course("Math"));
    link<ConnType>("bmember", "bclub", member("Ann"), club("Chess"));
    link<ConnType>("bmember", "bclub", member("Cat"), club("Chess"));
    link<ConnType>("bmember", "bclub", member("Cat"), club("Robotics"));

    ASSERT_TRUE((batch_insert<m2m::BEnrollment, ConnType>({
        {.pupil = {.id = pupil("Pia")}, .course = {.id = course("Math")}, .grade = "A"},
        {.pupil = {.id = pupil("Pia")}, .course = {.id = course("Physics")}, .grade = "B"},
        {.pupil = {.id = pupil("Quinn")}, .course = {.id = course("Chemistry")}, .grade = "A"},
    })));
    ASSERT_TRUE((batch_insert<m2m::BPost, ConnType>({
        {.title = "P1", .author = {.id = author("Austen")}},
        {.title = "P2", .author = {.id = author("Austen")}},
        {.title = "P3", .author = {.id = author("Bronte")}},
    })));
    ASSERT_TRUE((batch_insert<m2m::BBug, ConnType>({
        {.summary = "b1", .author = {.id = reporter("Rita")}, .reviewer = {.id = reporter("Sam")}},
        {.summary = "b2", .author = {.id = reporter("Rita")}, .reviewer = {.id = reporter("Sam")}},
        {.summary = "b3", .author = {.id = reporter("Sam")}, .reviewer = {.id = reporter("Rita")}},
        {.summary = "b4", .author = {.id = reporter("Rita")}, .reviewer = {.id = reporter("Tom")}},
    })));
}

} // namespace storm::test::relations
