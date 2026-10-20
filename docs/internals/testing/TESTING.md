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

**Takeaway**: for local iteration, run the gtest binaries directly. `commit.sh`'s
test step does this (three binaries: `storm_tests`, `mock_sqlite/storm_mock_tests`,
`mock_libpq/storm_pq_mock_tests`). `ctest` is still useful locally for `-R`/`-N`
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
  TYPED_TEST'd), plus 22 in `storm_mock_tests` and 37 in `storm_pq_mock_tests`
  (mock error-path binaries) — 100% passing
- ~33s wall time running the binaries directly (measured 2026-08-27)

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
