# Referential Integrity (#412)

Storm enforces foreign-key relationships at the database level. Every `fk<>` column emits
a `REFERENCES` clause, every auto-generated many-to-many junction table carries
`FOREIGN KEY` constraints, and SQLite connections open with `PRAGMA foreign_keys = ON`.
This is **always on** — there is no opt-in annotation. The per-FK `ON DELETE` policy
(#431) is configurable via the `fk<RefAction::...>` template argument (see below).

## Why always-on

Both backends recommend referential integrity:

- **PostgreSQL** enforces declared foreign keys natively; there is no switch.
- **SQLite** parses FK syntax always but only *enforces* it when the connection sets
  `PRAGMA foreign_keys = ON`. The default-off is a historical backward-compatibility
  choice; the SQLite docs recommend applications turn it on. Storm does so on every
  connection in `Connection::open()`.

Storm has no released schema to break, so enforcement is on by default rather than
opt-in. For an ORM, foreign-key enforcement is a defining feature.

## Base-table foreign keys

An `fk<>` field generates an `<name>_id` column with a `REFERENCES` clause to the related
model's table (the FK field's C++ type, optional-unwrapped) and primary key `id`:

```cpp
struct Message {
    [[= primary]] int    id{};
    std::string                                  content;
    [[= fk<>]] Person               sender;  // → sender_id
};
```

```sql
-- SQLite
CREATE TABLE Message (
    id INTEGER PRIMARY KEY,
    content TEXT NOT NULL,
    sender_id INTEGER NOT NULL REFERENCES Person(id)
);
-- PostgreSQL: sender_id BIGINT NOT NULL REFERENCES Person(id)
```

A nullable FK (`std::optional<Person>`) drops `NOT NULL` but keeps the `REFERENCES`:

```sql
owner_id INTEGER REFERENCES Person(id)
```

## Junction-table foreign keys

Auto-generated junction tables for `many_to_many<>` fields get a `FOREIGN KEY` on each
side, with `ON DELETE CASCADE` by default — an orphaned junction row is meaningless once
its owner or related entity is gone, so deleting the entity removes its link rows. The
default is overridable per field (see [ON DELETE policy](#on-delete-policy-431)):

```sql
CREATE TABLE Student_Course (
    Student_id INTEGER NOT NULL,
    Course_id INTEGER NOT NULL,
    PRIMARY KEY (Student_id, Course_id),
    FOREIGN KEY (Student_id) REFERENCES Student(id) ON DELETE CASCADE,
    FOREIGN KEY (Course_id) REFERENCES Course(id) ON DELETE CASCADE
);
```

The referenced column is each model's **actual primary-key member**, resolved per side (#519) —
`Student` and `Course` above both name theirs `id`, but a model whose PK is called something else
is referenced by that name:

```cpp
struct Badge { [[= primary]] int badge_id{}; std::string label; };
struct Hero {
    [[= primary]] int hero_id{};
    [[= many_to_many<>]] std::vector<Badge> badges;
};
```

```sql
CREATE TABLE Hero_Badge (
    Hero_id INTEGER NOT NULL,
    Badge_id INTEGER NOT NULL,
    PRIMARY KEY (Hero_id, Badge_id),
    FOREIGN KEY (Hero_id) REFERENCES Hero(hero_id) ON DELETE CASCADE,
    FOREIGN KEY (Badge_id) REFERENCES Badge(badge_id) ON DELETE CASCADE
);
```

The junction's own column names stay `<Model>_id` regardless of the PK member's name — they are the
junction's columns, not the referenced model's.

### Junction column type follows the referenced key (#565)

Each junction column is **typed from the model it references**, per side, the same way a regular FK
column is. An integer PK gives `INTEGER` (SQLite) / `BIGINT` (PostgreSQL); a `storm::UUID` PK (#507)
gives `TEXT` / `UUID`:

```cpp
struct Tag { [[= primary]] storm::UUID id{}; std::string label; };
struct Doc {
    [[= primary]] storm::UUID id{};
    [[= many_to_many<>]] std::vector<Tag> tags;
};
```

```sql
-- PostgreSQL
CREATE TABLE Doc_Tag (
    Doc_id UUID NOT NULL,
    Tag_id UUID NOT NULL,
    PRIMARY KEY (Doc_id, Tag_id),
    FOREIGN KEY (Doc_id) REFERENCES Doc(id) ON DELETE CASCADE,
    FOREIGN KEY (Tag_id) REFERENCES Tag(id) ON DELETE CASCADE
);
```

The two sides resolve independently, so a junction may legitimately mix types — an integer-PK owner
and a UUID-PK related model give `IntShelf_id BIGINT NOT NULL, UuidLabel_id UUID NOT NULL` in the
same table.

Before #565 the junction column was hardcoded to the integer type, so a UUID-PK m2m produced a
`BIGINT` column whose `REFERENCES` clause pointed at a `UUID` key. PostgreSQL rejects that
`CREATE TABLE` outright (foreign-key type mismatch), so such a model could not be created at all;
SQLite accepted it and stored mismatched affinities silently.

## ON DELETE policy (#431)

The `ON DELETE` referential action is configurable per FK via the `fk<RefAction>`
template argument. `RefAction` is one of `Cascade`, `SetNull`, `Restrict`, `NoAction`.

| Spelling                              | Emitted clause            | Effect on the child row when the parent is deleted |
|---------------------------------------|---------------------------|----------------------------------------------------|
| `fk<>` / `fk<RefAction::Restrict>`    | *(none — SQL default)*    | delete is **rejected** while children exist        |
| `fk<RefAction::Cascade>`              | `ON DELETE CASCADE`       | child row is **deleted** too                       |
| `fk<RefAction::SetNull>`              | `ON DELETE SET NULL`      | child FK column is set to **NULL**                 |
| `fk<RefAction::NoAction>`             | `ON DELETE NO ACTION`     | like `RESTRICT` for immediate constraints          |

`RefAction::Restrict` is the default, so a bare `fk<>` emits a plain `REFERENCES` with no
`ON DELETE` clause — byte-identical to the pre-#431 DDL.

```cpp
struct Comment {
    [[= primary]] int                          id{};
    [[= fk<storm::RefAction::Cascade>]] Post         post;     // delete post → delete comments
    [[= fk<storm::RefAction::SetNull>]] std::optional<User> author;  // delete user → author_id = NULL
};
```

**`SET NULL` requires a nullable FK** (`std::optional<Related>`) — it writes `NULL` into
the child column, which a `NOT NULL` column cannot hold. This is enforced at compile time
by the `ModelFkPoliciesValid<T>` constraint on `BaseStatement`: an `fk<RefAction::SetNull>`
on a non-optional field fails to compile with a clear constraint violation.

**`ON UPDATE` is not emitted.** Storm foreign keys reference synthetic identity primary
keys whose value never changes, so an `ON UPDATE` action would never fire. It can be added
later if natural-key FKs are introduced.

**An FK target must have a primary key** (#474). The `ValidForeignKey<FieldType>` concept —
`ModelWithPrimaryKey` applied to the FK type, optional-unwrapped — names this boundary.
`find_fk_primary_key` is constrained by it, and the `FKFieldOf<T, Member>` gate on
`join<>` / `left_join<>` selectors now also requires it, so selecting a `join<>` on an FK
whose target type has no primary key fails at the **call site** with a clear constraint
violation instead of deep inside result extraction. The check is single-level (only the
target's own PK, never the target's own FKs), so self-referential and mutually-referential
FKs resolve in one step.

### Junction `ON DELETE` override

The junction default is `CASCADE`, overridable per m2m field via the
`many_to_many<RefAction>` template argument — applied to **both** junction FK sides. Use
it for soft-delete/archival, audit trails of links, or through-data you do not want to lose:

```cpp
struct Member {
    [[= primary]] int id{};
    // both Member_id and Club_id sides emit ON DELETE RESTRICT instead of CASCADE
    [[= many_to_many<storm::RefAction::Restrict>]] std::vector<Club> clubs;
};
```

## Table-creation order

Because a referencing table cannot be created before its FK target exists (PostgreSQL
rejects a `REFERENCES` to a missing table; SQLite tolerated it),
`SchemaStatement<T>::create_table_if_not_exists(conn)` **creates the referenced parent
tables first**, recursively. The recursion is cycle-safe (self- and mutual-FK cycles are
broken by an internal set of already-created table names) and idempotent
(`CREATE TABLE IF NOT EXISTS`). Many-to-many related tables are likewise created before
their junction table.

```cpp
// Person does not need to be created first — create_table_if_not_exists<Message>
// creates Person, then Message.
storm::orm::schema::SchemaStatement<Message>::create_table_if_not_exists(conn);
```

## Behavior summary

With enforcement on, the database rejects:

- Inserting a child whose FK points at a non-existent parent.
- Deleting a parent that is still referenced by a `RESTRICT`/default FK. A `CASCADE` FK
  deletes the children; a `SetNull` FK nulls the child column instead.
- Leaving orphaned junction rows — deleting an owner/related entity cascades to its
  junction rows (unless the m2m field overrode the junction policy).

## Migration caveat

This is enforced on every connection. If you point Storm at an existing database that
already contains dangling foreign keys, enforcement will reject operations that touch
those rows (and, on SQLite, a `PRAGMA foreign_key_check` would report the existing
violations). Storm itself has no released schema, so this only matters when adopting
Storm against a pre-existing database.
