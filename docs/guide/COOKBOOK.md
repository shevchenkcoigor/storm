# Storm ORM Cookbook

Quick-reference patterns for common Storm ORM operations.

## Setup

```cpp
import storm;
using namespace storm;

// Define a model — reflection maps fields to columns automatically
struct Person {
    [[= primary]] int id{};
    std::string name;
    int age;
    double salary;
    bool is_active;
    std::optional<int> score;
};

// Connect (thread-local, one per thread)
DefaultConnection<>::set(":memory:");
QuerySet<Person> qs;
```

## INSERT

```cpp
// Single insert (id auto-generated)
Person p{.name = "Alice", .age = 30, .salary = 75000.0, .is_active = true};
qs.insert(p).execute();

// Batch insert
plf::hive<Person> people;
people.insert({.name = "Bob", .age = 25, .salary = 60000.0, .is_active = true});
people.insert({.name = "Carol", .age = 35, .salary = 80000.0, .is_active = false});
qs.insert(people).execute();
```

## SELECT

```cpp
// Select all
auto all = qs.select().execute();

// With WHERE
auto seniors = qs.where(fields::Person.age > 30).select().execute();

// With ORDER BY + LIMIT + OFFSET
auto page = qs
    .where(fields::Person.is_active == true)
    .order_by<fields::Person.name>()
    .limit(10)
    .offset(20)
    .select()
    .execute();
```

## WHERE Clauses

```cpp
// Comparison operators
qs.where(fields::Person.age == 30).select().execute();
qs.where(fields::Person.age != 30).select().execute();
qs.where(fields::Person.age > 30).select().execute();
qs.where(fields::Person.age >= 30).select().execute();
qs.where(fields::Person.age < 30).select().execute();
qs.where(fields::Person.age <= 30).select().execute();

// LIKE
qs.where(fields::Person.name.like("A%")).select().execute();

// IN
qs.where(fields::Person.age.in(25, 30, 35)).select().execute();

// BETWEEN
qs.where(fields::Person.age.between(25, 35)).select().execute();

// AND / OR
qs.where(fields::Person.age > 30 && fields::Person.is_active == true).select().execute();
qs.where(fields::Person.age < 25 || fields::Person.age > 40).select().execute();
```

## UPDATE

```cpp
// Single update
Person p = /* ... fetched from select ... */;
p.salary = 85000.0;
qs.update(p).execute();

// Batch update
auto people = qs.select().execute();
for (auto& p : people) { p.salary *= 1.1; }
qs.update(people).execute();
```

## Upsert

```cpp
// INSERT ... ON CONFLICT (name) DO UPDATE — conflict target must be a
// unique field / UniqueIndex (compile-time checked).
qs.insert(p).on_conflict<fields::Person.name>().update<fields::Person.age>().execute();

// INSERT ... ON CONFLICT (name) DO NOTHING
qs.insert(p).on_conflict<fields::Person.name>().nothing().execute();
```

See [reference/FIELD_TYPES.md](reference/FIELD_TYPES.md) and the QuerySet API section of
[CLAUDE.md](../../CLAUDE.md) for details (#205).

## Automatic Timestamps

```cpp
struct Article {
    [[=primary]] int id;
    std::string title;
    [[=storm::auto_create]] std::chrono::system_clock::time_point created_at;
    [[=storm::auto_update]] std::chrono::system_clock::time_point updated_at;
};

QuerySet<Article> qs;

// INSERT — created_at and updated_at are stamped automatically (any value you set is ignored).
qs.insert(Article{.title = "Hello"}).execute();

// The object in memory is NOT mutated — re-SELECT to read the stamped values.
auto saved = *qs.select().execute().value().begin();

// UPDATE — updated_at is re-stamped; created_at is preserved by passing the saved value back.
saved.title = "Hello (edited)";
qs.update(saved).execute();
```

`auto_create` stamps on INSERT only; `auto_update` stamps on INSERT and UPDATE. Both
fields must be `std::chrono::system_clock::time_point`. See
[reference/FIELD_TYPES.md](reference/FIELD_TYPES.md).

## DELETE

```cpp
// Single erase
qs.erase(p).execute();

// Conditional bulk erase — one DELETE ... WHERE statement (#198)
qs.where(fields::Person.is_active == false).erase().execute();

// Erase all (explicit full-table wipe). erase() with no where() is refused.
qs.erase_all().execute();
```

## Aggregates

```cpp
// Scalar aggregates (no GROUP BY). execute() returns std::expected<T, Error>.
// COUNT/SUM are never NULL; MIN/MAX/AVG over an empty set are NULL -> nullopt (#416).
auto count     = qs.count().execute();               // expected<int64_t>
auto total     = qs.sum<fields::Person.salary>().execute(); // expected<double>  (SUM over double/float, #420; 0.0 on empty)
auto age_total = qs.sum<fields::Person.age>().execute();    // expected<int64_t> (SUM over integer; 0 on empty)
auto avg       = qs.avg<fields::Person.salary>().execute(); // expected<std::optional<double>>
auto lo        = qs.min<fields::Person.age>().execute();    // expected<std::optional<double>>
auto hi        = qs.max<fields::Person.age>().execute();    // expected<std::optional<double>>

// MIN/MAX/AVG: nullopt means "no rows" — distinguishable from a genuine 0.
if (lo && lo->has_value()) {
    use(lo->value());        // a real minimum exists
} else if (lo) {
    // empty / all-filtered-out set: no minimum
}

// With WHERE
auto active_count = qs.where(fields::Person.is_active == true).count().execute();

// COUNT(DISTINCT field) — counts distinct values, not distinct rows
auto distinct_depts = qs.count_distinct<fields::Person.department>().execute(); // expected<int64_t>
auto active_distinct = qs.where(fields::Person.is_active == true)
    .count_distinct<fields::Person.department>()
    .execute();
```

## GROUP BY + HAVING

```cpp
// Group and count
auto groups = qs.group_by<fields::Person.is_active>().count().execute();

// GROUP BY with HAVING
auto large_groups = qs
    .group_by<fields::Person.age>()
    .having(fields::Person.age > 30)
    .count()
    .execute();
```

## DISTINCT

```cpp
// Single field
auto names = qs.distinct<fields::Person.name>().execute();

// Multi-field
auto combos = qs.distinct<fields::Person.name, fields::Person.age>().execute();
```

## Column Projection (VALUES)

```cpp
// Single column → hive<T>
auto names = qs.values<fields::Person.name>().execute();  // plf::hive<std::string>

// Multiple columns → hive<tuple<...>>
auto pairs = qs.values<fields::Person.name, fields::Person.age>().execute();
// plf::hive<std::tuple<std::string, int>>
```

## JOIN

```cpp
struct Message {
    [[= primary]] int id{};
    std::string content;
    [[= fk<>]] Person sender;
};

QuerySet<Message> msg_qs;
auto results = msg_qs.join<fields::Message.sender>().where(...).select().execute();
```

## Set Operations

```cpp
QuerySet<Person> qs1, qs2;

// UNION (deduplicated)
auto combined = qs1.where(fields::Person.age > 30)
    .union_(qs2.where(fields::Person.is_active == true))
    .select()
    .execute();

// UNION ALL (keeps duplicates)
auto all = qs1.where(...).union_all(qs2.where(...)).select().execute();

// INTERSECT / EXCEPT
auto common = qs1.where(...).intersect_(qs2.where(...)).select().execute();
auto diff   = qs1.where(...).except_(qs2.where(...)).select().execute();
```

## Transactions

```cpp
auto conn = DefaultConnection<>::get();

// RAII guard — explicit commit; rollback on early return/throw/scope exit.
auto txn = storm::begin(conn);
if (!txn) return std::unexpected(txn.error());
if (auto r = qs.insert(a).execute(); !r) return std::unexpected(r.error());
if (auto r = qs.update(b).execute(); !r) return std::unexpected(r.error());
return txn->commit();

// Scope helper — commits on a returned value, rolls back on std::unexpected/throw.
auto r = storm::transaction(conn, [&](auto& txn) -> std::expected<int, Error> {
    if (auto x = qs.insert(a).execute(); !x) return std::unexpected(x.error());
    return 42;
});
```

Nesting is cooperative: `begin()` on an already-open connection returns a passive guard, so
batch operations that open their own inner transaction never collide with an outer one (#415).
Never issue a raw `conn->execute("BEGIN TRANSACTION")`.

## Connection Tuning & Pooling

SQLite connections take a `busy_timeout_ms` and `journal_mode` (WAL opt-in), applied once at
`open()` — see [features/CONNECTION_TUNING.md](features/CONNECTION_TUNING.md) for pooled setups
and WAL recommendations (#410).

## Thread Safety

```cpp
// Each thread gets its own connection — safe
void worker() {
    DefaultConnection<>::set(":memory:");
    QuerySet<Person> qs;
    qs.where(fields::Person.age > 30).select().execute();
}

std::jthread t1(worker), t2(worker);

// Check before use — DefaultConnection::has() avoids the abort in
// DefaultConnection::get() when a thread might not have set one yet.
if (!DefaultConnection<>::has()) {
    DefaultConnection<>::set(":memory:");
}
```
