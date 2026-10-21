# Storm ORM Key Design Decisions

This document details the architectural decisions that make Storm ORM achieve near-raw SQLite performance while maintaining compile-time type safety.

## 1. C++26 Reflection-Based ORM

Uses compile-time reflection (`std::meta`) to automatically:
- Find primary key fields marked with `[[=primary]]`
- Generate SQL statements from struct definitions
- Bind struct fields to database columns

```cpp
struct Person {
    [[=primary]] int id;
    std::string name;
    int age;
};
```

**Benefits:**
- Zero runtime reflection overhead
- No macros or code generation
- Compile-time SQL validation
- Type-safe field access via `obj.[:member:]` splice operator

## 2. Concept-Based Database Abstraction

- `DatabaseConnection` concept defines interface for any database
- `DatabaseStatement` concept for prepared statements
- SQLite implementation satisfies these concepts
- Allows future PostgreSQL/MySQL support without changing ORM code

**Example:**
```cpp
template <typename Conn>
concept DatabaseConnection = requires(Conn conn, const std::string& sql) {
    { conn.prepare(sql) } -> std::same_as<Statement*>;
    { conn.execute(sql) } -> std::expected<void, Error>;
};
```

## 3. Connection Management Strategy

- **Default static connection** for simple use cases
- **Explicit connection passing** for multi-database scenarios
- **Thread Safety**: SQLite opened with `SQLITE_OPEN_FULLMUTEX`
- **WARNING**: Connection management layer NOT thread-safe due to compiler limitations with std::mutex in C++26 modules

## 4. Statement Architecture & Optimization

- **BaseStatement**: Shared utilities for transaction management, SQL execution patterns, compile-time field binding
- **Index Sequence Optimization**: `std::index_sequence` and fold expressions replace recursive templates
- **Statement Separation**: Individual modules with specialized optimizations
- **Compile-Time SQL Generation**: Pre-computed SQL using ConstexprString and consteval functions
- **Performance Optimization**: Smart thresholds for bulk vs individual operations (SQLite variable limit = 999)
- **Thread-Local SQL Caching**: 8-entry cache for bulk INSERT SQL strings
- **Statement-Level Caching**: EraseStatement, SelectStatement, UpdateStatement use statement caching pattern

## 5. Compile-Time Index Sequence Optimization

Major performance optimization using modern C++ compile-time features:

**Key Implementation:**
```cpp
using field_indices_t = std::make_index_sequence<field_count_>;

template <typename ConnType, typename Statement, size_t... Is>
auto bind_all_fields_impl(Statement& stmt, const T& obj, std::index_sequence<Is...>) {
    auto bind_result = (bind_field_at_index<ConnType, Is>(stmt, obj, Is + 1) && ...);
}
```

**Benefits:**
- Reduced template depth → faster compilation
- Better assembly generation
- Zero runtime overhead
- Pre-computed metadata cached in static constexpr variables

## 6. Thread-Local SQL Caching

Runtime optimization for bulk INSERT performance:

```cpp
struct BulkSQLCache {
    static constexpr size_t CACHE_SIZE = 8;
    std::array<CacheEntry, CACHE_SIZE> entries;
    size_t next_slot = 0; // Round-robin
};
thread_local BulkSQLCache bulk_sql_cache;
```

**Performance**: 94% improvement (0.253µs → 0.016µs) for cached sizes, thread-safe, zero synchronization overhead.

## 7. Compile-Time SQL Generation with ConstexprString

Moves SQL generation from runtime to compile-time:

```cpp
static consteval auto build_insert_sql_array() {
    ConstexprString<sql_size> result;
    result.append("INSERT INTO ");
    result.append(Base::table_name_);
    result.append(" (").append(field_names_).append(") VALUES (");
    result.append(placeholders_).append(")");
    return result;
}
```

**Benefits:**
- Zero runtime SQL generation
- Exact memory allocation (no dynamic reallocation)
- Compile-time validation
- Cache-friendly (static strings)

## 8. Batch Operations Architecture

Two optimized batch operation strategies:

### InsertStatement
- Returns `std::expected<std::vector<int64_t>, Error>` with generated IDs
- Bulk INSERT with multiple VALUES for ≤50 objects
- Individual statements with transactions for >50 objects
- Thread-local SQL caching with compile-time prefix optimization

### EraseStatement
- Bulk DELETE with IN clause: `DELETE FROM table WHERE id IN (?,?,?)`
- Same smart thresholds as InsertStatement

### SelectStatement
- `execute_optimized()` returns all rows
- Statement-level caching for repeated SELECT calls
- resize() pre-allocation (1.7x faster than reserve() + push_back())
- Optimized string extraction using `sqlite3_column_bytes()` (avoids strlen)
- Move semantics for optional field assignments (avoids copy)
- Inline column extraction with compiler hints (`__attribute__((hot/flatten/always_inline))`)

## 9. Auto-Generated ID Support

Returns generated IDs from insert operations:
- Single insert: `std::expected<int64_t, Error>`
- Batch insert: `std::expected<std::vector<int64_t>, Error>`
- Uses `RETURNING id` clause for both single and bulk INSERT

**Table creation uses a plain integer primary key (auto-assigned by SQLite):**
```cpp
conn.execute("CREATE TABLE Person ("
    "id INTEGER PRIMARY KEY, "
    "name TEXT NOT NULL, age INTEGER NOT NULL)");
```
Since #379 Storm emits plain `INTEGER PRIMARY KEY` by default; `AUTOINCREMENT`
(the SQLite never-reuse guarantee, ~358 ns/insert) is opt-in via
`primary_autoincrement`.

## 10. Statement-Level Caching Pattern

Unified caching pattern across UPDATE/DELETE/SELECT operations achieves near-raw SQLite performance.

See [Statement Caching](STATEMENT_CACHING.md) for detailed implementation.

**Performance Impact Examples:**
- **DELETE**: 947K → 21.6M ops/sec (22.8x speedup, 73% of raw SQLite)
- **UPDATE**: 100x improvement for repeated operations (12M peak)
- **SELECT**: 13.07M rows/sec (74% of raw SQLite)

## 11. JOIN Architecture (Type-Erased SQL Builder Pattern)

Enables single and multi-FK JOIN operations without `std::function` (which causes linker issues with custom libc++).

**Architecture:**
```cpp
// Abstract base for type erasure
class IJoinStatement {
    virtual std::string to_sql() const = 0;
    virtual std::string build_qualified_select_fields() const = 0;
    virtual void extract_row(void* stmt, void* obj) const = 0;
};

// Unified variadic template (single + multi FK)
template <typename T, ConnType, std::meta::info... FKFields>
class JoinStatement : public IJoinStatement {
    // Pure SQL builder - no execute(), no caching
    // Generates: " INNER JOIN table t2 ON t2.id = t1.fk_id"
};
```

**Key Benefits:**
- ✅ No `std::function` - avoids custom libc++ linker errors
- ✅ Single variadic template (not separate Single/Multi classes)
- ✅ Abstract base class for type erasure
- ✅ Compile-time SQL generation with fold expressions
- ✅ Zero runtime overhead with `if constexpr` dispatch
- ✅ JOIN and simple SELECT share the single Connection-level statement cache (distinct SQL text → distinct cache entries)

**Usage:**
```cpp
// Single FK - populates sender fully
auto result = message_qs.join<fields::Message.sender>().select().execute();

// Multi FK - populates both sender and receiver
auto result = message_qs.join<fields::Message.sender, fields::Message.receiver>().select().execute();
```

See [JOIN Performance Analysis](../performance/JOIN_ANALYSIS.md) for detailed performance data.

## 12. DISTINCT Query Support (Single & Multi-Field)

Storm ORM supports `DISTINCT` queries on one or more fields with compile-time type safety and near-raw SQLite performance.

**Architecture:**
```cpp
template <typename T, ConnType, auto... FieldPtrs>
    requires (sizeof...(FieldPtrs) > 0)
class DistinctStatement : private BaseStatement<T> {
    // Return type: single field → std::vector<FieldType>
    //              multiple fields → std::vector<std::tuple<Type1, Type2, ...>>
    using ResultType = std::conditional_t<
        NumFields == 1,
        std::vector<std::tuple_element_t<0, FieldTypesTuple>>,
        std::vector<FieldTypesTuple>
    >;
};
```

See [DISTINCT Analysis](../performance/DISTINCT_ANALYSIS.md) for detailed implementation and performance data.

## Implementation Notes

- **No REFL-CPP**: Project uses native C++26 reflection
- **Index Sequence Optimization**: Uses `std::index_sequence` and fold expressions
- **Pre-computed Metadata**: Field information cached in static constexpr variables
- **Module Naming**: Uses underscores (`storm_db_sqlite`) due to compiler limitations
- **Circular Dependencies**: Avoided by extracting shared declarations into dependency-free leaf modules (`storm_orm_field_attr` for the flag annotation objects / `is_primary_member`, #387/#492)
- **Compiler Crashes**: std::mutex in modules causes segfaults
- **std::function Linker Errors**: Avoid `std::function` with custom libc++ - use abstract base classes instead (see JOIN architecture)
- **Primary Key Access**: Uses reflection splice operator `obj.[:primary_key_:]`
- **Bulk INSERT Caching**: Thread-local 8-entry cache with optimized string pre-allocation
- **JOIN Implementation**: Abstract base class (`IJoinStatement`) + variadic template for type erasure without `std::function`
