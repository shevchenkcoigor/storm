#pragma once

/**
 * @file test_key_models.h
 * @brief The key-shape models of dataset `keys` of the unified YAML corpus (#872).
 *
 * One model per key shape a query or a write treats differently: a UUID primary key
 * (shared/models/uuid_pk_model.h), a composite key with an integer and a text part, a caller-assigned
 * integer key (`primary_assigned`), an FK to each of the first two, and the auto-timestamp model
 * (shared/models/timestamped_record.h). The UUID, composite and assigned keys are
 * caller data (only TimestampedRecord's and the two FK models' plain `primary` ints are DB-generated),
 * which is the shape INSERT / UPDATE / DELETE branch on. The test-only ones live here rather than in
 * shared/models/ so storm_bench is untouched.
 *
 * The two FKs are `RefAction::Cascade`: the write cases erase rows of the referenced models, and a
 * RESTRICT FK would turn that into an FK violation on PostgreSQL (SQLite enforces it too).
 *
 * Include AFTER `import storm;` and after <meta> and <string>.
 */

#include "../shared/models/timestamped_record.h"
#include "../shared/models/uuid_pk_model.h"

namespace storm::test::key_models {

// Composite key, mixed part types (int + text): the WHERE of an UPDATE / DELETE pins both parts.
struct KStock {
    [[= storm::primary_part]] int warehouse{};
    [[= storm::primary_part]] std::string sku;
    int quantity{};
};

// An FK to the composite key: two columns, stock_warehouse and stock_sku (#504).
struct KShipment {
    [[= storm::primary]] int id{};
    [[= storm::fk<storm::RefAction::Cascade>]] KStock stock;
    int amount{};
};

// An FK to the UUID key: a TEXT (SQLite) / UUID (PostgreSQL) FK column (#507).
struct KUuidRef {
    [[= storm::primary]] int id{};
    [[= storm::fk<storm::RefAction::Cascade>]] UuidPkModel owner;
    std::string value;
};

// A caller-assigned integer key (#586): INSERT emits it, nothing is RETURNed.
struct KTicket {
    [[= storm::primary_assigned]] int id{};
    std::string title;
};

} // namespace storm::test::key_models
