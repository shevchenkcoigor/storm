# DISTINCT Query Support - Implementation & Performance

## Performance Results

All benchmarks performed on 10,000 rows with 100 iterations in Release builds.

| Operation | Storm ORM | Raw SQLite | Efficiency |
|-----------|-----------|------------|------------|
| DISTINCT name (string) | 140,507 rows/sec | 147,083 rows/sec | 95% |
| DISTINCT age (int) | 148,841 rows/sec | 141,179 rows/sec | 105% |
| DISTINCT id/PK (int) | 35.8M rows/sec | 36.2M rows/sec | 99% |
| DISTINCT (name, age) | 132,336 rows/sec | 131,125 rows/sec | 101% |

**Average Efficiency: ~100%** - Storm ORM achieves parity with raw SQLite for DISTINCT operations!

## Architecture

Storm ORM supports `DISTINCT` queries on one or more fields with compile-time type safety and near-raw SQLite performance.

### Implementation

```cpp
// DistinctStatement (in src/orm/statements/distinct.cppm)
template <typename T, ConnType, auto... FieldPtrs>
    requires (sizeof...(FieldPtrs) > 0)
class DistinctStatement : private BaseStatement<T> {
    static constexpr size_t NumFields = sizeof...(FieldPtrs);
    static constexpr auto member_infos_ = std::array{get_member_info<FieldPtrs>()...};

    // Return type: single field → std::vector<FieldType>
    //              multiple fields → std::vector<std::tuple<Type1, Type2, ...>>
    using ResultType = std::conditional_t<
        NumFields == 1,
        std::vector<std::tuple_element_t<0, FieldTypesTuple>>,
        std::vector<FieldTypesTuple>
    >;

    auto execute() -> std::expected<ResultType, Error>;
    auto select() -> std::expected<ResultType, Error>;  // Alias for execute()
};
```

### Usage Examples

```cpp
QuerySet<Person> qs;

// Single field DISTINCT (backward compatible)
auto names = qs.distinct<&Person::name>().execute();
// Returns: std::vector<std::string>

// Multiple field DISTINCT
auto pairs = qs.distinct<&Person::name, &Person::age>().execute();
// Returns: std::vector<std::tuple<std::string, int>>

// Default to primary key
auto ids = qs.distinct().execute();
// Returns: std::vector<int>
```

### SQL Generation

- **Single field**: `SELECT DISTINCT name FROM Person`
- **Multiple fields**: `SELECT DISTINCT name, age FROM Person`
- Compile-time field list construction using fold expressions with index sequences
- FK fields automatically use column name (e.g., `sender_id` instead of `sender`)

## Key Features

- ✅ **Type Safety**: Return type automatically deduced at compile-time
- ✅ **Zero Overhead**: Compile-time SQL generation, no runtime string building
- ✅ **Multiple Fields**: Full support for DISTINCT on 1+ fields (SQLite supports 2000 columns)
- ✅ **Backward Compatible**: Single-field API unchanged from original implementation

## Implementation Highlights

1. **Simplified Architecture**: Direct statement executor (no wrapper layer), QuerySet handles convenience
2. **Architectural Consistency**: Follows same pattern as `InsertStatement`, `UpdateStatement`, etc. in `src/orm/statements/`
3. **Requires Clause**: Template constraint `requires (sizeof...(FieldPtrs) > 0)` enforces at least one field
4. **Compile-Time Field List**: Uses fold expressions with lambda templates to build comma-separated field list
5. **Type Deduction**: Automatically determines return type based on number and types of fields
6. **Index Sequences**: Leverages `std::index_sequence` for compile-time iteration
7. **Tuple Extraction**: Multi-field results extracted into `std::tuple` with perfect type matching
8. **Statement Caching**: Prepared statement cached for repeated DISTINCT queries on same fields

## Known Limitations

- **Type-based field matching** can be ambiguous when multiple fields have the same type (e.g., `&Person::id` and `&Person::age` are both `int`)
- **Workaround**: Use fields with unique types or access via different field orderings
- **Aggregate functions** (COUNT, SUM, AVG) require separate implementation (different return types)

## Testing

**Location**: `tests/query/test_distinct.cpp`

**Coverage** (every test runs on SQLite and PostgreSQL):
- Single-field and multi-field DISTINCT, including the default primary-key projection
- The same field named more than once
- Return types (`plf::hive<T>` / `plf::hive<std::tuple<...>>`)
- DISTINCT combined with WHERE, JOIN, ORDER BY, LIMIT and OFFSET
- `std::optional` fields (NULL is one distinct value)
- Statement-cache reuse
- Exact SQL text for the main shapes
- A raw-SQL workaround for DISTINCT over a column of the *joined* table, which the ORM cannot express yet (#804)

Empty-table cases and further chain cases also live in `tests/test_cases/unified_cases.yaml` (`distinct_*`).

## Why DISTINCT Achieves 100% Efficiency

1. **Minimal Overhead**: Simple type extraction without complex object construction
2. **Compile-Time SQL**: Zero runtime SQL generation overhead
3. **Statement Caching**: Prepared statement reused across multiple calls
4. **Optimized Extraction**: Direct column-to-type mapping without intermediate allocations
5. **No JOIN Complexity**: Simple SELECT DISTINCT without foreign key population

## Performance Breakdown

### Integer Fields (99-105% efficiency)
- Direct `sqlite3_column_int()` or `sqlite3_column_int64()` calls
- No allocation overhead
- Single instruction type extraction
- Actually faster than raw SQLite in some cases due to better caching

### String Fields (95% efficiency)
- Slight overhead from `std::string` allocation
- `sqlite3_column_bytes()` + `sqlite3_column_text()` calls
- Still within measurement variance of raw SQLite

### Multi-Field (101% efficiency)
- `std::tuple` construction is zero-cost abstraction
- No performance penalty for multiple fields
- Compiler optimizes tuple creation to simple memory copies

## Running DISTINCT Benchmarks

```bash
cmake --preset ninja-release -DENABLE_BENCH=ON
cmake --build --preset ninja-release
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/DISTINCT/.*'  # DISTINCT operations
```

## Caching Safety

Multiple QuerySet instances can safely share the same cached `DistinctQuerySet` and prepared statements because:

1. **Each QuerySet has its own `where_expr_` object** (NOT shared across instances)
2. **Parameter binding happens atomically** with execution in the same method call
3. **SQLite's binding is "last write wins"** - each bind overwrites previous parameters
4. **No suspension points** - no window for another QuerySet to interfere between bind and execute

**What gets shared:**
- `static thread_local DistinctQuerySet` object (per thread + field combination)
- The prepared statement returned by the Connection-level `prepare_cached(sql)`
  cache (same SQL text → same cache entry; there is no per-Statement
  `cached_where_stmt_` member)

**What does NOT get shared:**
- `where_expr_` object (each QuerySet instance has its own)
- Bound parameters (temporary, overwritten on each bind call)

See [Statement Caching](../architecture/STATEMENT_CACHING.md) for comprehensive explanation and examples.

## Future Enhancements

Potential improvements for DISTINCT functionality:

1. **DISTINCT in aggregates**: `COUNT(DISTINCT column)`, `SUM(DISTINCT column)`
2. **DISTINCT with JOINs**: Currently requires workarounds
3. **DISTINCT with WHERE**: Full integration with QuerySet filtering
4. **Performance monitoring**: Track efficiency over time in CI
