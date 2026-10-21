# Common Development Tasks

## Building and Serving Documentation

Storm uses [MkDocs Material](https://squidfunk.github.io/mkdocs-material/) for hosted documentation.

```bash
# Install (Arch Linux)
sudo pacman -S mkdocs-material python-pymdown-extensions

# Install (pip)
pip install mkdocs-material

# Live preview (http://127.0.0.1:8000)
mkdocs serve

# Build static site
mkdocs build

# Build with strict mode (fails on broken links)
mkdocs build --strict
```

Documentation auto-deploys to GitHub Pages on push to `develop` when `docs/` or `mkdocs.yml` changes.

## Adding a New Database Operation

1. Create statement class in `src/orm/statements/` inheriting from `BaseStatement<T>`
2. Implement single-object and batch operations (`std::span<const T>`)
3. Choose return type:
   - INSERT: `std::expected<int64_t, Error>` or `std::expected<std::vector<int64_t>, Error>`
   - DELETE/UPDATE: `std::expected<void, Error>`
   - SELECT: `std::expected<std::vector<T>, Error>`
4. Statement reuse is automatic: call `conn_->prepare_cached(sql)` per execution —
   the Connection-level cache (keyed by SQL text) returns the prepared statement
   on a cache hit; no per-Statement cached member is needed
   - Add `std::unique_ptr<YourStatement>` in QuerySet with lazy initialization
5. Implement compile-time SQL generation using ConstexprString
6. Add method to `QuerySet` class that delegates to statement
7. Add comprehensive tests in `tests/<category>/test_*.cpp`
8. Consider optimizations: `if constexpr` type dispatch, pre-allocation, inline functions with compiler hints

## Working with Bulk Operations

### INSERT operations with auto-generated IDs

```cpp
// Single insert - returns ID
auto result = queryset.insert(Person{0, "Dave", 40}).execute();
int64_t id = result.value();

// Batch insert - returns all IDs
std::vector<Person> people = {{0, "Alice", 25}, {0, "Bob", 30}};
auto result = queryset.insert(std::span<const Person>(people));
const auto& ids = result.value();
```

### UPDATE/DELETE operations

```cpp
// Single update
Person person{1, "Alice", 26};
queryset.update(person).execute();

// Batch update/delete
queryset.update(std::span<const Person>(people));
queryset.erase(std::span<const Person>(people));
```

### SELECT operations

```cpp
auto result = queryset.select();  // Returns all rows
const auto& people = result.value();

// Repeated calls use cached statement
for (int i = 0; i < 100; ++i) {
    auto result = queryset.select();  // Optimal performance
}
```

## Optimizing Statement Performance

1. **Use shared utilities**: `TransactionGuard` (`storm::begin`) for batch transactions, BaseStatement shared binding
2. **Implement compile-time field binding** with index sequences and fold expressions
3. **Smart thresholds**: Consider SQLite variable limit (999)
4. **Cache SQL strings**: Static methods + thread-local caching
5. **Pre-compute metadata**: `constexpr`/`consteval` functions
6. **Batch operations**: Support bulk SQL (IN, multiple VALUES) + transactions
7. **Optimize allocation**: Pre-calculate sizes, reserve memory
8. **Thread-local caching**: 8-entry cache for frequent patterns

## Index Sequence Optimization Template

```cpp
// In BaseStatement-derived class
static constexpr auto field_count_ = get_field_count();
static constexpr auto all_members_ = get_all_field_members<field_count_>();
using field_indices_t = std::make_index_sequence<field_count_>;

auto bind_all_fields(Statement& stmt, const T& obj) {
    return Base::template bind_all_fields_impl<ConnType, Statement>(
        stmt, obj, typename Base::field_indices_t());
}
```

## Compile-Time SQL Generation Template

```cpp
class YourStatement : private BaseStatement<T> {
    static consteval auto build_sql_array() {
        constexpr size_t sql_size = calculate_sql_size() + 50;
        ConstexprString<sql_size> result;
        result.append("SELECT * FROM ").append(Base::table_name_);
        // ... build SQL ...
        return result;
    }
    static constexpr auto sql_array = build_sql_array();
    static inline const std::string sql_string = std::string(sql_array);
};
```

## Thread-Local Caching Template

```cpp
struct YourStatementSQLCache {
    static constexpr size_t CACHE_SIZE = 8;
    struct CacheEntry { size_t key = 0; std::string sql; };
    std::array<CacheEntry, CACHE_SIZE> entries;
    size_t next_slot = 0;
};
thread_local YourStatementSQLCache your_cache;

static auto get_cached_sql(size_t key) -> std::string {
    // 1. Search cache, 2. Generate with pre-allocation, 3. Store round-robin
}
```

## SQL Generation Analysis

```bash
./sql_generation_analysis.sh  # Detailed analysis with color-coded results
```

**Features**:
- Batch size performance
- Cache hit/miss visualization
- 100-iteration tests with avg/min/max timing
- Optimization impact summary

**Use cases**:
- Validate cache performance
- Test batch optimizations
- Debug bottlenecks
- Regression testing

## Adding a New Database Backend (e.g. MySQL)

PostgreSQL support already ships this way (`src/db/postgresql.cppm`,
`postgresql_connection.cppm`, `postgresql_error.cppm`, `postgresql_statement.cppm`),
alongside SQLite. To add another backend:

1. Create `src/db/<backend>.cppm` implementing the concepts in `src/db/concept.cppm`
   (including the dialect-support concepts, e.g. `SupportsPgDialect`, `TransactionCapable`)
2. Add backend-specific connection/statement/error modules, mirroring the PostgreSQL split
3. Static-assert the dialect-support concepts next to the new `Connection` definition
4. Ensure concepts properly abstract differences — no ORM-layer (`src/orm/`) code changes needed

## Performance Checklist

1. ✅ **Compile-time generation**: Move computation to compile-time
2. ✅ **Memory pre-allocation**: Calculate exact sizes upfront
3. ✅ **Cache common patterns**: Thread-local caches
4. ✅ **Batch thresholds**: Optimize batch sizes
5. ✅ **Index sequences**: Use fold expressions
6. ✅ **Measurement**: Always validate with realistic data

## Thread Safety Patterns

### ✅ Safe: Per-Thread Connections

Default connection is thread-safe via `thread_local`:

```cpp
// ✅ SAFE: Each thread gets its own connection
void worker_thread() {
    // Initialize thread-local connection
    DefaultConnection<>::set(":memory:");

    // Each thread has isolated connection + QuerySet
    QuerySet<Person> qs;
    qs.where(age > 30).select();  // Thread-safe!
}

std::thread t1(worker_thread);
std::thread t2(worker_thread);  // No race - separate connections
```

### ❌ Unsafe: Sharing QuerySet Between Threads

```cpp
// ❌ UNSAFE: Sharing QuerySet between threads
QuerySet<Person> qs;

std::thread t1([&qs]() { qs.where(age > 30).select(); });
std::thread t2([&qs]() { qs.where(age > 50).select(); });  // RACE CONDITION!
```

**Problem**: QuerySet has mutable state (`where_expr_`, `join_stmt_`).

### ❌ Unsafe: Sharing Connection Between Threads

```cpp
// ❌ UNSAFE: Sharing connection between threads
auto conn = Connection::create("db.sqlite").value();

std::thread t1([&conn]() { QuerySet<Person>{conn}.select(); });
std::thread t2([&conn]() { QuerySet<Person>{conn}.select(); });  // RACE CONDITION!
```

**Problem**: SQLite connections are not thread-safe.

### ✅ Recommended Pattern

```cpp
// ✅ SAFE: Per-thread connections and QuerySets
void worker_thread() {
    auto conn = db::sqlite::Connection::create("database.db").value();
    QuerySet<Person> qs{conn};

    for (int i = 0; i < 1000; i++) {
        qs.where(age > 30).distinct<fields::Person.name>().execute();
    }
}

std::thread t1(worker_thread);
std::thread t2(worker_thread);
```

**Why safe:**
- Each thread has its own `Connection` instance
- Each thread has its own `QuerySet` instance
- `static thread_local` caching provides isolated storage per thread

## QuerySet Modes and Transformers

QuerySet operates in different **modes** via **transformers**:

### Default Mode (Object Mode)

```cpp
QuerySet<Person> qs;
auto results = qs.select();  // Returns plf::hive<Person>
```

### Tuple Mode (via distinct/values)

```cpp
// Single field - returns plf::hive<std::string>
auto names = qs.distinct<fields::Person.name>().execute();

// Multiple fields - returns plf::hive<std::tuple<std::string, int>>
auto pairs = qs.distinct<fields::Person.name, fields::Person.age>().execute();

// values() for specific columns
auto values = qs.values<fields::Person.name, fields::Person.age>().execute();
```

### Aggregate Mode (via aggregates)

```cpp
// Standalone aggregates - return scalar values via execute().
// MIN/MAX/AVG -> std::optional<double> (nullopt over an empty set, #416);
// COUNT -> int64_t; SUM -> int64_t (0 over an empty set).
auto min_age = qs.min<fields::Person.age>().execute();   // expected<std::optional<double>>
auto max_age = qs.max<fields::Person.age>().execute();   // expected<std::optional<double>>
auto count = qs.count().execute();                  // expected<int64_t>

// GROUP BY + aggregate - returns tuples (MIN/MAX/AVG columns are std::optional<double>)
auto by_dept = qs.group_by<fields::Person.department>().count().execute();
// Returns: plf::hive<std::tuple<DeptType, int64_t>>

// Multiple GROUP BY fields
auto by_age_dept = qs.group_by<fields::Person.age, fields::Person.department>()
                     .sum<fields::Person.salary>().execute();
```

### Available Methods

| Method | Description |
|--------|-------------|
| `where(Condition)` | Filter rows |
| `join<OtherModel>()` | Add JOIN clause |
| `order_by<Cols...>()` | Sort results |
| `limit(int)` | Restrict result count (value must be `>= 0`) |
| `offset(int)` | Skip results (value must be `>= 0`) |
| `group_by<Cols...>()` | Group results |
| `having(Condition)` | Filter groups |
| `distinct<Cols...>()` | Unique values (enters Tuple Mode) |
| `values<Cols...>()` | Select specific columns (enters Tuple Mode) |

### Available Aggregates

| Aggregate | Description |
|-----------|-------------|
| `count()` | COUNT(*) |
| `count<^^field>()` | COUNT(field) |
| `sum<^^field>()` | SUM(field) |
| `avg<^^field>()` | AVG(field) |
| `min<^^field>()` | MIN(field) |
| `max<^^field>()` | MAX(field) |

### Clause Ordering

Chain methods in any order - SQL clauses are reordered automatically:

```cpp
// These produce identical SQL:
qs.limit(10).where(age > 25).order_by<fields::Person.name>().select();
qs.where(age > 25).order_by<fields::Person.name>().limit(10).select();

// Generated: SELECT ... FROM Person WHERE age > 25 ORDER BY name LIMIT 10
```
