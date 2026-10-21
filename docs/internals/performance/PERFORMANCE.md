# Performance

Storm targets ≥95% of raw SQLite efficiency. This page collects the
performance guidelines, hot-path tips, and the benchmarking/testing workflow.

> **Note**: this page is about ORM/query performance (the SQL Storm generates
> and executes). For local *test-suite* run speed (how fast `ctest`/the gtest
> binaries finish), see
> [TESTING.md#local-test-suite-speed](../testing/TESTING.md#local-test-suite-speed).

## Guidelines

### Core Philosophy

When deciding which implementation approach is "good", **performance testing is mandatory**. Storm ORM's primary goal is achieving near-raw SQLite performance, so all implementation decisions must be validated with benchmarks.

### Performance Evaluation Requirements

#### 1. Benchmark Before Accepting

- Every new feature (WHERE clauses, LIMIT, ORDER BY, etc.) **requires** performance testing
- Compare Storm ORM against raw SQLite to measure efficiency percentage
- **Target**: ≥95% of raw SQLite performance for acceptable implementations
- Document results in docs with operation details and efficiency metrics

#### 2. Add a Benchmark via YAML

- Storm benchmarks are declared as YAML entries in `benchmarks/tests/benchmark_tests.yaml`;
  `benchmarks/register.cpp` walks `BENCHMARK_TESTS` and registers each one as a Google
  Benchmark automatically — there is no per-feature `.cpp` benchmark file to write.
- The reusable fixtures live in `benchmarks/query_benchmark.cppm` and
  `benchmarks/crud_benchmark.cppm`; a YAML entry routes a test to the right fixture.
- Run a category with `./build/release/benchmarks/storm_bench --benchmark_filter='Storm/<CATEGORY>/.*'`
  and compare against the raw baseline binary `./build/release/benchmarks/storm_anchors`.

#### 3. Raw SQL Comparison is Required

- Every benchmark must include raw SQLite baseline for efficiency calculation
- Report efficiency percentage: `(Storm_ORM_performance / Raw_SQLite_performance) * 100`
- Include in commit message: "feat: add LIMIT support (85% of raw SQLite efficiency)"

### Design Principles Balance

Storm ORM follows **DRY (Don't Repeat Yourself)** and **KISS (Keep It Simple, Stupid)** principles, but **performance takes precedence** when there's a conflict.

#### DRY Principle

- ✅ **Apply DRY**: Extract common patterns into BaseStatement utilities, use templates for reusable code
- ✅ **Apply DRY**: Compile-time SQL generation with ConstexprString (eliminates runtime duplication)
- ❌ **Break DRY if needed**: If abstraction costs >10% performance, duplicate code instead
- **Example**: Statement-level caching duplicates caching logic across Update/Delete/Select, but achieves 20x+ speedup

#### KISS Principle

- ✅ **Apply KISS**: Prefer simple, readable implementations when performance is equivalent
- ✅ **Apply KISS**: Use standard library features (`std::index_sequence`) over complex custom solutions
- ❌ **Break KISS if needed**: Complex optimizations justified if they improve performance significantly
- **Example**: Thread-local SQL caching adds complexity but provides 94% performance improvement

### Decision Matrix

| Scenario | DRY/KISS Compliant? | Performance Impact | Decision |
|----------|---------------------|-------------------|----------|
| Clean abstraction, no perf cost | ✅ Yes | None | Use abstraction (follow DRY/KISS) |
| Clean abstraction, <5% perf cost | ✅ Yes | Minimal | Use abstraction (follow DRY/KISS) |
| Clean abstraction, 5-10% perf cost | ⚠️ Maybe | Moderate | Profile carefully, document tradeoff |
| Clean abstraction, >10% perf cost | ❌ No | Significant | Duplicate code, prioritize performance |
| Complex optimization, >20% perf gain | ❌ No | Major improvement | Accept complexity, document thoroughly |

### Performance Testing Workflow

```bash
# 1. Implement feature (e.g., LIMIT support)
# 2. Add a YAML entry under benchmarks/tests/benchmark_tests.yaml describing the test
#    — register.cpp will pick it up automatically and route to the right fixture.

# 3. Build and run performance tests (Google Benchmark CLI)
cmake --preset ninja-release && cmake --build --preset ninja-release
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/.*LIMIT.*' \
    --benchmark_repetitions=10 --benchmark_report_aggregates_only=true

# 4. Compare with raw SQLite via the separate anchors binary (release-time only)
./build/release/benchmarks/storm_anchors

# 5. Document in benchmarks/README.md
# Add to performance results table with efficiency percentage

# 6. Commit with performance metrics
git add . && git commit -m "feat: add LIMIT support (85% of raw SQLite)"
```

### Mandatory Performance Documentation

When adding new features, update documentation with:

1. **Performance Results Table**: Add row with Storm vs Raw SQLite comparison
2. **Key Design Decisions**: Document optimization techniques used
3. **Benchmark Commands**: Show how to run performance tests for this feature

### Feature Implementation Checklist

- [ ] Implement feature in `src/orm/statements/`
- [ ] Add comprehensive tests in `tests/<category>/test_<feature>.cpp`
- [ ] Add a benchmark YAML entry in `benchmarks/tests/benchmark_tests.yaml`
- [ ] Run benchmark and measure efficiency vs raw SQLite
- [ ] If efficiency <95%, optimize or document reasons
- [ ] Update `benchmarks/README.md` with performance tables
- [ ] Document any DRY/KISS tradeoffs made for performance
- [ ] Commit with performance metrics in message

### Performance Regression Prevention

- All PRs with ORM changes must include benchmark results
- CI should fail if performance drops >5% without justification
- Run `./build/release/benchmarks/storm_bench` and `./build/release/benchmarks/storm_anchors` and compare efficiency before committing to catch regressions

### Profiling Tips

1. **Always use Release builds** for accurate profiling
2. **Use perf/valgrind** for hotspot analysis
3. **Profile with realistic data** (10K+ rows)
4. **Measure multiple iterations** for statistical significance
5. **Compare against raw SQLite** as baseline
6. **Document bottlenecks** in design decision docs

### Performance Goals

- **CRUD Operations**: ≥95% of raw SQLite efficiency
- **JOIN Operations**: ≥95% of raw SQLite efficiency
- **DISTINCT Operations**: ≥95% of raw SQLite efficiency
- **Batch Operations**: Match or exceed sqlite_orm by 2x+
- **Statement Caching**: 20x+ speedup for repeated operations

## Tips

Practical optimization techniques for Storm ORM.

### Transaction Wrapping for Loop Inserts

#### The Problem

When inserting objects in a loop without explicit transaction, each insert auto-commits:

```cpp
// SLOW: ~1.0M ops/sec - each insert triggers auto-commit
for (auto& obj : objects) {
    qs.insert(obj).execute();
}
```

#### The Solution

**Option 1: Use batch insert (recommended)**

```cpp
// FAST: ~1.9M ops/sec - handles transaction internally
qs.insert(objects).execute();
```

**Option 2: Manual transaction wrapping**

```cpp
// FAST: ~1.7M ops/sec - single commit at end
auto conn = DefaultConnection<>::get();
auto txn  = storm::begin(conn);          // RAII guard (#415)
for (auto& obj : objects) {
    qs.insert(obj).execute();
}
(void)txn->commit();                       // single COMMIT; auto-ROLLBACK on early exit
```

Prefer `storm::begin(conn)` over raw `conn->execute("BEGIN TRANSACTION")`: the
guard rolls back on failure and cooperates with batch ops' inner transactions
(no nested-BEGIN collision, #9).

#### Benchmark Results (10,000 inserts, in-memory SQLite)

| Method | Speed | Notes |
|--------|-------|-------|
| Loop without transaction | ~1.0 M ops/sec | Auto-commit overhead |
| Loop with transaction | ~1.7 M ops/sec | Single commit |
| Batch insert | ~1.9 M ops/sec | Optimized bulk SQL |

#### Why This Matters

Each auto-commit triggers expensive operations:

| Database | Auto-commit Overhead |
|----------|---------------------|
| **SQLite** | Journal write + fsync |
| **PostgreSQL** | WAL flush + fsync |
| **MySQL (InnoDB)** | Redo log flush |
| **All databases** | Lock acquire/release |

Wrapping N inserts in one transaction amortizes this overhead:
- Without transaction: N x (insert + commit overhead)
- With transaction: N x insert + 1 x commit overhead

#### Applies to All Databases

This optimization is **universal** - it works for PostgreSQL, MySQL, SQLite, and any ACID-compliant database. The principle is the same: reduce commit frequency to reduce I/O overhead.

#### When to Use Each Approach

| Scenario | Recommendation |
|----------|----------------|
| Insert many objects at once | `qs.insert(objects)` - batch insert |
| Insert objects one at a time from stream | Manual transaction wrapping |
| Single insert per request (web app) | No transaction needed - auto-commit is fine |
| Mixed operations (insert + update + delete) | Manual transaction wrapping |

#### Error Handling with Manual Transactions

```cpp
const auto& conn = DefaultConnection<>::get();

if (auto result = conn->execute("BEGIN TRANSACTION"); !result) {
    // Handle error
    return;
}

bool success = true;
for (auto& obj : objects) {
    if (auto result = qs.insert(obj); !result) {
        success = false;
        break;
    }
}

if (success) {
    conn->execute("COMMIT");
} else {
    conn->execute("ROLLBACK");
}
```

### Flat Code vs Nested Lambdas

**For hot paths, prefer flat code over nested lambdas.** Benchmarks show ~3-4% improvement.

#### The Problem

Nested lambdas add overhead from captures, indirect calls, and inlining barriers:

```cpp
// ❌ SLOW: Nested-lambda monadic wrappers (90% efficiency).
// (The old execute_with_transaction/execute_with_statement helpers that encoded
//  this shape were removed in #434 once every caller had moved to flat code.)
prepare_and_transact(conn, true,
    [this, objects]() {                          // Lambda 1: captures
        return run_with_statement(conn, sql,
            [this, objects](auto& stmt) {        // Lambda 2: captures again
                for (...) { ... }
            });
    });
```

#### The Solution

```cpp
// ✅ FAST: Flat code (93-94% efficiency)
auto* stmt = *conn_->prepare_cached(sql);  // Connection cache: one prepare, reused by SQL text
conn_->execute("BEGIN TRANSACTION");
for (const auto& obj : objects) {
    stmt->reset();
    bind(...);
    stmt->execute();
}
conn_->execute("COMMIT");
```

#### Why Lambdas Are Slower

- Capture storage overhead (storing `this`, spans, etc.)
- Indirect call through function pointer
- Compiler inlining barriers at lambda boundaries
- Extra stack frame creation per lambda

#### When to Use Each

| Flat Code | Lambdas |
|-----------|---------|
| Hot paths (millions of calls) | Cold paths (setup, config) |
| Inner loops, batch operations | Callbacks, event handlers |
| Performance-critical ORM ops | Code reuse across callers |

### Raw Pointer Caching in Hot Loops

**For query loops extracting many rows, cache the raw `sqlite3_stmt*` pointer.** Benchmarks show ~5-6% improvement.

#### The Problem

```cpp
// ❌ SLOW: unique_ptr::get() called on every column (90.6% efficiency)
while (stmt->step() == SQLITE_ROW) {
    obj.id = sqlite3_column_int64(stmt->handle(), 0);    // handle() = unique_ptr::get()
    obj.name = sqlite3_column_text(stmt->handle(), 1);   // handle() again
    obj.age = sqlite3_column_int(stmt->handle(), 2);     // handle() again
    // ... 6+ calls per row × millions of rows
}
```

#### The Solution

```cpp
// ✅ FAST: Cache raw pointer once (96% efficiency)
sqlite3_stmt* raw_stmt = stmt->handle();  // Cache ONCE before loop
while (sqlite3_step(raw_stmt) == SQLITE_ROW) {
    obj.id = sqlite3_column_int64(raw_stmt, 0);    // Direct pointer
    obj.name = sqlite3_column_text(raw_stmt, 1);   // No indirection
    obj.age = sqlite3_column_int(raw_stmt, 2);     // Maximum speed
}
```

#### Why This Matters

- `unique_ptr::get()` is not free - it's a function call with pointer dereference
- Called 6+ times per row (once per column)
- For 10,000 rows: 60,000+ unnecessary function calls
- Compiler may not inline across translation units

#### Benchmark Evidence (SELECT WHERE with 10K rows)

| Pattern | Efficiency |
|---------|------------|
| Without raw pointer cache | 90.6% |
| With raw pointer cache | 96% |

## Testing & Benchmarking

Performance testing is **mandatory** for all new Storm ORM features. Target: ≥95% of raw SQLite efficiency.

### Quick Start

```bash
cmake --preset ninja-release -DENABLE_BENCH=ON
cmake --build --preset ninja-release

./build/release/benchmarks/storm_bench                                       # All microbenchmarks
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/.*JOIN.*'    # JOIN performance
./build/release/benchmarks/storm_anchors                                     # Raw SQLite baseline (full comparison)
```

### Performance Requirements

Every new feature requires:

1. **Benchmark**: a YAML entry in `benchmarks/tests/benchmark_tests.yaml`
2. **Raw SQLite comparison**: Calculate efficiency percentage
3. **Documentation**: Add results to relevant docs
4. **Commit message**: Include performance metrics

**Example**: `feat: add LIMIT support (85% of raw SQLite)`

### Design Principles

#### DRY/KISS vs Performance

Storm follows DRY/KISS principles **unless** they cost >10% performance:

| Scenario | DRY/KISS | Performance | Decision |
|----------|----------|-------------|----------|
| Clean abstraction, <5% cost | ✅ Yes | Minimal | Follow DRY/KISS |
| Clean abstraction, >10% cost | ❌ No | Significant | Prioritize performance |
| Complex optimization, >20% gain | ❌ No | Major | Accept complexity |

#### Decision Matrix

1. **<5% performance impact**: Use abstraction (follow DRY/KISS)
2. **5-10% performance impact**: Profile carefully, document tradeoff
3. **>10% performance impact**: Duplicate code if needed
4. **>20% performance gain**: Accept complexity, document thoroughly

### Benchmark Workflow

#### 1. Implement Feature

```bash
# Example: Add LIMIT support
src/orm/statements/select.cppm
```

#### 2. Add a Benchmark YAML Entry

Add an entry to `benchmarks/tests/benchmark_tests.yaml` describing the test
(category, fixture, dataset size). `benchmarks/register.cpp` walks
`BENCHMARK_TESTS` and registers it automatically — no `.cpp` file to write.

#### 3. Run Benchmark

```bash
cmake --preset ninja-release -DENABLE_BENCH=ON
cmake --build --preset ninja-release
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/.*LIMIT.*'
```

#### 4. Calculate Efficiency

```
Efficiency = (Storm_ORM_performance / Raw_SQLite_performance) * 100%
```

**Example**:
- Storm: 8.5M rows/sec
- Raw: 10M rows/sec
- Efficiency: 85% ⚠️ NEEDS WORK (target: ≥95%)

#### 5. Document Results

Update relevant documentation:
- Performance tables
- Optimization techniques
- Benchmark commands

#### 6. Commit with Metrics

```bash
git add . && git commit -m "feat: add LIMIT support (85% of raw SQLite)"
```

### Example Benchmarks

#### SELECT Benchmark

```cpp
void benchmark_storm_select() {
    QuerySet<Person> qs(conn);
    for (int i = 0; i < iterations; ++i) {
        auto result = qs.select().execute();
    }
}

void benchmark_raw_select() {
    auto stmt = conn.prepare("SELECT id, name, age FROM Person");
    for (int i = 0; i < iterations; ++i) {
        while (stmt->step()) {
            // Extract manually
        }
        stmt->reset();
    }
}
```

#### WHERE Benchmark

```cpp
void benchmark_storm_where() {
    QuerySet<Person> qs(conn);
    for (int i = 0; i < iterations; ++i) {
        auto result = qs.where(fields::Person.age > 25).select().execute();
    }
}
```

### Writing Fair Benchmarks

**⚠️ CRITICAL: Unfair benchmarks lead to wrong optimization decisions.**

#### 1. Setup Outside Loop, Execute Inside

```cpp
// ❌ UNFAIR: Storm does setup inside, raw SQLite does setup outside
int storm_benchmark(int iterations) {
    for (int i = 0; i < iterations; i++) {
        qs.where(age > 30).select();  // WHERE built every iteration
    }
}

int raw_benchmark(int iterations) {
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);  // Prepared ONCE
    for (int i = 0; i < iterations; i++) {
        sqlite3_reset(stmt);
        sqlite3_step(stmt);
    }
}

// ✅ FAIR: Both do setup once, execute in loop
int storm_benchmark(int iterations) {
    auto where_clause = build_where();
    qs.where(where_clause);           // Set WHERE once
    for (int i = 0; i < iterations; i++) {
        qs.select();                  // Only execute in loop
    }
    qs.reset();                       // Cleanup after
}

int raw_benchmark(int iterations) {
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, value); // Bind once
    for (int i = 0; i < iterations; i++) {
        sqlite3_reset(stmt);          // Reset required for re-execution
        while (sqlite3_step(stmt) == SQLITE_ROW) { ... }
    }
}
```

#### 2. Same Algorithm for Both

```cpp
// ❌ UNFAIR: Storm uses chunked bulk SQL, raw uses single inserts
// Storm: INSERT INTO t VALUES (?,?),(?,?),(?,?)  -- 3 rows at once
// Raw:   INSERT INTO t VALUES (?,?) × 3          -- 3 separate statements

// ✅ FAIR: Both use same strategy
// Storm: INSERT INTO t VALUES (?,?),(?,?),(?,?)
// Raw:   INSERT INTO t VALUES (?,?),(?,?),(?,?)
```

#### 3. Same Container Types

```cpp
// ❌ UNFAIR: Storm uses plf::hive, raw uses std::vector
plf::hive<Model> storm_results;  // O(1) insert, stable iterators
std::vector<Model> raw_results;  // O(1) amortized, may reallocate

// ✅ FAIR: Both use same container
plf::hive<Model> storm_results;
plf::hive<Model> raw_results;
```

#### 4. Measure Same Work

```cpp
// ❌ MISLEADING: Comparing throughput with different result sizes
// DISTINCT + WHERE: 78 rows → 0.13M rows/sec (looks slow)
// DISTINCT + JOIN:  10K rows → 8.75M rows/sec (looks fast)

// ✅ CORRECT: Use latency (ms/query) for queries with different result sizes
// DISTINCT + WHERE: 0.588ms/query (actually FASTEST)
// DISTINCT + JOIN:  1.143ms/query (actually slower)
```

#### 5. Runtime vs Compile-Time Fairness

```cpp
// ❌ UNFAIR: Storm uses runtime batch_size, raw uses compile-time
template <int BatchSize>  // Raw gets free compile-time optimization
void raw_benchmark() {
    if constexpr (BatchSize < 100) { ... }  // Compiled away
}

void storm_benchmark(int batch_size) {  // Runtime check every call
    if (batch_size < 100) { ... }
}

// ✅ FAIR: Both use runtime values
void storm_benchmark(int batch_size) { ... }
void raw_benchmark(int batch_size) { ... }  // Same decision logic
```

#### Benchmark Checklist

- [ ] **Setup outside loop**: WHERE clauses, statement preparation, parameter binding
- [ ] **Same algorithm**: Both use identical strategies (chunked bulk, single row, etc.)
- [ ] **Same containers**: plf::hive vs plf::hive, not plf::hive vs std::vector
- [ ] **Same decision logic**: Runtime vs runtime, not runtime vs compile-time
- [ ] **Correct metric**: Latency for different result sizes, throughput for same sizes
- [ ] **Multiple runs**: 5+ runs to establish variance, report median not just mean

### Performance Regression Prevention

- **CI integration**: Run benchmarks on every PR
- **Threshold checks**: Fail if performance drops >5%
- **Before commit**: run `./build/release/benchmarks/storm_bench` and `./build/release/benchmarks/storm_anchors` and compare efficiency

### See Also

- [Adding Features](../building/ADDING_FEATURES.md) - Development workflow
- [Benchmarks](https://github.com/spiritEcosse/storm/blob/develop/benchmarks/README.md) - Current benchmark results

## PostgreSQL Binary Result Format (#600 Phase 1)

The PostgreSQL backend requests libpq **binary result format** for SELECT-shaped queries
when every selected column is a plain byte reinterpretation (bool/int/int64/double/float/
text/blob) — decoding network-byte-order bytes (`memcpy` + `std::byteswap` + `bit_cast`)
instead of paying the `strtol`/`strtoll`/`strtod`/`val[0]=='t'` ASCII-parse cost text
format pays on every column of every row. Classification is a compile-time whole-statement
AND, since libpq's format flag is per-statement, not per-column — one unsafe column
(`DATE`/`TIMESTAMP`/`UUID`/`storm::full_unsigned`/an FK member) keeps the entire statement
on the text path, unchanged from before this feature existed. SQLite is untouched (it
already extracts binary natively via `sqlite3_column_*`) — confirmed via an interleaved
A/B benchmark showing zero measurable delta (max 0.38%, inside a ~2.8% noise floor).

**Measured performance**: the original issue's own standalone raw-libpq microbenchmark
(isolated from Storm's build, not exercising the real extraction path) measured ~34%
faster bulk SELECT extraction (66.4% of text's time, 50k rows × 3 numeric columns).
**`storm_bench` currently has no PostgreSQL benchmark coverage at all** — every one of
its 184 benchmarks runs against a hardcoded SQLite `:memory:` connection — so the exact
speedup inside Storm's real Release-build extraction path is unverified; see
[issue #601](https://github.com/spiritEcosse/storm/issues/601) for the follow-up to add a
PG dimension to the bench harness. Until that lands, treat the 34% figure as a
mechanism-level estimate, not a measured Storm result.

Aggregates (`sum()`/`avg()`/`min()`/`max()`) and the RETURNING-id paths on `insert()`/
`update()`/`erase()` are deliberately **not** wired to binary format — verified against a
live PG server that their wire types don't match what the ORM extracts as (e.g.
`SUM(int_col)` returns PG `numeric`, not `bigint`), so wiring them naively would silently
corrupt results. See
[docs/internals/architecture/POSTGRESQL_BINARY_RESULTS.md](../architecture/POSTGRESQL_BINARY_RESULTS.md)
for the full design (safe/unsafe type table, classification logic, why each exclusion
exists).

## Local Test PostgreSQL Tuning

Not query performance — this is durability tuning for the **local dev
PostgreSQL instance used to run the test suite** (see
[TESTING.md#local-test-suite-speed](../testing/TESTING.md#local-test-suite-speed)
for the full before/after numbers). On a Postgres that holds only throwaway
test schemas, set in `postgresql.conf`:

```conf
fsync = off
synchronous_commit = off
```

Then `brew services restart postgresql@16` (or the equivalent for your install).
Measured 2026-08-27: cut the PG portion of the test suite from ~30.2s to ~25.7s
(−15%), full suite from ~38s to ~33s wall (−12%).

**⚠️ Never do this on an instance holding real data** — `fsync = off` means a
power loss or crash can corrupt or lose committed transactions. Fine for a
disposable test DB; not fine for anything else.
