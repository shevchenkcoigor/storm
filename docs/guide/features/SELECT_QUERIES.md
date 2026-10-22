# SELECT Queries

Storm ORM provides highly optimized SELECT operations achieving **74% of raw SQLite performance** (13.07M rows/sec) through statement-level caching and optimized row extraction. On PostgreSQL, binary result format further accelerates extraction when all columns support byte-wise decoding.

## Overview

Key optimizations:
- **Statement-level caching** - Reusable prepared statements
- **resize() pre-allocation** - 1.7x faster than reserve() + push_back()
- **Optimized string extraction** - Direct construction using sqlite3_column_bytes()
- **Move semantics** - For optional field assignments
- **Inline extraction** - Compiler hints for hot paths
- **PostgreSQL binary results** - Binary format (~34% faster) when all columns are binary-safe (#600)
- **74% efficiency** - 13.07M rows/sec on SQLite (vs 17.67M raw SQLite)

## Using SelectStatement Directly

`QuerySet` is a thin wrapper over `SelectStatement<T, ConnType>::Query`, which exposes the same chain. Import
only the statement module instead of `import storm;` — plus `storm_defaults`, which provides the short
names with their SQLite default (the statement module itself exports only the backend-free
`BasicSelectStatement<T, ConnType>`):

```cpp
import storm_db_default_connection;
import storm_db_sqlite;
import storm_defaults;                  // SelectStatement<T> / DefaultConnection<> aliases
import storm_orm_statements_select;

using storm::db::DefaultConnection;
using storm::orm::statements::SelectStatement;

DefaultConnection<>::set(":memory:");   // replaces QuerySet::set_default_connection (removed)

auto rows = SelectStatement<Person>{}   // alias: BasicSelectStatement<Person, sqlite::Connection>
    .where(fields::Person.age > 30)
    .order_by<fields::Person.name>()
    .limit(10)
    .execute();

auto count = SelectStatement<Person>{}.all().count().execute();
auto one   = SelectStatement<Person>{}.all().first().order_by<fields::Person.age>().execute();
```

`DefaultConnection<>::get()` aborts if the thread has no default connection; check with `has()` first when unsure.
Insert/Update/Erase statements follow the same pattern (`InsertStatement<Person>{}.query(p).execute()`).

## Basic Usage

### Select All Rows

```cpp
struct Person {
    [[=primary]] int id;
    std::string name;
    int age;
};

storm::QuerySet<Person> queryset(conn);

// Returns all rows from the table
auto result = queryset.select().execute();
if (result) {
    for (const auto& person : result.value()) {
        std::cout << person.id << ": " << person.name
                  << " (" << person.age << ")" << std::endl;
    }
}
```

**SQL Generated**:
```sql
SELECT id, name, age FROM Person
```

**Performance**: 13.07M rows/sec (74% of raw SQLite, 1.51x faster than sqlite_orm)

### Select with WHERE

```cpp

// Filter results
auto result = queryset.where(fields::Person.age > 25).select().execute();

// Complex conditions
auto result = queryset.where(fields::Person.age > 25 and
                              fields::Person.age < 50).select().execute();
```

See [WHERE Clauses](WHERE_CLAUSES.md) for detailed WHERE syntax.

### Select with JOIN

```cpp
// Single FK JOIN
auto result = message_qs.join<fields::Message.sender>().select().execute();

// Multi-FK JOIN
auto result = message_qs.join<fields::Message.sender, fields::Message.receiver>().select().execute();

// JOIN with WHERE
auto result = message_qs.join<fields::Message.sender>()
                        .where(fields::User.level > 5)
                        .select()
                        .execute();
```

See [JOIN Operations](JOIN_OPERATIONS.md) for detailed JOIN syntax.

## Statement-Level Caching

### Caching Architecture

SelectStatement reuses prepared statements through a single Connection-level
cache. The Connection keeps a pool of prepared statements keyed by SQL text, so
repeated identical SELECTs reuse a compiled statement instead of re-parsing and
re-planning it. There is no per-QuerySet or per-Statement cache — those layers
(L1/L2) were removed in #214 after benchmarks showed no measurable benefit.

See [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) for the full
design and the #214 measurements.

### Benefits of the Connection-level cache

- **Avoids SQL re-parsing** — a repeated SELECT reuses the already-compiled prepared statement keyed by SQL text, instead of re-parsing and re-planning it.
- **Shared across all QuerySets** — the cache lives on the Connection, so every QuerySet on that connection benefits automatically.
- **Automatic statement pooling** — statements are pooled and reused without any caller bookkeeping.

For measured performance, see [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) and the project benchmarks.

### Repeated SELECT Example



```cpp
// First call - creates and caches statement
auto result1 = queryset.select();  // Slower (cache miss)

// Subsequent calls - reuses cached statement
for (int i = 0; i < 100; ++i) {
    auto result = queryset.select();  // Optimal performance (cache hit)
}
```

**Cache hit performance**: Near-identical to raw SQLite

### SELECT and JOIN share the Connection cache

Simple SELECT and JOIN SELECT both use the single Connection-level statement
cache. Each is keyed by its own SQL text, so a JOIN query and a plain SELECT
naturally resolve to distinct cache entries without any per-query-type cache —
the `cached_join_stmt_` / `cached_stmt_` members that once held separate handles
were removed in #214.

## PostgreSQL Binary Result Format (#600)

When selecting from PostgreSQL, Storm automatically optimizes column extraction by requesting binary result format from libpq — but **only when all columns in the statement can be safely decoded as raw bytes**. This is transparent: no API call is needed, and the optimization activates automatically for compatible models.

### When You Get Binary Format (~34% Faster Extraction)

Binary format is used for SELECT queries on any model where **every** column belongs to the safe set:

**Safe types for binary decoding:**
- All integer types: `bool`, `int`, `int32_t`, `int64_t`, `unsigned int`, `unsigned long`, etc.
- Floating-point: `double`, `float`
- Text: `std::string`, `std::filesystem::path`
- Binary: `std::vector<uint8_t>` (BLOB)
- Optional variants: `std::optional<safe_type>`

### When You Stay on Text Format

If your model includes **any** of these column types, the entire statement falls back to text format (the safe default — same as before #600):

| Type | Reason |
|---|---|
| `std::chrono::year_month_day` | Binary wire format differs from text |
| `std::chrono::system_clock::time_point` | Binary wire format differs from text |
| `storm::UUID` | Binary wire format differs from text |
| `std::uint64_t` with `storm::full_unsigned` | Stored as `NUMERIC(20,0)` in PG, not `BIGINT` |
| FK fields | Binary classification deferred to future work |

**Example: mixed model stays on text**

```cpp
struct Article {
    [[= primary]] int id;
    std::string title;              // ✅ safe
    std::chrono::system_clock::time_point created_at;  // ❌ unsafe
};
// All columns extracted as text because of created_at
```

### Where Binary Format is Used

- `select()` — base entities
- `first()` / `get()` — single-row results
- `rows()` generator — streaming results
- m2m/reverse-FK eager-load Q1 — base entities only
- `values<>()` projections — only projected columns classified
- `distinct<>()` projections — only projected columns classified
- Set operations (`union_`, `except_`, `intersect_`) — base model classification

**Not included**: aggregates (`sum()`, `avg()`, `min()`, `max()`), RETURNING-id paths, m2m Q2 eager loads. These have separate wire-type considerations.

### Performance Expectation

Original issue #600 measured ~34% faster extraction on isolated libpq microbenchmarks (50K rows × 3 numeric columns). Exact speedup inside Storm's full Release-build extraction path is pending benchmark harness expansion (#601). SQLite shows zero measurable regression (the feature is PG-only via compile-time concept probing).

### SQLite is Unchanged

SQLite already extracts binary natively via `sqlite3_column_*` — there's no text-format path to optimize. The binary-format feature is PostgreSQL-specific and compiles away entirely on SQLite.

See [PostgreSQL Binary Results (Architecture)](../../internals/architecture/POSTGRESQL_BINARY_RESULTS.md) for the compile-time classification logic and design rationale.

## Row Extraction Optimizations

### 1. resize() Pre-allocation (1.7x Speedup)

Instead of `reserve() + push_back()`, use `resize()` for direct construction:

```cpp
auto execute_optimized() -> std::expected<std::vector<T>, Error> {
    std::vector<T> result;

    // Count rows first
    size_t row_count = 0;
    while (stmt->step()) { ++row_count; }
    stmt->reset();

    // Pre-allocate with resize()
    result.resize(row_count);

    // Extract directly into pre-allocated slots
    size_t idx = 0;
    while (stmt->step()) {
        extract_row_inline_fast(stmt->handle(), result[idx++]);
    }

    return result;
}
```

**Performance**:
- `reserve() + push_back()`: 7.67M rows/sec
- `resize() + direct construction`: 13.07M rows/sec
- **Speedup**: 1.7x faster

**Why it's faster**:
- Eliminates move operations from `push_back()`
- Better memory locality
- No vector reallocation during extraction

### 2. Optimized String Extraction (2.2x Speedup)

Direct string construction using `sqlite3_column_bytes()`:

```cpp
template <size_t Idx>
static auto extract_column_inline_fast(void* stmt_handle, T& obj) noexcept {
    using FieldType = std::meta::type_of(all_members_[Idx]);
    constexpr auto member = all_members_[Idx];

    if constexpr (std::is_same_v<FieldType, std::string>) {
        // Get pointer and size directly (avoids strlen)
        const char* text = sqlite3_column_text(stmt_handle, Idx);
        int size = sqlite3_column_bytes(stmt_handle, Idx);

        // Direct construction with known size
        obj.[:member:] = std::string(text, size);
    }
    // ... other types
}
```

**Performance**:
- `std::string(sqlite3_column_text())`: 5.94M rows/sec (calls strlen)
- `std::string(text, size)`: 13.07M rows/sec (known size)
- **Speedup**: 2.2x faster

**Why it's faster**:
- Avoids `strlen()` call
- Single allocation with exact size
- No null-terminator scanning

### 3. Move Semantics for Optional Fields

For `std::optional<T>` fields, use move semantics:

```cpp
if constexpr (std::is_same_v<FieldType, std::optional<SomeType>>) {
    if (sqlite3_column_type(stmt_handle, Idx) == SQLITE_NULL) {
        obj.[:member:] = std::nullopt;
    } else {
        SomeType value = /* extract value */;
        obj.[:member:] = std::move(value);  // Move, don't copy
    }
}
```

**Benefit**: Eliminates unnecessary copies for optional fields

### 4. Inline Extraction with Compiler Hints

Mark hot paths with compiler attributes:

```cpp
template <size_t Idx>
__attribute__((hot, flatten, always_inline))
static auto extract_column_inline_fast(void* stmt_handle, T& obj) noexcept {
    // Extraction code
}
```

**Benefits**:
- `hot`: Optimize for frequently executed code
- `flatten`: Inline all function calls within this function
- `always_inline`: Force inlining even at -O2

### 5. Compile-Time Type Dispatch

Use `if constexpr` for zero runtime overhead:

```cpp
if constexpr (std::is_same_v<FieldType, int>) {
    obj.[:member:] = sqlite3_column_int(stmt_handle, Idx);
} else if constexpr (std::is_same_v<FieldType, int64_t>) {
    obj.[:member:] = sqlite3_column_int64(stmt_handle, Idx);
} else if constexpr (std::is_same_v<FieldType, double>) {
    obj.[:member:] = sqlite3_column_double(stmt_handle, Idx);
} else if constexpr (std::is_same_v<FieldType, std::string>) {
    // Optimized string extraction
}
```

**Benefit**: All type checks happen at compile-time, zero runtime cost

## Supported Field Types

All SQLite types are supported with optimized extraction:

### Integer Types
```cpp
int id = sqlite3_column_int(stmt, idx);
int64_t big_num = sqlite3_column_int64(stmt, idx);
```

### Floating Point Types
```cpp
double price = sqlite3_column_double(stmt, idx);
float ratio = static_cast<float>(sqlite3_column_double(stmt, idx));
```

### Boolean Type
```cpp
bool active = sqlite3_column_int(stmt, idx) != 0;
```

### String Types
```cpp
const char* text = sqlite3_column_text(stmt, idx);
int size = sqlite3_column_bytes(stmt, idx);
std::string str(text, size);  // Optimized: known size
```

### Optional Types
```cpp
if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    obj.field = std::nullopt;
} else {
    obj.field = /* extract value */;
}
```

### BLOB Types
```cpp
const void* blob = sqlite3_column_blob(stmt, idx);
int size = sqlite3_column_bytes(stmt, idx);
std::vector<uint8_t> data(static_cast<const uint8_t*>(blob),
                           static_cast<const uint8_t*>(blob) + size);
```

See [CRUD Operations](CRUD_OPERATIONS.md) for complete type mapping.

## Performance Analysis

### Benchmark Results

| Operation | Storm ORM | Raw SQLite | sqlite_orm | Efficiency |
|-----------|-----------|------------|------------|------------|
| SELECT (all rows) | 13.07M rows/sec | 17.67M rows/sec | 8.66M rows/sec | 74% |
| SELECT (no JOIN) | 12.81M rows/sec | - | - | - |
| SELECT + WHERE (single) | 11.88M rows/sec | 13.82M rows/sec | - | 86% |
| SELECT + WHERE (multiple) | 9.45M rows/sec | 10.57M rows/sec | - | 89.5% |
| SELECT + JOIN (single FK) | 4.4-6.1M rows/sec | 5.0-7.4M rows/sec | - | 59-90% |

### Performance Bottlenecks

**Overhead breakdown** (10,000 rows):
- SQLite row stepping: ~40% (unavoidable)
- String allocations: ~30% (heap allocations for TEXT fields)
- Column extraction: ~20% (type checks + conversion)
- Object construction: ~10% (calling constructors, field assignment)

**Why not 100% of raw SQLite?**
1. **Object construction overhead** - Raw SQLite just reads, doesn't construct objects
2. **String allocations** - Each string field requires heap allocation
3. **Type dispatch** - Even with `if constexpr`, some overhead exists
4. **Vector management** - Resizing, moving objects in result vector

**74% efficiency is excellent** for a full ORM with automatic object construction.

## Optimization Recommendations

### For Read-Heavy Workloads

1. **Reuse QuerySet instances** - Benefit from statement caching
2. **Pre-allocate vectors** - If you know approximate result size
3. **Use `std::string_view`** - For read-only string access (future optimization)
4. **Minimize string fields** - Each string requires heap allocation
5. **Use SELECT projection** - Only select fields you need (future feature)

### For Memory-Constrained Scenarios

```cpp
// Instead of loading all rows at once
auto result = queryset.select();  // Loads all rows into memory

// Consider streaming results (future feature)
// for (auto& person : queryset.stream()) { ... }
```

## Testing

Comprehensive SELECT tests in `tests/crud/test_select.cpp`:
- Empty table scenarios
- Single row results
- Multiple rows results
- All field types
- Large datasets (10K+ rows)
- Statement caching verification
- Integration with WHERE and JOIN

## Benchmarking

Run SELECT performance benchmarks:

```bash
cmake --preset ninja-release -DENABLE_BENCH=ON
cmake --build --preset ninja-release
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/SELECT/.*'
./build/release/benchmarks/storm_anchors  # Raw SQLite baseline
```

See `benchmarks/query_benchmark.cppm` and `benchmarks/tests/benchmark_tests.yaml` for the SELECT benchmark definition.

## Future Optimizations

Potential improvements for even better performance:

### 1. String Interning
Cache commonly occurring strings to reduce allocations:
```cpp
// For repeated values like "active", "pending"
std::string_view intern_string(const char* text, int size);
```

### 2. Custom Allocator
Arena allocator for temporary result vectors:
```cpp
using PersonVector = std::vector<Person, ArenaAllocator<Person>>;
```

### 3. SIMD Column Extraction
Batch extraction for multiple rows:
```cpp
// Extract 4 rows at once using SIMD
extract_four_rows_simd(stmt, result, idx);
```

### 4. Zero-Copy String Views
For read-only access, avoid string allocation:
```cpp
struct PersonView {
    int id;
    std::string_view name;  // Points to SQLite's internal buffer
    int age;
};
```

### 5. Streaming Results
Iterator-based interface for large result sets:
```cpp
for (auto& person : queryset.stream()) {
    // Process one at a time, no vector allocation
}
```

## Key Benefits

✅ **74% of raw SQLite** - Excellent ORM performance
✅ **1.51x faster than sqlite_orm** - Best-in-class ORM performance
✅ **Connection-level statement cache** - Statement reuse
✅ **resize() pre-allocation** - 1.7x faster than reserve()
✅ **Optimized strings** - 2.2x faster with known size
✅ **Compile-time dispatch** - Zero runtime overhead
✅ **Inline extraction** - Compiler-guided optimization

## See Also

- [WHERE Clauses](WHERE_CLAUSES.md) - Filter SELECT results
- [JOIN Operations](JOIN_OPERATIONS.md) - SELECT with JOINs
- [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) - Caching architecture details
- [SQL Generation](../../internals/architecture/SQL_GENERATION.md) - Compile-time SQL generation
