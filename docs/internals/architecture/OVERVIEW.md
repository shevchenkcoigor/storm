# Architecture Overview

Storm ORM is built on several key architectural principles that enable near-raw SQLite performance while providing a modern, type-safe C++26 interface.

## Core Systems

### 1. [C++26 Reflection](REFLECTION.md)
Uses `std::meta` for automatic struct-to-table mapping without macros.

**Key features**:
- Compile-time field discovery
- Automatic SQL generation
- Type-safe bindings
- Zero runtime reflection overhead

### 2. [Statement Caching](STATEMENT_CACHING.md)
A single Connection-level statement cache achieving near-raw SQLite performance.

**Cache levels**:
- QuerySet → Statement instance
- Statement → Prepared statement
- Connection → Statement pool

**Performance impact**: 22.8x speedup for DELETE, 100x for UPDATE

### 3. [SQL Generation](SQL_GENERATION.md)
Compile-time SQL generation using ConstexprString.

**Benefits**:
- Zero runtime SQL construction
- Exact memory allocation
- Compile-time validation
- Cache-friendly

### 4. [Module System](MODULE_SYSTEM.md)
C++26 module structure with careful dependency management.

**Structure**:
```
storm (main) → db → orm → statements
```

**Challenges**:
- Circular dependency avoidance
- Custom Clang compiler requirements
- Module cache management

## Design Principles

### Performance-First

Storm prioritizes performance over code elegance:
- DRY/KISS principles **unless** they cost >10% performance
- Aggressive optimization (compile-time generation, caching, inlining)
- Target: ≥95% of raw SQLite performance

### Type Safety

Compile-time type checking throughout:
- Field types via `std::meta::type_of()`
- WHERE clause type validation
- Parameter binding type dispatch

### Zero Runtime Overhead

Extensive use of compile-time features:
- `constexpr`/`consteval` functions
- `if constexpr` type dispatch
- Template metaprogramming
- Index sequences and fold expressions

## Module Structure

```
src/
├── storm.cppm                      # Main module
├── db/
│   ├── concept.cppm                # Database concepts (incl. dialect-support concepts, #477)
│   ├── pool.cppm                   # PoolConfig, connection pooling
│   ├── sqlite.cppm                 # SQLite implementation
│   ├── postgresql.cppm             # PostgreSQL implementation
│   ├── postgresql_connection.cppm  # PostgreSQL connection management
│   ├── postgresql_error.cppm       # PostgreSQL error mapping
│   └── postgresql_statement.cppm   # PostgreSQL prepared statements
└── orm/
    ├── queryset.cppm               # QuerySet interface
    ├── ref_action.cppm             # RefAction, ref_action_sql (leaf module, #562)
    ├── field_attr.cppm             # Free-standing flag annotation objects (leaf module, #387/#492)
    ├── relation_meta.cppm          # m2m/reverse-fk annotation types + is_relation_field (leaf module, #408)
    ├── model_concepts.cppm         # Model-validation concepts (ModelWithPrimaryKey, PrimaryKeyType, …) + relation/FK helpers (leaf below statements_base)
    ├── indexes.cppm                # Index, UniqueIndex, Indexes<T> trait
    ├── schema.cppm                 # DDL/schema generation
    ├── generator.cppm              # storm-schema CLI codegen support
    ├── transaction.cppm            # TransactionGuard, storm::begin/transaction (#415)
    ├── where.cppm                  # WHERE-clause expression builders
    ├── utilities.cppm              # ConstexprString, SQLCache, sql_len/buffer_size; re-exports value_bind + stitch_key (#671)
    ├── value_bind.cppm             # UUID, chrono_conv, BindableType, bind_parameter_value/bind_* helpers (leaf module, #671)
    ├── stitch_key.cppm             # StitchKey — composite-PK m2m stitch-map key (leaf module, #504/#671)
    └── statements/
        ├── base.cppm               # BaseStatement utilities
        ├── extract.cppm            # ColumnExtractor — row→value converters (#434) + full_unsigned parse (#436)
        ├── field_names.cppm        # FieldNameGrammar — column-list SQL builders (#434) + PK WHERE clause (#501)
        ├── insert.cppm             # InsertStatement
        ├── insert_grammar.cppm     # InsertGrammar — INSERT / bulk-VALUES SQL builders (#671)
        ├── select.cppm             # SelectStatement + JOIN
        ├── select_grammar.cppm     # SelectGrammar — base SELECT + LIMIT 1/2 SQL builders (#671)
        ├── update.cppm             # UpdateStatement
        ├── update_grammar.cppm     # UpdateGrammar — UPDATE SQL builders (#434)
        ├── upsert_grammar.cppm     # Upsert (ON CONFLICT) SQL builders (#205)
        ├── erase.cppm              # EraseStatement
        ├── erase_grammar.cppm      # EraseGrammar — DELETE SQL builders (#501)
        ├── join.cppm               # JoinStatement (SQL builder)
        ├── join_grammar.cppm       # Shared consteval Q2 column/IN-subquery/junction SQL helpers (#671)
        ├── aggregate.cppm          # COUNT/SUM/AVG/MIN/MAX (#475)
        ├── distinct.cppm           # DISTINCT
        ├── orderby.cppm            # ORDER BY
        └── setop.cppm              # Set-op helpers (e.g. GROUP BY/HAVING support)
```

## Cross-Module Dependencies

```
storm (main module)
├── storm_db_concept
├── storm_db_pool
├── storm_db_sqlite
├── storm_db_postgresql{,_connection,_error,_statement}
├── storm_orm_ref_action
├── storm_orm_field_attr
├── storm_orm_relation_meta
├── storm_orm_indexes
├── storm_orm_schema
├── storm_orm_generator
├── storm_orm_transaction
├── storm_orm_where
├── storm_orm_utilities               (re-exports storm_orm_value_bind + storm_orm_stitch_key, #671)
├── storm_orm_model_concepts          (re-exported by storm_orm_statements_base)
├── storm_orm_statements_base
├── storm_orm_statements_{insert,insert_grammar,update,update_grammar,upsert_grammar,erase,erase_grammar,select,select_grammar,join,join_grammar}
├── storm_orm_statements_{aggregate,distinct,orderby,setop,extract,field_names}
└── storm_orm_queryset
```

**Dependency rules**:
- Modules use underscores (compiler limitation)
- No circular dependencies
- BaseStatement provides shared utilities
- Concepts define database interfaces

## Key Optimizations

### 1. Index Sequence Optimization

Replace recursive templates with fold expressions:

```cpp
using field_indices_t = std::make_index_sequence<field_count_>;

template <typename ConnType, typename Statement, size_t... Is>
auto bind_all_fields_impl(Statement& stmt, const T& obj, std::index_sequence<Is...>) {
    return (bind_field_at_index<ConnType, Is>(stmt, obj, Is + 1) && ...);
}
```

**Benefits**: Reduced template depth, faster compilation, better assembly

### 2. Thread-Local SQL Caching

8-entry cache for bulk INSERT SQL strings:

```cpp
struct BulkSQLCache {
    static constexpr size_t CACHE_SIZE = 8;
    std::array<CacheEntry, CACHE_SIZE> entries;
    size_t next_slot = 0;
};
thread_local BulkSQLCache bulk_sql_cache;
```

**Performance**: 94% improvement (0.253µs → 0.016µs)

### 3. Statement-Level Caching Pattern

Unified caching across UPDATE/DELETE/SELECT via the single Connection-level
prepared-statement cache, keyed by SQL text. There is no per-QuerySet or
per-Statement cache — those L1/L2 layers were removed in #214. Every
statement class follows the same per-call pattern:

```cpp
template <typename T> class XStatement {
    auto execute_optimized(const T& obj) -> std::expected<void, Error> {
        auto stmt_result = conn_->prepare_cached(get_sql());
        if (!stmt_result) {
            return std::unexpected(stmt_result.error());
        }
        auto* stmt_ptr = *stmt_result;
        stmt_ptr->reset();
        // Bind and execute with stmt_ptr
    }
};
```

**Performance**: 20x+ speedup for repeated operations

### 4. Optimized Row Extraction

Multiple optimizations in SelectStatement:
- resize() pre-allocation (1.7x faster than reserve())
- Direct string construction (2.2x faster, avoids strlen)
- Move semantics for optional fields
- Inline extraction with compiler hints

**Result**: 74% of raw SQLite performance (13.07M rows/sec)

## Concept-Based Database Abstraction

Database independence through concepts:

```cpp
concept DatabaseConnection = requires(T conn) {
    { conn.execute(std::string{}) } -> std::same_as<void>;
    { conn.prepare(std::string{}) } -> std::convertible_to<DatabaseStatement>;
    // ... more requirements
};

concept DatabaseStatement = requires(T stmt) {
    { stmt.step() } -> std::same_as<bool>;
    { stmt.reset() } -> std::same_as<void>;
    // ... more requirements
};
```

**Benefits**:
- PostgreSQL support shipped alongside SQLite (MySQL: future work)
- No ORM code changes needed
- Compile-time interface verification

## Thread Safety

**SQLite Level**: Thread-safe (SQLITE_OPEN_FULLMUTEX)

**Connection Management**: NOT thread-safe (compiler limitations with std::mutex in modules)

**SQL Caching**: Thread-local (zero synchronization overhead)

**Recommendation**: Per-thread connections or external mutex

## Testing Strategy

- **GoogleTest** with C++26 module support
- In-memory database (`:memory:`)
- Comprehensive sanitizer support (Address, Leak, Thread)
- ID validation for INSERT operations
- Performance regression tests

## See Also

- [C++26 Reflection](REFLECTION.md) - How reflection enables ORM
- [Statement Caching](STATEMENT_CACHING.md) - Single Connection-level cache details
- [SQL Generation](SQL_GENERATION.md) - Compile-time SQL generation
- [Module System](MODULE_SYSTEM.md) - Module structure and dependencies
