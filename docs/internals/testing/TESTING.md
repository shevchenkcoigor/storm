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

`tests/test_cases/unified_cases.yaml` is split by `scripts/split_unified_cases.py` into one JSON per
category, `#embed`ded into `tests/yaml/test_unified_yaml_*.cpp` and run on SQLite and PostgreSQL. A case
checks **results** (`expected:`). SQL text is pinned separately in C++ (`test_sql_verify.cpp`,
`test_fields_sql_parity.cpp`).

A case may also carry an optional `expected_sql:` — the exact `sql()` text, compared **before** the
statement executes. A case without it stays result-only. It is **prototyped on the aggregate runner only**
(`AggregateRunner`, all 58 scalar-aggregate cases); other runners ignore the key.

```yaml
- name: count_where_age_gte_35
  query_type: count
  where: {conditions: [{field: age, op: '>=', values: [35]}]}
  expected_sql: 'SELECT COUNT(*) FROM "person" WHERE "age" >= ?'
  expected: {int_val: 11}
```

- `sql()` is backend-independent for these statements (placeholders stay `?`), so one string covers both
  backends. A per-dialect `expected_sql_sqlite` / `expected_sql_pg` is not implemented: no aggregate case needs
  one (that is `NULLS FIRST/LAST` and `LIMIT ALL` vs `LIMIT -1`, which no aggregate case emits).
- Write the string from the case, not by pasting the code's output — a pinned wrong string passes forever.
- The capacity is `EXPECTED_SQL_CAPACITY` (160) in `tests/test_parser.hpp`, mirrored in
  `scripts/split_unified_cases.py`, which fails the build on a string of 160 or more characters (the C++ parser
  cannot report an overflow), on non-printable or non-ASCII text, on `{ } [ ]` (the parser's brace skipper ignores string boundaries), and on `expected_sql` in a category whose
  runner does not read it yet (it would pass without ever comparing SQL). The parser unescapes `\"` and `\\`, which is how the quoted identifiers survive the JSON hop.
- Write paths (`to_sql()` inlines bound values), set-ops and upsert still need C++ tests.
- **Build cost** (Release, warm, aggregate TU, 3 runs each): 25.5–27.3 s before, 26.9–27.0 s after
  (about +1 s, +4%, at the edge of run-to-run noise); the select TU, which carries no `expected_sql` but whose
  `TestCase` grew by 160 bytes, is unchanged (28.7–29.2 s vs 28.7–29.2 s).

## How a YAML case reaches the query API (#834)

The runners (`tests/test_select_runner.h`, `tests/test_unified_yaml_body.h`) have no branch per model,
aggregate or field. Names in the YAML are resolved by reflection at compile time:

- **`model:`** looks up `YAML_MODELS` in `test_unified_yaml_body.h` (`person`, `message`, `simple`), and the
  type is spliced into whichever runner the `query_type` picks. To support a new model, add one row there. An
  unknown name is a compile error (`KnownYamlModel`). Before #834 a missing branch silently ran nothing:
  three `message` cases (`select_join_where_*`, `distinct_join_where_like_o`) never executed, and two of
  them had lost their `LIKE` patterns unnoticed.
- **Aggregates** — `query_type` (`count`, `sum`, `avg`, `min`, `max`, `count_distinct`; `count_field` →
  `count`), the `group_<func>` suffix, and each chain `func:` are the **method names** themselves.
  `member_function_named<Q>(name)` finds the member function template, and `call_query_method` splices it with
  zero selectors (`COUNT(*)`) or one. Both live in `shared/query_builder.hpp` (#835), so the tests and
  `benchmarks/query_benchmark.cppm` use one copy; `QueryBuilder::resolve_aggregate` (the benchmark path) is the same lookup, with
  `ValidAggregate` kept as the allow-list of names a YAML `func` may reach there; the test runners have no allow-list. Chains are built by recursion over
  `aggregations:`, one step per entry. A name that is not a member function template of the query type is a
  compile error, but any such member would be called, so a `func:` must be an aggregate.
- **`expected.first:`** keys are row **member names**. Each is compared according to the member's C++ type
  (string, bool, floating with a 0.01 tolerance, integer). A key with no matching member, or a value of the
  wrong kind, is a compile error. Before #834 such a key was skipped.
- Results are checked according to the C++ result type: `std::optional<double>` (MIN/MAX/AVG, #416) against
  `dbl_val` / `res_value`, integers exactly. Per-group values (`group_keys`, `group_agg_*`) are now checked
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
- **`order_by[].collate`** is an enumerator name of `Collate` (other than `None`) matched case-insensitively (`NOCASE` → `NoCase`);
  an unknown name is a compile error (`ValidCollate`) instead of falling back to `NoCase`.
- **`distinct.fields`** takes any number of fields through one path (`QueryBuilder::build_distinct_stmt`); a
  third entry used to be ignored (`distinct_dept_age_name_three_fields`).
- Still explicit: the set-operation keyword (`union` → `union_`), the JOIN type, `typed_value_as`, `apply_limit`
  and the terminal choice. Their branches differ in behaviour, not only in a name.

### Runner structure (#837)

- **No base class.** Every runner (`SelectRunner`, `AggregateRunner`, `ChainAggRunner`, `DistinctRunner`,
  `GroupByRunner`, `InsertRunner`, `UpdateRunner`, `EraseRunner`) is a stateless class with a
  `static run<Tc>()`. The two helpers the old `QueryRunnerBase` mixed are free functions:
  `base_query<Tc, Model, Conn>()` (the case's filtered/joined `QuerySet`) and
  `seed_records<Tc, Model, Conn>()` (insert the dataset). The query is a local, not runner state.
- **One WHERE tree (#843).** Every WHERE form fills one `WhereSpec` tree of `WhereNode`s
  (`benchmarks/schema.cppm`): `where:` is its leaves joined left to right by `combine` (AND when absent), and the
  test-only `where_expr:` is written as the tree itself (nodes numbered in pre-order from the root). `QueryBuilder`
  builds the tree (`build_where_node`), and every leaf goes through the shared `build_compare_expr` resolver, so an
  unknown or ill-typed operator is a compile error. The operands of all leaves live in one pool
  (`WhereSpec::values`), typed by the key that wrote them: `value_dbl: 0.0` is a double, not the int 0. A case that
  writes both `where:` and `where_expr:` does not compile (two trees cannot share one set of nodes).
- **FK by member name.** `QueryBuilder`'s `fk_resolver` defaults to `member_name_resolver<Model>`, so a
  `join.fk:` is a member name on the model. The benchmark still passes its own (`registry::resolve_fk_field`).
- **One value check.** `expect_agg_value(actual, expected, expect_null, what)` is chosen by result type
  (`std::optional<double>` → near / `nullopt`, floating → near, integer → exact) and used by the scalar,
  chain and group runners; `verify_groups` checks `group_keys` / `group_agg_*` (sorted by key).
- **`query_type` → runner** is the `YAML_RUNNERS` table in `test_unified_yaml_body.h` (`{name, is_prefix,
  ^^Runner}`), spliced with `template [:runner:]<Model, Conn>::template run<tc>()`. A `query_type` missing from the
  table fails `KnownYamlQueryType` at compile time. To add a runner, add its rows there.

### Spec limits are compile errors (#843)

A few spec arrays are capped (`BenchmarkTest::MAX_ORDER_BY`, `JoinSpec::MAX_FKS`, `GroupBySpec::MAX_FIELDS`,
`DistinctSpec::MAX_FIELDS`, `WhereSpec::MAX_NODES` = 7 and `WhereSpec::MAX_VALUES` = 10, `TestCase::MAX_CHAIN`). A case that lists more entries than the cap **does not compile**; before #843 the parsers
dropped the extra entries and the case passed while checking less than it said.

- **Counting is in the schema.** `benchmarks/schema.cppm` owns the rule, once, in `add_capped`: `add_order_by`,
  `add_fk` and `add_field` count every entry and store only what fits. Both parsers (`benchmarks/parser.cppm`,
  `tests/test_parser.hpp`) call them and still read an over-long array to its end, so the key after it is parsed.
  An element of the wrong kind (a number in a list of names) is skipped, not counted, and a repeated key replaces
  the first instead of adding to it.
- **The check is one concept.** `BenchmarkTest::within_limits()` is false when any count passed its cap;
  `storm::orm::query_builder::SpecWithinLimits<spec>` is the constraint on `QueryBuilder<Model, spec, ...>`, so an
  over-cap case fails at the `QueryBuilder` use site (`BuilderFor` in `test_select_runner.h`).
- **To raise a limit**, change the `MAX_*` constant next to its struct and keep the current corpus maximum plus headroom;
  every slot costs compile time per case (~190 ms/case, #561).
- **Tested once, run against both parsers.** `tests/test_spec_limits.h` holds the cases;
  `tests/yaml/test_spec_limits_benchmark_parser.cpp` and `test_spec_limits_test_parser.cpp` each pass their parser to it.
  Merging the two parsers is #845.
- **WHERE.** 7 nodes is 4 conditions (4 leaves + 3 joins); the value pool of 10 is shared by the whole tree. The
  corpus needs at most 5 nodes and 3 values. A repeated `conditions` or `values` key replaces the first; a
  non-condition element is skipped, and a stray `}` inside an array is stepped over so the parse ends.
  `TestCase::within_limits()` adds the chain cap to the spec's. Separately from the caps, `SpecWellFormed` rejects a
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
