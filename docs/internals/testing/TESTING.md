# Testing Strategy

## Test Framework

- **GoogleTest** with C++26 module support
- Tests located in `tests/` directory
- In-memory database (`:memory:`) for fast execution
- Comprehensive sanitizer support (ASAN, TSAN, LSAN)

## Test Categories

### Unit Tests

**Location**: `tests/<category>/test_*.cpp` (category subdirs: crud, query, schema, db, errors, tools, …)

**Coverage**:
- **ID Validation**: Tests verify returned auto-generated IDs
- **SELECT Testing**: Empty table, single/multiple rows, field types, large datasets, statement caching, integration tests
- **JOIN Testing**: Single FK and multi-FK JOINs with full object population verification (`tests/schema/test_fk_fields.cpp`)
- **FK Field Testing**: INSERT/UPDATE/DELETE with FK fields, batch operations with FKs
- **WHERE Clause Testing**: Various conditions, operators, LIKE patterns, IN, BETWEEN
- **DISTINCT Testing**: Single and multi-field operations with type safety validation

### Performance Benchmarks

**Location**: `benchmarks/tests/benchmark_tests.yaml` (YAML-declared; fixtures in `benchmarks/query_benchmark.cppm` / `benchmarks/crud_benchmark.cppm`)

**Coverage**:
- CRUD operations (INSERT, SELECT, UPDATE, DELETE)
- JOIN operations (INNER, LEFT, RIGHT)
- DISTINCT queries
- Batch operations

See [benchmarks/README.md](https://github.com/spiritEcosse/storm/blob/develop/benchmarks/README.md) for detailed benchmark documentation.

## Running Tests

```bash
# SQLite + PostgreSQL — fast path, run the binary directly (see "Local Test
# Suite Speed" below for why this beats `ctest --preset ninja-debug`).
# The socket is Linux's; macOS (Homebrew PostgreSQL) listens in /tmp: use host=/tmp.
STORM_PG_CONNSTR="host=/var/run/postgresql dbname=storm_db user=storm_db" \
    ./build/debug/tests/storm_tests

# Same, via ctest (slower, but gives per-test filtering/listing via -R/-N).
# The preset pins the Linux socket, so on macOS use the direct-binary command above.
ctest --preset ninja-debug

# SQLite only
ctest --preset ninja-debug-sqlite

# Filter specific test suite
./build/debug/tests/storm_tests --gtest_filter="SelectTest.*"

# Run with verbose output
./build/debug/tests/storm_tests --gtest_verbose

# Run with ASAN + UBSAN (memory errors, leaks, undefined behavior)
cmake --preset ninja-asan-ubsan && cmake --build --preset ninja-asan-ubsan
ctest --preset ninja-asan-ubsan

# Run with TSAN (data races)
cmake --preset ninja-tsan && cmake --build --preset ninja-tsan
ctest --preset ninja-tsan
```

See [SANITIZERS.md](SANITIZERS.md) for full details on sanitizer presets.

## Local Test Suite Speed

Measured 2026-08-27 on the full `storm_tests` binary (2961 tests, SQLite + PostgreSQL):

| Approach | Wall time | Notes |
|---|---:|---|
| `./build/debug/tests/storm_tests` (direct) | ~33s | gtest's own in-process loop, one process |
| `ctest --preset ninja-debug` | ~260s (7.8x slower) | `gtest_discover_tests` registers each test case as its own CTest test, so ctest re-launches the whole 129MB binary process per test — that per-process overhead swamps the preset's `jobs: 50` parallelism (and 50 also oversubscribes an 8-core machine) |

**Takeaway**: for local iteration, run the gtest binary directly. `commit.sh`'s
test step does this (`storm_tests`, the only test binary since #787). `ctest` is still useful locally for `-R`/`-N`
filtering/listing. CI also uses `ctest` (via `ctest --test-dir build/<dir>`, with
its own service-container connstr, not the `ninja-debug` testPreset), where
per-test pass/fail visibility in the Actions UI matters more than raw wall time.

**Known divergence**: `ctest`'s per-test-case registration incidentally gave
each test its own OS process. Running all ~3000 cases in one process locally
means `thread_local`/static state (the per-process PG schema name in
`storm::test::detail::current_test_schema`, `QuerySet`'s default-connection
thread-local) now persists across tests within a run in a way CI's per-process
model doesn't reproduce. Nothing is currently known to depend on this, but a
future test that leaks such state could pass locally and fail in CI (or vice
versa) with no code change between them — worth knowing if that ever happens.

Within that ~33s, PostgreSQL-backed tests are ~80% of the time (~30s) vs
SQLite's ~12% (~4.6s) despite running slightly fewer tests — PG pays a real
network/socket round-trip per query that SQLite's in-process engine doesn't.
One lever that helps: on a local dev Postgres used only for this test suite
(never for real data), setting `fsync = off` and `synchronous_commit = off` in
`postgresql.conf` cut the PG bucket from ~30.2s to ~25.7s (−15%) and the full
run from ~38s to ~33s wall (−12%) — see
[PERFORMANCE.md](../performance/PERFORMANCE.md#local-test-postgresql-tuning)
for the exact config and the durability caveat.

## PostgreSQL Test Isolation

PG tests use **per-process schema isolation** (`test_<pid>`). Each CTest process gets its own PG schema via `SET search_path`, giving complete isolation with zero lock contention. Fully parallel, no TRUNCATE, no transactions, no deadlocks.

- `ensure_table()` creates the schema on first call per process
- `rollback_test_txn()` drops the schema in TearDown (`DROP SCHEMA ... CASCADE`)
- `begin_test_txn()` is a no-op for PG (schema provides isolation)
- `backend_available<ConnType>()` returns bool; call `GTEST_SKIP()` directly in `SetUp()` (not in a helper — `GTEST_SKIP()` contains `return` that only exits the calling function)

Key constraints discovered:
- **ORM batch operations** (chunked erase/update) issue their own `BEGIN`/`COMMIT` — incompatible with outer test transactions
- **TRUNCATE** takes `ACCESS EXCLUSIVE` lock — deadlocks with concurrent `INSERT` + `ALTER TABLE`
- **Per-process schemas** solve both problems: each process has its own namespace
- **Rejected `STORM_REUSE_DB`** (TRUNCATE instead of DROP/CREATE schema) — CTest spawns a fresh process per test binary, so no cross-process schema reuse; TRUNCATE per-table was actually slower than one-time DROP+CREATE

## Test Structure

```cpp
#include <gtest/gtest.h>
import storm;

TEST(YourTestSuite, YourTestCase) {
    // Setup: Create connection and table
    auto conn = storm::db::sqlite::Connection::create(":memory:");
    conn->execute("CREATE TABLE ...");

    // Execute: Perform operation
    QuerySet<YourModel> qs(conn);
    auto result = qs.insert(...).execute();

    // Verify: Check results
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), expected_id);
}
```

## Test Coverage Goals

- ✅ All public APIs
- ✅ Edge cases (empty tables, null values, large datasets)
- ✅ Error conditions (invalid operations, constraint violations)
- ✅ Performance regression prevention
- ✅ Thread safety (where applicable)

## Current Test Statistics

Per-category counts go stale fast as the suite grows, so this tracks only the
total, measured alongside [Local Test Suite Speed](#local-test-suite-speed):

- **2,961 tests** across 373 test suites in `storm_tests` (SQLite + PostgreSQL,
  TYPED_TEST'd, incl. the SQLite and PostgreSQL error paths on the fake API policies,
  #784/#787) — 100% passing
- ~33s wall time running the binaries directly (measured 2026-08-27)

## YAML-driven cases and `expected_sql` (#818)

The corpus is the topic files `tests/test_cases/*.yaml` (`select`, `where`, `order_limit`, `aggregates`,
`group_by`, `chain`, `distinct`, `writes`, `typed_values`, `join`, `models`; #846, #869, #872). They are authoring groups only: a case spans
several topics and a topic spans several runners, so a case in the "wrong" file is not an error, and a new
topic file is picked up by a CMake glob with no edit. `scripts/split_unified_cases.py` reads all of them, buckets
every case by `query_type` into one JSON per category (`select` / `grouped` / `aggregate` / `crud`, the compile
slices -- unchanged; plus `join`, #869: every case on `dataset: relations`, whatever its `query_type`, so only
`test_unified_yaml_join.cpp` creates the relation tables and instantiates statements on those models — which SQL
such a case may pin is still decided by its runner group; and `models`, #872: the same for `dataset: keys`, owned by
`test_unified_yaml_models.cpp`), and rejects an unknown `query_type` or a case name repeated anywhere in the corpus. It
rewrites only a JSON whose bytes changed, so editing one case recompiles one slice. The JSONs are
`#embed`ded into `tests/yaml/test_unified_yaml_*.cpp` and run on SQLite and PostgreSQL. A case
checks **results** (`expected:`). SQL text is pinned separately in C++ (`test_sql_verify.cpp`,
`test_fields_sql_parity.cpp`).

**No two cases may run the same input (`scripts/ci/check_yaml_duplicates.py`).** Because the corpus is split
by topic, the same query is easy to add twice under two names in two files; `commit.sh` and CI's `script-guards`
reject it, naming both as `<file>:<case>`. A case's *input* is the case minus `name`, `description`, the result
keys `expected*` and `allow_duplicate` (a pinned `expected_sql*` stays in: the text it pins depends on how the
query was written), normalised before hashing, so there is no spelling rule to follow when writing a case: key
order, the order of `where` conditions, and `where` vs `where_expr` (flattened to one AND/OR tree; nested
same-operator nodes merged; `IN` lists sorted, `BETWEEN` bounds not) all hash alike. Typed values stay distinct
(`value_int: 30` is not `value_dbl: 30.0`). Not caught: filters that are equal only by logic (`a > 5` vs
`NOT (a <= 5)`) and cases whose input differs only by a defaulted key written out. A case that pins
`expected_sql*` is compared by that text too, so reordering its conditions makes it a different test. Before adding a case, run
`python3 scripts/ci/check_yaml_duplicates.py`. A deliberate repeat (same query, different intent) carries
`allow_duplicate: <reason>` and is left out of the comparison; an empty reason is an error.

A case may also carry an optional `expected_sql:` — the exact `sql()` text, compared **before** the
statement executes. A case without it stays result-only. It is read by `AggregateRunner` (all 56 scalar-aggregate
cases), by `SelectRunner` on `query_type: select` (#844), by `ValuesRunner` on `values` (#867) and by
`SetOpRunner` on `setop` (#870); `first` /
`one` / `rows` and the other runners reject it (a compile error and a `split_unified_cases.py` error), since they
would pass without comparing SQL. Chain and group cases share `AggregateRunner` but the split script still rejects
the key outside `aggregate` and `select`.

```yaml
- name: count_where_age_gte_35
  query_type: count
  where: {conditions: [{field: age, op: '>=', values: [35]}]}
  expected_sql: 'SELECT COUNT(*) FROM "person" WHERE "age" >= ?'
  expected: {int_val: 11}
```

- `sql()` is backend-independent for most statements (placeholders stay `?`), so one string covers both
  backends. Where the text differs (`NULLS FIRST/LAST` after ORDER BY, `LIMIT ALL` vs `LIMIT -1` for an
  OFFSET without a LIMIT) a case gives **both** `expected_sql_sqlite` and `expected_sql_pg` (#844):

  ```yaml
  - name: dialect_sql_order_by_asc_nulls_first
    model: simple
    query_type: select
    dataset: empty
    order_by: [{field: name, direction: ASC}]
    expected_sql_sqlite: 'SELECT "id", "name", "value" FROM "simplerecord" ORDER BY "name" ASC'
    expected_sql_pg: 'SELECT "id", "name", "value" FROM "simplerecord" ORDER BY "name" ASC NULLS FIRST'
    expected: {count: 0}
  ```

  A case sets `expected_sql`, or both dialect keys, **never both forms, never one dialect alone** (the other
  backend would run with no SQL check). `TestCase::expected_sql_well_formed()` / the `WellFormedExpectedSql`
  concept reject it at compile time and `split_unified_cases.py` rejects it earlier; `expected_sql_for(pg)`
  picks the text. `scripts/ci/tests/test_split_unified_cases.py` covers the script's rules.
- Write the string from the case, not by pasting the code's output — a pinned wrong string passes forever.
- The capacity is `EXPECTED_SQL_CAPACITY` (256, raised from 160 for set-op chains in #870) in `tests/test_parser.hpp`, mirrored in
  `scripts/split_unified_cases.py`, which fails the build on a string of 256 or more characters (the C++ parser
  rejects every over-long case at compile time too, #845; the script keeps its check so the error names the case and the key), on non-printable or non-ASCII text, and on `expected_sql` in a category whose
  runner does not read it yet (it would pass without ever comparing SQL). The parser unescapes `\"` and `\\`, which is how the quoted identifiers survive the JSON hop.
- Write paths (`to_sql()` inlines bound values) and upsert still need C++ tests.
- **Build cost** (Release, warm, aggregate TU, 3 runs each): 25.5–27.3 s before, 26.9–27.0 s after
  (about +1 s, +4%, at the edge of run-to-run noise); the select TU, which carries no `expected_sql` but whose
  `TestCase` grew by 160 bytes, is unchanged (28.7–29.2 s vs 28.7–29.2 s).

## How a YAML case reaches the query API (#834)

The runners (`tests/test_select_runner.h`, `tests/test_unified_yaml_body.h`) have no branch per model,
aggregate or field. Names in the YAML are resolved by reflection at compile time:

- **`model:`** looks up `YAML_MODELS` in `tests/test_yaml_models.h` (`person`, `message`, `simple`, `extended`; and,
  since #869, the relation models `student`, `member`, `pupil`, `author`, `reporter`, `bug`), and the
  type is spliced into whichever runner the `query_type` picks. To support a new model, add one row there. An
  unknown name is a compile error (`KnownYamlModel`). Before #834 a missing branch silently ran nothing:
  three `message` cases (`select_join_where_*`, `distinct_join_where_like_o`) never executed, and two of
  them had lost their `LIKE` patterns unnoticed.
- **Aggregates** — `query_type` (`count`, `sum`, `avg`, `min`, `max`, `count_distinct`; `count_field` →
  `count`), the `group_<func>` suffix, and each chain `func:` are the **method names** themselves.
  `member_function_named<Q>(name)` finds the member function template, and `call_query_method` splices it with
  the step's field pack: none (`COUNT(*)`, and only `count` may omit it), one, or several. Both live in
  `shared/query_builder.hpp` (#835; since #869 the by-name resolvers live in its part
  `query_builder_resolve.hpp` and the typed values in `query_builder_values.hpp`, both included by it). Since #868 the tests run the builder's own pipeline:
  `AggregateRunner` calls `QueryBuilder::build_terminal`, which builds the chain by recursion over
  `spec.aggregations` (`apply_aggregations`, one step per entry), so the tests and
  `benchmarks/query_benchmark.cppm` build an aggregate the same way. `ValidAggregate` is the allow-list of
  names a YAML `func` may reach on every step.
- **Aggregate steps (#868).** `aggregations:` is a list of `{func, field | fields, result}`, at most 5 steps of
  at most 3 fields each (`BenchmarkTest::MAX_AGGREGATIONS`, `AggregateSpec::fields` = `CappedFields<3>`); both
  counts go past their caps, so an over-long list does not compile. `aggregate: {func, field}` is the one-step
  shorthand; writing both forms does not compile either. `field:` is a one-element `fields:`, an empty `field`
  is no field (`COUNT(*)`) and so is an empty name inside `fields:` (skipped, so `["", a]` is `[a]`), and when
  both keys are written the later one wins (`scripts/ci/check_yaml_duplicates.py` reads a step the same way).
  Several fields are ONE operand, `func(a + b)` — so `SUM` of a string field
  is a compile error (the existing `NumericAggregateSelectors`, #475), not a coerced sum. A `chain` case may
  also `group_by` (with `having`, `where`, `order_by`, `limit`): the rows are `tuple<key, a1, ..., aN>`, and
  each step's `result.groups: [v0, ...]` lists its value per group in ascending key order, beside
  `expected.group_keys`. The `group_agg_*` arrays still serve the single-aggregate `group_<func>` cases.
  `result` reads `value` and `groups` only (the old `type` key was never read and is gone from the corpus;
  any other key is skipped). A key the runner would not read does not compile
  (`TestCase::aggregate_results_well_formed`): `value` on a grouped step, `groups` on an ungrouped one, or
  `groups` without `expected.groups_count` / `group_keys` to line up with. `QueryBuilder` itself rejects a
  `group_by` with no aggregation and a step other than `count` with no field (`SpecAggregatesWellFormed`, a
  class constraint, so the shared `Buildable<...>` asserts in `test_spec_limits_aggregates.cpp` see it).
  **Known limits:** a grouped chain has ONE `expected.group_agg_null` row for all its steps, not one per step;
  a grouped `chain` case cannot pin `expected_sql` (`split_unified_cases.py` rejects it on chain cases); and the
  two new benchmarks have no raw-SQLite anchor (unanchored, tracked in #780); and a `result:` inside the
  `aggregate:` shorthand is skipped, not read (that form takes its value from `expected`).
  `benchmark_tests.yaml` (via `benchmarks/scripts/yaml_to_json.py`, which now rejects a key it does not know
  instead of dropping it; `scripts/ci/tests/test_yaml_to_json.py` pins that and converts the real corpus) has one case of each shape: `aggregate_avg_two_fields` and `group_by_chain_count_sum_avg`.
- **Projections and terminals (#867)** — `query_type: values` plus `values: {fields: [...]}` (1..4 fields, like
  `distinct:`) runs `values<f...>()`: one field yields `plf::hive<T>`, several `plf::hive<std::tuple<...>>`.
  `expected.rows: [[a, b], ...]` lists every result row, one typed cell per field (each cell is checked like an
  `expected.first` value, so `null` and the #844 typed values work); it must agree with `count`, has at most 8 rows
  of 4 cells, and more than one row needs an `order_by` because rows are compared in result order. `where`,
  `order_by`, `limit` / `offset` and `join` compose with it. `query_type: rows` iterates `rows()` (every row must
  be `has_value()`, `count` is checked, `expected.first` is checked on the first row). `first` / `one` run on
  `select` shapes (where, order, offset, inner / left join); the projection, aggregate, group and set-op
  statements have no `first()` / `get()` in the API, so there is nothing to spell. A case names ONE result shape
  (set-op, aggregate or group, distinct, values): two would silently run the first one in `build_terminal`, so
  `BenchmarkTest::within_limits()` is false and the case does not compile.
- **Set operations (#870)** — `query_type: setop` plus `setop: {operands: [...]}` runs a chain of 2..4
  SELECTs over the case's model (`SetOpRunner`, in the select slice). The first operand is `{where}`, each
  later one `{op, where}` with `op` one of `union` / `union_all` / `except` / `intersect` (the member names
  `union_` / `union_all` / `except_` / `intersect_`, resolved by `set_op_member` + `member_function_named`;
  `ValidSetOp` is the allow-list). An operand without `where` is the whole table; `where` is the same tree as
  the case's own (shorthand, `conditions` + `combine`), built by `build_where_node` and value-checked like it.
  The operand trees are stored in the case's own `where` pool (`BenchmarkTest::where`, left disabled), each
  operand keeping only its root (`SetOpOperandSpec::root`), so together they share the WHERE caps of 7 nodes
  and 10 values and are checked as one forest (`WhereSpec::is_forest_well_formed`: no node dropped or shared).
  A first cut stored a `WhereSpec` per operand; that made every `TestCase` of the corpus about 8 KB larger and
  the YAML TUs roughly 1.5–3x slower to compile (up to 12x for the spec-limit TUs), cases with no set
  operation included. **Build cost of the pool layout** (Debug, warm, min of 3, HEAD and this change
  interleaved twice on a quiet machine): crud 4.41/4.99 s → 4.36/4.57 s, aggregate 7.97/8.45 s →
  8.35/9.04 s, grouped 12.93/15.07 s → 13.19/14.20 s, i.e. within run-to-run noise (aggregate perhaps +5%);
  the select TU, which also gains the 21 set-op cases, 13.0 s → 16.3 s. `order_by` (up to 2 terms, applied in ONE `SetOpBuilder::order_by` call: unlike a
  QuerySet, a SetOpBuilder keeps only its last `order_by`) and `limit` / `offset` apply to the combined
  result. Each of these is a compile error rather than a silent fallback (the old builder ran INTERSECT for
  any unknown type): fewer than 2 or more than 4 operands, an `op` on the first operand, a later operand
  without one, an unknown `op`, an unknown key in `setop:` or an operand (the old `type:` among them), a
  top-level `where:` / `join:` beside `setop:` (it would not say which operand it filters), and a chain with
  an `intersect` after any other operator. That last rule is about **portability**: Storm emits the
  compound SELECT without parentheses; SQLite evaluates it left to right, PostgreSQL binds `INTERSECT`
  tighter, so `A UNION B INTERSECT C` returns different rows per backend (pinned by
  `SetOpTest.IntersectAfterUnionFollowsEachBackendsPrecedence` in `tests/query/test_setop.cpp`). An
  `INTERSECT`-only prefix (`A INTERSECT B INTERSECT C UNION D`) reads the same on both. `expected_sql` is
  accepted; `expected.first` over several rows needs an `order_by` (asserted by the runner).
- **`expected.first:`** keys are row **member names**. Each is compared according to the member's C++ type
  (string, bool, floating with a 0.01 tolerance, integer, and the typed values of #844 below). A key with no
  matching member, or a value that does not fit the member (`ExpectedValueFits`), is a compile error. Before
  #834 such a key was skipped.
- Results are checked according to the C++ result type: `std::optional<double>` (MIN/MAX/AVG, #416) against
  `dbl_val` / `result.value`, integers exactly. Per-group values (`group_keys`, `group_agg_*`) are now checked
  for `having` cases too. They used to be ignored there.
- **WHERE/HAVING operators** (#835) are resolved by name in `shared/query_builder.hpp`, with no branch per
  operator and one resolver for the benchmark, the `QueryBuilder` and `QueryBuilder::build_where_node`
  (`build_compare_expr<field, op>(values...)`). A symbolic operator (`> >= < <= == !=`) is the C++ operator's
  own symbol (`operator_of` / `symbol_of`); a word operator is the member name with the case lowered and spaces
  turned into `_` (`LIKE` → `like`, `IS NOT NULL` → `is_not_null`). A name can carry several templates
  (`where::Field` has a real `operator==` and a `= delete`d composite-FK twin), and the reflection of a template
  does not report it deleted, so candidates are judged by their specialization for the value types:
  unsatisfied constraints and deleted ones are skipped, and exactly one must remain. The argument *shape*
  (one string / two values / a pack / none) stays explicit in `build_where_leaf`, because that is behaviour.
- **Value counts** are checked at compile time per operator, in `build_compare_expr` so every caller is covered (`ValidValueCount`): a comparison and `LIKE` take one
  value, `BETWEEN` two, `IN` any number (an empty list is valid and matches no row, `1 = 0`), `IS [NOT] NULL` none. `values: []` on a `LIKE` used to read
  `values[0]` and compile to `LIKE ''`. In the WHERE tree the leaves share one value pool, so the count a leaf CARRIES is
  checked too (`WhereNode::operands_match_op`, via the `SpecWellFormed` constraint on `QueryBuilder`): a leaf short of
  values would otherwise read the next leaf's operand, and extra values would be dropped silently (#843).
- **`ILIKE`** (#814) resolves to `Field::ilike` like `LIKE` does: one string value. A WHERE leaf may carry `collate:`
  (`NOCASE` / `BINARY` / `RTRIM`, built as `field.collate(...)`); it is not allowed on `LIKE` / `ILIKE` (`CollateApplies`,
  SQLite's LIKE ignores collations) nor on an AND/OR node. (SQLite documents LIKE only as ASCII case-insensitive and controlled by `case_sensitive_like`; that a collation does not change it was checked with `sqlite3`, not taken from its docs. `CollatedField` has no `ilike`.) A collation exists on SQLite only, so a case using one
  (WHERE leaf or `order_by[].collate`) must say `backends: [sqlite]`: that is the only accepted list, anything else is
  a compile error, and the case is not registered for PostgreSQL at all (absent from that suite, not a pass).
  A `where_expr:` leaf takes `values: [...]` like the flat form, so `BETWEEN` / `IN` / `LIKE` work inside the tree.
- **`order_by[].collate`** is an enumerator name of `Collate` (other than `None`) matched case-insensitively (`NOCASE` → `NoCase`);
  an unknown name is a compile error (`ValidCollate`) instead of falling back to `NoCase`.
- **`distinct.fields`** takes any number of fields through one path (`QueryBuilder::build_distinct_stmt`); a
  third entry used to be ignored (`distinct_dept_age_name_three_fields`).
- Still explicit: the set-operation keyword (`union` → `union_`, `set_op_member`), the JOIN type, `typed_value_as`, `apply_limit`
  and the terminal choice. Their branches differ in behaviour, not only in a name.

### Runner structure (#837)

- **No base class.** Every runner (`SelectRunner`, `ValuesRunner`, `SetOpRunner`, `AggregateRunner`, `DistinctRunner`,
  `InsertRunner`, `UpdateRunner`, `EraseRunner`) is a stateless class with a
  `static run<Tc>()`. The two helpers the old `QueryRunnerBase` mixed are free functions:
  `base_query<Tc, Model, Conn>()` (the case's filtered/joined `QuerySet`) and
  `seed_records<Tc, Model, Conn>()` (insert the dataset). The query is a local, not runner state.
- **One WHERE tree (#843).** Every WHERE form fills one `WhereSpec` tree of `WhereNode`s
  (`benchmarks/schema.cppm`): `where:` is its leaves joined left to right by `combine` (AND when absent), and the
  test-only `where_expr:` is written as the tree itself (nodes numbered in pre-order from the root). `QueryBuilder`
  builds the tree (`build_where_node`), and every leaf goes through the shared `build_compare_expr` resolver, so an
  unknown or ill-typed operator is a compile error. The operands of all leaves live in one pool
  (`WhereSpec::values`), typed by the key that wrote them: `value_dbl: 0.0` is a double, not the int 0. A leaf also takes the general
  `value:` key, read by the same `parse_typed_value` as `where:` (so `null`, int64, strings, UUIDs and dates work in
  the tree too, #853). A case that
  writes both `where:` and `where_expr:` does not compile (two trees cannot share one set of nodes).
- **FK by member name.** `QueryBuilder`'s `fk_resolver` defaults to `member_name_resolver<Model>`, so a
  `join.fk:` is a member name on the model. The benchmark still passes its own (`registry::resolve_fk_field`).
  The test runners pass `yaml_join_resolver<Model>` (`tests/test_yaml_models.h`, #869), which also reads
  `<yaml model>.<member>` as a cross-model FK selector.
- **JOIN over relations (#869).** `tests/test_cases/join.yaml`, dataset `relations`: the models of
  `tests/test_relation_models.h` (BStudent auto m2m — the m2m benchmark's own model from `benchmarks/m2m_models.hpp` —, BMember two auto m2m, BPupil m2m through BEnrollment, BAuthor
  reverse_fk of BPost, BBug two FKs to BReporter), created by `relations::create_tables` and seeded by
  `relations::seed` (`tests/test_relations_dataset.h`; rows looked up by name, since a PostgreSQL IDENTITY does not
  restart; auto-junction rows are raw INSERTs, Storm has no write API for them). Spellings: `join.fk: courses`
  (an FK, m2m or reverse_fk member of the case's model), `join.fks: [courses, clubs]` (several of ONE kind in one
  `join<>` call — `JoinableFields` rejects a mix or a duplicate), `join.fk: bug.author` (cross-model FK selector,
  valid on aggregate / filter chains). In `expected.first` an m2m / reverse_fk member is checked by its **size**
  (a non-negative int; `RelationSizeExpected`). Every case runs on SQLite and PostgreSQL; select, `first`, `one`,
  `rows`, aggregates (which count (base, related) pairs, cartesian tuples for several relations), `group_*` and
  `distinct` all compose with a relation join.
  **Compile errors, not silent fallbacks:** `join.type` other than `inner` / `left` (it used to build an inner
  join), `fk` and `fks` together or neither (one used to be dropped; a one-entry `fks` used to be ignored for an
  empty `fk`) — `JoinSpec::is_well_formed`, in `SpecWellFormed`; a member name the resolver cannot find
  (`QueryBuilder::join_member`'s `static_assert`); a relation model without `dataset: relations`
  (`YamlModelDatasetMatches`). `tests/yaml/test_spec_limits_join.cpp` pins these.
  **Known limits:** relation contents are checked by size only, not element by element; `join.related` is
  parsed but not read by anything (informational); and no `benchmark_tests.yaml` case runs an m2m / reverse_fk
  join yet (the m2m benchmark stays hand-written in `benchmarks/register.cpp`).
  **Build cost** (Debug, warm, single TU, min of 3 runs before / 4 after, machine under outside load so read
  ±1 s as noise): the existing slices are unchanged — select 15.3 → 13.6 s, grouped 13.9 → 13.5 s, aggregate
  9.0 → 9.5 s, crud 4.4 → 4.8 s; the new join slice (28 cases) is 10.8 s and runs in parallel with them.
  Release `register.cpp` (where the benchmark instantiates `QueryBuilder`) 15.9 → 15.7 s.
  **Inputs of the join slice beyond `tests/`:** `benchmarks/m2m_models.hpp` (BStudent / BCourse) and
  `shared/query_builder*.hpp`. `commit.sh`'s local smart-skip does not count those as test inputs, so an edit
  that touches only them skips the local test run; CI still runs the suite.
- **Models & key shapes (#872).** `tests/test_cases/models.yaml`, dataset `keys`: one model per key shape a
  query or a write treats differently — `UuidPkModel` (`storm::UUID` key) and `TimestampedRecord`
  (`auto_create` / `auto_update`) from `shared/models/`, plus the test-only models of `tests/test_key_models.h`
  (`KStock` composite key int + text, `KShipment` composite FK to it, `KUuidRef` FK to the UUID key, `KTicket`
  `primary_assigned` key; test-only so `storm_bench` is untouched). Tables and rows come from `keys::create_tables`
  / `keys::seed` (`tests/test_keys_dataset.h`, which also specialises `make_record` / `make_updated_record` /
  `is_original_record` for the write cases, with keys that never collide with the seeded ones). YAML names:
  `uuid_pk`, `uuid_ref`, `timestamped`, `stock`, `shipment`, `ticket`; plus `course` (the m2m benchmark's related
  model, on dataset `relations`). The other field types (`ExtendedTypes`, `Color`) are covered by `typed_values.yaml`
  (#844). A write case's expected counts include the seeded rows.
  **Compile errors, not silent fallbacks** (`tests/yaml/test_spec_limits_models.cpp`): a key-shape model without
  `dataset: keys`; a WHERE value not of the key's type (an int or an upper-case text on the UUID key, a string on an
  int key part, a date alone on a `time_point`); an `expected.first` value on an FK member (a whole related row); a
  write case on `KShipment` / `KUuidRef` (no `make_record`: their records need an FK target row).
  **Fixed on the way:** `QueryBuilder::apply_order_by` applied each `order_by:` term in its own `order_by<>()`
  call, and `QuerySet::order_by()` replaces the clause, so a multi-term `order_by:` silently sorted by its last
  term only (also in `storm_bench`'s `ORDER_BY_MULTI` cases, which are not in the `bench-ratio` matrix). The terms
  are now collected into one call; `keys_composite_pk_where_in_order_both_parts` pins the two-term SQL.
  **Build cost:** not measured (skipped on request).
- **One value check.** `expect_agg_value(actual, expected, expect_null, what)` is chosen by result type
  (`std::optional<double>` → near / `nullopt`, floating → near, integer → exact) and used by
  `AggregateRunner`; `verify_groups` checks `group_keys` and every step's `result.groups` (or `group_agg_*` for a
  single step), sorted by key.
- **One aggregate pipeline (#841).** `AggregateRunner` runs scalar, `chain` and `group_<func>` cases as
  `base_query → [group_by + having] → aggregates 1..N → check`. The parser (`normalise_aggregate_case` in
  `test_parser.hpp`) rewrites a scalar case (`int_val` / `dbl_val`) and a `group_<func>` case into one
  aggregation: the func is the query_type's, the fields the `aggregate:` shorthand's (or none), and
  `results[0].value` the int or double value, so the YAML corpus is unchanged. Such a case is exactly ONE
  aggregation (`TestCase::aggregate_shape_well_formed`, asserted by `AggregateRunner::run`): a second step in
  `aggregations:` is a `chain` case, an explicit `func` must agree with the query_type (`count_field` accepts
  `count` and `count_field`), and the value comes from `expected`, not a step's `result.value` — each of these
  was silently overwritten or dropped before.
  The check follows the result shape: a scalar value, a tuple (chain), or the grouped rows.
- **`query_type` → runner** is the `YAML_RUNNERS` table in `test_unified_yaml_body.h` (`{name, is_prefix,
  ^^Runner}`), spliced with `template [:runner:]<Model, Conn>::template run<tc>()`. A `query_type` missing from the
  table fails `KnownYamlQueryType` at compile time. To add a runner, add its rows there.

### Typed values and one null spelling (#844)

A YAML value is read in the **type of the field it is compared with** (WHERE operands) or the member it is
checked against (`expected.first`). The parser only records what was written, as a `TypedValue` kind: `Null`
(`null`), `Int` (fits 32 bits), `Int64` (wider; the one rule is `TypedValue::of_integer`), `Double`, `Bool`,
`String`. A value that does not fit its field is a **compile error, never a coercion** (`SpecValuesFit` for
WHERE, `ExpectedValueFits` for `expected.first`; both are `value_fits<FieldType>`, once, in
`shared/query_builder.hpp`). `typed_value_as` itself is constrained by the same predicate (`ValueFits`, #848),
so it can never read a member the value did not set (a Double or Bool on an integer or optional field, or a null):

| Field type | Written as | Rejected |
|---|---|---|
| nullable (`std::optional<T>`) | `null` with `==` / `!=` (WHERE) or as the expected value | `null` on a NOT NULL column; with any other operator; inside `IN`; `IS NULL` / `IS NOT NULL` |
| integer / floating | a number (an int64 literal needs a 64-bit field; a fraction needs a floating field; an integer written for a `float` must be exact in it: `16777217` is rejected) | out of range for the field, a string |
| bool | `true` / `false`, or the integer `0` / `1` | any other integer (`7`), a string |
| `std::chrono::duration` | an integer in the range of the duration's `rep`, `Int` or `Int64` alike (`expected.first` only, see below) | out of range for the `rep`, a fraction, a string |
| `storm::UUID` | `'550e8400-e29b-41d4-a716-446655440000'` (8-4-4-4-12 **lower-case** hex) | upper-case hex (`storm::UUID` keeps its text verbatim: SQLite compares TEXT case-sensitively, PostgreSQL returns lower case), any other string or a number |
| `std::chrono::year_month_day` | `'2024-01-15'` (ISO date; the day must exist) | `'2023-02-29'`, words, a number |
| `std::chrono::system_clock::time_point` | `'2024-01-15T10:30:00'` (UTC) | an out-of-range time, a number |
| enum | the enumerator **name** (`Green`) | an unknown name, a number |
| `std::vector<std::byte>` / `<uint8_t>` | hex (`'DEADBEEF'`, even length) | odd length, non-hex |

- **`null` is the one spelling of a null check.** `== null` / `!= null` lower to `IS NULL` / `IS NOT NULL`, the
  same rule as `== std::nullopt` in the C++ API. `op: IS NULL` / `IS NOT NULL` are no longer YAML operators (a
  compile error). The six corpus cases and the two `benchmark_tests.yaml` cases (`where_is_null`,
  `where_is_not_null`) were migrated; the SQL they produce is unchanged.
- **Quote the operator `>`** in YAML (`op: '>'`): a bare `>` is a block-scalar indicator and the operator would
  silently read as empty.
- **Dataset `extended`** (`model: extended` = `ExtendedTypes`): three rows, `alpha` / `beta` / `gamma`, defined in
  `extended_dataset()` in `test_unified_yaml_body.h` and tabulated in the comment above the `typed_*` cases in
  `typed_values.yaml`. Quote hex and numeric-looking strings (`'0102'`, or YAML reads an int).
- **A blob or a duration is checked in `expected.first` only** (#853). `ExpressionVariant` has no `ComparisonExpr`
  arm for either, so a WHERE on such a column cannot be built: the constraint rejects it (`NoWhereComparison`,
  `where_leaf_fits`) instead of failing deep inside `make_shared`. Engine support is tracked in #863.
- **A joined WHERE** is not checked against the base model by `QueryBuilder`'s class constraint (the WHERE may target
  the related model, whose field of the same name can have another type); `apply_where`'s per-`WhereModel`
  `static_assert` is the check that knows which model it is.
- **Unquoted dates** (`2024-01-15`) are read by PyYAML as a date, not text: `split_unified_cases.py` rejects them
  naming the case and key. `0102` and a bare time of day (`10:30:00`) are not detectable after loading (PyYAML reads them as the octal 66 and the base-60 integer 37800), so quote them.
- **HAVING is the WHERE tree (#868).** `group_by.having` is a `WhereSpec` (`HavingSpec` is an alias): the same
  `conditions` / `combine` YAML, operators, typed values and limits (7 nodes, 10 values) as `where`, built by
  `QueryBuilder::with_having` through the same `build_where_node`. GROUP BY takes up to 4 fields (more is a
  compile error); a case with more than one field pins `expected.count` only, because `group_keys` /
  `group_agg_*` are per single key. A HAVING condition compares fields, not aggregates (a Storm limit).
- Not covered here: write values (Phase 7 of #838).
- **Build cost of #868** (Debug, one TU touched and rebuilt alone, 3 runs each, load average 3-8, so read
  ±1 s): aggregate 11.1–11.2 s → 13.0–13.5 s, grouped 17.8–18.0 s → 20.1–22.0 s. The machinery alone (the
  same TUs with the corpus at `HEAD`) is aggregate 11.7–11.9 s and grouped ~18.5 s, so the five
  multi-field aggregate cases (~0.3 s each) and the four grouped chain cases (~0.4 s each) are most of it. A
  `verify_groups` that sorted the rows themselves cost ~2 s on the grouped TU (one `std::sort` per row
  type, i.e. per case); it sorts `(key, row index)` pairs instead, one instantiation for every case.
- **Build cost** (Debug, one TU touched and rebuilt alone, 3 runs each, quiet machine, `a4a11fc` vs #844):
  select 9.5–9.7 s → 14.2–14.9 s (+51%), aggregate 9.4–9.5 → 10.5–10.6 s (+12%), grouped 13.9–14.4 → 15.2–15.3 s
  (+8%), crud 6.1–6.4 → 6.9–7.0 s (+11%), measured with the first 22 cases. Where it goes (CPU seconds, select
  TU): 10.8 s without the new cases, 14.9 s with them, so the cases are almost all of it. A new case costs about
  what an old one does (~160 ms, #561: every case instantiates its own query type; the lever is #798), the
  dialect cases ~250 ms each (4 when measured, 3 remain). The check that values fit (`SpecValuesFit`) is not measurable. Five overlapping
  cases were dropped afterwards (17 remain). Creating the `ExtendedTypes` table only in the select TU was tried
  and reverted: it moved the other three TUs by less than the measurement noise, so the ~1 s they gained is not
  the fixture (unexplained).

### One spec parser (#845)

There is one JSON spec parser, `storm_benchmark_parser` (`benchmarks/parser.cppm`). It reads every key a
`BenchmarkTest` holds, with each accepted alias listed once: `test_name` / `name`, `test_category` / `category`,
`init_dataset_size` / `dataset_size`. `tests/test_parser.hpp` adds only the test-only keys (`query_type`,
`dataset`, `expected`, `expected_sql`, `expected_sql_sqlite`, `expected_sql_pg`, `where_expr`,
`insert_count` / `update_count` / `erase_count`, and each aggregation's `result`) and passes every other key to
`parse_bench_field` — `aggregations` itself is read by the shared `parse_aggregations`, which hands the test
parser every step key it does not know (#868), so no key is
parsed in two places. Add a shared key in `parse_bench_field` / `parse_bench_scalar`; a test-only key in
`parse_test_only_field`.

- **No corpus in the parser header.** `test_parser.hpp` holds no `#embed`; the corpus driver
  (`load_unified_tests()`) and the `STORM_UNIFIED_CASES_FILE` `#error` (#582) live in `test_unified_yaml_body.h`,
  which only the five slice TUs include. A parser-only TU (`test_spec_limits_test_parser.cpp`) includes
  `test_parser.hpp` alone.
- **`combine` is `and` or `or`**, exactly and case-sensitively (`WhereSpec::combine_of`). Any other spelling,
  `OR` included, makes the spec ill-formed, so the case does not compile (`SpecWellFormed`, #847), also when the
  `where:` has a single condition. A non-string `combine` (`combine: null`, a number) is rejected the same way.
  `yaml_to_json.py` and `split_unified_cases.py` pass the YAML value through unchanged.
- **Strings.** A brace or bracket inside a string is text: skipping a `{...}` / `[...]` value steps over its
  strings whole (escapes included), so `split_unified_cases.py` no longer rejects `{ } [ ]` in pinned SQL. A
  string longer than its buffer keeps what fits, is still read to its closing quote, and is counted in
  `BenchmarkTest::truncated_strings`, so `within_limits()` rejects the case like an over-long array. A value that is not a string where a string belongs (`"model": 5`) is consumed whole and leaves the field
  empty; a string cut off by the end of the input after a backslash stops at the end of the input.
- **Why the raw-pointer primitives.** The parser indexes a `const char*`, not `std::string_view::operator[]`:
  each call is constexpr steps. Measured by bisecting `-fconstexpr-steps` on a probe TU per corpus (the #582
  method; clang's default limit is 1,048,576):

  | Corpus | string_view parser (before) | raw-pointer test parser (before) | one parser (after) |
  |---|---|---|---|
  | grouped slice | 756,670 ¹ | 328,364 | 343,186 |
  | select slice | 770,488 ¹ | 316,287 | 329,288 |
  | aggregate slice | 571,967 ¹ | 210,589 | 216,883 |
  | crud slice | 267,513 ¹ | 94,231 | 94,718 |
  | `benchmark_tests.json` | 863,276 | 377,373 ¹ | 341,492 |

  ¹ that parser skipped the keys it did not know, so it parsed fewer keys than the "after" column. The test slices
  cost 0.5–4.5% more than the old test parser: the dispatcher knows the benchmark keys too, a skipped value
  steps over its strings, and an over-long string is read to its end. The benchmark corpus, which was at 82% of
  the default limit, is at 33%. No `-fconstexpr-steps` flag is needed.
- **Where the steps go** (grouped / crud, bisected one change at a time): `jkey_eq` is written out in full rather
  than forwarding to a helper (−4.4% / −4.6%: one call per candidate key is the dispatch's cost, the length is
  already compared first); `parse_str` copies in one loop and reads an over-long tail in a second one, so a
  string that fits pays no per-character overflow test (−3% / −4%); skipping strings inside a skipped value
  costs 0.4%. Dispatching keys on a `switch` on their length was tried and dropped: −2% on grouped, +4% on crud
  and the benchmark corpus.
- **Every case is checked against the caps.** `UnifiedYamlBody` static_asserts `TestCase::within_limits()`, so a
  case that does not build a `QueryBuilder` (a write) is rejected too. It is a separate constant evaluation: the
  real crud TU's step maximum is still the parse (94,718), and its compile time moved within noise.
  `load_benchmark_tests()` (`benchmarks/benchmark_tests.hpp`) static_asserts the same over `BENCHMARK_TESTS`
  (`all_within_limits`), so a benchmark write case is held to the string buffers as well; `register.cpp`'s
  Release compile time moved within noise (21.4 s either way).

### Spec limits are compile errors (#843)

A few spec arrays are capped (`BenchmarkTest::MAX_ORDER_BY`, `JoinSpec::MAX_FKS`, `GroupBySpec::MAX_FIELDS`,
`DistinctSpec::MAX_FIELDS`, `WhereSpec::MAX_NODES` = 7 and `WhereSpec::MAX_VALUES` = 10, `BenchmarkTest::MAX_AGGREGATIONS` = 5 steps of `AggregateSpec::fields` = 3, #868). A case that lists more entries than the cap **does not compile**; before #843 the parsers
dropped the extra entries and the case passed while checking less than it said.

- **Counting is in the schema.** `benchmarks/schema.cppm` owns the rule, once, in `add_capped`: `add_order_by`,
  `add_fk` and `add_field` count every entry and store only what fits. The parser (`benchmarks/parser.cppm`)
  calls them and still reads an over-long array to its end, so the key after it is parsed.
  An element of the wrong kind (a number in a list of names) is skipped, not counted, and a repeated key replaces
  the first instead of adding to it.
- **The check is one concept.** `BenchmarkTest::within_limits()` is false when any count passed its cap;
  `storm::orm::query_builder::SpecWithinLimits<spec>` is the constraint on `QueryBuilder<Model, spec, ...>`, so an
  over-cap case fails at the `QueryBuilder` use site (`BuilderFor` in `test_select_runner.h`).
- **To raise a limit**, change the `MAX_*` constant next to its struct and keep the current corpus maximum plus headroom;
  every slot costs compile time per case (~190 ms/case, #561).
- **Tested once, run through both entry points.** `tests/test_spec_limits.h` holds the cases;
  `tests/yaml/test_spec_limits_benchmark_parser.cpp` (`parse_test_object`) and `test_spec_limits_test_parser.cpp`
  (`parse_test_case_into`, which forwards every shared key) each pass their entry point to it. There is one parser
  since #845; the second TU checks that the test-side extension hands every shared key on.
- **WHERE.** 7 nodes is 4 conditions (4 leaves + 3 joins); the value pool of 10 is shared by the whole tree. The
  corpus needs at most 5 nodes and 3 values. A repeated `conditions` or `values` key replaces the first; a
  non-condition element is skipped, and a stray `}` inside an array is stepped over so the parse ends.
  `TestCase::within_limits()` adds each step's `result.groups` cap (20) to the spec's, which holds the chain
  caps since #868 (`test_spec_limits_aggregates.h`, run by both entry points, plus
  `tests/yaml/test_spec_limits_aggregates.cpp` for the test-only `result` and the `QueryBuilder` rejection). Separately from the caps, `SpecWellFormed` rejects a
  tree that cannot be built: a leaf with the wrong number of values for its operator, a join missing a child or
  carrying a field of its own, an empty tree, or a node the root cannot reach (a subtree that would be dropped).

## Writing New Tests

1. **Create test file**: `tests/<category>/test_<feature>.cpp`
2. **Include dependencies**:
   ```cpp
   #include <gtest/gtest.h>
   import storm;
   ```
3. **Follow AAA pattern**: Arrange, Act, Assert
4. **Test edge cases**: Empty input, null values, boundary conditions
5. **Verify error handling**: Check `std::expected` error cases
6. **Add performance tests**: If feature impacts performance

## Test Maintenance

- Run tests before committing: `./commit.sh` (or `./build/debug/tests/storm_tests` directly — see [Local Test Suite Speed](#local-test-suite-speed))
- Update tests when changing APIs
- Add regression tests for fixed bugs
- Keep tests fast (use in-memory database)
- Document test intent with clear names
