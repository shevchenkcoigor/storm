# Composite & Non-Integer Primary Keys

Design notes for primary keys that are not a single DB-generated integer: multi-column
composite keys (`primary_part`, #500-#537) and `storm::UUID` keys (#507, #565). Also covers
the PK/FK column-naming rules those keys forced (#422, #506, #519).

These are implementation notes — the rationale behind each decision and the failure mode it
prevents — kept out of `CLAUDE.md` to keep that file navigable. For user-facing usage see
[FIELD_TYPES.md](../../guide/reference/FIELD_TYPES.md) and
[REFERENTIAL_INTEGRITY.md](../../guide/features/REFERENTIAL_INTEGRITY.md).

## The recurring failure mode

Nearly every bug below is one of two shapes, worth naming once:

**A hardcode that only a non-default model reaches.** A literal `"id"`, a literal `INTEGER`, a
`primary_key_` that assumed one column. Each was correct for every in-tree model, so each shipped
green and stayed dead until a model with a differently-named, differently-typed, or multi-column
key existed. `if constexpr` guards make these invisible: the branch does not instantiate, so it
does not even have to compile.

**SQLite and PostgreSQL disagree about what is an error.** SQLite does not type-check FK targets
and does not validate that a `REFERENCES` column exists; PostgreSQL rejects both outright. So the
same wrong DDL *silently misbehaves* on SQLite and *fails loudly* on PG. SQLite-only testing is
therefore actively misleading here, which is why the tests for these paths are `TYPED_TEST`s
executing on both backends rather than SQL-text assertions alone.

## Composite keys (`primary_part`)

### Annotation and DDL (#500)

`[[= primary_part]]` on two or more members declares a
multi-column PK — a free-standing tag object like `primary` (#492), re-exported to top-level
`storm::`. Deliberately a SEPARATE tag rather than "two `primary` members means composite": reusing
`primary` would silently turn a double-`primary` typo into a legal key. `is_primary_part_member`
detects it; `is_primary_member` now matches it too, so `ModelWithPrimaryKey<T>` accepts composite
models. `BaseStatement` gains `primary_key_members_` (the full PK list in DECLARATION order — the
order the DDL clause emits) and `has_composite_pk_`, alongside the unchanged `primary_key_`/`pk_name_`
(= the first element, so the ~13 files reading them are untouched). `schema.cppm` routes composite
models past the single-PK branch and emits every part as a regular column plus one table-level
`PRIMARY KEY (a, b)`, reusing the junction-table pattern; single-PK DDL is byte-identical.
`needs_index` now excludes every PK member (the table-level key already indexes each part).
New concept `ModelPrimaryKeyValid<T>` (ANDed into the `BaseStatement` constraint list next to
`ModelAnnotationsValid`) rejects: `primary` + `primary_part`, `primary_autoincrement` +
`primary_part`, exactly one `primary_part`, **two or more `primary`/`primary_autoincrement`
members** (the double-`primary` typo — without this the widened machinery would silently accept it
as composite), and a PK annotation on an m2m/reverse-FK container, on a `std::optional<T>`, or
`primary_part` + `unique`. The `std::optional` and relation-container rejections are gated on
`is_primary_member`, so they cover a **single** `primary` too — a deliberate widening (a nullable PK
was accepted before #500 and is never correct: SQLite's legacy NULL quirk admits duplicate keys while
PG rejects them). No in-tree model used that shape. The PK clause writes parts via the canonical column-name writer
(`append_column_name`, #422), so an FK part emits `warehouse_id` — a composite key over FKs is the
canonical association-table case, and naming the bare member would emit a PK over a nonexistent
column. `calculate_column_defs_size` keeps FK columns on the FK-suffix budget for composite models
(the regular suffix under-counts and `ConstexprString` truncates silently); `build_sql_impl` gained a
whole-SQL `std::unreachable()` backstop, since the per-column one can't catch a sizing shortfall. (#682 later deleted both hand-maintained budgets: the CREATE TABLE buffer is now measured by running the writer itself against the counting sink — see [COMPILE_TIME.md](../performance/COMPILE_TIME.md#exact-size-create-table-buffer-682).) Autoincrement on a composite key is **unrepresentable**,
not just useless — SQLite rejects both spellings at parse time and PG's identity is single-column —
hence the compile-time rejection. **Scope**: annotation + DDL only; UPDATE/DELETE by composite key
land in #501 (below), INSERT/JOIN and composite FKs in #502–#504.
See [FIELD_TYPES.md](../../guide/reference/FIELD_TYPES.md).

### UPDATE and DELETE (#501)

The by-key `WHERE pk = ?` widens to
`WHERE a = ? AND b = ?`, AND-joined in declaration order. Three things changed, each with its own
failure mode. (1) **SQL text**: the clause writer/sizer pair `append_pk_where_clause` /
`pk_where_clause_size` are free functions in `statements/field_names.cppm` (NOT `BaseStatement` methods — that class is
inherited by every statement type, and `SchemaStatement` already sits at the S1448 ceiling), taking
the PK-member array as a parameter so `erase.cppm` and `update_grammar.cppm` share them without
depending on each other. Parts go through `append_column_name` (#422), so an FK part emits
`warehouse_id`. (2) **Bind arithmetic**: `bind_pk_values` binds all N parts via an index sequence
(each part may be a DIFFERENT type — `int` + `std::string` is the canonical shape, so the dispatch is
per-part at compile time) and threads `param_index` by REFERENCE, which is what removes the old
`++param_index`-per-row stride assumption. In UPDATE the key follows the SET values, so it starts at
N_set+1. (3) **SET-target gate**: `is_settable_member` in BOTH `update_grammar.cppm` and
`upsert_grammar.cppm` now tests `Base::is_pk_member(M)` instead of `M != primary_key_` — the old test
excluded only the FIRST part, leaving `SET b=? WHERE a=? AND b=?` (rewriting the key it matches on)
legal. `bind_field_at_index` gained a **separate** `SkipAllPK` flag rather than widening `SkipPK`:
INSERT passes `SkipPK` too, and composite INSERT is #502 — widening it would silently change INSERT's
bind order as a side effect. Bulk DELETE uses a **row-value IN list**, `(a, b) IN ((?,?),(?,?))`
(PG always, SQLite ≥ 3.15 vs the project's 3.35 floor); the per-column form `a IN (…) AND b IN (…)`
is WRONG — it matches the parts' cross product and deletes unlisted keys. Chunking is now
`MAX_CHUNK_ROWS = 799/N` since each row costs N parameters. The consteval DELETE grammar moved to a
new `erase_grammar.cppm` leaf (mirroring `update_grammar.cppm`, #434) to keep `erase.cppm` under the
file-size limit; its `append_row_placeholder_list` is shared by the consteval max-chunk builder AND
the runtime per-count builder, so the two cannot drift into disagreeing placeholder counts.
Single-PK SQL is byte-identical (regression-asserted), and single-PK UPDATE/DELETE benchmarks are
within noise (≤1.4% deltas at cv 1.1–2.9%).

### INSERT (#502)

A composite key is never DB-generated — `AUTOINCREMENT` (SQLite)
and `GENERATED ... AS IDENTITY` (PG) are single-integer-column features, so every key part is
caller data. That inverts the single-PK rule twice. (1) **The key columns join the INSERT**: the
`SkipPrimaryKey` skip in `FieldNameGrammar::for_each_field_name` and the plain-`SkipPK` branch of
`skips_pk_column` are both gated on `!has_composite_pk_`, so a composite model emits and binds ALL
fields in declaration order (one shared consteval iterator feeds the column list, its sizer, and
the placeholders — they cannot drift). (2) **There is nothing to RETURN**: `insert().execute()` on
a composite model returns `std::expected<void, Error>` with no `RETURNING` emitted, riding the
pre-existing `ReturnId::No` fast path. The plain call resolves there via `default_return_id<T>()`
(consteval: `No` for composite, `Yes` for single-PK), so the call site is IDENTICAL across model
shapes; an explicit `insert<ReturnId::Yes>` on a composite model is a **compile-time error** via
the `ReturnIdSupported<T, R>` concept (unconstrained it would emit `RETURNING <first part>` —
silently lossy), constraining BOTH `QuerySet::insert` and `InsertStatement::query` so the
diagnostic fires at the user's call site. `ReturnId::No` stays valid on every model shape (generic
code that spells it out keeps compiling). The #413 auto-`DEFAULT` caveat was checked and does not
fire: that `DEFAULT` is recovered from a C++ default-member-initializer — a compile-time constant
already in the caller's object, not a DB-generated value. `placeholders_count()` widened to
`field_count_` for composite models (feeds the upsert `auto_update` tail offset — #503's path);
chunking needed NO change (the auto batch size already divides by ALL fields, which becomes exactly
right once the key columns are bound). Single-PK INSERT SQL is byte-identical
(regression-asserted) and the `RETURNING id` path is unchanged. Upsert/`ON CONFLICT` is #503,
JOIN #504.

### FKs and JOINs (#504)

An FK may reference a composite-PK model, and every JOIN
path (regular FK, m2m, reverse-FK) works over multi-column keys. An FK to an N-part target occupies
**N columns**, named `<member>_<part>` (`line_order_id`, `line_product_id`) — the single-column
`<member>_id` spelling is unchanged for single-PK targets, so all pre-#504 SQL is byte-identical
(regression-asserted at every layer). Nullable composite FKs work: `std::optional<Target>` makes all
N columns nullable together.

Four things had to widen in lockstep, each with its own failure mode. (1) **ON clauses** AND-join
one equality per part (`t2.a = t1.fk_a AND t2.b = t1.fk_b`) — matching only the first part would
silently join rows sharing just that part. (2) **The m2m/reverse-FK stitch key** gained `StitchKey`,
a fixed 32-byte inline buffer, for composite owners — a composite key is not one integer and may mix
`int`/`std::string`/`int64_t`; the statement-side extractor and the object-side builder dispatch per
part on the declared type and must stay in agreement. A **single-column PK still keys the map on a
bare `std::uint64_t`**, selected by `if constexpr` on `primary_key_column_count_`: `std::hash<long>`
is the identity function and `operator==` on a `long` is one inlined compare, so routing single-PK
models through `StitchKey` cost ~4% on the m2m eager-load path (`M2MRelation` carries a second
fn-pointer for the narrow extractor; exactly one is populated). (3) **The Q2 `IN`
subquery** uses the row-value form `(a, b) IN (SELECT a, b FROM …)` — the outer comparison side is
parenthesized but the inner SELECT list is **left unwrapped**, because parenthesizing it makes
SQLite parse it as one scalar expression rather than N columns. (4) **Junction DDL** emits one
column per PK part per side, `<Side>_<part>`, with the PRIMARY KEY listing all N+M and each
`FOREIGN KEY` naming the target's real key columns. That last part also fixed a latent bug: the
old `REFERENCES <Side>(id)` named a nonexistent column for a composite side — SQLite silently
accepts an unvalidated FK target, but PG rejects it outright, so composite-PK junction tables could
not be created on PG at all. Composite PK is supported on **either or both** m2m sides.

An FK member used as a `primary_part` stores the *referenced* key's type, not the whole struct —
`pk_part_storage_type` routes through the same `is_fk_field` check as `bind_one_pk_part` (#501).
Without it, a composite key over FKs (the canonical association-table shape) hard-errors inside a
sizer with a message naming neither model nor field. Column names always go through
`append_column_name` (#422), so an FK part emits `warehouse_id`. Sizing is exact: the junction
sizer runs the **writer itself** into a counting sink, so the two cannot drift — and note
`ConstexprString<N>` holds only `N-1` bytes (its `append` stops at `len < N - 1`), so a buffer
sized to exactly the rendered length silently drops the final character.

### Through-model m2m (#536)

`many_to_many_through<Model>` over a composite side
failed with `no such column: t2.<fk>_<part>_id`. Root cause: **two junction tables, two naming
rules, one code path**. The auto-junction is Storm's SYNTHETIC table, so Storm names its columns —
`<Side>_<part>` with each part routed through `append_column_name` (#422), so an FK part gains
`_id`. A through model is a REAL user-declared model whose junction columns are ordinary
composite-FK columns emitted by the regular model DDL (`append_composite_fk_part_column`), which
spells the target part's **bare** identifier: `<member>_<part>`, no `_id`. The rules agree for a
single-column PK and for a composite key of plain columns; they diverge exactly when a PK part is
itself an FK — the canonical association-table shape, which nothing in-tree exercised. `join_grammar.cppm`'s
`append_junction_side_col`/`_list` now take a `JunctionNaming` enum (`Auto`/`Through`) derived from
the same `Through` alias that picks the table and column base names, so the three cannot disagree
about which junction is addressed; the single-PK branch stays SHARED (both rules emit exactly
`<side>_id` there), which is what keeps all pre-#536 junction SQL byte-identical.

Fixing the naming alone was not enough — the FK-part shape was unreachable, broken in three more
places that #504 left on the "does not occur in the current fixtures" path, all the same
one-hop-out mistake (an FK **part of the FK target's key** stores the REFERENCED row's key, not the
whole struct — exactly what `bind_one_pk_part` (#501) already handled for T's OWN key):
`bind_one_fk_part` bound the whole struct (a hard `BindableType` error, so this shape did not
compile at all), `extract_one_fk_part` mirrored it, `join.cppm`'s `extract_relation_fk_part` (the
RELATION-side twin, for Q2 result rows — m2m related entities and reverse-FK owners) mirrored it
again, and `append_composite_fk_part_column` typed the column from the part's DECLARED type —
`StorageClass::Fallback` → nullable `TEXT` — so the
table-level `FOREIGN KEY` compared TEXT against the target's `BIGINT` key. That last one is the
#519/#506 asymmetry again: **SQLite accepted it and silently never matched; PG rejected the
`CREATE TABLE`** — so SQLite-only testing was actively misleading, which is why the tests are
TYPED_TEST executing on both backends rather than SQL-text assertions. All four now route through
the existing single-source helpers (`find_fk_primary_key`, `pk_part_storage_type`, moved above its
first use in `schema.cppm`). Every fix is `if constexpr`-dispatched; single-PK and auto-junction SQL
is byte-identical and benchmarks are unchanged.

### At most 4 parts (#537)

A composite key may span at most **4** columns;
a 5th part is a compile-time error via `ModelPrimaryKeyPartLimit<T>` (ANDed into the
`BaseStatement` constraint list next to `ModelPrimaryKeyValid`). The bound comes from
`StitchKey` (#504), the m2m/reverse-FK stitch-map key: a fixed 32-byte inline buffer into which
every part writes exactly one 8-byte word, so 4 parts fill it with **zero slack**. The only prior
guard was `assert(len_ + sizeof(word) <= CAPACITY)` in `append_word`, which `NDEBUG` compiles
out — absent from exactly the Release configuration where the overrun matters, making a 5-part
key a SILENT buffer overrun there. The gate reads `StitchKey::MAX_PARTS` (`= CAPACITY /
sizeof(uint64_t)`), **derived not hardcoded**, so the limit cannot go stale against the buffer.
Growing `CAPACITY` was rejected: #504 measured a wider key costing ~4% on the stitch hot path
(hence single-PK models bypassing `StitchKey` for a bare `uint64_t`); folding surplus parts was
rejected in #504 as a *mis-stitch* (rows attached to the wrong owner), not a recoverable
collision. Gated at the MODEL rather than at the stitch for two reasons: the stitch runs behind
`M2MRelation`'s type-erased fn-pointer vtable where `T` is out of scope (no call site to name the
model in a diagnostic), and gating only models that currently declare a relation would make
adding an unrelated m2m field later fail in a distant file. Single-PK and all in-tree composite
models (2–3 parts) are unaffected — no SQL or hot-path change, so no benchmark movement.

## Key column naming and typing

### PK/FK naming uses the real identifier, not a literal `"id"` (#506)

The single-PK
`CREATE TABLE` branch (`append_single_pk_column_def`) emits `Base::pk_name_` (the PK member's
actual identifier) ahead of the type/constraint suffix, and the per-field FK `REFERENCES
<Related>(<pk>)` clause (`append_fk_column_def`) emits `find_fk_primary_key<FieldType>()`'s
identifier — the same helper `bind_value_by_type` already used for the FK bind splice, so DDL and
DML are guaranteed to name the same column. Previously both hardcoded the literal `"id"`, so a
model whose PK member was not named `id` got DDL and queries that disagreed at runtime ("no such
column"). Composite-PK models are unaffected (routed around this branch, #500). The consteval size
budgets (`column_size_budget`'s `pk_size`, `fk_references_len`) were widened to measure the real
identifier length instead of assuming the fixed 2-char `"id"`; `id`-named models (the ~50 existing
ones) stay byte-identical. (Those budgets no longer exist since #682 — the buffer is measured from the emitted text.)

### m2m junction REFERENCES uses the real PK identifier (#519)

The same fix as #506, one path later.
`detail::append_junction_fk`'s single-PK branch emitted the literal `REFERENCES <Side>(id)`, so an
m2m whose owner or related PK member is not named `id` produced junction DDL naming a nonexistent
column — unexecutable `CREATE TABLE`, the #506 failure class. (#504 fixed the COMPOSITE branch and
deliberately left this one: generalising it there would have changed existing junction DDL outside
that issue's scope.) The branch now emits `SideBase::pk_name_` — the same source `join.cppm`'s ON
clauses read, so junction DDL and the two-query eager load cannot drift. Each side resolves
INDEPENDENTLY (owner `id` + related `sticker_id` is a legal shape). The junction's OWN columns stay
`<Side>_id` (`append_junction_side_column_name`): those are the junction's columns, and `join.cppm`
derives them from `table_name_ + "_id"`, never from `pk_name_` — renaming the DDL side alone would
INTRODUCE the very drift this fixes. No sizing change was needed: #504 replaced the old
`5×name + 256` heuristic with a budget measured by rendering the emitter into `ClauseSizer`, so it
tracks the real identifier automatically. `id`-named models keep byte-identical junction DDL
(regression-asserted on the whole string).

### m2m junction column TYPE follows the referenced PK (#565)

The third and last hardcode in
the same function family. `append_junction_side_column_def`'s single-PK branch typed every junction
column `INTEGER`/`BIGINT` unconditionally, so an m2m between `storm::UUID`-PK models (#507) emitted
an integer column whose `REFERENCES <Side>(<uuid_pk>)` clause — correctly NAMED since #519 — pointed
at a `UUID`/`TEXT` key. **PG rejects that `CREATE TABLE` outright** (FK type mismatch, so such a
model could not be created at all); **SQLite accepts it** and stores mismatched affinities silently —
the #536/#519 asymmetry, which is why the tests are TYPED_TEST executing on both backends rather
than SQL-text assertions alone. The branch is now GONE, not fixed: `primary_key_members_[0]` IS
`primary_key_` for a single-PK model, so both shapes route through the one
`pk_part_storage_type` + `storage_class_of` + `sql_type_for` chain the composite branch (#504) and
the per-field FK path (`append_fk_column_def`) already used. An integer PK still renders exactly
`INTEGER NOT NULL`/`BIGINT NOT NULL` — byte-identical, regression-asserted on the whole string.
Each side resolves INDEPENDENTLY, so an integer-PK owner + UUID-PK related is a legal mixed
junction. No sizing change (the #504 `ClauseSizer` budget runs the writer itself).

## UUID primary keys

### The UUID-PK path was dead code until #565

Writing #565's executable test exposed that **the whole UUID-PK path had never run**. #507
shipped as a self-described "partial implementation" whose only UUID-PK model appears in a
`static_assert` — it never generated DDL, never ran a query — so three defects sat behind
`if constexpr` guards that nothing instantiated. All three are fixed here, since #565's DoD
(executable junction DDL + insert through it, on both backends) is unmeetable without them:

1. **UUID-PK DDL never COMPILED**: `SchemaStatement`'s two `uuid_type<D>()` calls
   (`append_single_pk_column_def`, `append_fk_column_def`) were UNQUALIFIED, but that helper lives
   in `namespace detail` and `SchemaStatement` sits outside it. `detail::`-qualified now.
2. **INSERT dropped the key**: `FieldNameGrammar::is_skipped_pk` and
   `BaseStatement::skips_pk_column` skipped any single-column PK as DB-generated, emitting
   `INSERT INTO UuidDoc (title) VALUES (?)` — the caller's key silently discarded, every row
   landing with a NULL id. Both now also exclude `has_uuid_pk_()`, the #502 rule one shape wider:
   `AUTOINCREMENT`/`GENERATED ... AS IDENTITY` are single-INTEGER-column features, so a UUID key is
   always caller data. The two must change TOGETHER — one owns the column list, the other the bind
   sequence, and a disagreement misaligns values against columns.
3. **The UUID bind was wrong twice**: `bind_optional_or_uuid_pk_field` passed a `Statement*` where
   `bind_uuid_pk` takes `StmtType&`, and returned without `++param_index` (every sibling branch
   reaches that through `bind_one`). The missing increment bound the NEXT field over the key at the
   same slot and left the last placeholder unset — surfacing as a NOT NULL violation naming the
   WRONG column.

### #572 — nothing is RETURNed for a caller-supplied key (BREAKING)

The fourth defect of the same #507 batch, split out because it changes a return type. Plain
`insert(uuid_model)` defaulted to `ReturnId::Yes`, emitting `RETURNING <uuid_pk>` and extracting
that TEXT/UUID column with `extract_int64` — `0` on SQLite, garbage or an error on PostgreSQL.
The caller got an `std::expected<int64_t, Error>` whose value could never identify the row.

This is #502's reasoning one shape wider, and the fix is a single named predicate rather than the
condition repeated per site:

```cpp
// BaseStatement<T>, next to has_composite_pk_ / has_uuid_pk_()
static consteval auto pk_is_db_generated_() -> bool {
    return !has_composite_pk_ && !has_uuid_pk_();
}
```

`AUTOINCREMENT` and `GENERATED ... AS IDENTITY` are single-INTEGER-column features, so only a
single-column integer key has a value the database produces. Four decision points read the
predicate, and they are load-bearing in PAIRS — a disagreement emits a `RETURNING` clause the
runtime never reads, or reads a row the SQL never asked for:

| Site | Decides |
|---|---|
| `default_return_id<T>()` | what plain `insert()` resolves to |
| `ReturnIdSupported<T, R>` | rejects an explicit `ReturnId::Yes` at the CALL SITE |
| `UpsertGrammar::nothing_sql()` / `update_sql()` | whether the SQL TEXT carries `RETURNING` |
| `execute_upsert_nothing()` / `UpsertUpdateRunner::run()` | whether the runtime READS a returned row |

**The upsert half was an unowned defect.** #572 as filed covered only plain `insert()`; the two
`UpsertGrammar` clauses gated `RETURNING` on `has_composite_pk_` alone, so
`insert(uuid_model).on_conflict<...>().update<...>()` kept emitting the same lossy statement one
call away from the fix. Folded in here rather than left for a fifth issue — the #507 pattern this
whole document exists to record.

Breaking surface: `insert(uuid_model).execute()` and both upsert terminals now return
`std::expected<void, Error>`. `insert<ReturnId::No>` was already valid and is unchanged, so the
in-tree callers written against the gap (`test_junction_column_type.cpp`, `test_upsert.cpp`)
needed no edit.

Note the two halves are gated the same way but reached differently: `ReturnId` is a template
argument on `insert()` only, and does NOT propagate through `on_conflict()`. The upsert terminals
read the model shape directly, so `insert<ReturnId::No>(int_pk_obj).on_conflict<…>().update<…>()`
still emits `RETURNING` and still returns the id. That predates this issue and is unchanged by it.

### #585 — a key INSERT EMITS can be an ON CONFLICT target

Folded into the same change, because it is the same dead premise one clause over.
`ConflictTargetUnique` accepts a single `unique` field, a matching `UniqueIndex<...>`, or the full
PK column set — the last via `target_matches_primary_key`, which was gated on `has_composite_pk_`
and justified by its own comment:

> Gated on `has_composite_pk_` so a single-column PK is NOT matched here: INSERT omits it from the
> statement entirely, so it can never conflict.

True for a DB-generated integer key. False for a `storm::UUID` key ever since #565 put that column
back into the INSERT — a duplicate UUID key really does raise a PK violation. So the natural
upsert was a compile error:

```cpp
qs.insert(doc).on_conflict<fields::UuidDoc.id>().update<fields::UuidDoc.title>().execute();
// pre-#585: constraints not satisfied — ConflictTargetUnique<UuidDoc, ^^UuidDoc::id> is false
```

…workaround-able only by declaring a redundant `UniqueIndex<^^UuidDoc::id>` over the primary key,
which emits a duplicate unique constraint.

The gate is now `!pk_is_db_generated_()` — the same "does INSERT emit this key" question, so the
integer case stays rejected **for the reason that actually applies to it** rather than by accident
of arity:

| Key | In the INSERT? | Can collide? | Valid conflict target |
|---|---|---|---|
| single INTEGER (`AUTOINCREMENT`/IDENTITY) | no — DB generates it | no | rejected |
| single `storm::UUID` | yes — caller data (#565) | yes | **accepted (#585)** |
| composite (all parts) | yes — caller data (#502) | yes | accepted (#503) |
| composite (strict subset) | — | — | rejected (not unique alone) |

PostgreSQL is the backend that proves this rather than merely accepting it: it requires the
`ON CONFLICT` target to be backed by a real unique constraint and errors otherwise, so the
round-trip tests fail loudly there if the PK were not genuinely the constraint named.

**Known limitation (#586)**: the table above classifies *every* single-column INTEGER key as
DB-generated, so there is no way to declare a **caller-assigned integer key** — `insert()` silently
drops the caller's value and lets the DB assign its own. The DDL already permits one on both
backends (PG emits `GENERATED BY DEFAULT AS IDENTITY`, the permissive form, not `GENERATED ALWAYS`;
SQLite's plain `INTEGER PRIMARY KEY` is a rowid alias), and `primary` vs
`primary_autoincrement` currently differ in exactly one DDL keyword — nothing under
`src/orm/statements/**` reads `is_primary_autoincrement` at all. Supporting it is one more term in
`pk_is_db_generated_()`, which already feeds all five decisions that follow. The blocking question
is how "caller assigns" gets *declared*: unlike a UUID (empty = unset), an integer `id == 0` is
indistinguishable from a deliberately-assigned zero, so it needs an annotation rather than a
runtime check.

### #573 — UPDATE/DELETE by an unset UUID key silently no-opped

`BaseStatement::bind_one_pk_part` bound every by-key PK part (the WHERE clause `update(obj)` /
`erase(obj)` use) through the generic `bind_value_by_type` → `bind_parameter_value` →
`utilities::bind_uuid`, which **auto-generates a fresh random UUID when the value is empty**.
Correct for a non-PK UUID column (an unset `storm::UUID` field on INSERT should get a value); wrong
for a KEY used to locate a row. An unset key silently bound a random UUID into the WHERE clause,
matched zero rows, and reported success — the #501/#502 failure mode (a hardcode/wrong-path only a
non-default model shape reaches) one call site over, this time on the READ-back-by-key side rather
than DDL or INSERT.

INSERT already had the right guard (`bind_optional_or_uuid_pk_field` → `utilities::bind_uuid_pk`,
which rejects an empty value instead of generating one — see "UUID-PK path was dead code until
#565" above). The fix routes `bind_one_pk_part` through the same `bind_uuid_pk`, dispatched
`if constexpr` on the part's own type (or, for an FK part, the referenced row's PK type via
`find_fk_primary_key<FKType>()`) so an integer-PK model's bind is byte-identical to before — no
runtime branch added to the single-row UPDATE/DELETE hot path.

**Residual gap fixed separately — same root cause, different call site, tracked as #609:** a
`.where(field == storm::UUID{})` filter comparison still auto-generated. WHERE operands bind through
`ComparisonExpr`/`InExpression`/`BetweenExpr` → `bind_parameter_value` → `bind_uuid`, one level below
where the #573 fix landed — genuinely silent (no FK constraint to catch it), whether or not `field`
happens to be the PK. See
[docs/guide/features/WHERE_CLAUSES.md](../../guide/features/WHERE_CLAUSES.md) and
[docs/guide/reference/FIELD_TYPES.md](../../guide/reference/FIELD_TYPES.md) for the shipped guard.
A narrower residual — the guard keys off the *operand's* type, not the *column's*, so a
string/YAML-spelled operand against a UUID column still bypasses it — remains open as #622.

### #608 — INSERT of a composite key whose FK part targets a UUID key still auto-generated

`bind_field_at_index` dispatches `is_fk_field(member)` *before* it would reach the UUID-PK guard
`bind_optional_or_uuid_pk_field` gives a bare (non-FK) UUID PK member, so `bind_fk_field_at_index`
routed the FK target's value straight through `bind_one` → `bind_value_by_type` →
`utilities::bind_uuid`, which auto-generates a fresh random UUID when the value is empty — the same
failure mode as #573, one call site over, on the INSERT side rather than the by-key WHERE side. Not
silent — SQLite (`PRAGMA foreign_keys = ON`) and PostgreSQL both reject the fabricated key with a
FOREIGN KEY constraint error — but it names the wrong problem instead of #573's own guard message.

Fixed by threading the same `is_pk_member` + `bind_pk_scalar` dispatch #573 gave `bind_one_pk_part`
into `bind_fk_field_at_index`'s single-column-target branch: when the FK member is itself a
composite-PK part, its target value routes through `bind_pk_scalar` (which dispatches to
`bind_uuid_pk` for a `storm::UUID` target) instead of the generic `bind_one`. Scoped to the
single-column FK-target shape (`fk_primary_key_count<FKType>() == 1`, the shape #608 and its tests
describe) — the composite-FK-target path (`bind_fk_parts`/`bind_one_fk_part`) is left alone because
it is unreachable with `is_pk_member(member)` true, not merely untested: `valid_fk_key_target`
(`model_concepts.cppm`, guarding `is_primary_key_part_typed_member`) rejects a `primary_part` FK whose target
itself has a composite key at the `PrimaryKeyType<T>` constraint, before such a model can even
instantiate `BaseStatement`. The same constraint rejects an optional FK as a PK part, which is why
the sibling optional-FK branch a few lines above is untouched too — both guards must move together
if `valid_fk_key_target` is ever widened (see the warning above `valid_fk_key_target` in `model_concepts.cppm`).
