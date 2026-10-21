# Batch Operations

Storm ORM provides highly optimized batch operations for INSERT, UPDATE, and DELETE, achieving **2.7M-3.9M rows/sec** through smart thresholds and optimized SQL generation.

## Overview

Key features:
- **Smart thresholds** - Automatic strategy selection based on SQLite limits
- **Bulk SQL** - Multiple VALUES or IN clauses where beneficial
- **Transaction wrapping** - Automatic transactions for consistency
- **Thread-local caching** - 8-entry cache for bulk INSERT SQL
- **Auto-generated IDs** - Returns all generated IDs for batch INSERT

## Batch INSERT

### Basic Usage

```cpp
struct Person {
    [[=primary]] int id;
    std::string name;
    int age;
};

storm::QuerySet<Person> queryset(conn);

std::vector<Person> people = {
    {0, "Alice", 25},
    {0, "Bob", 30},
    {0, "Charlie", 35},
    {0, "Dave", 40}
};

// Returns all generated IDs
auto result = queryset.insert(std::span<const Person>(people));
if (result) {
    const auto& ids = result.value();
    for (size_t i = 0; i < ids.size(); ++i) {
        std::cout << people[i].name << " inserted with ID: " << ids[i] << std::endl;
    }
}
```

**Performance**: 2.7M rows/sec (6.4x faster than sqlite_orm)

### Batch INSERT Strategy

Storm ORM uses smart thresholds based on SQLite's variable limit (999):

#### Strategy 1: Bulk INSERT (≤50 objects)

For small batches, use a single SQL statement with multiple VALUES:

```sql
INSERT INTO Person (name, age) VALUES (?, ?), (?, ?), (?, ?), ...
```

**Benefits**:
- Single SQL parse
- Single transaction
- Minimal overhead

**Limitations**:
- SQLite has 999-parameter limit
- Each object with N fields needs N parameters
- Max objects = floor(999 / field_count)

#### Strategy 2: Individual Statements with Transaction (>50 objects)

For large batches, wrap individual statements in a transaction:

```sql
BEGIN TRANSACTION;
INSERT INTO Person (name, age) VALUES (?, ?);
INSERT INTO Person (name, age) VALUES (?, ?);
...
COMMIT;
```

**Benefits**:
- No parameter limit
- Consistent with UPDATE/DELETE strategy
- Predictable performance

**Why 50?**:
- For Person (3 fields): floor(999 / 3) = 333 max
- But 50 provides good balance between:
  - SQL string generation cost
  - Statement preparation overhead
  - Transaction commit frequency

### Thread-Local SQL Caching

Batch INSERT uses an 8-entry thread-local cache for frequently used batch sizes:

```cpp
struct BulkSQLCache {
    static constexpr size_t CACHE_SIZE = 8;
    struct CacheEntry {
        size_t key = 0;        // Batch size
        std::string sql;       // Cached SQL string
    };
    std::array<CacheEntry, CACHE_SIZE> entries;
    size_t next_slot = 0;      // Round-robin replacement
};
thread_local BulkSQLCache bulk_sql_cache;
```

**Cache lookup**:
```cpp
static auto get_cached_sql(size_t count) -> std::string {
    // 1. Search cache for matching batch size
    for (const auto& entry : bulk_sql_cache.entries) {
        if (entry.key == count) {
            return entry.sql;  // Cache hit!
        }
    }

    // 2. Cache miss - generate SQL with pre-allocation
    std::string sql;
    sql.reserve(calculate_sql_size(count));
    // ... build SQL ...

    // 3. Store in cache (round-robin)
    bulk_sql_cache.entries[bulk_sql_cache.next_slot] = {count, sql};
    bulk_sql_cache.next_slot = (bulk_sql_cache.next_slot + 1) % CACHE_SIZE;

    return sql;
}
```

**Performance improvement**: 94% faster (0.253µs → 0.016µs) for cached sizes

**Cache hit rate**: >90% for common sizes (1, 5, 10, 25, 50)

**Thread safety**: Each thread has its own cache (thread_local), zero synchronization overhead


## Batch UPDATE

### Basic Usage

```cpp
std::vector<Person> people = {
    {1, "Alice", 26},
    {2, "Bob", 31},
    {3, "Charlie", 36}
};

auto result = queryset.update(std::span<const Person>(people));
if (!result) {
    std::cerr << "Batch update failed: " << result.error().message() << std::endl;
}
```

### Batch UPDATE Strategy

UPDATE uses individual statements wrapped in a transaction:

```sql
BEGIN TRANSACTION;
UPDATE Person SET name=?, age=? WHERE id=?;
UPDATE Person SET name=?, age=? WHERE id=?;
UPDATE Person SET name=?, age=? WHERE id=?;
COMMIT;
```

**Why no bulk UPDATE?**
- SQL doesn't have multi-row UPDATE syntax like INSERT's VALUES
- UPDATE ... CASE WHEN approach is complex and not faster
- Individual statements with transaction provide:
  - Consistent per-row error handling
  - Predictable performance
  - Simple implementation

**Performance**: Still benefits from:
- Statement caching (reuse prepared statement)
- Single transaction (one commit)
- Compile-time field binding

### Statement Caching Benefit

UpdateStatement caches the prepared statement:

```cpp
// First update - creates and caches statement
queryset.update(person1).execute();

// Batch update - reuses cached statement
queryset.update(std::span<const Person>(people));  // Fast!
```

**Performance**: 2M operations/sec sustained, 12M peak

## Batch DELETE

### Basic Usage

```cpp
std::vector<Person> people = {
    {1, "Alice", 25},
    {2, "Bob", 30},
    {3, "Charlie", 35}
};

auto result = queryset.erase(std::span<const Person>(people));
```

**Performance**: 3.9M rows/sec

### Batch DELETE Strategy

DELETE uses bulk IN clause for efficiency:

```sql
DELETE FROM Person WHERE id IN (?, ?, ?)
```

**SQL generation**:
```cpp
std::string sql = "DELETE FROM Person WHERE id IN (";
for (size_t i = 0; i < count; ++i) {
    if (i > 0) sql += ", ";
    sql += "?";
}
sql += ")";
```

**Benefits**:
- Single SQL statement
- Single transaction
- Efficient SQLite execution plan

**Limitations**:
- Same 999-parameter limit as INSERT
- For >999 objects, falls back to multiple DELETE statements

### Why DELETE Uses IN Clause

DELETE is simpler than INSERT/UPDATE:
- Only binds primary key (single parameter per object)
- 999-parameter limit allows ~999 deletions per statement
- IN clause is SQLite-optimized for this pattern

**Performance comparison**:
- Individual statements: ~1M rows/sec
- Bulk IN clause: 3.9M rows/sec
- **Speedup**: 3.9x faster

## Performance Comparison

### Batch vs Single Operations

| Operation | Single | Batch | Speedup |
|-----------|--------|-------|---------|
| INSERT | 992K/sec | 2.7M/sec | 2.7x |
| UPDATE | 2M/sec | (similar) | Cached |
| DELETE | 21.6M/sec | 3.9M/sec | 0.18x* |

*Single DELETE is faster due to statement caching; batch is still efficient for multiple deletions.

### Storm vs sqlite_orm

| Operation | Storm Batch | sqlite_orm | Advantage |
|-----------|-------------|------------|-----------|
| INSERT | 2.7M/sec | 422K/sec | 6.4x faster |
| SELECT | 13.07M/sec | 8.66M/sec | 1.51x faster |

Storm's batch operations significantly outperform sqlite_orm.

## Smart Threshold Analysis

### Why Threshold at 50?

Testing with various thresholds (10,000 row benchmark):

| Threshold | INSERT Performance | Notes |
|-----------|-------------------|-------|
| 10 | 2.1M/sec | Too many transactions |
| 25 | 2.5M/sec | Good but not optimal |
| **50** | **2.7M/sec** | Optimal balance |
| 100 | 2.4M/sec | SQL generation overhead |
| 200 | 2.0M/sec | Diminishing returns |

**Optimal at 50**:
- Amortizes SQL generation cost
- Stays well under 999-parameter limit
- Balances transaction frequency
- Works for most struct sizes (up to ~19 fields)

### Parameter Limit Calculation

```cpp
static constexpr size_t SQLITE_MAX_PARAMS = 999;
static constexpr size_t MAX_BULK_SIZE = 50;

// Calculate max safe batch size
static constexpr auto calc_max_batch() {
    constexpr auto non_pk_fields = get_non_pk_field_count();
    constexpr auto max_from_params = SQLITE_MAX_PARAMS / non_pk_fields;
    return std::min(max_from_params, MAX_BULK_SIZE);
}
```

**Example**:
- Person has 2 non-PK fields (name, age)
- Max from params: 999 / 2 = 499
- Max from threshold: 50
- **Actual max**: min(499, 50) = 50

## Optimization Tips

### 1. Batch Size Selection

```cpp
// Good: Let Storm decide
queryset.insert(std::span<const Person>(people));

// Also good: explicit batch size via InsertOptions
queryset.insert(std::span<const Person>(people),
                storm::orm::statements::InsertOptions{.batch_size = 100});

// Also good: Explicit batch size control
constexpr size_t BATCH_SIZE = 50;
for (size_t i = 0; i < people.size(); i += BATCH_SIZE) {
    size_t end = std::min(i + BATCH_SIZE, people.size());
    queryset.insert(std::span<const Person>(people.data() + i, end - i));
}
```

> **`batch_size` is clamped to `[1, floor(999 / field_count)]`.** A `batch_size`
> of `0` is treated as `1` (it would otherwise never advance the chunk loop);
> values above the SQLite parameter limit are capped. Leave it unset
> (`std::nullopt`) to let Storm pick the maximum safe batch.

### 2. Pre-allocate Vectors

```cpp
// Good: Reserve space
std::vector<Person> people;
people.reserve(1000);
// ... populate ...
queryset.insert(std::span<const Person>(people));

// Better: Use resize() if size known
std::vector<Person> people(1000);
// ... populate via direct assignment ...
queryset.insert(std::span<const Person>(people));
```

### 3. Minimize String Allocations

```cpp
// Slower: Many temporary strings
for (int i = 0; i < 1000; ++i) {
    people.push_back({0, "User" + std::to_string(i), 25});
}

// Faster: Pre-allocate and reuse
std::string name_buffer;
for (int i = 0; i < 1000; ++i) {
    name_buffer = "User" + std::to_string(i);
    people.push_back({0, name_buffer, 25});
}
```

### 4. Use the Public Transaction API

For complex workflows combining multiple operations, wrap them in a
`storm::begin()` scope (#415) instead of raw `conn->execute("BEGIN TRANSACTION")`:

```cpp
auto txn = storm::begin(conn);            // RAII guard; BEGIN issued
if (!txn) return std::unexpected(txn.error());

// Multiple batch operations — any early return / throw auto-ROLLBACKs.
if (auto r = queryset.insert(std::span<const Person>(new_people)).execute(); !r)
    return std::unexpected(r.error());
if (auto r = queryset.update(std::span<const Person>(modified_people)).execute(); !r)
    return std::unexpected(r.error());
if (auto r = queryset.erase(std::span<const Person>(deleted_people)).execute(); !r)
    return std::unexpected(r.error());

return txn->commit();                       // explicit COMMIT
```

**Benefit**: Single commit for all operations (faster than individual commits),
RAII rollback on any failure, and — unlike a raw `BEGIN TRANSACTION` — the inner
chunked-batch transactions cooperate with the outer scope instead of colliding
with it (fixes the nested-BEGIN bug, #9). A `storm::begin()` on a connection that
is already inside a transaction returns a passive guard: no nested BEGIN, the
outer guard owns the single commit/rollback.

## Benchmarking

Run batch operation benchmarks:

```bash
# Build the release benchmarks
cmake --preset ninja-release -DENABLE_BENCH=ON
cmake --build --preset ninja-release

# Storm ORM batch operations
./build/release/benchmarks/storm_bench

# Raw SQLite baseline
./build/release/benchmarks/storm_anchors
```

See `benchmarks/crud_benchmark.cppm` and `benchmarks/tests/benchmark_tests.yaml` for the batch benchmark definition.

## Testing

Comprehensive batch operation tests:
- `tests/crud/test_crud.cpp` - Basic batch operations
- `tests/schema/test_fk_fields.cpp` - Batch with FK fields
- Various batch sizes (1, 10, 50, 100, 1000)
- Error handling and rollback
- ID validation for batch INSERT

## Thread Safety

Batch operations are thread-safe at the SQLite level (SQLITE_OPEN_FULLMUTEX), but:

**Thread-local caching**:
- Each thread has its own SQL cache
- No synchronization needed
- Scales linearly with threads

**Connection-level caching**:
- NOT thread-safe (experimental compiler limitations)
- Use per-thread connections or external mutex

**Recommendation**: Per-thread QuerySet instances with per-thread connections

## Key Benefits

✅ **Smart thresholds** - Automatic optimal strategy selection
✅ **Thread-local caching** - 94% improvement for cached sizes
✅ **Bulk SQL** - Single statement where beneficial
✅ **Auto transactions** - Consistent and efficient
✅ **Auto-generated IDs** - Returns all IDs from batch INSERT
✅ **2.7-6.4x faster than sqlite_orm** - Best-in-class performance

## See Also

- [CRUD Operations](CRUD_OPERATIONS.md) - Single operation details
- [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) - Caching architecture
- [SQL Generation](../../internals/architecture/SQL_GENERATION.md) - Compile-time SQL generation
- [Performance Testing](../../internals/performance/PERFORMANCE.md#testing-benchmarking) - Benchmarking guidelines
