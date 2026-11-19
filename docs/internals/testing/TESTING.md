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
`group_by`, `chain`, `distinct`, `writes`, `typed_values`; #846). They are authoring groups only: a case spans
several topics and a topic spans several runners, so a case in the "wrong" file is not an error, and a new
topic file is picked up by a CMake glob with no edit. `scripts/split_unified_cases.py` reads all of them, buckets
every case by `query_type` into one JSON per category (`select` / `grouped` / `aggregate` / `crud`, the compile
slices -- unchanged), and rejects an unknown `query_type` or a case name repeated anywhere in the corpus. It
rewrites only a JSON whose bytes changed, so editing one case recompiles one slice. The JSONs are
`#embed`ded into `tests/yaml/test_unified_yaml_*.cpp` and run on SQLite and PostgreSQL. A case
checks **results** (`expected:`). SQL text is pinned separately in C++ (`test_sql_verify.cpp`,
`test_fields_sql_parity.cpp`).

A case may also carry an optional `expected_sql:` — the exact `sql()` text, compared **before** the
statement executes. A case without it stays result-only. It is read by `AggregateRunner` (all 58 scalar-aggregate
cases) and by `SelectRunner` on `query_type: select` (#844); `first` / `one` and the other runners reject it
(a compile error and a `split_unified_cases.py` error), since they would pass without comparing SQL. Chain and
group cases share `AggregateRunner` but the split script still rejects the key outside `aggregate` and `select`.

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
- The capacity is `EXPECTED_SQL_CAPACITY` (160) in `tests/test_parser.hpp`, mirrored in
  `scripts/split_unified_cases.py`, which fails the build on a string of 160 or more characters (the C++ parser
  rejects every over-long case at compile time too, #845; the script keeps its check so the error names the case and the key), on non-printable or non-ASCII text, and on `expected_sql` in a category whose
  runner does not read it yet (it would pass without ever comparing SQL). The parser unescapes `\"` and `\\`, which is how the quoted identifiers survive the JSON hop.
- Write paths (`to_sql()` inlines bound values), set-ops and upsert still need C++ tests.
- **Build cost** (Release, warm, aggregate TU, 3 runs each): 25.5–27.3 s before, 26.9–27.0 s after
  (about +1 s, +4%, at the edge of run-to-run noise); the select TU, which carries no `expected_sql` but whose
  `TestCase` grew by 160 bytes, is unchanged (28.7–29.2 s vs 28.7–29.2 s).

## How a YAML case reaches the query API (#834)

The runners (`tests/test_select_runner.h`, `tests/test_unified_yaml_body.h`) have no branch per model,
aggregate or field. Names in the YAML are resolved by reflection at compile time:

- **`model:`** looks up `YAML_MODELS` in `test_unified_yaml_body.h` (`person`, `message`, `simple`, `extended`), and the
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
  (string, bool, floating with a 0.01 tolerance, integer, and the typed values of #844 below). A key with no
  matching member, or a value that does not fit the member (`ExpectedValueFits`), is a compile error. Before
  #834 such a key was skipped.
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

- **No base class.** Every runner (`SelectRunner`, `AggregateRunner`, `DistinctRunner`,
  `InsertRunner`, `UpdateRunner`, `EraseRunner`) is a stateless class with a
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
  (`std::optional<double>` → near / `nullopt`, floating → near, integer → exact) and used by
  `AggregateRunner`; `verify_groups` checks `group_keys` / `group_agg_*` (sorted by key).
- **One aggregate pipeline (#841).** `AggregateRunner` runs scalar, `chain` and `group_<func>` cases as
  `base_query → [group_by + having] → aggregates 1..N → check`. The parser (`normalise_aggregate_case` in
  `test_parser.hpp`) rewrites a scalar case (`int_val` / `dbl_val`) and a `group_<func>` case into a
  one-element `aggregations` list (`res_value` = the int or double value), so the YAML corpus is unchanged.
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
| integer / floating / bool | a number (an int64 literal needs a 64-bit field; a fraction needs a floating field) | out of range for the field, a string |
| `storm::UUID` | `'550e8400-e29b-41d4-a716-446655440000'` (8-4-4-4-12 hex) | any other string or a number |
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
- A blob is checked in `expected.first` only; a WHERE on a blob column compiles only where the field proxy
  accepts a blob operand.
- Not covered here: HAVING values (still one int) and write values (Phase 7 of #838).
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
`dataset`, `expected`, `expected_sql`, `expected_sql_sqlite`, `expected_sql_pg`, `aggregations`, `where_expr`,
`insert_count` / `update_count` / `erase_count`) and passes every other key to `parse_bench_field`, so no key is
parsed in two places. Add a shared key in `parse_bench_field` / `parse_bench_scalar`; a test-only key in
`parse_test_only_field`.

- **No corpus in the parser header.** `test_parser.hpp` holds no `#embed`; the corpus driver
  (`load_unified_tests()`) and the `STORM_UNIFIED_CASES_FILE` `#error` (#582) live in `test_unified_yaml_body.h`,
  which only the four slice TUs include. A parser-only TU (`test_spec_limits_test_parser.cpp`) includes
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
`DistinctSpec::MAX_FIELDS`, `WhereSpec::MAX_NODES` = 7 and `WhereSpec::MAX_VALUES` = 10, `TestCase::MAX_CHAIN`). A case that lists more entries than the cap **does not compile**; before #843 the parsers
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
