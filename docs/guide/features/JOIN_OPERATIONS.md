# JOIN Operations

Storm ORM supports efficient single and multi-FK JOIN operations using a type-erased SQL builder pattern that achieves **77% average efficiency** compared to raw SQLite.

## Overview

Key features:
- **Single and multi-FK JOINs** - Variadic template handles both cases
- **Type erasure without std::function** - Uses abstract base class pattern
- **Compile-time SQL generation** - Zero runtime SQL construction
- **Automatic FK population** - Fully populates joined objects
- **77% average efficiency** - Near-raw SQLite performance

## Basic Usage

### Single FK JOIN

```cpp
struct User {
    [[=primary]] int id;
    std::string username;
    int level;
};

struct Message {
    [[=primary]] int id;
    std::string content;
    User sender;  // FK field
};

storm::QuerySet<Message> message_qs(conn);

// Populates sender field fully for each message
auto result = message_qs.join<fields::Message.sender>().select().execute();
if (result) {
    for (const auto& msg : result.value()) {
        std::cout << msg.content << " from " << msg.sender.username << std::endl;
    }
}
```

**SQL Generated**:
```sql
SELECT t1.id, t1.content, t1.sender_id, t2.id, t2.username, t2.level
FROM Message t1
INNER JOIN User t2 ON t2.id = t1.sender_id
```

**Performance**: 4.4M rows/sec (59% of raw SQLite for INNER JOIN)

### Multi-FK JOIN

```cpp
struct Message {
    [[=primary]] int id;
    std::string content;
    User sender;    // First FK
    User receiver;  // Second FK
};

// Populates both sender and receiver fields
auto result = message_qs.join<fields::Message.sender, fields::Message.receiver>().select().execute();
if (result) {
    for (const auto& msg : result.value()) {
        std::cout << msg.sender.username << " → "
                  << msg.receiver.username << ": "
                  << msg.content << std::endl;
    }
}
```

**SQL Generated**:
```sql
SELECT t1.id, t1.content, t1.sender_id, t1.receiver_id,
       t2.id, t2.username, t2.level,
       t3.id, t3.username, t3.level
FROM Message t1
INNER JOIN User t2 ON t2.id = t1.sender_id
INNER JOIN User t3 ON t3.id = t1.receiver_id
```

**Performance**: 3.7M rows/sec (62% of raw SQLite for INNER JOIN)

## JOIN Types

Storm ORM supports INNER and LEFT JOIN. RIGHT JOIN was removed in #397: its only
distinguishing output — unmatched related-side rows materialized as defaulted base
entities — was not useful. The related-side query ("all users, each with the
messages pointing at them") is tracked as a reverse-relation feature in #398.

### INNER JOIN (Default)

Only returns rows where FK relationship exists:

```cpp
auto result = message_qs.join<fields::Message.sender>().select().execute();
```

**Use when**: You only want messages with valid senders

### LEFT JOIN

Returns all base table rows, FK fields may be NULL:

```cpp
auto result = message_qs.left_join<fields::Message.sender>().select().execute();
```

**Use when**: You want all messages, even those without senders
**Note**: FK fields must use `std::optional<User>` for NULL safety

## Many-to-Many Joins (#203)

Many-to-many relationships use a container field annotated with `many_to_many<>`
(auto-generated junction table) or `many_to_many_through<Model>` (explicit
junction model). The same `join<^^Field>()` API eager-loads the relationship with
a **predicate-pushdown two-query** strategy (#391) — no N+1 problem. See
[Execution Strategy](#execution-strategy-391) below for why two queries beat one.
Several m2m fields of the same model can be loaded in **one call** (#392) — see
[Multiple m2m relations](#multiple-m2m-relations-392).

### Phase 1: Auto-Generated Junction Table

```cpp
struct Course {
    [[= primary]] int id{};
    std::string title;
};

struct Student {
    [[= primary]] int id{};
    std::string name;
    int age{};
    [[= many_to_many<>]] std::vector<Course> courses;
};

// Two queries (in one transaction): base students, then their courses, stitched
// client-side by a pk→entity hash map.
auto students = QuerySet<Student>().join<fields::Student.courses>().select().execute();
for (const auto& s : *students) {
    // s.courses holds ALL of the student's courses
}
```

- The related type (`Course`) is extracted from the container via C++26 `std::meta`
  — `std::vector<T>`, `plf::hive<T>`, `std::deque<T>`, and
  `std::vector<std::shared_ptr<T>>` / `unique_ptr` elements all work.
- `create_table_if_not_exists<Student>` also creates the junction table
  `Student_Course (Student_id, Course_id, PRIMARY KEY(both))` — naming is
  `<OwnerTable>_<RelatedTable>` with `<Table>_id` columns.
- The m2m container member is **not a column**: plain `select()`, `insert()`,
  `update()` ignore it entirely (zero cost for models without m2m fields).
  Explicitly naming it as a SET target — `update<fields::Student.courses>(proto)`,
  `update_all<fields::Student.courses>(proto)`, or upsert
  `.on_conflict<…>().update<fields::Student.courses>()` — is **rejected at compile
  time** (#486): the `is_settable_member` gate on both grammars ANDs in
  `!is_relation_field(Member)`, so the error fires at the call site as a
  constraint violation instead of at runtime with "no such column". The same
  holds for reverse-FK containers. A relation member is likewise not a valid
  conflict target (it carries no `unique` annotation and cannot appear in a
  `UniqueIndex`).

**SQL Generated** (two statements, run inside one transaction):
```sql
-- Q1: the base entities to load (a plain SELECT — no join, no sorter)
SELECT id, name, age FROM Student;

-- Q2: their courses, filtered by the SAME base subquery; stitched by owner pk
SELECT t2.Student_id, t3.id, t3.title
FROM Student_Course t2 INNER JOIN Course t3 ON t2.Course_id = t3.id
WHERE t2.Student_id IN (SELECT id FROM Student)
```

`WHERE` / `ORDER BY` / `LIMIT` / `OFFSET` are applied to Q1 **and** repeated inside
Q2's `IN (…)` subquery, so both pick exactly the same base entities.

### Phase 2: Explicit Junction Model (metadata)

```cpp
struct Enrollment {
    [[= primary]] int id{};
    [[= fk<>]] Pupil pupil;
    [[= fk<>]] Course course;
    std::string grade;  // relationship metadata
};

struct Pupil {
    [[= primary]] int id{};
    std::string name;
    [[= many_to_many_through<Enrollment>]] std::vector<Course> courses;
};

// Simple access — metadata ignored, same two-query eager load
auto pupils = QuerySet<Pupil>().join<fields::Pupil.courses>().select().execute();

// Metadata access — query the junction model directly (regular FK joins)
auto enrollments = QuerySet<Enrollment>()
        .join<fields::Enrollment.pupil, fields::Enrollment.course>()
        .where(fields::Enrollment.grade == "A")
        .select().execute();
```

The junction table is the through model's own table; FK columns come from its
field names (`pupil_id`, `course_id`). The through model must have exactly one
`fk<>` field per side (enforced at compile time).

### Multiple m2m relations (#392)

A model may carry several m2m fields; one `join<>()`/`left_join<>()` call loads
any subset of them:

```cpp
struct Member {
    [[= primary]] int id{};
    std::string name;
    int age{};
    [[= many_to_many<>]] std::vector<Course> courses;
    [[= many_to_many<>]] std::vector<Club> clubs;
};

// One Q1 (base members) + one Q2 PER relation, all in one transaction:
auto members = QuerySet<Member>()
        .join<fields::Member.courses, fields::Member.clubs>()
        .select().execute();
```

- **Cost is additive** (`K1 + K2` related rows), never multiplicative — each
  relation is one extra Q2 keyed off the same Q1 `pk → entity` map. There is no
  `K1×K2` cartesian product anywhere on the eager-load path.
- **INNER** (`join`) drops a base entity if its container is empty in **any**
  listed relation; **LEFT** (`left_join`) keeps every base entity and fills each
  relation's container independently (Django semantics).
- `create_table_if_not_exists<Member>` creates **one junction table per
  auto-m2m field** (`Member_Course`, `Member_Club`).
- Duplicate fields (`join<fields::M.courses, fields::M.courses>`) and mixing FK fields
  with m2m fields in one call are rejected at compile time.

### Semantics

| Aspect | Behavior |
|---|---|
| `WHERE` / `ORDER BY` / `LIMIT` / `OFFSET` | Apply to **base entities** (Q1 and the Q2 IN-subquery) — `limit(1)` returns one student with ALL courses |
| `first()` / `get()` | Entity-level: `get()` errors only on 0 or >1 *entities*, never on multiple relations |
| `rows()` | Yields aggregated entities; **materialized eagerly** (Q2 needs the full base set, so true streaming is not possible) |
| `join` (INNER) | Drops base entities empty in **any** listed relation (a post-stitch filter) |
| `left_join` | Keeps them with empty container(s) (the natural case — Q1 already yielded them) |
| Aggregates (`count()` …) | Count (base, related…) **tuples** over the flat chained join (`get_complete_sql`) — pairs for one relation, cartesian tuples for several |
| Result order | The order Q1 returns base entities (`order_by` if given) — no pk tiebreak is needed, since the stitch is a hash map, not row-adjacency |
| Consistency | Q1 + Q2 run inside one transaction (snapshot consistency) |

### Limitations

- Self-referential m2m, duplicate fields in one call, and mixed FK + m2m in one
  call are rejected at compile time. INNER vs LEFT applies to the whole call —
  mixing per relation is not expressible (chaining a second `join()` call
  replaces the first, as for FK joins).
- Write-side helpers (`add()`/`remove()`) are not provided — insert junction
  rows via the through model (`QuerySet<Enrollment>`) or raw SQL for Phase 1.
- The annotation spelling differs from issue #203's sketch
  (`many_to_many<><T>` is impossible — the flag annotations are plain tag objects; the FK marker `fk<>` is likewise a class-template annotation).

### Execution Strategy (#391)

The m2m eager load runs as **two queries inside one transaction**, stitched
client-side by a `pk → entity` hash map (`SelectStatement::execute_m2m_2query`):

1. **Q1** selects the base entities (`build_base_subquery`) — a plain `SELECT`,
   no join, no sorter. Its results go into a `plf::hive<T>` (stable pointers),
   and a `std::unordered_map<int64_t, T*>` indexes them by primary key.
2. **Q2** — one per eager-loaded relation (#392) — selects `(owner_pk, related.*)`
   from that relation's junction ⋈ related table, filtered by
   `WHERE owner_id IN (<the same base subquery>)` (`build_q2_sql`).
   Each row is appended to its owner's container via the hash-map lookup.
3. **INNER** then drops entities whose container stayed empty in any inner
   relation; **LEFT** keeps them. Every Q2 is the *same* shape (an INNER
   junction ⋈ related join) — the INNER/LEFT difference is a post-stitch
   filter, never an SQL difference.

**Why two queries beat one.** The former single-query plan was a 3-table join over
a base-table subquery that ended in `ORDER BY t1.<pk>` so one entity's rows were
adjacent (the aggregation loop relied on that). SQLite executes that `ORDER BY`
with a `USE TEMP B-TREE FOR ORDER BY` — *every* one of the `N×K` result rows,
including the duplicated base columns, passes through the sorter. The two-query
plan never sorts (the stitch is a hash map) and reads each base row's columns
once. Measured in-Storm (Release, fan-out sweep, N=100 base entities):

| fan-out | rows | 1-query | 2-query | speedup |
|---|---|---|---|---|
| 1 | 100 | 24.0 µs | 26.1 µs | −8.7% (FK-shaped; least representative) |
| 10 | 1,000 | 190 µs | 127 µs | **+33.5%** |
| 50 | 5,000 | 907 µs | 519 µs | **+42.8%** |
| 200 | 20,000 | 3.70 ms | 2.01 ms | **+45.6%** |

Fan-out 1 (one related row per entity) is FK-shaped, not m2m-shaped, and pays a
~2 µs constant cost (the extra prepare + transaction); every representative m2m
fan-out wins by 33–46%. `count()` and other aggregates still run the modifier-free
chained join (`get_complete_sql`) — `(base, related)` pairs for one relation,
cartesian tuples when several relations are joined (#392).

## Reverse-FK Joins (#398)

Storm's other join selectors start from the base model: FK fields (`fields::Task.assignee`)
or m2m containers (`fields::Student.courses`). A **reverse-FK join** starts from the
*related* side: "all Persons, each with the Tasks that point at them". The SQL
identity is `Task RIGHT JOIN Person ≡ Person LEFT JOIN Task` — Storm expresses the
right-hand side directly, with the base model (`Person`) on the correct side.

### 1. Annotated container destination (`select()`)

`select()` returns `hive<Person>`, so `Person` needs a member to receive the tasks.
Declare a reverse-FK container (not a column — invisible to CRUD, like m2m):

```cpp
struct Task; // forward declaration breaks the Person⟷Task reference cycle

struct Person {
    [[= primary]] int id{};
    std::string name;
    int age{};
    // Filled on eager load. The annotation names the OWNER TYPE; the unique FK
    // back at Person is resolved at instantiation.
    [[= storm::reverse_fk<^^Task>]] std::vector<Task> tasks;
};

struct Task {
    [[= primary]] int id{};
    std::string title;
    [[= fk<>]] Person assignee;   // FK → Person
};

person_qs.left_join<fields::Person.tasks>().select();   // all Persons; .tasks empty when none
person_qs.join<fields::Person.tasks>().select();        // INNER: drops Persons with no Tasks
```

The annotation argument is **the owning model type** (`^^Task`), resolved to that
model's unique `fk<>` member pointing back at `Person`. The type form (not
a member splice) is required: `Person` holds `vector<Task>` and `Task` holds `Person`
by value, a reference cycle that only compiles when `Task` is forward-declared at the
annotation site — which rules out a `^^Task::assignee` member splice there.
The container element may be `T`, `plf::hive<T>`, or `vector<shared_ptr<T>>`, as for m2m.

### 2. Cross-model FK selector (aggregate / filter chains)

`join`/`left_join` also accept an FK field **of another model** that points at the
base. No destination is needed because these chains never materialize base+related
entities — they absorb the aggregate-over-RIGHT-JOIN capability that `right_join`
(removed in #397) pretended to offer:

```cpp
// Tasks per person, INCLUDING persons with zero tasks (LEFT keeps the NULL row)
QuerySet<Person>().left_join<fields::Task.assignee>().count().execute();

// Disambiguation: when the owner has several FKs to the base, name the exact field
QuerySet<Reporter>().join<fields::Bug.author>().count().execute();    // ON t2.author_id = t1.id
QuerySet<Reporter>().join<fields::Bug.reviewer>().count().execute();  // ON t2.reviewer_id = t1.id
```

### Execution & semantics

Identical to m2m (#391): `select()` runs **two queries in one transaction** stitched
by a `pk → entity` hash map. Q1 is the base subquery; Q2 hits the **owning table
directly** (no junction) —
`SELECT t2.<fk>_id, t2.<owner cols> FROM <Owner> t2 WHERE t2.<fk>_id IN (<base subquery>)`.
WHERE / ORDER BY / LIMIT / OFFSET bound the **base** entities; INNER drops
zero-relation entities after the stitch, LEFT keeps them. Multi-relation joins
compose per #392 (one Q2 per relation, additive cost). Aggregates / anti-joins use
the modifier-free complete SQL `Person t1 <KW> Owner t2 ON t2.<fk>_id = t1.<pk>`.

### Limitations

- **`select()` disambiguation** of several owner FKs to the base is not expressible:
  the destination annotation must use the owner *type* (the reference cycle forbids a
  member splice), so the owner must have exactly **one** FK back at the base.
  Multi-FK disambiguation is available only on the aggregate/filter selector path
  (`fields::Bug.author` vs `fields::Bug.reviewer`), which has no container and no cycle.
- Eager only (no lazy loading), no tuple/pair result shapes, no `right_join`-style
  base rows with defaulted fields (removed in #397).
- The annotation spelling carries the owner type, not `reverse_fk<>` —
  the flag annotations are plain tag objects, so a templated flag is impossible (as for m2m).

## Composite Primary Keys in JOINs (#504)

Every join path above works when a model has a composite primary key
(two or more `[[= primary_part]]` members, #500). No API changes — the
same `join<>()` / `left_join<>()` calls apply.

### FK columns

An FK to an N-part target occupies **N columns**, named `<member>_<part>`:

```cpp
struct OrderLine {
    [[= primary_part]] int order_id{};
    [[= primary_part]] int product_id{};
    int quantity{};
};

struct Shipment {
    [[= primary_autoincrement]] int id{};
    [[= fk<>]] OrderLine line;   // → line_order_id, line_product_id
};
```

A single-PK target keeps the exact `<member>_id` spelling it always had, so
pre-#504 schemas and SQL are byte-identical. `std::optional<OrderLine>` makes
all N columns nullable together.

### ON clauses

Joins AND-join one equality per key part, in declaration order:

```sql
INNER JOIN OrderLine t2
        ON t2.order_id   = t1.line_order_id
       AND t2.product_id = t1.line_product_id
```

Matching only the first part would silently join rows that share it but differ
later, so all parts are always compared.

### Junction tables (m2m)

A composite side contributes one junction column per PK part, `<Side>_<part>`.
Composite PK is supported on **either or both** sides:

```sql
CREATE TABLE ShelfAssignment_StorageBin (
    ShelfAssignment_warehouse_no INTEGER NOT NULL,
    ShelfAssignment_shelf_code TEXT NOT NULL,
    StorageBin_aisle INTEGER NOT NULL,
    StorageBin_bin_code TEXT NOT NULL,
    StorageBin_revision INTEGER NOT NULL,
    PRIMARY KEY (ShelfAssignment_warehouse_no, ShelfAssignment_shelf_code,
                 StorageBin_aisle, StorageBin_bin_code, StorageBin_revision),
    FOREIGN KEY (ShelfAssignment_warehouse_no, ShelfAssignment_shelf_code)
        REFERENCES ShelfAssignment(warehouse_no, shelf_code) ON DELETE CASCADE,
    FOREIGN KEY (StorageBin_aisle, StorageBin_bin_code, StorageBin_revision)
        REFERENCES StorageBin(aisle, bin_code, revision) ON DELETE CASCADE
)
```

(The real DDL puts each `PRIMARY KEY` / `FOREIGN KEY` clause on one line; wrapped
here for readability.)

Each `FOREIGN KEY` names the target's real key columns. This also fixed a latent
bug: the old DDL emitted `REFERENCES <Side>(id)`, naming a column a composite-PK
model does not have. SQLite does not validate FK targets at `CREATE TABLE` time
so it accepted this silently, but PostgreSQL rejects it — composite-PK junction
tables could not be created on PG at all.

A single-PK junction on both sides is unchanged, byte for byte.

### Through models (`many_to_many_through<>`, #536)

An explicit junction model works over composite keys too, but its columns follow
a **different naming rule** from the auto-junction above, because it is a
different kind of table. The auto-junction is Storm's own synthetic table, so
Storm names its columns (`<Side>_<part>`, an FK part gaining `_id`). A through
model is a **real model you declared**, so its junction columns are ordinary
composite-FK columns, named by the FK rule at the top of this section —
`<member>_<part>`, always the target part's **bare** identifier:

```cpp
struct Entry {
    [[= primary_part]][[= fk<>]] Warehouse warehouse;
    [[= primary_part]] int sku{};
    [[= many_to_many_through<EntryLink>]] std::vector<Topic> topics;
};

struct EntryLink {
    [[= primary_autoincrement]] int id{};
    [[= fk<>]] Entry entry;   // → entry_warehouse, entry_sku
    [[= fk<>]] Topic topic;   // → topic_id
};
```

Note `entry_warehouse`, **not** `entry_warehouse_id`, even though `warehouse` is
itself an FK — the `_id` suffix belongs to the auto-junction rule, not this one.
The two rules agree for a single-column PK and for a composite key of plain
columns; they diverge exactly here, which is why the query path selects between
them rather than sharing one.

Because the junction is a model, you write its rows with an ordinary `insert()`
and can query it directly for its own metadata — no raw SQL, unlike the
auto-junction.

The FK-part shape above also needs the part's **storage type** resolved: the
column holds the referenced row's key (`BIGINT`), not the whole struct. Getting
that wrong typed the column as `TEXT`, which SQLite accepted and silently never
matched, while PostgreSQL rejected the `CREATE TABLE` outright — the same
SQLite-accepts/PG-rejects asymmetry as the junction `REFERENCES` bug above.

### Stitching

The two-query strategy (#391) stitches Q2 rows onto Q1 entities by owner key.
A composite owner key is not one integer and may mix `int`, `std::string`, and
`int64_t` parts, so those models key the stitch map on a `StitchKey` — a fixed
32-byte inline buffer holding every part.

A **single-column PK keeps the plain `int64_t` key** it always had, chosen at
compile time. This is a deliberate performance split, not an implementation
detail: `std::hash<long>` is the identity function and comparing two `long`s is
one inlined instruction, whereas both operations on a 33-byte key become
out-of-line calls made once per result row. Routing single-PK models through
`StitchKey` measured ~4% slower on the m2m eager-load benchmarks.

The Q2 `IN` subquery uses the row-value form:

```sql
-- reverse-FK Q2 over a 2-part owner key
WHERE (t2.line_order_id, t2.line_product_id)
   IN (SELECT order_id, product_id FROM OrderLine)
```

Note the inner `SELECT` list is deliberately **not** parenthesized — wrapping it
makes SQLite parse it as a single scalar expression instead of N columns.

### Notes

- An FK member may itself be a `primary_part`; it stores the referenced key's
  type, so a composite key over FKs (the usual association-table shape) works.
- A key part that is an FK emits its `_id` column name, e.g. `warehouse_id`.
- Composite keys are never DB-generated, so every part is caller data on INSERT
  (#502).

## Architecture

### Type-Erased SQL Builder Pattern

Storm uses an abstract base class for type erasure without `std::function` (which causes linker issues with custom libc++):

```cpp
// Abstract base for type erasure
class IJoinStatement {
    virtual std::string to_sql() const = 0;
    virtual std::string build_qualified_select_fields() const = 0;
    virtual void extract_row(void* stmt, void* obj) const = 0;
};

// Unified variadic template (handles single + multi FK)
template <typename T, ConnType, std::meta::info... FKFields>
class JoinStatement : public IJoinStatement {
    // Pure SQL builder - no execute(), no caching

    std::string to_sql() const override {
        // Generate: " INNER JOIN table t2 ON t2.id = t1.fk_id"
        // Uses fold expressions for variadic FKFields
    }

    void extract_row(void* stmt, void* obj) const override {
        // Extract FK fields using compile-time type information
    }
};
```

### QuerySet Integration

QuerySet stores the JOIN statement using type erasure:

```cpp
template <class T> class QuerySet {
    mutable std::unique_ptr<IJoinStatement> join_stmt_;

    template <std::meta::info... FKFields>
    auto&& join(this auto&& self) {
        self.join_stmt_ = std::make_unique<JoinStatement<T, ConnType, FKFields...>>();
        return self;  // Chainable
    }

    auto select() {
        // Builds SELECT t1.*, t2.* FROM table t1 + join_stmt_->to_sql() when a
        // JOIN is attached, plain SELECT otherwise.
    }
};
```

### SelectStatement Integration

There is no per-Statement or per-QuerySet JOIN cache. SELECT (with or without a
JOIN) goes through the single Connection-level prepared-statement cache, keyed
by SQL text; the L1/L2 layers were removed in #214. See
[STATEMENT_CACHING.md](../../internals/architecture/STATEMENT_CACHING.md).

## Performance Analysis

### Benchmark Results (10,000 rows, 100 iterations, Release build)

| JOIN Operation | Storm ORM | Raw SQLite | Efficiency |
|----------------|-----------|------------|------------|
| Simple SELECT (no JOIN) | 8.4M rows/sec | - | - |
| LEFT JOIN (single FK) | 6.1M rows/sec | 6.8M rows/sec | 90% |
| LEFT JOIN (multi FK) | 3.9M rows/sec | 5.2M rows/sec | 75% |
| INNER JOIN (single FK) | 4.4M rows/sec | 7.4M rows/sec | 59% |
| INNER JOIN (multi FK) | 3.7M rows/sec | 6.0M rows/sec | 62% |

**Average Efficiency**: ~77% of raw SQLite performance

### Performance Characteristics

**LEFT JOINs**: 75-90% efficiency (excellent)
- Less object construction complexity
- Simpler NULL handling

**INNER JOINs**: 59-62% efficiency (good)
- More complex object construction
- Higher allocation overhead

### Performance Bottlenecks

The real performance bottlenecks are **NOT function pointers** (we tested this):

1. **String allocations** (30-40% of runtime)
   - Each `std::string` field requires heap allocation
   - Multiple string fields per joined object

2. **Object construction** (~10%)
   - Creating and populating complex objects
   - Multiple fields per object

3. **Vector management**
   - Resizing, copying, moving objects in result vectors

4. **Multi-column extraction** (~35%)
   - 5-8 columns for JOIN vs 2-3 for simple SELECT
   - Type dispatch overhead for each field

5. **SQL execution** (~20%)
   - Unavoidable, same as raw SQLite

### Template vs Function Pointer Experiment

We attempted to eliminate function pointer overhead using templates to preserve FK information at compile-time:

**Results: Templates Made It WORSE**
- **Function pointer approach** (current): 6.9M rows/sec (70% of raw) ✅ **BEST**
- **Template approach** (attempted): 4.9M rows/sec (49% of raw) ❌ **28% SLOWER**

**Why templates failed:**
1. **Code bloat** - More template instantiations → worse instruction cache locality
2. **Compiler optimization** - Modern compilers optimize indirect calls well with PGO
3. **Inlining limits** - Deep call chains hit compiler inlining budget
4. **Register pressure** - Fully inlined code increased register spilling

**Key lesson**: Don't assume eliminating indirect calls improves performance. Profile first, optimize second.

## Optimization Recommendations

Current 50-70% efficiency is **respectable for a full ORM** with reflection-based mapping. Further gains require architectural changes:

### 1. String Handling (Biggest Potential Win)
- Use `std::string_view` for read-only operations
- Implement move semantics throughout extraction chain
- Consider string interning for repeated values
- Arena allocator for temporary strings

### 2. Memory Management
- Object pooling to reuse allocated objects
- Custom allocator optimized for ORM access patterns
- Better size estimation (currently pre-allocates 10K)

### 3. Column Extraction
- Batch extraction by type (all ints, then all strings)
- SIMD for type checking and conversion
- Specialized fast paths for common type combinations

### 4. Caching Improvements
- Cache field offset calculations
- Reuse extraction buffers across queries
- Pre-build type dispatch tables at compile-time

## Chaining with WHERE

JOIN operations can be combined with WHERE clauses:

```cpp

// Filter on both base table and joined table
auto result = message_qs.join<fields::Message.sender>()
                        .where(fields::User.level > 5 and
                               fields::Message.content.like("%urgent%"))
                        .select();
```

**SQL Generated**:
```sql
SELECT t1.id, t1.content, t1.sender_id, t2.id, t2.username, t2.level
FROM Message t1
INNER JOIN User t2 ON t2.id = t1.sender_id
WHERE t2.level > 5 AND t1.content LIKE '%urgent%'
```

See [WHERE Clauses](WHERE_CLAUSES.md) for more details.

## Testing

Comprehensive JOIN tests in `tests/schema/test_fk_fields.cpp`:
- Single FK JOINs
- Multi-FK JOINs
- Full object population verification
- INSERT/UPDATE/DELETE with FK fields
- Batch operations with FK fields

## Benchmarking

Run JOIN performance benchmarks:

```bash
cmake --preset ninja-release -DENABLE_BENCH=ON
cmake --build --preset ninja-release

# Run all JOIN benchmarks (dataset sizes are configured in benchmarks/tests/benchmark_tests.yaml)
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/.*JOIN.*'

# Repeated runs for stats (median/mean/stddev)
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/.*JOIN.*' --benchmark_repetitions=10
```

See `benchmarks/query_benchmark.cppm` and `benchmarks/tests/benchmark_tests.yaml` for the JOIN benchmark definition.

## Key Benefits

✅ **No std::function** - Avoids custom libc++ linker errors
✅ **Single variadic template** - Not separate Single/Multi classes
✅ **Abstract base class** - Clean type erasure pattern
✅ **Compile-time SQL generation** - Zero runtime overhead
✅ **Zero runtime overhead** - Uses `if constexpr` dispatch
✅ **Single Connection-level cache** - JOIN and simple SELECT share one cache (#214)
✅ **77% average efficiency** - Respectable ORM performance

## See Also

- [WHERE Clauses](WHERE_CLAUSES.md) - Combine JOINs with filtering
- [SELECT Queries](SELECT_QUERIES.md) - Statement caching details
- [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) - Caching architecture
- [SQL Generation](../../internals/architecture/SQL_GENERATION.md) - Compile-time SQL generation
