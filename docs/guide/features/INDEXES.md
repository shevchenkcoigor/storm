# Indexes

Storm generates `CREATE INDEX` statements from model annotations — both single-column
tags on individual fields and composite (multi-column) indexes declared separately.

## Single-column indexes

Two tag annotations request a single-column index:

```cpp
struct Person {
    [[= primary]] int id;
    [[= unique]] std::string email;    // CREATE UNIQUE INDEX
    [[= indexed]] std::string name;    // CREATE INDEX
};
```

| Tag | Effect |
|---|---|
| `unique` | `CREATE UNIQUE INDEX IF NOT EXISTS idx_<field> ON <table>(<field>)` |
| `indexed` | `CREATE INDEX IF NOT EXISTS idx_<field> ON <table>(<field>)` |

Foreign-key fields (`[[= fk<>]]`) are indexed automatically — no `indexed`/`unique`
tag needed — because JOINs and cascade deletes both hit the FK column:

```cpp
struct Message {
    [[= primary]] int id;
    [[= fk<>]] Person sender;   // implicit CREATE INDEX idx_sender_id ON Message(sender_id)
};
```

The primary key is never separately indexed (SQLite's `INTEGER PRIMARY KEY` already is one).
`unique` and `indexed` are mutually exclusive with each other in the sense that `unique`
already implies indexing — combining both on the same field is redundant, not an error, but
prefer just `unique` when you need the uniqueness constraint.

## Composite (multi-column) indexes

A single-column tag can't express "unique on the *pair* of columns" — use `storm::Index<>` /
`uniqueIndex<>` for that, specifying the member list as `std::meta::info` NTTPs:

```cpp
struct Enrollment {
    [[= primary]] int id;
    [[= fk<>]] Student student;
    [[= fk<>]] Course  course;

    using storm_indexes = std::tuple<
        uniqueIndex<^^Enrollment::student, ^^Enrollment::course>
    >;
};
```

```sql
CREATE UNIQUE INDEX IF NOT EXISTS idx_Enrollment_student_id_course_id
    ON Enrollment(student_id, course_id)
```

`storm::Index<^^A::x, ^^A::y>` (non-unique) and `uniqueIndex<^^A::x, ^^A::y>` both
accept any number of member selectors. FK members in the list resolve to their `_id` column
name automatically (#422), same as everywhere else in Storm.

### Declaring composite indexes: the nested-typedef form

Declare the `storm_indexes` nested typedef **inside the model struct**, as shown above,
rather than specializing `storm::Indexes<T>` externally:

```cpp
// ❌ Don't specialize Indexes<T> in a header included by multiple module TUs
template <> struct storm::Indexes<Enrollment> {
    using type = std::tuple<uniqueIndex<^^Enrollment::student, ^^Enrollment::course>>;
};

// ✅ Nested typedef — safe everywhere
struct Enrollment {
    // ...
    using storm_indexes = std::tuple<uniqueIndex<^^Enrollment::student, ^^Enrollment::course>>;
};
```

An explicit specialization of a module-owned template (`storm::Indexes<T>`) in a header
included by several module TUs' Global Module Fragments breaks with "reference to 'type' is
ambiguous" once the import graph offers a second BMI path (issue #464). The nested-typedef
opt-in sidesteps this entirely — `storm::Indexes<T>` picks it up via a `requires { typename
T::storm_indexes; }` constraint — and is the only supported way to declare composite indexes.
A model with no `storm_indexes` typedef simply has no composite indexes (the default
`Indexes<T>::type` is an empty `std::tuple<>`).

## Where indexes are created

All index SQL — single-column and composite — is generated at compile time and executed as
part of `create_table()` / schema setup, alongside the table's `CREATE TABLE` statement. There
is no separate "create indexes" step to call.

See also:
- [reference/FIELD_TYPES.md](../reference/FIELD_TYPES.md) — the full annotation reference
- [REFERENTIAL_INTEGRITY.md](REFERENTIAL_INTEGRITY.md) — FK `ON DELETE` policies (a related but separate concern from FK indexing)
