#pragma once

/**
 * @file test_keys_dataset.h
 * @brief Dataset `keys` of the unified YAML corpus (#872): the key-shape models of tests/test_key_models.h
 *        and TimestampedRecord / UuidPkModel, their tables, the rows tests/test_cases/models.yaml is written
 *        against, and the records its write cases insert.
 *
 * Every key here is caller data (UUID, composite, primary_assigned) except TimestampedRecord's and the two FK
 * models', so the seed spells the keys out; the generated ones are looked up by a unique column instead.
 *
 * Include AFTER `import storm;` (with test_db_helpers.h before the imports, which test_record_helpers.h enforces);
 * it pulls test_record_helpers.h itself, whose make_record and siblings it specialises below.
 */

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <string_view>

#include "test_fixture.h"
#include "test_record_helpers.h"
#include "test_seed_helpers.h"
#include "test_yaml_models.h"

namespace storm::test::keys {

namespace km = storm::test::key_models;

// The tables of dataset `keys`, referenced tables first (PostgreSQL checks a REFERENCES target exists).
// Each test starts on an empty database, so the tables need creating, not clearing (as for `relations`).
template <typename ConnType> auto create_tables(const std::shared_ptr<ConnType> &conn) -> bool {
    return storm::test::ensure_tables<ConnType, UuidPkModel, km::KUuidRef, TimestampedRecord, km::KStock, km::KShipment,
                                      km::KTicket>(conn);
}

inline constexpr std::string_view UUID_ONE = "00000000-0000-4000-8000-000000000001";
inline constexpr std::string_view UUID_TWO = "00000000-0000-4000-8000-000000000002";
inline constexpr std::string_view UUID_THREE = "00000000-0000-4000-8000-000000000003";

// The rows of tests/test_cases/models.yaml (its header lists them).
template <typename ConnType> auto seed() -> void {
    ASSERT_TRUE((batch_insert<UuidPkModel, ConnType>({
        {.id = storm::UUID{UUID_ONE}, .name = "one"},
        {.id = storm::UUID{UUID_TWO}, .name = "two"},
        {.id = storm::UUID{UUID_THREE}, .name = "three"},
    })));
    ASSERT_TRUE((batch_insert<km::KUuidRef, ConnType>({
        {.owner = {.id = storm::UUID{UUID_ONE}}, .value = "r1"},
        {.owner = {.id = storm::UUID{UUID_ONE}}, .value = "r2"},
        {.owner = {.id = storm::UUID{UUID_TWO}}, .value = "r3"},
    })));
    ASSERT_TRUE(
        (batch_insert<TimestampedRecord, ConnType>({{.name = "first"}, {.name = "second"}, {.name = "third"}})));
    ASSERT_TRUE((batch_insert<km::KStock, ConnType>({
        {.warehouse = 1, .sku = "apple", .quantity = 10},
        {.warehouse = 1, .sku = "pear", .quantity = 0},
        {.warehouse = 2, .sku = "apple", .quantity = 5},
        {.warehouse = 2, .sku = "plum", .quantity = 7},
    })));
    ASSERT_TRUE((batch_insert<km::KShipment, ConnType>({
        {.stock = {.warehouse = 1, .sku = "apple"}, .amount = 3},
        {.stock = {.warehouse = 1, .sku = "apple"}, .amount = 4},
        {.stock = {.warehouse = 2, .sku = "plum"}, .amount = 1},
    })));
    ASSERT_TRUE((batch_insert<km::KTicket, ConnType>(
        {{.id = 10, .title = "alpha"}, {.id = 20, .title = "beta"}, {.id = 30, .title = "gamma"}})));
}

// "Updated<text>": the marker the update runner's is_original_record looks for.
inline auto updated(std::string_view text) -> std::string {
    std::string out{"Updated"};
    out += text;
    return out;
}

} // namespace storm::test::keys

namespace storm::test {

// The records of the write cases on dataset `keys` (test_write_runner.h). Their keys never collide with the
// seeded rows: a fresh UUID, warehouse 100 + i, ticket 1000 + i. KShipment / KUuidRef have none (an FK record
// needs a target row), so a write case on them does not compile.
template <> inline auto make_record<UuidPkModel>(int i) -> UuidPkModel { return {.name = numbered("U", i)}; }
template <> inline auto make_record<TimestampedRecord>(int i) -> TimestampedRecord {
    return {.name = numbered("T", i)};
}
template <> inline auto make_record<key_models::KStock>(int i) -> key_models::KStock {
    return {.warehouse = 100 + i, .sku = numbered("S", i), .quantity = i};
}
template <> inline auto make_record<key_models::KTicket>(int i) -> key_models::KTicket {
    return {.id = 1000 + i, .title = numbered("K", i)};
}

// An update keeps the key (it is the WHERE) and changes a non-key column.
template <> inline auto make_updated_record<UuidPkModel>(const UuidPkModel &r) -> UuidPkModel {
    UuidPkModel u = r;
    u.name = keys::updated(r.name);
    return u;
}
template <> inline auto make_updated_record<TimestampedRecord>(const TimestampedRecord &r) -> TimestampedRecord {
    TimestampedRecord u = r;
    u.name = keys::updated(r.name);
    return u;
}
// KStock's only non-key column is an int: an update adds UPDATED_QUANTITY, so an original quantity stays below it.
inline constexpr int UPDATED_QUANTITY = 1000;
template <> inline auto make_updated_record<key_models::KStock>(const key_models::KStock &r) -> key_models::KStock {
    key_models::KStock u = r;
    u.quantity = r.quantity + UPDATED_QUANTITY;
    return u;
}
template <> inline auto make_updated_record<key_models::KTicket>(const key_models::KTicket &r) -> key_models::KTicket {
    key_models::KTicket u = r;
    u.title = keys::updated(r.title);
    return u;
}

template <> inline auto is_original_record<UuidPkModel>(const UuidPkModel &r) -> bool {
    return !r.name.starts_with("Updated");
}
template <> inline auto is_original_record<TimestampedRecord>(const TimestampedRecord &r) -> bool {
    return !r.name.starts_with("Updated");
}
template <> inline auto is_original_record<key_models::KStock>(const key_models::KStock &r) -> bool {
    return r.quantity < UPDATED_QUANTITY;
}
template <> inline auto is_original_record<key_models::KTicket>(const key_models::KTicket &r) -> bool {
    return !r.title.starts_with("Updated");
}

} // namespace storm::test
