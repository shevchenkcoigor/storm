# UPSERT Operations

Storm supports single-row `INSERT ... ON CONFLICT` through the insert proxy.
Choose an explicit unique conflict target, then select either `DO UPDATE` or
`DO NOTHING`. The target is checked at compile time: it must be a unique field,
exactly match a declared `UniqueIndex`, or exactly match a composite primary key
(#503, see below). A caller-supplied single-column key (`storm::UUID` or
`primary_assigned`) is part of the INSERT and is a valid target (#585); a
DB-generated integer key is omitted from inserts and cannot be one.

The chain consumes the insert proxy: `on_conflict()`, `update()` and `nothing()`
are callable on an rvalue only, so write it as one expression (as in every
example below). A proxy kept in a variable must be moved first —
`std::move(proxy).on_conflict<...>()`; calling it on the lvalue is a compile error.

## DO UPDATE

`update<Members...>()` overwrites the listed columns with the attempted
insert's values. It always returns the ID of the row that was inserted or
updated.

```cpp
auto result = qs.insert(user)
    .on_conflict<fields::User.email>()
    .update<fields::User.name, fields::User.age>()
    .execute();  // std::expected<int64_t, storm::db::Error>
```

```sql
INSERT INTO User (name, email, age) VALUES (?, ?, ?)
ON CONFLICT (email) DO UPDATE SET name=excluded.name, age=excluded.age
RETURNING id
```

Only the `update<...>()` members are overwritten. An `auto_update` timestamp is
also refreshed automatically. Foreign-key members use their database column
name (`owner_id`) in both the conflict target and update clause. Each SET column
is checked at compile time: it must be a non-static data member that is not the
primary key and not a relation container (m2m / reverse-FK members are not
columns, so they are rejected at the call site — #486).

## DO NOTHING

`nothing()` inserts a row when the target is new and skips it when it already
exists.

```cpp
auto result = qs.insert(user)
    .on_conflict<fields::User.email>()
    .nothing()
    .execute();  // std::expected<std::optional<int64_t>, storm::db::Error>
```

```sql
INSERT INTO User (name, email, age) VALUES (?, ?, ?)
ON CONFLICT (email) DO NOTHING
RETURNING id
```

| Operation | Result type | Conflict result |
| --- | --- | --- |
| `.update<...>()` | `std::expected<int64_t, Error>` | Existing row is updated; its ID is returned. |
| `.nothing()` | `std::expected<std::optional<int64_t>, Error>` | No row is changed; the value is `std::nullopt`. |

`DO NOTHING` is useful for get-or-create flows, but it does **not** return the
existing row's ID when a conflict is skipped. Use `DO UPDATE` when the ID is
needed on every call.

Bulk insert spans are not supported by the upsert chain.

## Composite primary key targets (#503)

On a model with a composite primary key ([`primary_part`](../reference/FIELD_TYPES.md)),
the conflict target may be the **full** key column set, in declaration order — a
partial (strict-subset) target is a compile-time error, since one column of a
composite key is not unique on its own. A composite key is never DB-generated
(#502), so there is nothing to `RETURNING`:

```cpp
auto result = qs.insert(order_item)
    .on_conflict<fields::OrderItem.order_id, fields::OrderItem.product_id>()
    .update<fields::OrderItem.quantity>()
    .execute();  // std::expected<void, storm::db::Error> — no RETURNING
```

| Operation | Single-column PK / unique target | Composite-PK target |
| --- | --- | --- |
| `.update<...>()` | `std::expected<int64_t, Error>` | `std::expected<void, Error>` |
| `.nothing()` | `std::expected<std::optional<int64_t>, Error>` | `std::expected<void, Error>` |

For a composite target, `.nothing()` cannot report whether the row was inserted
or the conflict was skipped — that signal is unavailable, not silently wrong.
