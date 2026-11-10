#pragma once

/**
 * @file test_record_helpers.h
 * @brief Model record generators used by InsertRunner/UpdateRunner/EraseRunner.
 *
 * Split out of test_models.h (issue #634): these specializations are the only
 * reason the runner headers need Person/SimpleRecord/Message, so a TU that uses
 * neither runner should not pay for them.
 *
 * IMPORTANT: Include AFTER `import storm;` — the model headers below require it.
 */

#ifndef STORM_TESTS_TEST_DB_HELPERS_H
#error                                                                                                                 \
    "test_record_helpers.h: #include \"test_db_helpers.h\" BEFORE `import storm;` (it declares storm::test::numbered)"
#endif

#include "../shared/models/message.h"
#include "../shared/models/person.h"
#include "../shared/models/simple_record.h"

namespace storm::test {

template <typename Model> auto make_record(int i) -> Model = delete;
template <> inline auto make_record<Person>(int i) -> Person {
    return {.name = numbered("P", i + 1),
            .age = 20 + (i % 50),
            .salary = 1000.0 * (i + 1),
            .is_active = (i % 2 == 0),
            .years_experience = i % 30};
}
template <> inline auto make_record<SimpleRecord>(int i) -> SimpleRecord { return {0, numbered("R", i), i}; }
template <> inline auto make_record<Message>(int i) -> Message { return {.content = numbered("M", i), .value = i}; }

template <typename Model> auto make_updated_record(const Model &) -> Model = delete;
template <> inline auto make_updated_record<Person>(const Person &p) -> Person {
    Person u = p;
    u.name = numbered("Updated", p.id);
    return u;
}
template <> inline auto make_updated_record<SimpleRecord>(const SimpleRecord &r) -> SimpleRecord {
    return {r.id, numbered("Updated", r.id), r.value * 2};
}

template <typename Model> auto is_original_record(const Model &) -> bool = delete;
template <> inline auto is_original_record<Person>(const Person &p) -> bool { return p.name.starts_with("P"); }
template <> inline auto is_original_record<SimpleRecord>(const SimpleRecord &r) -> bool {
    return r.name.starts_with("R");
}

} // namespace storm::test
