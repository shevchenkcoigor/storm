# CRUD Operations

Storm ORM provides full CRUD (Create, Read, Update, Delete) operations with auto-generated IDs and optimized performance.

## Overview

All CRUD operations use:
- **Compile-time SQL generation** - Zero runtime SQL construction
- **Statement caching** - Reusable prepared statements
- **Type-safe binding** - Compile-time type dispatch
- **Auto-generated IDs** - Returned from INSERT operations

## INSERT Operations

### Return Types (Breaking Change)

Storm ORM returns auto-generated IDs from all INSERT operations:
- **Single INSERT**: `std::expected<int64_t, Error>` (returns the generated ID)
- **Batch INSERT**: `std::expected<std::vector<int64_t>, Error>` (returns all generated IDs)

### Table Requirements

Tables use a plain integer primary key, which SQLite auto-assigns on insert (it
aliases the rowid). Since #379 Storm emits plain `INTEGER PRIMARY KEY` by default —
`AUTOINCREMENT` (the never-reuse guarantee, ~358 ns/insert) is opt-in via
`primary_autoincrement`.

```cpp
conn.execute("CREATE TABLE Person ("
    "id INTEGER PRIMARY KEY, "
    "name TEXT NOT NULL, "
    "age INTEGER NOT NULL)");
```

To opt into the never-reuse guarantee for a specific model, annotate its PK with
`primary_autoincrement` instead of `primary`:

```cpp
struct Audit {
    [[= primary_autoincrement]] int id{};  // id INTEGER PRIMARY KEY AUTOINCREMENT
    // ...
};
```

### Single INSERT

```cpp
struct Person {
    [[= primary]] int id;
    std::string name;
    int age;
    double salary;
    bool is_active;
    std::string department;
};

storm::QuerySet<Person> queryset(conn);

// Insert returns the generated ID
auto result = queryset.insert(Person{0, "Alice", 25}).execute();
if (result) {
    int64_t id = result.value();
    std::cout << "Inserted person with ID: " << id << std::endl;
}
```

### Batch INSERT

```cpp
std::vector<Person> people = {
    {0, "Alice", 25},
    {0, "Bob", 30},
    {0, "Charlie", 35}
};

// Returns all generated IDs
auto result = queryset.insert(std::span<const Person>(people));
if (result) {
    const auto& ids = result.value();
    for (size_t i = 0; i < ids.size(); ++i) {
        std::cout << people[i].name << " ID: " << ids[i] << std::endl;
    }
}
```

### Batch INSERT Strategy

Storm ORM uses smart thresholds based on SQLite's variable limit (999):

1. **≤50 objects**: Bulk INSERT with multiple VALUES
   ```sql
   INSERT INTO Person (name, age) VALUES (?, ?), (?, ?), ...
   ```

2. **>50 objects**: Individual statements within a transaction
   ```sql
   BEGIN TRANSACTION;
   INSERT INTO Person (name, age) VALUES (?, ?);
   INSERT INTO Person (name, age) VALUES (?, ?);
   ...
   COMMIT;
   ```

### Thread-Local SQL Caching

Batch INSERT uses an 8-entry thread-local cache for SQL strings:

```cpp
struct BulkSQLCache {
    static constexpr size_t CACHE_SIZE = 8;
    std::array<CacheEntry, CACHE_SIZE> entries;
    size_t next_slot = 0; // Round-robin replacement
};
thread_local BulkSQLCache bulk_sql_cache;
```

## UPDATE Operations

### Single UPDATE

Updates all non-primary-key fields:

```cpp
Person person{1, "Alice", 26};  // ID = 1, new age
auto result = queryset.update(person).execute();
if (!result) {
    std::cerr << "Update failed: " << result.error().message() << std::endl;
}
```

**SQL Generated**:
```sql
UPDATE Person SET name=?, age=? WHERE id=?
```

### Batch UPDATE

```cpp
std::vector<Person> people = {
    {1, "Alice", 26},
    {2, "Bob", 31},
    {3, "Charlie", 36}
};

auto result = queryset.update(std::span<const Person>(people));
```

**Strategy**: Individual UPDATE statements within a transaction for consistency

### Conditional UPDATE (#403)

Update every row matching a `where()` filter in a single statement — no need to
SELECT, mutate, and UPDATE row-by-row. Chain `update<Members...>(proto)` onto a
filtered QuerySet. The `Members...` are field selectors (`fields::T.field`) chosen at
compile time; their new values are read from the `proto` object:

```cpp

auto result = QuerySet<Person>()
    .where(fields::Person.salary < 50000)
    .update<fields::Person.salary, fields::Person.is_active>(Person{.salary = 60000, .is_active = true})
    .execute();          // std::expected<void, Error>
```

**SQL Generated**:
```sql
UPDATE Person SET salary=?, is_active=? WHERE salary<?
```

The SET column list is compile-time; the WHERE body reuses the full expression
system, so every operator works (`==`, `!=`, `<`, `<=`, `>`, `>=`, `IN`, `BETWEEN`,
`LIKE`, `IS NULL`, `AND`/`OR`, nested groups). Chained `where()` calls are
AND-combined. Bind order is SET parameters first, then WHERE.

**FK columns** are written as `<name>_id`:

```cpp
QuerySet<Message>()
    .where(fields::Message.id == 1)
    .update<fields::Message.sender>(Message{.sender = Person{.id = 7}})
    .execute();          // UPDATE Message SET sender_id=? WHERE id=?
```

**`auto_update` timestamps are refreshed automatically.** If the model has an
`[[= storm::auto_update]]` `time_point` field, it is appended to the SET clause
(stamped `now()`) even when not listed — matching single-row UPDATE semantics:

```cpp
QuerySet<TimestampedRecord>()
    .where(fields::TimestampedRecord.id == 1)
    .update<fields::TimestampedRecord.name>(TimestampedRecord{.name = "renamed"})
    .execute();          // UPDATE ... SET name=?, updated_at=? WHERE id=?
```

A **primary-key** member may not carry `auto_create` or `auto_update` (#511) — an
auto-stamped key would be rewritten by the very statement matching on it. This is a
compile-time error (`ModelTimestampPkValid`), and it covers composite `primary_part`
members as well as a single `primary`:

```cpp
struct Bad {
    [[= primary]] [[= storm::auto_update]]      // ERROR: constraint not satisfied
    std::chrono::system_clock::time_point id{};
};
```

**Safety — empty WHERE is refused.** Calling `update<...>()` with **no** `where()`
filter would write the whole table, so it is rejected at `execute()`/`to_sql()` time
with `std::unexpected(Error)`:

```cpp
QuerySet<Person>().update<fields::Person.age>(Person{.age = 0}).execute();
// → std::unexpected: refuses full-table write
```

To intentionally update every row, use the explicit `update_all<...>()` (the symmetric
counterpart of `erase_all()`):

```cpp
QuerySet<Person>().update_all<fields::Person.department>(Person{.department = "Global"}).execute();
// UPDATE Person SET department=?   (no WHERE — explicit full-table write)
```

`update_all<...>()` shares the SET-clause behaviour of conditional `update<...>()`: FK
columns emit `<name>_id` and `auto_update` fields are auto-appended and stamped `now()`.

Returns `std::expected<void, Error>` (consistent with the rest of the CRUD family).
The primary key cannot be a SET target (compile-time rejected).

### Statement Caching

UpdateStatement reuses prepared statements through the single Connection-level
cache (`prepare_cached`): identical UPDATEs reuse a compiled statement keyed by
SQL text. There is no per-Statement handle cache — that layer was removed in
#214.

See [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) for details.

## DELETE Operations

### Single DELETE

```cpp
Person person{1, "Alice", 25};  // Only ID matters
auto result = queryset.erase(person).execute();
```

**SQL Generated**:
```sql
DELETE FROM Person WHERE id=?
```

### Batch DELETE

```cpp
std::vector<Person> people = {
    {1, "Alice", 25},
    {2, "Bob", 30},
    {3, "Charlie", 35}
};

auto result = queryset.erase(std::span<const Person>(people));
```

**SQL Generated** (bulk IN clause):
```sql
DELETE FROM Person WHERE id IN (?, ?, ?)
```

### Conditional DELETE (#198)

Delete every row matching a `where()` filter in a single statement — no need to
SELECT and loop. Chain `erase()` (no argument) onto a filtered QuerySet:

```cpp

auto result = QuerySet<Person>()
    .where(fields::Person.age > 30)
    .erase()
    .execute();          // std::expected<void, Error>
```

**SQL Generated**:
```sql
DELETE FROM Person WHERE age>?
```

The full WHERE expression system is reused, so every operator works
(`==`, `!=`, `<`, `<=`, `>`, `>=`, `IN`, `BETWEEN`, `LIKE`, `IS NULL`,
`AND`/`OR`, nested groups). Chained `where()` calls are AND-combined:

```cpp
QuerySet<Person>()
    .where(fields::Person.department == "Legacy")
    .where(fields::Person.is_active == false)
    .erase()
    .execute();          // DELETE FROM Person WHERE (department=? AND is_active=?)
```

**Safety — empty WHERE is refused.** Calling `erase()` with **no** `where()`
filter would emit `DELETE FROM Person` and wipe the whole table, so it is
rejected at `execute()`/`to_sql()` time with `std::unexpected(Error)`:

```cpp
QuerySet<Person>().erase().execute();   // → std::unexpected: refuses full-table wipe
```

To intentionally delete every row, use the explicit `erase_all()`:

```cpp
QuerySet<Person>().erase_all().execute();   // DELETE FROM Person (explicit full wipe)
```

> **See also:** conditional bulk **UPDATE** (`where(cond).update<Members...>(proto)`)
> shipped in #403 — see [Conditional UPDATE](#conditional-update-403) above.

## SQL Inspection — `.sql()` vs `.to_sql()` (#197)

Every query builder exposes two inspection methods with a consistent split:

| Method | Returns | Content |
|---|---|---|
| `.sql()` | `std::string` | raw SQL **template** (`?` placeholders), no binding |
| `.to_sql()` | `std::expected<std::string, Error>` | SQL with bound parameter values **inlined** |

Both are **debug / inspection aids only** — execution always binds `?` parameters and
never uses these strings. As of #197 the split is uniform across all proxies: SELECT,
INSERT/upsert, UPDATE, DELETE, set operations, **DISTINCT / VALUES**, and the
**aggregates** (`count()`, `sum()`, `avg()`, `min()`, `max()`, with or without
`group_by()` / `having()`).

```cpp
qs.distinct<fields::Person.name>().sql();                  // std::string (template)
qs.distinct<fields::Person.name>().to_sql();               // expected<string, Error> (bound)
qs.where(fields::Person.age > 30).count().to_sql();   // "SELECT COUNT(*) … WHERE age > 30"
```

### `to_sql()` backend behavior (#411)

Because `.to_sql()` is produced by a different mechanism per backend, the rendered text
is **not byte-identical across SQLite and PostgreSQL**:

- **SQLite** uses the engine-native `sqlite3_expanded_sql()`.
- **PostgreSQL** hand-rolls `?`-placeholder substitution, storing every bound value as
  text and wrapping it in single quotes.

| Operand | SQLite | PostgreSQL | Same? |
|---|---|---|---|
| int / int64 | `30` (bare) | `'30'` (quoted) | value yes, quoting no |
| bool | `1` / `0` (bare) | `'1'` / `'0'` (quoted) | value yes, quoting no |
| double | engine float→text (bare) | `'%.17g'` (quoted) | value yes, formatting/quoting no |
| NULL (empty `std::optional`) | `NULL` | `NULL` | ✅ identical |
| embedded quote `O'Brien` | `'O''Brien'` | `'O''Brien'` | ✅ identical |
| literal `?` inside text | preserved | preserved | ✅ identical |
| BLOB (`std::vector<uint8_t>`) | `x'4849'` hex literal | raw bytes inside `'…'` | ❌ different encoding |

**Takeaways.** NULL handling, single-quote escaping, and literal `?` inside string
literals are identical across backends. Scalar operands carry the same value but PG
quotes them. BLOBs differ most: SQLite renders a hex literal, PG renders the raw bytes
inside a quoted string. Treat `to_sql()` output as a per-backend debugging view, not a
portable SQL artifact. This behavior is pinned by the cross-backend tests in
`tests/schema/test_sql_inspection.cpp` (see #411).

## Error Handling

All operations return `std::expected<T, Error>`:

```cpp
auto result = queryset.insert(person).execute();
if (result) {
    // Success
    int64_t id = result.value();
} else {
    // Error
    std::cerr << "Error: " << result.error().message() << std::endl;
    std::cerr << "Code: " << result.error().code() << std::endl;
}
```

## Supported Field Types

Storm ORM supports all standard SQLite types through compile-time type dispatch:

### Integer Types
- `int`, `short` → `bind_int()`
- `unsigned int`, `int64_t`, `long`, `long long`, `uint64_t` → `bind_int64()` (`unsigned int` widened in #729)

### Floating Point Types
- `double` → `bind_double()`
- `float` → `bind_double()` (cast)

### Boolean Type
- `bool` → Stored as INTEGER (0/1)

### String Types
- `std::string`, `const char*`, `std::string_view` → `bind_text()`

### Optional Types (NULL Support)
- `std::optional<T>` for any supported type T
- Binds NULL when `std::nullopt`, otherwise binds the contained value
- Examples: `std::optional<int>`, `std::optional<std::string>`

### BLOB Types
- `std::vector<uint8_t>`, `std::vector<unsigned char>` → `bind_blob()`

### Type Dispatch

The binding uses compile-time `if constexpr` for zero runtime overhead:

```cpp
template <typename ConnType, typename Statement, typename FieldType>
static auto bind_value_by_type(Statement& stmt, int idx, const FieldType& value) {
    if constexpr (std::is_same_v<FieldType, int>) {
        return stmt.bind_int(idx, value);
    } else if constexpr (std::is_same_v<FieldType, int64_t>) {
        return stmt.bind_int64(idx, value);
    } else if constexpr (std::is_same_v<FieldType, std::string>) {
        return stmt.bind_text(idx, value);
    }
    // ... more type checks
}
```

## Foreign Key Support

### INSERT with FK Fields

```cpp
struct Message {
    [[=primary]] int id;
    std::string content;
    User sender;  // FK field - entire object
};

Message msg{0, "Hello", User{1, "Alice", 10}};
auto result = message_qs.insert(msg).execute();
```

**Behavior**:
- Only the FK's primary key is bound (sender.id)
- Other FK fields are ignored during INSERT
- Returned ID is for the Message, not the FK

### UPDATE with FK Fields

```cpp
msg.content = "Updated content";
msg.sender.id = 2;  // Change FK reference
auto result = message_qs.update(msg).execute();
```

**SQL Generated**:
```sql
UPDATE Message SET content=?, sender_id=? WHERE id=?
```

### DELETE with FK Fields

FK fields are ignored during DELETE - only the primary key matters:

```cpp
auto result = message_qs.erase(msg);  // Only msg.id is used
```

## Transactions

To run several CRUD operations atomically, wrap them in a `storm::begin()` scope
(#415) — a RAII guard that issues `BEGIN`, commits on `txn->commit()`, and
auto-`ROLLBACK`s on any early return, exception, or scope exit without a commit.

```cpp
auto conn = DefaultConnection<ConnType>::get();

auto txn = storm::begin(conn);
if (!txn) return std::unexpected(txn.error());

storm::QuerySet<Person, ConnType> qs;
if (auto r = qs.insert(alice).execute(); !r) return std::unexpected(r.error());
if (auto r = qs.insert(bob).execute();   !r) return std::unexpected(r.error());

return txn->commit();   // both inserts commit together; any failure rolls back
```

For a more compact style, `storm::transaction(conn, body)` wraps the same guard:
the body returns `std::expected<T, Error>`, a value COMMITs (and is forwarded), a
`std::unexpected` or thrown exception ROLLBACKs and propagates.

```cpp
auto r = storm::transaction(conn, [&](auto& txn) -> std::expected<void, Error> {
    if (auto x = qs.insert(alice).execute(); !x) return std::unexpected(x.error());
    if (auto x = qs.insert(bob).execute();   !x) return std::unexpected(x.error());
    return {};
});   // commit/rollback handled automatically
```

`storm::TransactionGuard<ConnType>` is the underlying type (re-exported from
`storm`); `storm::begin(conn)` is the thin factory. **Do not** use raw
`conn->execute("BEGIN TRANSACTION")` — batch ops issue their own inner
transaction for chunked writes, and a raw outer BEGIN collides with it. The guard
handles this: a `begin()` on an already-open connection returns a *passive* guard
(no nested BEGIN), so nested batch ops cooperate with the outer scope (fixes #9).

## Performance Optimization Tips

1. **Use batch operations** for multiple inserts/updates/deletes
2. **Reuse QuerySet instances** to benefit from statement caching
3. **Pre-allocate vectors** for batch operations
4. **Use the public transaction API** (`storm::begin(conn)` → `txn->commit()`) for
   complex multi-operation workflows — see [Transactions](#transactions) below
5. **Avoid unnecessary copies** - use const references and std::span

## Testing

All CRUD operations are thoroughly tested:
- Empty table scenarios
- Single object operations
- Batch operations (various sizes)
- Error conditions
- ID validation
- FK field handling

See `tests/crud/test_crud.cpp` for comprehensive test coverage.

## See Also

- [SELECT Queries](SELECT_QUERIES.md) - Read operations with caching
- [Batch Operations](BATCH_OPERATIONS.md) - Detailed batch operation strategies
- [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) - Caching architecture details
- [SQL Generation](../../internals/architecture/SQL_GENERATION.md) - Compile-time SQL generation
