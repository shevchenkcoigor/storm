# Field Selectors — `fields::Model.field`

How you name a column in a query. For what *types* a column may be, see
[FIELD_TYPES.md](FIELD_TYPES.md).

Storm has exactly one selector spelling:

```cpp
qs.where(fields::Person.age > 30)
  .order_by<fields::Person.name>()
  .select();
```

The model is named once per field. No wrapper, no `^^`, no macro.

---

## Declaring the proxies

Two mechanical lines per model, placed **after** the model struct is complete:

```cpp
struct Person {
    [[= primary]] int id{};
    std::string              name;
    int                      age{};
};

namespace fields {

struct PersonT;
consteval { std::meta::define_aggregate(^^PersonT, storm::field_specs_for(^^Person)); }
inline constexpr PersonT Person{};

} // namespace fields
```

The declaration names **no fields**, so it cannot drift: add or remove a member
and the proxy follows automatically on the next build.

Requirements:

- the header needs `#include <meta>` — `import std;` does not export `std::meta::`;
- it must come **after** `import storm;`, because `field_specs_for` is a
  function, a harder dependency than the `[[= storm::*]]` annotations;
- put the block at **namespace scope**. A `namespace fields` nested inside an
  anonymous namespace makes every unqualified `fields::` in that translation
  unit ambiguous against the global one.

For a model inside a namespace, declare the proxies in the same namespace and
spell them fully:

```cpp
namespace bench { struct Run { /* … */ }; namespace fields { /* … */ } }

qs.order_by<bench::fields::Run.id>()
```

---

## Where selectors are used

Every public position takes the same spelling:

```cpp
qs.where(fields::Person.age > 30 && fields::Person.department == "Eng")
  .order_by<fields::Person.name, false>()      // false = DESC
  .distinct<fields::Person.department>()
  .values<fields::Person.name, fields::Person.age>()
  .group_by<fields::Person.department>()
  .sum<fields::Person.salary>()
  .join<fields::Message.sender>()
  .update<fields::Person.salary>(proto)
  .on_conflict<fields::Person.name>()
```

In a `where()` clause a selector supports the full comparison surface:
`==`, `!=`, `<`, `<=`, `>`, `>=`, plus `.in(...)`, `.between(a, b)`, `.like(...)`,
`.is_null()`, `.is_not_null()` and `.collate(...)`.

---

## Columns and relations are different types

`field_specs_for` emits one of two proxies per member:

| Member | Proxy | `where()` | `join()` |
|---|---|---|---|
| a persisted column (incl. FK) | `FieldRef` | ✅ | FK only |
| `many_to_many` / `reverse_fk` container | `RelationRef` | ❌ | ✅ |

A relation is a collection, not a scalar, so comparing it to a value is
meaningless and rejected at compile time:

```cpp
qs.join<fields::Article.tags>()      // ✅ eager-load the relation
qs.where(fields::Article.tags == 5)  // ❌ compile error
```

```
error: overload resolution selected deleted operator '==': this member is a
relation (many_to_many / reverse_fk), not a column: it cannot be compared in
where(). Use join<fields::Model.relation>() to eager-load it.
```

Filtering *through* a relation ("articles having a tag named X") is a separate
capability Storm does not have yet — tracked as
[#553](https://github.com/spiritEcosse/storm/issues/553).

Note that an **FK member is a column**, so `fields::Message.sender` is a
`FieldRef` and is joinable. Its column in SQL is `sender_id`, derived from the
member name — you always write the member name, in every clause:

```cpp
qs.order_by<fields::Message.sender>()          // ORDER BY sender_id
qs.count_distinct<fields::Message.sender>()    // COUNT(DISTINCT sender_id)
qs.where(fields::Message.sender == 1)          // WHERE sender_id = ?
```

`SELECT`, `INSERT`/`UPDATE`, `JOIN`, DDL, `ORDER BY`, `COUNT(DISTINCT)` and
`WHERE` route the member through the one canonical column-name writer
(`meta::append_column_name`, [#422]), so the `_id` suffix cannot be derived in
one clause and forgotten in another. `ORDER BY` and `COUNT(DISTINCT)` did
forget it until [#570], and `WHERE` (every comparison operator, `like()`,
`between()`, `is_null()`/`is_not_null()`, and the `collate()` path) did until
[#575] — all failed at runtime with `no such column: sender`.

**`in()` works on a single-column FK member** ([#610]) —
`fields::Message.sender.in(1, 2, 3)` constructs each value against the FK
**target's primary-key type** (not the related model type) and emits
`sender_id IN (?, ?, ?)`. Comparing a field to a non-bindable operand (e.g.
`fields::Message.sender == some_person_object`, passing a model struct
instead of its key) is likewise a clean compile-time rejection rather than a
hard error inside SQL generation.

**One limitation.** If the FK's target has a *composite* primary key, the
member has no single column — it spreads over `<member>_<part>` columns
(e.g. `line_order_id`, `line_product_id`). `ORDER BY`, `COUNT(DISTINCT)`,
`count()`, `distinct()`, `values()`, `group_by()` and `WHERE` (including `in()`) all reject
such a member at **compile time**, since none has a correct multi-column
form: `ORDER BY`'s `ASC`/`DESC` would bind to the last part only,
`COUNT(DISTINCT a, b)` is a syntax error in SQLite while PostgreSQL reads it
as a row constructor, plain `COUNT`/`DISTINCT`/column-projection would name a
`<member>_id` column that doesn't exist, `in()` has no single key type to
construct each value against, and `WHERE`'s row-value syntax `(a, b) = (?,
?)` (used by [#501]'s composite-PK UPDATE/DELETE) would only ever be
well-defined for `==`/`!=`, not for `>`, `LIKE`, `BETWEEN`, etc. — so `WHERE`
stays rejected uniformly rather than accepting equality alone. Order, count,
or filter by the target's parts explicitly instead. `join<>` on such an FK is
fully supported ([#504]).

`WHERE`'s rejection uses the same `= delete("...")` pattern as the relation
case above ([#613]), so a bare comparison names the reason instead of a bare
"invalid operands to binary expression":

```cpp
qs.where(fields::Shipment.line == 1)  // ❌ compile error — Shipment.line targets a composite-PK model
```

```
error: overload resolution selected deleted operator '==': this member is a
foreign key to a composite-PK target: it spreads over multiple
"<member>_<part>" columns, so there is no single column to compare in
where().
```

`count()`, `distinct()` and `values<>()` do **not** yet reject a composite-PK
FK member the way `ORDER BY`/`COUNT(DISTINCT)`/`WHERE` do — tracked as
[#611]. Avoid naming such a member in those three until it lands.

[#422]: https://github.com/spiritEcosse/storm/issues/422
[#501]: https://github.com/spiritEcosse/storm/issues/501
[#504]: https://github.com/spiritEcosse/storm/issues/504
[#570]: https://github.com/spiritEcosse/storm/issues/570
[#575]: https://github.com/spiritEcosse/storm/issues/575
[#610]: https://github.com/spiritEcosse/storm/issues/610
[#611]: https://github.com/spiritEcosse/storm/issues/611
[#613]: https://github.com/spiritEcosse/storm/issues/613

---

## Two caveats when destructuring

A proxy object is destructurable, which is occasionally convenient in a narrow
scope:

```cpp
const auto& [id, name, age] = fields::Person;
qs.where(age > 30).select();
```

**It is arity-checked, loudly.** Get the count wrong and the compiler says so:

```
error: type 'const PersonT' decomposes into 3 elements, but only 2 names were provided
```

**It is positional, and that is the dangerous part.** Reordering the model's
fields silently rebinds the names — right arity, wrong fields, wrong query, **no
compile error**. `fields::Person.age` is the primary form for exactly this
reason; treat destructuring as opt-in for short scopes.

A structured binding also cannot be `constexpr`. Bind with `const auto&` to the
`constexpr` proxy object, as above.

---

## What still uses `^^`

Reflection splices remain where you are *declaring* something rather than
querying:

```cpp
struct Person {
    // …
    using storm_indexes = std::tuple<storm::Index<^^Person::department, ^^Person::age>>;
};

struct RfPerson {
    [[= storm::reverse_fk<^^RfTask>]] std::vector<RfTask> tasks;  // names a TYPE
};
```

The rule: **queries use `fields::`, model declarations use `^^`.**

---

## Migrating from the old spellings

Both previous forms were removed in
[#518](https://github.com/spiritEcosse/storm/issues/518):

| Before | Now |
|---|---|
| `f<^^Person::age>() > 30` | `fields::Person.age > 30` |
| `order_by<^^Person::name>()` | `order_by<fields::Person.name>()` |
| `join<^^Message::sender>()` | `join<fields::Message.sender>()` |

Both are compile errors today; there is no deprecation period and no dual
acceptance. Add the two-line declaration block per model, then rewrite call
sites — the transform is mechanical.

---

## How it works

`FieldRef<M>` derives from `where::Field<M>`, which is stateless: every operator
reads `field_name_sv`, a `static constexpr` off the template parameter. That
inheritance is what donates the whole comparison surface to the proxy with no
duplication and no per-instance data, and it is why the bare form
`fields::Person.age == 30` needs no wrapper.

The member reflection also lives in the *type*, via `M`. That is what lets the
same object be passed as a template argument in the NTTP positions —
`selector_info<S>()` recovers the `std::meta::info` at compile time, and
everything below that boundary (the statement classes, grammars and sizers)
still works in plain `std::meta::info`. The proxy is the boundary, not a
new layer through the whole stack.

Generic code that resolves a field by *name* at compile time — Storm's
YAML-driven query builder, for instance — cannot spell `fields::Model.member`
because `Model` is a template parameter. It uses `storm::meta::selector_for<M>()`
instead, which maps an `info` back to the right proxy. Hand-written call sites
never need it.
