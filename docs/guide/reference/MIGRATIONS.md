# Database Migrations with Atlas

Storm integrates [Atlas](https://atlasgo.io) for database schema management. Models are auto-discovered via C++26 namespace reflection — any struct with `[[= primary]]` in the target namespace is picked up automatically.

## Architecture

```
namespace schema { struct User { ... }; struct Post { ... }; }
        |
        v  compile-time reflection (std::meta::members_of)
auto-discover all structs with primary key
        |
        v
storm_enable_migrations(NAMESPACE "schema")  -->  schema binary + targets
        |
        v
cmake --build . --target makemigrations   -->  migrations/ (versioned SQL)
        |
        v
STORM_DB_URL="..." cmake --build . --target migrate  -->  live database
```

## Prerequisites

- **Storm library** linked to your project
- **Atlas CLI**: `curl -sSf https://atlasgo.sh | sh`

## Quick Start

### 1. Define Models in a Namespace

```cpp
// schema/models.h (or any file — auto-detected)
#pragma once
#include <optional>
#include <string>

namespace schema {

struct User {
    [[= primary]] int id{};
    [[= unique]] std::string email;
    std::string name;
    std::optional<std::string> bio;
};

struct Post {
    [[= primary]] int id{};
    std::string title;
    std::string body;
    [[= fk<>]] User author;
};

// Add 1000 more models — all auto-discovered, no registration needed.

} // namespace schema
```

### 2. One Line of CMake

```cmake
include(cmake/storm_migrations.cmake)

storm_enable_migrations(NAMESPACE "schema")
```

That's it. Storm:
- Auto-detects the header containing `namespace schema {`
- Generates a schema binary using compile-time reflection
- Creates `makemigrations`, `migrate`, `migrate-validate`, `migrate-status`, `migrate-hash`, and `validate-schema` targets

### 3. Generate and Apply Migrations

```bash
# Generate a migration from model changes
cmake --build . --target makemigrations

# Apply to database
STORM_DB_URL="sqlite://myapp.db" cmake --build . --target migrate

# Validate migration directory integrity (checksums, order)
cmake --build . --target migrate-validate

# Show which migrations are applied vs pending
STORM_DB_URL="sqlite://myapp.db" cmake --build . --target migrate-status

# Recalculate atlas.sum after manual edits to migration files
cmake --build . --target migrate-hash

# Validate entity definitions match the live database (no changes made)
STORM_DB_URL="sqlite://myapp.db" cmake --build . --target validate-schema
```

## How Auto-Discovery Works

At compile time, the generated schema binary:

1. Calls `std::meta::members_of(^^schema)` to enumerate all types in the namespace
2. Filters to class types only (`is_class_type`) — skips enums, aliases, etc.
3. Checks each class for a `[[= primary]]` annotation
4. Emits `CREATE TABLE` + `CREATE INDEX` SQL for each model found

No manual registration, no model list, no macros.

### Column Defaults and `ADD COLUMN` on Populated Tables

A non-nullable scalar/text field with a C++ default member initializer emits a
SQL `DEFAULT` clause. Since #413 this covers `bool`, integers, `double`/`float`,
and string types (not just `bool` — that was the original #344 scope):

| C++ field                  | SQLite                                   | PostgreSQL                                       |
|----------------------------|------------------------------------------|--------------------------------------------------|
| `bool x{true};`            | `x INTEGER NOT NULL DEFAULT 1`           | `x BOOLEAN NOT NULL DEFAULT TRUE`                |
| `int priority{1};`         | `priority INTEGER NOT NULL DEFAULT 1`    | `priority INTEGER NOT NULL DEFAULT 1`            |
| `double rate{0.0};`        | `rate REAL NOT NULL DEFAULT 0.0`         | `rate DOUBLE PRECISION NOT NULL DEFAULT 0.0`     |
| `std::string status{"new"};` | `status TEXT NOT NULL DEFAULT 'new'`   | `status TEXT NOT NULL DEFAULT 'new'`             |

Text defaults are SQL-escaped (embedded `'` is doubled: `"O'Brien"` → `DEFAULT 'O''Brien'`).

This matters for migrations: `ALTER TABLE ... ADD COLUMN <x> NOT NULL` is
**rejected by SQLite on a table that already has rows** ("Cannot add a NOT NULL
column with default value NULL"). Emitting the `DEFAULT` makes the generated
`ADD COLUMN ... NOT NULL DEFAULT <v>` valid on populated tables — for every
covered type, not only `bool`.

### PostgreSQL Integer Column Width (#603)

PG integer columns are now width-matched to the C++ field's declared type (`short` →
`SMALLINT`, `int` → `INTEGER`, `int64_t` → `BIGINT`, …; see
[FIELD_TYPES.md](FIELD_TYPES.md#integer-types)) instead of always `BIGINT`. This is a
**breaking DDL change for any existing PostgreSQL database Storm has already generated a
schema against** — a column narrowed from `BIGINT` to `INTEGER`/`SMALLINT` needs an actual
migration (`ALTER COLUMN ... TYPE ...`, checked against existing data first), not just a
regenerated `CREATE TABLE`. It was implemented without a migration path because no
production Storm database exists yet. Also note the tradeoff this gives up: **widening a
field's C++ type later (e.g. `int` → `int64_t`) now needs a matching PG column-type
migration**, where before every integer field was already `BIGINT` and no DDL change was
needed for such a widening.

The value is recovered at compile time from a default-constructed instance of the
model. **Reflection cannot distinguish `int x{};` from `int x = 0;`** — both report
"has default initializer, value 0" — so a `{}`-value-initialized field gets a
`DEFAULT` too (e.g. `int age{};` → `DEFAULT 0`). A field with **no** initializer
at all (`int x;`) gets no `DEFAULT`. Nullable (`std::optional<T>`), date, BLOB, and
UUID columns are never given a `DEFAULT`; a non-nullable column of one of those
types needs a server-side default in a hand-edited migration. Models that are not
literal types (e.g. those with a many-to-many `plf::hive`/`std::vector` member)
cannot have their scalar defaults read at compile time, so their columns keep the
no-`DEFAULT` shape.

### Primary Key DDL — plain `INTEGER PRIMARY KEY` by default (#379, breaking)

The SQLite schema generator emits **plain `id INTEGER PRIMARY KEY`** for an `int`
primary key. SQLite still auto-assigns ids (a plain `INTEGER PRIMARY KEY` aliases
the rowid). The `AUTOINCREMENT` keyword — which adds the *never-reuse* guarantee at
~358 ns/insert of `sqlite_sequence` bookkeeping — is **opt-in** via
`primary_autoincrement`:

| C++ PK annotation                         | SQLite DDL                              |
|-------------------------------------------|-----------------------------------------|
| `[[= primary]] int id;`        | `id INTEGER PRIMARY KEY`                 |
| `[[= primary_autoincrement]] int id;` | `id INTEGER PRIMARY KEY AUTOINCREMENT` |

PostgreSQL is unaffected (it uses `GENERATED BY DEFAULT AS IDENTITY` regardless).

**Migration impact (breaking):** the emitted `CREATE TABLE` for any `int`-PK model
changed, so `atlas migrate diff` against a baseline that still carries
`AUTOINCREMENT` will produce a table-rebuild migration. For existing databases this
is cosmetic (both forms auto-assign ids); regenerate your migration baseline if you
want it to match the new output, or keep the old baseline if id-reuse behaviour
must not change. Opt back into the old behaviour per-model with
`primary_autoincrement`.

## CMake Function Reference

```cmake
storm_enable_migrations(
  NAMESPACE     <name>          # Required: namespace containing model structs
  MODELS_HEADER <path>          # Optional: explicit header (skips auto-detection)
  MIGRATION_DIR <path>          # Optional: migration directory (default: "migrations")
  DIALECT       <dialect>       # Optional: "sqlite" or "postgresql" (default: "sqlite")
  TARGET_NAME   <name>          # Optional: schema binary name (default: "storm_schema")
)
```

## Manual Schema Export

The generated schema binary supports direct use:

```bash
# Print SQLite schema to stdout
./build/debug/storm_schema

# PostgreSQL dialect
./build/debug/storm_schema --dialect postgresql

# Write to file
./build/debug/storm_schema --output schema.sql
```

## Integration Tests

Storm dogfoods `storm_enable_migrations()` in `tests/tools/storm_schema/`. Run the test suite:

```bash
./scripts/test-atlas-migration.sh
```

## Supported Backends

| Backend    | Schema export | Declarative apply | Versioned migrations |
|-----------|--------------|-------------------|---------------------|
| SQLite     | Yes          | Yes               | Yes                 |
| PostgreSQL | Yes          | Yes               | Yes                 |
