# C++26 Module System: Database-Agnostic Design

This document describes the technique for writing database-agnostic modules in Storm ORM while maintaining zero-cost abstraction through cross-module inlining.

## The Problem

C++20/26 modules have a significant limitation: **function bodies defined in module implementation units are not visible to importers**, preventing inlining across module boundaries.

```cpp
// sqlite.cppm - Module interface
export module storm_db_sqlite;

export class Statement {
    // This function CANNOT be inlined by callers in other modules
    // because the body is not visible at the call site
    [[nodiscard]] auto step_raw() noexcept -> int {
        return sqlite3_step(raw_);  // Body compiled separately
    }
};

// select.cppm - Different module
import storm_db_sqlite;

// This loop has ~2-3% overhead because step_raw() cannot be inlined
while ((result = stmt->step_raw()) == SQLITE_ROW) {
    // Hot loop - function call overhead per iteration
}
```

**Why this matters:**
- Function call overhead: push/pop stack, jump, return
- Cannot inline small functions (even with `always_inline` attribute)
- Prevents compiler optimizations across module boundaries
- LTO (Link-Time Optimization) can help, but requires special toolchain support

## The Solution: Template Trick

**Making functions templates forces the compiler to include their bodies in the module interface**, because templates must be instantiated at the call site.

```cpp
// sqlite.cppm - Module interface
export module storm_db_sqlite;

export class Statement {
    // Template with dummy parameter - body IS visible to importers
    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto step_raw() noexcept -> int {
        return sqlite3_step(raw_);  // Body available for inlining!
    }
};

// select.cppm - Different module
import storm_db_sqlite;

// Now step_raw() CAN be inlined - zero overhead!
while ((result = stmt->step_raw()) == SQLITE_ROW) {
    // Compiler inlines sqlite3_step() directly here
}
```

## Implementation Pattern

### Step 1: Define Template Methods in Database Backend

```cpp
// src/db/sqlite.cppm
export module storm_db_sqlite;

export class Statement {
    sqlite3_stmt* raw_;  // Cached raw pointer

public:
    // All hot-path methods are templates for cross-module inlining

    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto step_raw() noexcept -> int {
        return sqlite3_step(raw_);
    }

    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto extract_int(int col) const noexcept -> int {
        return sqlite3_column_int(raw_, col);
    }

    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto extract_int64(int col) const noexcept -> int64_t {
        return sqlite3_column_int64(raw_, col);
    }

    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto extract_double(int col) const noexcept -> double {
        return sqlite3_column_double(raw_, col);
    }

    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto extract_text_ptr(int col) const noexcept
            -> const unsigned char* {
        return sqlite3_column_text(raw_, col);
    }

    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto extract_bytes(int col) const noexcept -> int {
        return sqlite3_column_bytes(raw_, col);
    }

    template <typename = void>
    [[nodiscard]] __attribute__((always_inline)) auto is_null(int col) const noexcept -> bool {
        return sqlite3_column_type(raw_, col) == SQLITE_NULL;
    }

    template <typename = void>
    __attribute__((always_inline)) auto reset() noexcept -> void {
        sqlite3_reset(raw_);
    }

    // Database-agnostic constants
    static constexpr int ROW_AVAILABLE = SQLITE_ROW;
    static constexpr int NO_MORE_ROWS  = SQLITE_DONE;
};
```

### Step 2: Use Statement Methods in ORM Modules

```cpp
// src/orm/statements/select.cppm
module;

#include <meta>  // C++26 reflection
// NOTE: No #include <sqlite3.h> - fully database-agnostic!

export module storm_orm_statements_select;

import storm_db_concept;  // Concepts only — no concrete backend

export template <typename T, storm::db::DatabaseConnection ConnType>
class BasicSelectStatement {
    // Extract column using Statement methods (database-agnostic)
    template <typename FieldType>
    static auto extract_column(Statement* stmt, int col_idx) -> FieldType {
        if constexpr (std::is_same_v<FieldType, int>) {
            return stmt->extract_int(col_idx);
        } else if constexpr (std::is_same_v<FieldType, int64_t>) {
            return stmt->extract_int64(col_idx);
        } else if constexpr (std::is_same_v<FieldType, double>) {
            return stmt->extract_double(col_idx);
        } else if constexpr (std::is_same_v<FieldType, std::string>) {
            const unsigned char* text = stmt->extract_text_ptr(col_idx);
            if (text) {
                auto len = static_cast<size_t>(stmt->extract_bytes(col_idx));
                return std::string(reinterpret_cast<const char*>(text), len);
            }
            return {};
        }
        // ... other types
    }

    // Query loop using Statement methods
    auto execute_query_loop(Statement* stmt) {
        while (stmt->step_raw() == Statement::ROW_AVAILABLE) {
            T obj;
            extract_all_columns(stmt, obj);
            results.insert(std::move(obj));
        }
        stmt->reset();
        return results;
    }
};
```

### Step 3: The PostgreSQL Backend

Storm ships a PostgreSQL backend alongside SQLite (`src/db/postgresql.cppm`,
`postgresql_connection.cppm`, `postgresql_error.cppm`, `postgresql_statement.cppm`), following the
same-interface pattern described above — `Statement` exposes the same template hot-path
methods (`step_raw`, `extract_int`, ...), just backed by `PGresult`/`PQgetvalue` instead of
`sqlite3_stmt`/`sqlite3_column_*`. A future third backend (e.g. MySQL) would follow the identical
recipe: a new backend module with the same `Statement`/`Connection` interface, gated by the
concepts in `src/db/concept.cppm`.

### Dialect-Support Concepts (#477)

Because two backends now genuinely differ in SQL dialect, `src/db/concept.cppm` declares named
capability gates instead of ad-hoc `if constexpr (requires { ConnType::trait; })` probes at each
call site:

```cpp
// The dialect switch itself — true for PG, false for SQLite. Used to pick
// PG-specific SQL (NULLS FIRST/LAST ordering, LIMIT ALL, the schema Dialect enum).
template <typename ConnType>
concept SupportsPgDialect = requires {
    { ConnType::uses_pg_dialect } -> std::convertible_to<bool>;
};

// Existence probe: both backends declare `supports_limit_all`; call sites still
// read the bool VALUE to pick the spelling (PG: LIMIT ALL, SQLite: LIMIT -1).
template <typename ConnType>
concept SupportsLimitAll = requires {
    { ConnType::supports_limit_all } -> std::convertible_to<bool>;
};

// The surface storm::begin / TransactionGuard (#415) needs to run an RAII
// transaction — a mis-typed connection fails at the begin() call site.
template <typename ConnType>
concept TransactionCapable = requires(ConnType& conn, std::string_view sql) {
    typename ConnType::Error;
    { conn.in_transaction() } -> std::convertible_to<bool>;
    conn.enter_transaction();
    conn.leave_transaction();
    { conn.execute(sql) } -> std::same_as<std::expected<void, typename ConnType::Error>>;
};
```

`SupportsPgDialect` gates the `Dialect` enum selection in `schema.cppm` and `append_order_by`'s
NULLS FIRST/LAST behavior in `base.cppm`. `SupportsLimitAll` gates `append_limit_offset`.
`TransactionCapable` constrains `TransactionGuard` and `storm::begin`/`storm::transaction`. Each
backend `static_assert`s these next to its `Connection` definition. Two candidate concepts were
considered and dropped: `SupportsRightJoin` (`right_join()` was removed — no call site) and
`SupportsReturning`/`SupportsStrictTables` (declared on the connections but never read by any
call site).

### Step 4: The Default Backend Lives in One Module (`storm_defaults`)

The generic ORM templates name **no** backend — not even as a default template argument. Each
public name is an alias in `src/storm_defaults.cppm`, the only module that binds a default
backend:

| Public name (alias, unchanged spelling) | Generic template (backend-free) | Module of the template |
|---|---|---|
| `storm::QuerySet<T, C = sqlite::Connection, F = false>` | `storm::BasicQuerySet<T, C, F>` | `storm_orm_queryset` |
| `storm::orm::statements::SelectStatement<T, C = …>` | `BasicSelectStatement<T, C>` | `storm_orm_statements_select` |
| `storm::orm::statements::InsertStatement<T, C = …>` | `BasicInsertStatement<T, C>` | `storm_orm_statements_insert` |
| `storm::orm::statements::UpdateStatement<T, C = …>` | `BasicUpdateStatement<T, C>` | `storm_orm_statements_update` |
| `storm::orm::statements::EraseStatement<T, C = …>` | `BasicEraseStatement<T, C>` | `storm_orm_statements_erase` |
| `storm::db::DefaultConnection<C = sqlite::Connection>` | `storm::db::BasicDefaultConnection<C>` | `storm_db_default_connection` |

```
storm_db_concept ─> storm_db_pool ─> storm_db_default_connection ─> storm_orm_* (statements, schema, queryset) ─┐
                                                                                                                 ├─> storm_defaults ─> storm
storm_db_sqlite ─────────────────────────────────────────────────────────────────────────────────────────────────┘
          (no edge from storm_db_sqlite into any storm_orm_* module, storm_db_pool or storm_db_default_connection)
```

`import storm;` re-exports `storm_defaults`, so code that imports the umbrella module is
unchanged. A TU that imports individual statement/queryset modules and uses the short names must
also `import storm_defaults;`. A PostgreSQL-only program can skip it and spell
`BasicQuerySet<T, postgresql::Connection>` — no generic module it imports reaches the SQLite
backend. (This is import-graph decoupling: the single `storm` CMake target still compiles and links
every backend.)

The `SchemaStatement` helpers `create_table_if_not_exists()` / `create_indexes_if_not_exist()` no
longer default their `ConnType` either: pass a connection (`create_table_if_not_exists(conn)`
deduces it) or name it (`create_table_if_not_exists<sqlite::Connection>()`).

**BREAKING (pre-release)**: the class templates were renamed to `Basic*`; the old names survive
as alias templates, so they cannot be forward-declared or specialised under the old name, and
diagnostics, `display_string_of` and trait/partial-specialisation matching see `Basic*` (none of
these occur in-tree). Two guards keep the layering: `scripts/check-orm-backend-imports.sh` (run
by `commit.sh` and CI's `script-guards` job) fails if `src/orm/**`,
`src/db/default_connection.cppm`, `src/db/pool.cppm` or `src/storm_model.cppm` names
`storm_db_sqlite`/`sqlite::` (or the PostgreSQL equivalents) or imports `storm_defaults`/`storm`, and `tests/db/test_backend_layering.cpp` static-asserts that the
`Basic*` templates cannot be named without a backend.

### Step 5: `import storm;` Exports Only the Public Surface

`src/storm.cppm` splits its imports in two. Modules that make up the public surface (re-exported whole) are
`export import`ed: `storm_model`, `storm_defaults`, `storm_db_default_connection`,
`storm_orm_transaction`, `storm_db_concept`, `storm_db_sqlite`, `storm_db_postgresql`,
`storm_db_pool`, `storm_orm_where`, `storm_orm_fields`, `storm_orm_queryset`, `storm_orm_indexes`
and `storm_orm_schema`. The implementation modules — `storm_orm_generator`,
`storm_orm_utilities`, `storm_orm_statements_base` (and with it `storm_orm_model_concepts`),
`_insert`, `_upsert_grammar`, `_select`, `_projection`, `_join`, `_setop`, `_orderby` — are
plain `import`s: their declarations stay **reachable** (the `BasicInsertStatement` that
`QuerySet::insert()` returns is fully usable), but an importer of `storm` alone cannot **name**
them.

The few user-facing names declared inside those implementation modules are re-exported one by
one with a `using` declaration inside an `export namespace` block, in their original namespace,
so their spelling is unchanged (`Collate` is additionally re-exported into `storm::`):

| Name | Declared in | Why it is public |
|---|---|---|
| `storm::generator` | `storm_orm_generator` | return type of `rows()` |
| `storm::UUID` | `storm_orm_utilities` (alias of `orm::utilities::UUID`) | UUID field / primary-key type |
| `storm::orm::utilities::Collate` | `storm_orm_utilities` | `.collate(Collate::NoCase)`, `order_by<F, Collate::…>` |
| `storm::Collate` | `storm.cppm` (`using` of the same `storm_orm_utilities` enum) | `using namespace storm;` + bare `Collate::NoCase`, as the guide writes it (#679); pinned by `tests/query/test_collate_umbrella.cpp` |
| `storm::orm::statements::ReturnId` | `storm_orm_statements_insert` | `insert<ReturnId::Yes>` |
| `storm::orm::statements::InsertOptions` | `storm_orm_statements_insert` | `insert(span, InsertOptions{.batch_size = …})` |

Code that names an internal — a `JoinStatement`, a grammar (`UpsertGrammar`), a model-validation
concept (`ModelWithPrimaryKey`, `ModelStorageAnnotated`, `NumericAggregateable`,
`ValidForeignKey`), `BindableType`, `ConstexprString`, the canonical `orm::utilities::UUID`
spelling — imports that module explicitly (`import storm_orm_model_concepts;`,
`import storm_orm_utilities;`, …), the same way the tests already import individual statement
modules. A new public name in an implementation module needs its own `using` line in
`storm.cppm`; a new module whose entire surface is public is `export import`ed there instead.

## Performance Results

The template trick enables cross-module inlining without LTO:

| Approach | SELECT Efficiency | JOIN Efficiency |
|----------|-------------------|-----------------|
| Direct `sqlite3_*` calls | 96-97% | 100-106% |
| Non-template Statement methods | 92-94% | 98-100% |
| **Template Statement methods** | **95-96%** | **106%** |

The ~1% difference from direct calls is acceptable for full database abstraction.

## Rules for Database-Agnostic Modules

### DO:

1. **Make all hot-path methods templates** with `template <typename = void>`
2. **Use `__attribute__((always_inline))`** as a hint (templates enable it to work)
3. **Cache raw pointers** in Statement class to avoid `unique_ptr::get()` overhead
4. **Define constants** like `ROW_AVAILABLE` in Statement class
5. **Keep database headers** only in backend modules (sqlite.cppm, postgresql.cppm)
6. **Use Statement methods** instead of direct database API calls in ORM modules

### DON'T:

1. **Don't include database headers** in ORM modules (no `#include <sqlite3.h>`)
2. **Don't use database-specific types** in ORM module interfaces
3. **Don't rely on LTO** for inlining - it may not be available
4. **Don't make non-hot-path methods templates** - unnecessary complexity
5. **Don't default a template to a concrete backend** in an ORM module — add the alias to `storm_defaults` instead (enforced by `scripts/check-orm-backend-imports.sh`)

## Verification Checklist

Before committing database-agnostic changes:

- [ ] No `#include <sqlite3.h>` (or other DB headers) in ORM modules
- [ ] `scripts/check-orm-backend-imports.sh` passes (no `import storm_db_sqlite;` / `sqlite::` in ORM modules)
- [ ] All hot-path Statement methods are templates
- [ ] Run benchmarks: efficiency should be ≥95% vs raw database calls
- [ ] All tests pass (290 tests)
- [ ] Build succeeds without database headers in ORM modules

## Why This Works

1. **Template instantiation requires visible body**: The C++ standard requires template definitions to be available where they're instantiated
2. **Module interface includes template bodies**: When a class has template methods, those bodies are part of the module interface
3. **Compiler can inline**: With the body visible, the compiler can apply `always_inline` and optimize

## Limitations

1. **Compile time**: Template methods increase module interface size slightly
2. **Error messages**: Template errors can be harder to debug
3. **IDE support**: Some IDEs may not fully support C++26 modules + templates

## References

- [P1103R3: Merging Modules](https://wg21.link/p1103r3)
- [P2996: Reflection for C++26](https://wg21.link/p2996)
- Storm ORM implementation: `src/db/sqlite.cppm`, `src/orm/statements/select.cppm`
