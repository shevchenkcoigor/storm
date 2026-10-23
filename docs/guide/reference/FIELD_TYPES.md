# Supported Field Types

Storm ORM supports all standard SQLite types through compile-time type dispatch in `BaseStatement::bind_value_by_type()` (src/orm/statements/base.cppm) and `SelectStatement::extract_column_inline_fast()` (src/orm/statements/select.cppm).

> **Annotation spelling (#442).** All field annotations are re-exported into the top-level
> `storm` namespace, so models spell them as `primary`, `fk<>`,
> `fk<storm::RefAction::Cascade>`, `many_to_many<>`, `storm::reverse_fk<...>`.
> The longer `storm::meta::` spelling (`storm::meta::fk<>`, …) still works unchanged — the
> re-exports are purely additive. Examples in this document use the short `storm::` form.

## Integer Types

SQLite has one dynamic `INTEGER` storage class for every width. PostgreSQL does not, so
`schema.cppm` picks the narrowest PG column that safely holds the C++ type's full value
range (#603) — a signed N-byte source needs the same or a wider signed PG type; an
unsigned N-byte source needs one size class wider (its top half doesn't fit a same-width
signed range). `short`/`signed char`/`unsigned char`/`char` (1-byte types fit `SMALLINT`
regardless of signedness) map to `SMALLINT`; `int`/`unsigned short` map to `INTEGER`; `int64_t`/`long`/`long long`/
`unsigned int` map to `BIGINT`. Enums and `std::chrono::duration` fields follow their
underlying/`rep` type. SQLite's mapping is unchanged — this is PG-only.

| C++ Type | SQLite Type | PostgreSQL Type | Binding Method | Extraction Method |
|----------|-------------|------------------|----------------|-------------------|
| `int` | INTEGER | INTEGER | `bind_int()` | `extract_int()` |
| `short` | INTEGER | SMALLINT | `bind_int()` (cast) | `extract_int()` (cast) |
| `unsigned short` | INTEGER | INTEGER | `bind_int()` (cast) | `extract_int()` (cast) |
| `unsigned int` | INTEGER | BIGINT | `bind_int()` (cast) | `extract_int()` (cast) |
| `int64_t` | INTEGER | BIGINT | `bind_int64()` | `extract_int64()` |
| `long` | INTEGER | BIGINT | `bind_int64()` | `extract_int64()` |
| `long long` | INTEGER | BIGINT | `bind_int64()` | `extract_int64()` |
| `uint64_t` | *annotation-gated* | *annotation-gated* | see below | see below |
| `unsigned long` | *annotation-gated* | *annotation-gated* | see below | see below |
| `unsigned long long` | *annotation-gated* | *annotation-gated* | see below | see below |

> ⚠️ **64-bit unsigned fields require an explicit storage annotation (#436).**
> Neither SQLite nor PostgreSQL has an unsigned 64-bit integer type, so a bare
> `uint64_t` / `unsigned long` / `unsigned long long` field is a **compile-time
> error**. Annotate the field with exactly one of:
>
> | Annotation | Column type | Range with correct ordering | Cost |
> |---|---|---|---|
> | `storm::signed_storage` | `INTEGER` (SQLite) / `BIGINT` (PG) | `0 .. INT64_MAX` (2⁶³−1) | none — same `bind_int64`/`extract_int64` hot path |
> | `storm::full_unsigned` | zero-padded 20-char `TEXT` (SQLite) / `NUMERIC(20,0)` (PG) | `0 .. 2⁶⁴−1` (full range) | slower string bind/extract, wider column |
>
> **`signed_storage`** keeps today's behavior. It is byte-identical to a plain
> signed 8-byte column and routes through the same `bind_int64`/`extract_int64`
> calls, so there is **zero performance change**. Use it when the values are
> always ≤ `INT64_MAX`. **Caveat:** a value > `INT64_MAX` is stored as a negative
> `int64` — equality still round-trips (the read casts the signed bits back), but
> `ORDER BY` / `>` / `<` / `BETWEEN` sort by the signed interpretation (so `2⁶³+1`
> sorts *before* `1`) and any external reader (raw SQL, a BI tool) sees a negative
> number. Pinned by `Uint64SignedStorageTest` in `tests/schema/test_types.cpp`.
>
> **`full_unsigned`** stores the value order-preservingly: a 20-character
> zero-padded decimal string on SQLite (`uint64_max` = `18446744073709551615` is
> 20 digits, so lexicographic order == numeric order) and `NUMERIC(20,0)` on
> PostgreSQL. The **full `0 .. 2⁶⁴−1` range round-trips for equality AND sorts
> correctly**, and external readers see the true unsigned value. It pays a string
> bind/extract on every access, so reach for it only when you actually need values
> above `INT64_MAX`. Pinned by `Uint64FullUnsignedTest` in the same file.
>
> Signed 64-bit types (`int64_t` / `long` / `long long`) and all smaller integer
> types are unaffected — no annotation needed.

**Usage (64-bit unsigned):**
```cpp
struct Account {
    [[=primary]] int id;
    [[=storm::signed_storage]] std::uint64_t small_id;   // ≤ INT64_MAX
    [[=storm::full_unsigned]]  std::uint64_t full_range; // 0 .. 2⁶⁴−1
};
```

**Usage:**
```cpp
struct Example {
    [[=primary]] int id;
    int64_t big_number;
    unsigned short count;
};
```

## Floating Point Types

| C++ Type | SQLite Type | Binding Method | Extraction Method |
|----------|-------------|----------------|-------------------|
| `double` | REAL | `bind_double()` | `extract_double()` |
| `float` | REAL | `bind_double()` (cast) | `extract_float()` |

**Usage:**
```cpp
struct Measurement {
    [[=primary]] int id;
    double precision_value;
    float approximate_value;
};
```

## Boolean Type

| C++ Type | SQLite Type | Binding Method | Extraction Method |
|----------|-------------|----------------|-------------------|
| `bool` | INTEGER | `bind_int()` | `extract_bool()` |

**Storage**: Stored as INTEGER (0 = false, 1 = true)

**Usage:**
```cpp
struct User {
    [[=primary]] int id;
    bool is_active;
    bool is_admin;
};
```

## String Types

| C++ Type | SQLite Type | Binding Method | Extraction Method |
|----------|-------------|----------------|-------------------|
| `std::string` | TEXT | `bind_text()` | `extract_text_ptr()` |
| `const char*` | TEXT | `bind_text()` | N/A (output only as std::string) |
| `std::string_view` | TEXT | `bind_text()` | N/A (output only as std::string) |

**Notes:**
- Any type convertible to `std::string_view` can be used for binding
- Extraction always returns `std::string` (owns the data)
- Optimized string extraction using `sqlite3_column_bytes()` (avoids strlen)

**Usage:**
```cpp
struct Document {
    [[=primary]] int id;
    std::string title;
    std::string content;
};

// All these work for binding:
Document doc1{0, "Title", "Content"};
Document doc2{0, std::string("Title"), std::string("Content")};
std::string_view title_view = "Title";
Document doc3{0, std::string(title_view), "Content"};
```

### Bounded length (`max_length<N>`) (#493)

Annotate a text field with `max_length<N>` to bound its length **in the database**:

```cpp
struct Account {
    [[=primary]] int id;
    [[=storm::max_length<50>]] std::string name;              // NOT NULL, ≤ 50
    [[=storm::max_length<20>]] std::optional<std::string> tag; // nullable, ≤ 20
};
```

| Field | PostgreSQL | SQLite |
|-------|-----------|--------|
| `std::string name` + `max_length<50>` | `name VARCHAR(50) NOT NULL` | `name TEXT NOT NULL CHECK(length(name) <= 50)` |
| `std::optional<std::string> tag` + `max_length<20>` | `tag VARCHAR(20)` | `tag TEXT CHECK(length(tag) <= 20)` |

**Both dialects genuinely enforce the bound** on every write path — Storm's own
INSERT/UPDATE/upsert *and* any raw SQL. This is stronger than Django
(`CharField(max_length=…)`) and SQLAlchemy (`String(…)`), which emit `varchar(N)` that
**SQLite silently ignores**; they rely on an optional app-layer validator. Storm has no
app layer, so it emits a real `CHECK` on SQLite — nothing to remember or bypass.

- **Nullable + bounded**: the SQLite `CHECK` passes when the value is NULL (standard SQL),
  so a `std::optional<std::string>` column allows NULL and still rejects over-limit values.
- **Combines with** `unique` (appended after the CHECK/VARCHAR), a C++ default-member
  initializer (`#413`, DEFAULT precedes the CHECK on SQLite), and `indexed` (orthogonal).
  SQLite regular-field order: `<name> TEXT [NOT NULL] [DEFAULT <v>] [CHECK(length(<name>) <= N)] [UNIQUE]`.
- **Type guard**: `max_length<N>` on a non-text field is a **compile error** at the model
  boundary (`ModelMaxLengthValid<T>`), consistent with the bare-`uint64` hard error — only
  `std::string` / `std::string_view` / `std::optional<those>` accept it.

Out of scope (YAGNI): `min_length` (separate follow-up — `CHECK(length >= N)` passes on NULL,
so it does *not* mean "required") and `check<"expr">` (raw-SQL escape hatch).

### Single-column indexes (`unique` / `indexed`)

```cpp
struct Person {
    [[= primary]] int id;
    [[= unique]] std::string email;    // CREATE UNIQUE INDEX
    [[= indexed]] std::string name;    // CREATE INDEX
};
```

`unique` adds a uniqueness constraint (`UNIQUE INDEX`); `indexed` adds a plain lookup index
with no uniqueness constraint. Foreign-key fields (`fk<>`) are indexed automatically without
either tag. See [features/INDEXES.md](../features/INDEXES.md) for full coverage, including
composite (multi-column) indexes via `storm::Index<>` / `uniqueIndex<>`.

## Primary Keys (`primary` / `primary_autoincrement` / `primary_assigned` / `primary_part`)

Every model needs a primary key — `ModelWithPrimaryKey<T>` is a `BaseStatement<T>` constraint,
so a model without one fails to compile at a named concept.

```cpp
struct Person {
    [[= primary]] int id;                 // INTEGER PRIMARY KEY
};

struct Event {
    [[= primary_autoincrement]] int id;   // + AUTOINCREMENT (SQLite never-reuse, #379)
};

struct ExternalRef {
    [[= primary_assigned]] int id;        // caller supplies the id (#586)
};
```

### Composite (multi-column) primary keys (#500)

Annotate two or more members with `primary_part` to make the key span several columns:

```cpp
struct OrderItem {
    [[= primary_part]] int order_id;
    [[= primary_part]] int product_id;
    int quantity;
};
```

The parts are emitted as ordinary columns plus one table-level clause, in **declaration
order** (column order is significant for the index the key creates):

```sql
CREATE TABLE OrderItem (
    order_id INTEGER NOT NULL DEFAULT 0,
    product_id INTEGER NOT NULL DEFAULT 0,
    quantity INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (order_id, product_id)
)
```

- **Separate tag, by design**: `primary_part` is its own annotation rather than "two
  `primary` members means composite". Reusing `primary` would silently turn an accidental
  double-`primary` typo into a legal composite key; keeping them distinct leaves that an error.
- **No autoincrement on a composite key** — and this is not merely useless, it is
  *unrepresentable*. SQLite rejects both spellings at parse time (a column-level `PRIMARY KEY`
  cannot coexist with a table-level `PRIMARY KEY (...)`, and `AUTOINCREMENT` is only
  grammatical directly after `INTEGER PRIMARY KEY`); PostgreSQL's `GENERATED … AS IDENTITY`
  is single-column too. So the caller always supplies the full key — no part is ever DB-generated.
- **Both backends**: SQLite and PostgreSQL emit the same table-level clause. Each part's own
  column type follows the usual per-dialect mapping (#603) — an `int` part renders `INTEGER`
  on both backends now; only wider/narrower part types (e.g. `int64_t`, `short`) diverge.
- **No separate index per part**: the table-level `PRIMARY KEY` already indexes each part.
- **`DEFAULT 0` on the parts** is the ordinary auto-DEFAULT for NOT NULL columns (#413),
  harmless here since the key is always caller-supplied.

**Rejected at compile time** by `ModelPrimaryKeyValid<T>` (a `BaseStatement<T>` constraint):

| Declaration | Why it's rejected |
|---|---|
| Two or more `primary` / `primary_autoincrement` / `primary_assigned` members | The accidental double-`primary` typo — the case the separate tag exists to catch |
| `primary` + `primary_part` | Two competing PK declarations — which one is the key? |
| `primary_autoincrement` + `primary_part` | Unrepresentable in SQL on either backend (above) |
| `primary_assigned` + `primary_part` | Unrepresentable in SQL — a caller-assigned key is still a single-column INTEGER PRIMARY KEY |
| `primary_assigned` + `primary_autoincrement` | Conflicting annotations on the same member |
| Exactly one `primary_part` | That is a plain PK — spell it `primary` |
| A PK annotation on an m2m / reverse-FK container | Not a column — the key would name something no column definition emits |
| A PK annotation on `std::optional<T>` | A nullable PK is not a key (SQLite's legacy NULL quirk even admits duplicates, diverging from PG). Applies to a **single** `primary` too — a deliberate widening in #500, since a nullable PK was never correct |
| `primary_part` + `unique` | UNIQUE on one part allows at most one row per that part, defeating the composite key |

FK members make good parts — `[[= primary_part]] [[= fk<>]] Warehouse warehouse`
is the canonical association-table key, and the clause names the FK's actual column
(`PRIMARY KEY (warehouse_id, sku)`).

#### At most 4 parts (#537)

A composite primary key may span **at most 4 columns**. A 5th part is a compile-time error
via `ModelPrimaryKeyPartLimit<T>` (a `BaseStatement<T>` constraint):

```cpp
struct TooWide {
    [[= primary_part]] int a;
    [[= primary_part]] int b;
    [[= primary_part]] int c;
    [[= primary_part]] int d;
    [[= primary_part]] int e;   // ❌ compile error — 5 parts
};
```

The limit comes from the m2m / reverse-FK eager load. That runs as two queries and stitches
the second query's rows onto their owner through a hash map keyed on the owner's primary key;
for a composite key, that map key is a fixed 32-byte inline buffer into which each part writes
exactly one 8-byte word. Four parts fill it exactly.

The buffer is **not** simply grown to make room, because its size is load-bearing for
performance: a wider key measurably slows the stitch (which is why single-column keys bypass
the buffer entirely for a bare 64-bit word). Folding surplus parts together was rejected too —
two distinct keys colliding in the fold would attach rows to the *wrong* owner, a correctness
bug rather than a recoverable hash collision.

The limit applies to every model, not only to those that currently declare a relation: adding
an m2m field later must not turn a previously fine model into a compile error in a distant file.
Single-column keys are unaffected, as are the 2- and 3-part keys in the examples above.

#### Part types (#517)

Every `primary_part` member's type is checked at compile time by `PrimaryKeyType<T>`
(a `BaseStatement<T>` constraint):

| Part type | Verdict |
|---|---|
| `short`, `int`, `long`, `long long` + `std::int16_t`/`int32_t`/`int64_t` | ✅ accepted |
| `std::uint64_t` / `unsigned long` / `unsigned long long` | ✅ **only** with `[[= storm::signed_storage]]` |
| `storm::UUID` | ✅ accepted |
| `std::string` | ✅ accepted — **composite parts only** |
| `std::string_view` | ✅ accepted, but see the note below — it cannot be read back |
| An FK member (`[[= fk<>]] Related`) whose target has a well-typed **single** PK (e.g. `[[= primary]] int id`) | ✅ accepted |
| An FK member whose target has **no** primary key | ❌ rejected |
| An FK member whose target's single PK is itself a rejected type (e.g. `double`, `std::string`) | ❌ rejected |
| An FK member whose target is itself **composite-keyed** | ❌ rejected — needs a multi-column FK (#504), out of scope here |
| `bool`, `char` | ❌ a two-valued or 1-byte identity is not a key |
| `std::optional<T>` | ❌ a nullable key is not a key |
| `unsigned int` and other bare unsigned types | ❌ not in the allowed set |
| `std::uint64_t` with `[[= storm::full_unsigned]]` | ❌ stores as zero-padded TEXT, unreadable through the int64 PK paths |
| `double`, `std::vector<uint8_t>` (BLOB) | ❌ not a key type |

**An FK part is validated through its target's primary key, not its own declared type**
— it binds the *referenced* row's key (`bind_one_pk_part` splices
`find_fk_primary_key<FKType>()`), so the member's own type is not the right question.
This is the subtlest rule in the table, and it cuts both ways:

- The check is **strictly stronger** than "does the target have a PK at all"
  (`ValidForeignKey`, used for ordinary FK fields and `join<>()`). Here, the target's
  PK must *also* pass the single-PK allowlist (#505) — an FK part pointing at a model
  whose PK is `double` is rejected, even though `join<fields::T.fk>()` on that same field
  would be fine.
- **TEXT does not reach through an FK.** The parts-only TEXT widening applies to the
  FK member's *own* declared type, but an FK's declared type is never TEXT — it is the
  related model type. So an FK part pointing at a `std::string`-PK model is rejected:
  that target's single PK already fails #505 on its own terms, and the FK part
  inherits that rejection rather than acquiring a new TEXT allowance.
- **A composite-keyed target is refused outright** — not because composite FKs are
  unsupported (#504 shipped those for ordinary FK fields), but because the PK-part bind
  path still binds a single column: `bind_one_pk_part` splices the single-column
  `find_fk_primary_key`, so an FK part pointing at an N-part key would bind one column
  for a key spanning N. Supporting it means widening that bind path first.

> **`std::string_view` parts bind but cannot be read back.** The concept accepts a
> `std::string_view` part, and it generates DDL and binds correctly — but
> `ColumnExtractor` has no `std::string_view` arm, so SELECTing a model that contains one
> is a compile error. This is a project-wide limitation of `std::string_view` fields, not
> something specific to composite keys. **Prefer `std::string` for any part you intend to
> read back.**

**TEXT is allowed for a composite part but not for a single primary key.** That split
tracks a real distinction rather than an arbitrary one. A single PK still flows through
Storm's integer-identity machinery (`RETURNING id`, `last_insert_rowid`, the int64 PK
paths in `select.cppm`/`join.cppm`/`insert.cppm`), which is what makes a TEXT single PK
a problem here. A composite key is **never DB-generated** (#502 — no `AUTOINCREMENT`, no
`IDENTITY`, no `RETURNING`; the caller always supplies every part), so none of that
machinery applies to a part.

This also matches SQL itself and the wider ecosystem — SQLAlchemy's
`Column(String(50), primary_key=True)`, Django's `CharField(primary_key=True)` and JPA's
`@Id String` are all ordinary. The rule these systems actually enforce is not "primary
keys must be integers" but *"auto-generated primary keys must be integers"*, which is
exactly where Storm's line falls too.

```cpp
struct Inventory {
    [[= primary_part]] int         warehouse;   // ✅
    [[= primary_part]] std::string sku;         // ✅ TEXT part
    int on_hand;
};

struct Broken {
    [[= primary_part]] int    id;
    [[= primary_part]] double amount;           // ❌ compile error
};
```

> `indexed` on a part is currently a no-op: the PK already indexes the parts, so the
> per-column index is suppressed for every PK member, as it is for a single-column PK.

### UPDATE and DELETE by composite key (#501)

`update(obj)` and `erase(obj)` match on the **whole** key: the `WHERE pk = ?` of a
single-PK model becomes `WHERE a = ? AND b = ?`, with every part bound in declaration
order. A row matching only *some* parts is not touched.

```cpp
qs.erase(OrderItem{.order_id = 1, .product_id = 20}).execute();
// DELETE FROM OrderItem WHERE order_id = ? AND product_id = ?

qs.update(OrderItem{.order_id = 1, .product_id = 20, .quantity = 7}).execute();
// UPDATE OrderItem SET quantity=? WHERE order_id = ? AND product_id = ?
```

- **No key part is ever a SET target.** The UPDATE (and upsert `DO UPDATE`) SET clause
  excludes *every* PK member, so a statement can never rewrite part of the key it matches on.
- **Batch DELETE uses a row-value `IN` list** — `WHERE (a, b) IN ((?,?),(?,?))`. This is
  standard SQL (PostgreSQL always; SQLite since 3.15, well under this project's 3.35 floor).
  The per-column form `a IN (…) AND b IN (…)` would match the *cross product* of the parts
  and delete keys that were never listed, so it is not used.
- **Batch chunking shrinks with key width.** Each row costs one bound parameter per PK
  column, so an N-part key fits `799 / N` rows per chunk instead of 799. Single-PK models
  keep the historical 799.
- **Single-PK SQL is byte-identical** to what Storm emitted before composite support.

### INSERT by composite key (#502)

A composite key is never DB-generated — `AUTOINCREMENT` (SQLite) and `GENERATED ... AS IDENTITY`
(PostgreSQL) are single-column features. So `insert().execute()` on a composite model returns
`std::expected<void, Error>` with **no `RETURNING` clause** (nothing to return). Every key part is
caller-supplied and appears in the INSERT column list in **declaration order**.

```cpp
struct OrderItem {
    [[= primary_part]] int order_id;
    [[= primary_part]] int product_id;
    int quantity;
};

OrderItem oi{.order_id = 7, .product_id = 42, .quantity = 3};  // caller supplies both PK parts
qs.insert(oi).execute();   // → std::expected<void, Error>
// SQL: INSERT INTO OrderItem (order_id, product_id, quantity) VALUES (?, ?, ?)
```

**Return type and constraints:**

- **`insert().execute()` returns `std::expected<void, Error>`** — no row ID to return since the
  key is caller data.
- **No `RETURNING` clause is emitted** — the single-PK `RETURNING id` fast path detects the
  composite key at compile time and routes through `ReturnId::No` instead.
- **`insert<ReturnId::Yes>` is a compile-time error** — the `ReturnIdSupported<T, R>` concept
  rejects it (would emit `RETURNING <first part>` — silently lossy and wrong). The diagnostic
  fires at the user's call site.
- **`insert<ReturnId::No>` is accepted and identical** to the plain call. Generic code that
  explicitly spells out the return type stays portable across model shapes.
- **Duplicate key surfaces as `Error`** — SQLite/PostgreSQL UNIQUE constraint violations return
  an error code in the result; the caller can check `!result` or use `and_then()` to handle it.
- **Chunking unchanged** — the auto batch size already divides by field count, which becomes
  exactly right once the key columns count toward the parameter budget.

**Single-PK models remain unchanged** — they still return `int64_t` and emit `RETURNING id`.

### UPSERT by composite key (#503)

The `on_conflict<...>()` target may be the **full** composite-PK column set, in
declaration order — a composite PK is itself a unique constraint over its N columns.
A **partial** target (a strict subset) is rejected at compile time: one column of a
composite key is not unique on its own.

```cpp
qs.insert(oi).on_conflict<fields::OrderItem.order_id, fields::OrderItem.product_id>()
   .update<fields::OrderItem.quantity>().execute();
// SQL: INSERT INTO OrderItem (order_id, product_id, quantity) VALUES (?, ?, ?)
//      ON CONFLICT (order_id, product_id) DO UPDATE SET quantity=excluded.quantity
//      -- no RETURNING clause

qs.insert(oi).on_conflict<fields::OrderItem.order_id>()
   // COMPILE ERROR: order_id alone is not unique — it's one part of a 2-part key
```

Same reasoning as plain INSERT (#502): a composite key is never DB-generated, so
there is nothing for `RETURNING` to echo back.

- **No `RETURNING` clause is emitted** for a composite target — both `DO UPDATE` and
  `DO NOTHING` resolve to `std::expected<void, Error>` instead of the single-PK
  `int64_t` / `std::optional<int64_t>`.
- **`DO NOTHING`'s "was it skipped?" signal is unavailable** for a composite target
  (documented here rather than reconstructed some other way — there is no row to
  report on since `RETURNING` is omitted).
- **`DO UPDATE SET` still cannot target any key part** — the SET-target gate excludes
  every PK member, same as plain UPDATE (#501).
- **Single-PK upsert SQL is byte-identical** to before — `RETURNING id` is still
  emitted, and the return types are unchanged.

> **Scope**: composite keys now cover the annotation (#500), compile-time PK machinery,
> `CREATE TABLE` DDL, UPDATE/DELETE by key (#501), INSERT by key (#502), and upsert
> `ON CONFLICT` targets (#503). Composite foreign keys / JOIN (#504) is still open.

### UUID Primary Keys (#507)

Storm supports `storm::UUID` as a primary key. UUID PKs are **always client-generated** — Storm does not auto-generate them at the database level (unlike integer `AUTOINCREMENT`/`IDENTITY`).

`storm::UUID` is accepted under any PK annotation, including `primary_assigned` — combining them
(`[[= primary_assigned]] storm::UUID id;`) is redundant but harmless: `has_uuid_pk_()` and
`has_caller_assigned_pk_()` are independent checks OR'd together in `pk_is_db_generated_()`, so
either one alone already routes the model to the caller-supplied-key behavior described below.

```cpp
struct User {
    [[= primary]] storm::UUID id;  // Caller must set this before insert()
    std::string name;
};

// Usage
User user{.id = storm::UUID::generate(), .name = "Alice"};  // generate() call is required
QuerySet<User> qs;
qs.insert(user).execute();  // std::expected<void, Error> — nothing to return (#572)
```

**Key differences from integer PKs:**

- **`insert()` returns `std::expected<void, Error>`, not the id** (#572). A UUID key is
  caller-supplied, so there is nothing for `RETURNING` to hand back — no `RETURNING` clause is
  emitted, and the same holds for `on_conflict().update()` / `.nothing()`. An explicit
  `insert<ReturnId::Yes>` on a UUID-PK model is a **compile error** rather than a silently
  meaningless integer; `insert<ReturnId::No>` stays valid on every model shape. You already have
  the key — read it off the object you inserted.
- **The UUID key column is INSERTed like any other column** (#565) — the caller's key is written,
  not skipped as if it were an identity column. Same rule as a composite key (#502): neither is
  ever DB-generated.
- **The caller must call `storm::UUID::generate()`** and assign it before insert — binding an empty UUID PK is an **error**.
- **On PostgreSQL**, emits `id UUID PRIMARY KEY`. **On SQLite**, emits `id TEXT PRIMARY KEY`.
- **FK columns referencing a UUID-PK model** emit `UUID` (PG) / `TEXT` (SQLite) — **not** INTEGER/BIGINT. The column type matches the referenced primary key type.
- **m2m auto-junction columns** follow the same rule per side (#565): an m2m between UUID-PK models
  emits `Doc_id UUID NOT NULL, Tag_id UUID NOT NULL` on PG (`TEXT` on SQLite). The two sides resolve
  independently, so an integer-PK owner and a UUID-PK related model legitimately mix column types in
  one junction. See
  [REFERENTIAL_INTEGRITY.md](../features/REFERENTIAL_INTEGRITY.md#junction-column-type-follows-the-referenced-key-565).
- **Foreign keys, m2m, and reverse-FK eager loading** work identically to integer PKs, with **no performance impact** — integer PKs are byte-identical (zero codegen overhead).
- **SELECT by UUID PK** works identically to integer PKs.
- **UPDATE / DELETE by UUID PK reject an unset (empty) key** with the same error INSERT raises
  ("Primary key UUID must be explicitly set; auto-generation not allowed for PKs", #573) instead of
  silently matching zero rows. The by-key WHERE clause binds the key through the PK-specific
  binder, not the auto-generating binder INSERT/SET positions use.
- **Any `.where()`/`.having()` comparison against a `storm::UUID` column — PK or not, direct or
  via a single-column FK to a UUID-PK model — rejects an unset (empty) operand** ("UUID
  comparison value must be explicitly set; auto-generation not allowed in a WHERE/HAVING clause",
  #609) instead of silently matching nothing; a non-empty operand is also validated as RFC-4122
  text. Ordering (`>`, `>=`, `<`, `<=`) and `.between()` on a UUID column are compile-time errors
  — UUID is equality/`IN` only (#609/#407/**#622, BREAKING**: a string-spelled ordering
  comparison against a UUID column used to compile as a plain lexicographic TEXT compare; it is
  now rejected too, for consistency with the equality/IN-only contract). The equality guard keys
  on the **column's** declared type, not just the operand's (#622): a plain-string `==`/`!=`
  comparison (`== ""`, or the equality/`!=` case of a YAML/JSON-driven filter —
  `query_builder.hpp`'s `typed_value_as` has no `storm::UUID` branch, so its `IN`/`BETWEEN` cases
  don't get this conversion yet) is converted to `storm::UUID{...}` before binding, so it goes
  through the same empty/malformed rejection as a `storm::UUID{}`-spelled operand. A non-UUID
  operand (e.g. `uuid_col == 5`) is also now a compile-time rejection rather than a silent
  wrong-type bind.
- **Thread safety**: Same as integer PKs — per-thread `QuerySet` instances with thread-local connections.

**Example with foreign keys:**

```cpp
struct Message {
    [[= primary]] storm::UUID id;
    [[= fk<>]] User sender;        // FK to UUID-PK User — sender_id column is TEXT/UUID
    std::string text;
};

// Insert a message referencing a UUID-PK user
User user{.id = storm::UUID::generate(), .name = "Alice"};
QuerySet<User>().insert(user).execute();

Message msg{.id = storm::UUID::generate(), .sender = user, .text = "Hello"};
QuerySet<Message>().insert(msg).execute();  // sender_id bound as TEXT/UUID, not INTEGER
```

**Example with m2m and UUID-PK owner:**

```cpp
struct Course {
    [[= primary]] int id;
    std::string title;
};

struct Student {
    [[= primary]] storm::UUID id;     // UUID PK
    std::string name;
    [[= many_to_many<>]] std::vector<Course> courses;  // m2m joins through junction
};

// Eager-load m2m with UUID-PK owner — stitch map keys on UUID, not int64_t
auto students = QuerySet<Student>()
    .join<fields::Student.courses>()
    .select();

for (const auto& student : students) {
    for (const auto& course : student.courses) {
        std::cout << student.name << " takes " << course.title << "\n";
    }
}
```

### Gotchas: UUID Primary Keys

**Q: My UUID PK insert fails with "Primary key UUID must be explicitly set"**

A: UUID PKs require explicit generation. This is intentional — Storm doesn't auto-generate UUIDs like it does AUTOINCREMENT integers, so you must call `storm::UUID::generate()` at the call site:

```cpp
// ❌ Wrong — empty UUID not allowed for PKs
User user{.name = "Alice"};
QuerySet<User>().insert(user).execute();  // Error: empty PK UUID

// ✅ Correct — explicitly set the UUID
User user{.id = storm::UUID::generate(), .name = "Alice"};
QuerySet<User>().insert(user).execute();  // OK
```

This ensures the caller is aware of (and can control) the generated key. On INSERT, a non-PK UUID
column left unset is still auto-generated (same as any non-PK field) — the PK-specific rejection
above is only about the primary key itself. A `.where()`/`.having()` filter is different: ANY UUID
column compared against an unset value rejects it too (#609), because auto-generating a random
UUID to compare against is never meaningful in a filter position.

**Q: Can I use composite keys with UUID?**

A: Yes. Mix UUID and integer parts:

```cpp
struct OrderLine {
    [[= primary_part]] storm::UUID order_id;
    [[= primary_part]] int product_id;
    int quantity;
};

// Caller supplies both parts
OrderLine line{.order_id = storm::UUID::generate(), .product_id = 42, .quantity = 1};
QuerySet<OrderLine>().insert(line).execute();  // → std::expected<void, Error>
```

Composite keys follow the same rules as integer composites (#500): every part is caller-supplied, `AUTOINCREMENT`/`IDENTITY` is not emitted, and INSERT returns `void`.

`storm::UUID` is one option for a text-shaped part, not the only one — a plain
`std::string` part is equally valid (#517). Use `storm::UUID` when you want RFC 4122
validation on every write; use `std::string` for natural keys like a SKU or country code.
See [Part types](#part-types-517).

**Q: Can I upsert on a UUID PK?**

A: Yes. Use `on_conflict<fields::Model.id>()` with the UUID PK member:

```cpp
User user{.id = storm::UUID::generate(), .name = "Alice"};
QuerySet<User>()
    .insert(user)
    .on_conflict<fields::User.id>()
    .update<fields::User.name>()
    .execute();
// On conflict, update the name; insert if no conflict
```

Return type follows composite-key upsert rules (#503): `std::expected<void, Error>` (no RETURNING).

## Optional Types (NULL Support)

| C++ Type | SQLite Type | Binding Method | Extraction Method |
|----------|-------------|----------------|-------------------|
| `std::optional<T>` | NULL / T's type | `bind_null()` / recursive | `is_null()` check |

**Supported optional types:**
- `std::optional<int>`
- `std::optional<int64_t>`
- `std::optional<double>`
- `std::optional<float>`
- `std::optional<bool>`
- `std::optional<std::string>`
- `std::optional<std::vector<uint8_t>>`

**Behavior:**
- `std::nullopt` → SQLite NULL
- Value present → Recursively binds the contained value

**Usage:**
```cpp
struct Contact {
    [[=primary]] int id;
    std::string name;
    std::optional<std::string> email;     // Can be NULL
    std::optional<int> age;               // Can be NULL
    std::optional<bool> is_verified;      // Can be NULL
};

// Examples
Contact c1{1, "Alice", "alice@example.com", 25, true};      // All fields set
Contact c2{2, "Bob", std::nullopt, std::nullopt, std::nullopt}; // NULLs
Contact c3{3, "Charlie", "charlie@example.com", std::nullopt, false}; // Mixed
```

## BLOB Types (Binary Data)

| C++ Type | SQLite Type | Binding Method | Extraction Method |
|----------|-------------|----------------|-------------------|
| `std::vector<uint8_t>` | BLOB | `bind_blob()` | `extract_blob()` |
| `std::vector<unsigned char>` | BLOB | `bind_blob()` | `extract_blob()` |

**Usage:**
```cpp
struct FileData {
    [[=primary]] int id;
    std::string filename;
    std::vector<uint8_t> data;
};

// Example with binary data
std::vector<uint8_t> binary_data = {0x89, 0x50, 0x4E, 0x47}; // PNG header
FileData file{0, "image.png", binary_data};
```

## Automatic Timestamps (`auto_create` / `auto_update`)

Two field attributes populate `std::chrono::system_clock::time_point` columns with
the current time automatically, so you never set them by hand (#209):

| Attribute | INSERT | UPDATE |
|-----------|--------|--------|
| `auto_create` | set to `now()` | preserved (bound from the object's stored value) |
| `auto_update` | set to `now()` | set to `now()` |

```cpp
struct User {
    [[=primary]] int id;
    std::string name;
    [[=storm::auto_create]] std::chrono::system_clock::time_point created_at;
    [[=storm::auto_update]] std::chrono::system_clock::time_point updated_at;
};

// INSERT — both stamped automatically; any value you set is ignored.
QuerySet<User>().insert(User{.name = "John"}).execute();
// row: created_at = now, updated_at = now

// UPDATE — only updated_at re-stamped; created_at preserved.
User u{.id = 1, .name = "Jane", .created_at = original_created_at};
QuerySet<User>().update(u).execute();
// row: created_at unchanged, updated_at = now
```

**Contract and constraints:**

- **Bind-time only, no write-back.** The value is computed in C++ (`system_clock::now()`)
  and bound as a parameter. The caller's in-memory object is **not** mutated — re-SELECT
  the row to read the stamped value.
- **Preserving `created_at` on UPDATE** requires the object to carry its original
  `created_at` (UPDATE binds it from the object). Load-modify-save, or pass the value
  you read on INSERT.
- **One `now()` per batch.** A bulk INSERT/UPDATE reads the clock once and shares it
  across every row, so all rows in a batch get the same timestamp.
- **Type-checked at compile time.** An `auto_create`/`auto_update` field that is not a
  `std::chrono::system_clock::time_point` fails to compile with a clear message.
- **Never on a primary key** (#511). A key that stamps itself would be rewritten by the
  statement matching on it, so `auto_create`/`auto_update` on a `primary`,
  `primary_autoincrement`, or `primary_part` member is a compile-time error
  (`ModelTimestampPkValid`) rather than a silently ignored annotation.
- **Zero cost when unused.** Models without any timestamp field do not pay for the
  clock read — the call compiles away.

The column maps to `TIMESTAMP` (PostgreSQL) / `TEXT` (SQLite) and round-trips via the
existing `time_point` ↔ `"YYYY-MM-DD HH:MM:SS"` conversion.

## Many-to-Many Container Fields (`many_to_many<>` / `many_to_many_through`)

A container member annotated with `[[= many_to_many<>]]` (or
`many_to_many_through<JunctionModel>`) declares a many-to-many relationship (#203):

```cpp
struct Student {
    [[= primary]] int id{};
    std::string name;
    [[= many_to_many<>]] std::vector<Course> courses;
};
```

- **Not a column.** The member maps to a junction table, not to a column —
  `INSERT`/`SELECT`/`UPDATE`/schema generation skip it entirely. Plain `select()`
  leaves it empty; `join<fields::Student.courses>()` eager-loads it.
- **Supported containers:** `std::vector<T>`, `plf::hive<T>`, `std::deque<T>`,
  and smart-pointer elements (`std::vector<std::shared_ptr<T>>`); the related
  model type is extracted via C++26 `std::meta`.
- **Junction DDL** is auto-generated for the `many_to_many<>` form (see
  [JOIN_OPERATIONS.md](../features/JOIN_OPERATIONS.md#many-to-many-joins-203)).

## Persistable vs. Filterable

Every type on this page is **persistable and readable** — you can `INSERT` it and `SELECT`
it back. A **narrower** set is **filterable in a WHERE clause** (`fields::T.field == …`,
`.between(…)`, `.in(…)`): the expression system stores operands in a closed variant, so a
type must have a variant arm to be filtered on (#407). Temporal types
(`year_month_day`, `system_clock::time_point`) and `storm::UUID` are both persistable **and**
filterable; `std::chrono::duration`, `std::filesystem::path`, and BLOB are persistable but
**not** filterable. See the full filterability table in
[WHERE_CLAUSES.md → Filterable field types](../features/WHERE_CLAUSES.md#filterable-field-types).

## Type Dispatch Implementation

The binding uses compile-time `if constexpr` type dispatch to select the appropriate SQLite binding function with **zero runtime overhead**.

```cpp
template <typename U>
static constexpr auto bind_value_by_type(Statement& stmt, int index, const U& value) -> bool {
    if constexpr (std::is_same_v<U, int>) {
        return stmt.bind_int(index, value);
    } else if constexpr (std::is_same_v<U, int64_t> || std::is_same_v<U, long> || std::is_same_v<U, long long>) {
        return stmt.bind_int64(index, static_cast<int64_t>(value));
    } else if constexpr (std::is_same_v<U, double>) {
        return stmt.bind_double(index, value);
    } else if constexpr (std::is_same_v<U, bool>) {
        return stmt.bind_int(index, value ? 1 : 0);
    } else if constexpr (/* string types */) {
        return stmt.bind_text(index, value);
    } else if constexpr (/* optional types */) {
        // Handle std::optional recursively
    } else if constexpr (/* blob types */) {
        return stmt.bind_blob(index, value.data(), value.size());
    }
}
```

### `BindableType` concept (#473)

> Not exported by `import storm;` (#679) — add `import storm_orm_value_bind;` to name it directly. See [MODULE_SYSTEM.md Step 5](../../internals/architecture/MODULE_SYSTEM.md#step-5-import-storm-exports-only-the-public-surface).

The parameter binder (`bind_parameter_value`) and the WHERE operand path
(`normalize_operand`) are both constrained by
`storm::orm::utilities::BindableType<T>` — a concept whose disjunction mirrors the
`if constexpr` dispatch arms exactly (integer/64-bit-integer, `enum`, `bool`,
`double`/`float`, chrono date/datetime/duration, `filesystem::path`, `UUID`, text,
BLOB byte-vectors, and `std::optional<U>` where `U` is itself bindable). Passing an
unsupported operand type now fails at the call site with a clear constraint
violation instead of deep inside the binder's dispatch. Composed from the same
`is_*_source_v` predicates the dispatcher branches on, so the concept cannot drift
from what actually binds.

## Future Type Support

Planned additions:
- `std::chrono::time_point` (as INTEGER or TEXT)
- `std::chrono::duration` (as INTEGER)
- `std::filesystem::path` (as TEXT)
- Custom types via user-defined conversions
- `std::span<uint8_t>` for BLOB (non-owning)

## Table Creation Guidelines

When creating tables, ensure column types match the C++ type mappings:

```sql
CREATE TABLE Example (
    id INTEGER PRIMARY KEY,  -- AUTOINCREMENT is opt-in (#379): primary_autoincrement
    name TEXT NOT NULL,
    age INTEGER,
    salary REAL,
    is_active INTEGER,  -- bool
    email TEXT,         -- std::optional<std::string> (can be NULL)
    data BLOB
);
```

## Type Safety

Storm ORM provides compile-time type safety:
- Incorrect type usage → Compilation error
- No runtime type checking overhead
- SQLite type affinity automatically handled

## Model Type Validation (`Entity`) (#472)

`storm::meta::Entity<T>` is the compile-time **structural** gate for model types — true iff
`T` is a reflectable class:

```cpp
template <typename T>
concept Entity = std::meta::is_class_type(^^T) && requires {
    std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked());
    std::meta::identifier_of(^^T);
};
```

`QuerySet<T>` and `BaseStatement<T>` (and therefore every statement class) `require` it, so a
non-model `T` — `int`, a pointer, a function type — fails at this named boundary with a clear
constraint violation instead of deep inside reflection-based code.

`Entity` sits **above** and stays **separate from** the semantic model concepts on
`BaseStatement<T>` — `ModelWithPrimaryKey`, `ModelStorageAnnotated`, `ModelFkPoliciesValid`.
Those check model *policy* (a primary key exists, every `uint64_t` field is storage-annotated,
every `SET NULL` FK is nullable); `Entity` checks *reflectability*. They are deliberately not
merged: `QuerySet<T>` must be usable without requiring a primary key, so it needs the structural
gate alone.

`Entity` is structural only — a `union` also satisfies it. Full model-ness is guaranteed by the
semantic concepts together with `Entity`, not by `Entity` alone.

## Field Selector Validation (`ValidFieldInfo`) (#478)

`storm::meta::ValidFieldInfo<MemberInfo>` is the compile-time gate that a `std::meta::info`
**NTTP** names a real field — a non-static data member that has an identifier:

```cpp
template <std::meta::info MemberInfo>
concept ValidFieldInfo =
    std::meta::is_nonstatic_data_member(MemberInfo) && std::meta::has_identifier(MemberInfo);
```

The field selector `fields::Model.member` and its `Field` / `CollatedField` proxies `require` it,
so a selector that is not a field — a static member, a member function, a whole-type reflection
such as `^^Person`, or a scalar like `^^int` — fails at this named boundary instead of deep inside
`identifier_of` / `type_of`. Both requirements are load-bearing: on the clang-p2996 build,
`is_nonstatic_data_member(^^int)` and `is_nonstatic_data_member(^^Type::static_member)` are both
`false`, so the concept genuinely rejects (contrast `Entity`, whose structural requirements alone
accept `int` and rely on `is_class_type` to gate).

`ValidFieldInfo` is orthogonal to `Entity` (which gates the whole model *type*) and to the selector
call site's extra `!is_relation_field(MemberInfo)` check, which excludes `many_to_many` /
`reverse_fk` container members that are not persisted columns — that check is still ANDed alongside
`ValidFieldInfo` at each selector site.

## Foreign Key Target Validation (`ValidForeignKey`) (#474)

> Not exported by `import storm;` (#679) — add `import storm_orm_model_concepts;` to name it directly. See [MODULE_SYSTEM.md Step 5](../../internals/architecture/MODULE_SYSTEM.md#step-5-import-storm-exports-only-the-public-surface).

`storm::orm::statements::ValidForeignKey<FieldType>` is the compile-time gate that an FK field's
target has a primary key — a plain alias over the existing model concept, unwrapping one level of
`std::optional` first (an FK field is often `std::optional<Related>`):

```cpp
template <typename FieldType>
concept ValidForeignKey = ModelWithPrimaryKey<utilities::optional_inner_type_t<FieldType>>;
```

`join<>()` / `left_join<>()` and `find_fk_primary_key<FKType>()` `require` it, so calling
`join<fields::Message.sender>()` when `sender`'s target type has no primary key fails at the join call
site instead of deep inside FK-column extraction. It is single-level only — it checks the target's
own PK, never recursing into the target's FKs (a Base⟷Owner reference cycle would otherwise never
terminate). Loop bodies that walk members at runtime (e.g. `FKFieldOf`) can't splice a loop variable
into `ValidForeignKey<typename[:...:]>`, so `model_concepts.cppm` also carries an info-value twin,
`valid_fk_target(std::meta::info fk_type) -> bool`, computing the same "target, optional-unwrapped,
has a PK" check for that path.

## Numeric Aggregate Validation (`NumericAggregateable`) (#475)

> Not exported by `import storm;` (#679) — add `import storm_orm_statements_aggregate;` to name it directly. See [MODULE_SYSTEM.md Step 5](../../internals/architecture/MODULE_SYSTEM.md#step-5-import-storm-exports-only-the-public-surface).

`storm::orm::statements::NumericAggregateable<T>` is the compile-time gate on the target field(s)
of `sum()` / `avg()` / `min()` / `max()` — true for an arithmetic, non-`bool` type, unwrapping one
level of `std::optional` (a nullable numeric column, e.g. `std::optional<int>`, is a legitimate
aggregate target; SQL simply skips NULLs):

```cpp
template <typename T>
constexpr bool is_numeric_aggregateable_v =
        utilities::is_optional_v<T> ? is_numeric_scalar_v<utilities::optional_inner_type_t<T>>
                                    : is_numeric_scalar_v<T>;

template <typename T>
concept NumericAggregateable = is_numeric_aggregateable_v<T>;
```

`AllNumericAggregateable<FieldInfos...>` folds this over the whole field pack (multi-field
aggregates sum/min/max several columns, e.g. `SUM(a + b)`) and is the `requires`-clause on
`QuerySet::sum/avg/min/max`. A string/BLOB/enum/UUID/temporal/`bool` field is a **compile error**
at the aggregate call site, not a silent coercion. `count()` / `count_distinct()` are unconstrained
— `COUNT` is type-agnostic and accepts any column.

## Primary Key Type Validation (`PrimaryKeyType`) (#505)

> Not exported by `import storm;` (#679) — add `import storm_orm_model_concepts;` to name it directly. See [MODULE_SYSTEM.md Step 5](../../internals/architecture/MODULE_SYSTEM.md#step-5-import-storm-exports-only-the-public-surface).

`storm::orm::statements::PrimaryKeyType<T>` is the compile-time gate on the type of `T`'s
primary-key member (the field annotated `primary` / `primary_autoincrement`).
**Supported primary-key types: `short`, `int`, `long`, `long long`**, their fixed-width
spellings (`std::int16_t`, `std::int32_t`, `std::int64_t`, …), and **`storm::UUID`** (#507).
A `uint64_t` (or `unsigned long` / `unsigned long long`) primary key is
accepted only when explicitly annotated `storm::signed_storage`; `storm::full_unsigned` is
rejected even though it is a storage annotation, because it stores as zero-padded `TEXT`,
which would silently misread through every hardcoded-`int64` primary-key extraction site
(`select.cppm`, `join.cppm`, `insert.cppm`).

Rejected, each for a specific reason:
- **`bool`** — integral, so a naive `std::is_integral_v` check would admit it, but a
  two-valued primary key is nonsense. Same carve-out `NumericAggregateable` makes (#475).
- **`char` / `signed char`** — a 1-byte identity is a pathological primary key; not in the
  accepted width list.
- **Unsigned types without `storm::signed_storage`** — a bare `uint64_t` primary key is
  already a compile error via `ModelStorageAnnotated` (#436); this concept additionally
  rejects `full_unsigned` specifically as a primary-key type (see above).
- **`std::optional<T>`** — a nullable primary key is meaningless; rejected outright, unlike
  `ValidForeignKey`/`is_unsigned64_member`, which unwrap `std::optional` before checking.
- **Text types** (`std::string`, `std::string_view`) — not supported as a primary key today.
  Tracked as a future enhancement.

**UUID Primary Keys** (#507):
`storm::UUID` is now a fully supported primary-key type. UUIDs are **client-generated** (not
DB-generated like `AUTOINCREMENT`), so `insert().execute()` returns `std::expected<void, Error>`
with no RETURNING clause (see "UUID Primary Keys" section above). Empty UUID PK values are rejected
at bind time with an error — the caller must explicitly call `storm::UUID::generate()` before
insert. FK columns, m2m, and reverse-FK joins work identically to integer PKs with full type safety
and zero codegen overhead.

```cpp
template <typename T>
concept PrimaryKeyType = /* T's primary-key member's type is in the allowed set above */;
```

`PrimaryKeyType<T>` is ANDed into `BaseStatement<T>`'s constraint list alongside
`ModelWithPrimaryKey<T>`. `QuerySet<T>` itself only requires `Entity<T>` (it must stay
usable without a primary key), so a model with an unsupported primary-key type — `bool`,
`std::optional<int>`, a bare `unsigned int`, `std::string` — compiles as a
bare `QuerySet<T>` but fails at the first call that instantiates a statement
(`select()`, `insert()`, `join()`, …), naming `T` and `PrimaryKeyType` in the "constraints
not satisfied" diagnostic trail. This replaces the previous behavior of failing
inconsistently — sometimes at runtime with no mention of the primary key at all, sometimes
at compile time deep inside `select.cppm` / `join.cppm` / `insert.cppm`.
