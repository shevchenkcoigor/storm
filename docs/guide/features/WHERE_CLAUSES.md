# WHERE Clauses

Build WHERE filters using field expressions and operators.

Selectors are the `fields::` proxies declared next to each model, so no extra
`using` is needed beyond `storm` itself:

```cpp
import storm;
using namespace storm;
```

## Basic Syntax

WHERE expressions use field access via reflection with compile-time operators.

```cpp
auto results = QuerySet<Person>()
    .where(fields::Person.age > 30)
    .select().execute();
```

`fields::Person.age` is a generated selector proxy — see
[FIELD_SELECTORS.md](../reference/FIELD_SELECTORS.md) for the two-line per-model
declaration it needs. It carries the comparison operators directly, so no wrapper
is involved.

Only persisted columns are comparable. A relation member — a many-to-many
container (`[[= many_to_many]]`) or a reverse-FK container
(`[[= storm::reverse_fk<...>]]`) — is a **compile-time error** (#408): those are not
columns, so a WHERE clause on one would reference a non-existent column. The constraint
fails at the call site instead of producing an opaque "no such column" at prepare time.
Filter on a relation's own columns by joining to it (`join<fields::T.field>()`) first.

## Comparison Operators

All 6 comparison operators are supported.

```cpp
// Equals
auto results = QuerySet<Person>()
    .where(fields::Person.age == 30)
    .select();

// Not equals
auto results = QuerySet<Person>()
    .where(fields::Person.age != 30)
    .select();

// Greater/Less than
auto results = QuerySet<Person>()
    .where(fields::Person.age > 30)
    .select();

auto results = QuerySet<Person>()
    .where(fields::Person.age <= 65)
    .select();
```

| Operator | SQL | Example |
|---|---|---|
| `==` | `=` | `age == 30` |
| `!=` | `!=` | `age != 30` |
| `>` | `>` | `age > 30` |
| `>=` | `>=` | `age >= 30` |
| `<` | `<` | `age < 30` |
| `<=` | `<=` | `age <= 30` |

### Filterable field types

A field can be **read back** (any [supported field type](../reference/FIELD_TYPES.md)) but
only a subset is **filterable in a WHERE clause** — the expression system stores operands in a
closed `std::variant`, so a type needs a variant arm to appear in `where()`. The two sets are
no longer the same width by accident; this table is the contract (#407).

| Field type | Filterable? | Operators | Notes |
|---|---|---|---|
| `int`, `int64_t`, `long`, `long long` | ✅ | all 6, `BETWEEN`, `IN` | |
| `short`, `unsigned` (`short`/`int`/`long`/…), `char`, `signed`/`unsigned char` | ✅ | all 6, `BETWEEN`, `IN` | Fold to `int` / `int64_t` (like enums) |
| `double`, `float` | ✅ | all 6, `BETWEEN`, `IN` | |
| `bool` | ✅ | `==`, `!=` | |
| `std::string`, `std::string_view` | ✅ | all 6, `BETWEEN`, `IN`, `LIKE`, `COLLATE` | |
| enum | ✅ | all 6, `IN` | Folds to underlying `int` |
| `std::chrono::year_month_day` | ✅ | all 6, `BETWEEN`, `IN` | Compared as `"YYYY-MM-DD"` TEXT (lexicographic == chronological) |
| `std::chrono::system_clock::time_point` | ✅ | all 6, `BETWEEN`, `IN` | Compared as `"YYYY-MM-DD HH:MM:SS"` TEXT |
| `storm::UUID` | ✅ | `==`, `!=`, `IN` | Equality/IN only — ordering/`BETWEEN` rejected at compile time (#609/#407), including a string-spelled operand against a UUID column (#622, **BREAKING**: this used to compile as a plain TEXT compare). Applies to a direct column or a single-column FK to a UUID-PK model alike. An unset (empty) `==`/`!=`/`IN` comparison value is rejected at bind time rather than silently matching nothing, and a non-empty value is validated as RFC-4122 text. For `==`/`!=` (not yet `IN`/`BETWEEN` via the YAML/JSON query builder), a plain string/string_view operand is accepted and converted to `storm::UUID` before that same validation (#622); a non-UUID-constructible operand (e.g. `== 5`) is rejected at compile time |
| `std::optional<T>` | ✅ | `is_null()`, `is_not_null()`, `== nullopt`, `!= nullopt` | Plus any operator `T` itself supports |
| `std::chrono::duration` | ❌ | — | Persistable/readable, not yet filterable |
| `std::filesystem::path` | ❌ | — | Persistable/readable, not yet filterable |
| BLOB (`std::vector<uint8_t>` / `std::vector<std::byte>`) | ❌ | — | Persistable/readable; byte-blob comparison is not exposed |

Temporal comparisons sort correctly because both serializations are zero-padded and
lexicographically ordered, so `>`, `<`, and `BETWEEN` on a date/datetime match chronological order.

```cpp
using std::chrono::year, std::chrono::month, std::chrono::day, std::chrono::year_month_day;

// Datetime range filter
auto recent = QuerySet<Event>()
    .where(fields::Event.created_at >= cutoff_time_point)
    .select().execute();

// Date BETWEEN
auto q2 = QuerySet<Event>()
    .where(fields::Event.on_date.between(
        year_month_day{year{2024}, month{4}, day{1}},
        year_month_day{year{2024}, month{6}, day{30}}))
    .select().execute();

// UUID equality / IN
auto byId = QuerySet<Event>()
    .where(fields::Event.id == storm::UUID{"…"})
    .select().execute();
```

### Operand lifetime

`where()` is deferred — the expression node is built now and the operand is bound only at
`.select()`. Comparison operands are therefore stored **by owning value**: text operands
(`std::string_view`, `const char*`, string literals) are copied into a `std::string` at
construction. This means an expression survives the buffer it was built from:

```cpp
auto make_filter() {
    std::string name = load_name();           // local buffer
    return fields::Person.name == name;    // operand is COPIED, not viewed
}                                              // `name` is destroyed here — safe

auto results = QuerySet<Person>().where(make_filter()).select();  // no dangling bind
```

## String Operations

### LIKE pattern matching

The `%` wildcard matches any sequence of characters.

```cpp
// WHERE name LIKE 'Al%'
auto results = QuerySet<Person>()
    .where(fields::Person.name.like("Al%"))
    .select();
```

### BETWEEN range queries

The `between()` method creates a range check.

```cpp
// WHERE age BETWEEN 25 AND 65
auto results = QuerySet<Person>()
    .where(fields::Person.age.between(25, 65))
    .select();
```

## IN

The `in()` method takes a variadic list of values and matches any of them.

```cpp
// WHERE id IN (100, 200, 300)
auto results = QuerySet<Person>()
    .where(fields::Person.id.in(100, 200, 300))
    .select();
```

Works on any comparable field type also supported by the comparison operators — int,
`int64_t`, double, float, string, temporal (`year_month_day` / `time_point`), and UUID.

## COLLATE

Collation for string comparisons (case-insensitive, etc).

```cpp
auto results = QuerySet<Person>()
    .where(fields::Person.name.collate(Collate::NoCase) == "alice")
    .select();
```

| Option | Behavior |
|---|---|
| `Collate::Binary` | Binary comparison (default) |
| `Collate::NoCase` | Case-insensitive (locale-independent) |
| `Collate::RTrim` | Right-trim whitespace before comparing |

Collation is applied during query construction and compiled into the WHERE SQL.

## NULL Checks

Test for NULL or NOT NULL values on optional fields.

### is_null()

Generate an `IS NULL` check.

```cpp
// SELECT * FROM person WHERE score IS NULL
auto nulls = QuerySet<Person>()
    .where(fields::Person.score.is_null())
    .select();
```

### is_not_null()

Generate an `IS NOT NULL` check.

```cpp
// SELECT * FROM person WHERE score IS NOT NULL
auto non_nulls = QuerySet<Person>()
    .where(fields::Person.score.is_not_null())
    .select();
```

### Using nullopt for NULL checks

Comparison with `std::nullopt` generates the same SQL.

```cpp
// These are equivalent:
.where(fields::Person.score.is_null())
.where(fields::Person.score == std::nullopt)

// These are equivalent:
.where(fields::Person.score.is_not_null())
.where(fields::Person.score != std::nullopt)
```

### NULL checks with COLLATE

COLLATE can be combined with NULL checks on optional string fields.

```cpp
// SELECT * FROM person WHERE nickname COLLATE NOCASE IS NULL
auto results = QuerySet<Person>()
    .where(fields::Person.nickname.collate(Collate::NoCase).is_null())
    .select();
```

### Composing NULL checks

NULL checks compose with AND/OR like any other expression.

```cpp
// SELECT * FROM person WHERE score IS NULL AND age > 30
auto results = QuerySet<Person>()
    .where(fields::Person.score.is_null() && fields::Person.age > 30)
    .select();
```

## Logical Composition

Expressions can be combined with `&&` (AND) and `||` (OR).

```cpp
// WHERE (age > 30) AND (name == "Alice")
auto results = QuerySet<Person>()
    .where((fields::Person.age > 30) && (fields::Person.name == "Alice"))
    .select();

// WHERE (age < 25) OR (salary > 100000)
auto results = QuerySet<Person>()
    .where((fields::Person.age < 25) || (fields::Person.salary > 100000))
    .select();

// Complex nesting
auto results = QuerySet<Person>()
    .where(
        (fields::Person.age > 30 && fields::Person.salary > 50000) ||
        (fields::Person.years_experience >= 10)
    )
    .select();
```

Parentheses improve readability and ensure correct precedence.
