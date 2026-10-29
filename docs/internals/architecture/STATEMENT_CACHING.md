# Statement Caching Architecture

Storm ORM achieves near-raw SQLite performance through a single
Connection-level prepared-statement cache. Each `Connection` keeps a pool of
prepared statements keyed by their SQL text, so repeated operations reuse a
compiled statement instead of re-parsing and re-planning it.

> **History.** Storm once had three cache levels: L1 (per-`QuerySet`
> `unique_ptr` statement members), L2 (per-`Statement` raw `cached_*_stmt_`
> pointers), and L3 (the Connection pool). Issue #214 measured all three and
> found L1 and L2 give no measurable benefit on any workload, so they were
> removed. Only the Connection-level cache remains. See
> [Why only one cache level (#214)](#why-only-one-cache-level-214).

## The statement cache (Connection pool)

Connection maintains a pool of prepared statements:

```cpp
auto prepare_cached(const std::string& sql) -> std::expected<Statement*, Error> {
    // Check if already prepared
    if (auto it = statement_cache_.find(sql); it != statement_cache_.end()) {
        return it->second.get();  // Cache hit!
    }

    // Prepare new statement
    auto stmt = prepare(sql);
    auto [pos, _] = statement_cache_.try_emplace(sql, std::move(*stmt));
    return pos->second.get();
}
```

The cache type is `unordered_map<string, unique_ptr<Statement>>`.

**Benefits**:
- Shared across all QuerySets on that Connection
- Avoids SQL parsing and re-planning
- `reset()` instead of `finalize()` on reuse
- Bounded by a configurable capacity with CLOCK/second-chance eviction (#273)

## Bounded growth: CLOCK eviction + statistics (#273)

Before #273 the cache had no eviction — it grew by one entry per distinct SQL
string ever prepared, for the life of the connection. That only matters for a
long-lived connection preparing a genuinely unbounded set of distinct SQL
(dynamic literal-varying SQL, ad-hoc reporting); Storm parameterises WHERE
values, so the common ORM case plateaus on its own. #273 makes growth bounded.

**Capacity.** `Connection::open()` takes an optional config; the connection
pool propagates it via `PoolConfig`:

```cpp
auto conn = Connection::open(":memory:", {.statement_cache_capacity = 512});
// 0 = unbounded (the pre-#273 behavior). Default is 512.

auto pool = ConnectionPool<Connection>::create(
        conninfo, {.statement_cache_capacity = 512});
```

**Policy — CLOCK / second-chance (approximate LRU).** Each cache entry carries
an atomic *reference bit*. A cache **hit** sets that bit under the existing
`shared_lock` — a single relaxed atomic store, no structural mutation — so the
read-parallel hot path added in #271 is preserved (a strict list-LRU would turn
every hit into a write-locked list splice). When an insert would exceed
capacity, the **eviction sweep** (run under the `unique_lock` the insert already
holds) walks the entries: an entry whose bit is set gets it cleared and is
skipped (its "second chance"); the first entry with a clear bit is evicted. If a
full pass clears every bit without finding a victim, the next entry is evicted to
guarantee progress. Frequently-reused statements keep getting their bit re-set
and survive; cold one-off statements are evicted.

**Statistics.** `cache_stats()` returns a snapshot:

```cpp
struct CacheStats {
    std::uint64_t hits;          // lifetime cache hits
    std::uint64_t misses;        // lifetime misses (prepared + inserted)
    std::uint64_t evictions;     // lifetime evictions
    std::size_t   current_size;  // live entry count
};
```

Counters are lifetime totals — `clear_statement_cache()` empties the map but
does **not** reset them.

## Statement lifetime (per-call, owned by proxy)

Statements are **per-call temporaries**, not cached objects (Approach A,
adopted in #214).

For each operation, the QuerySet constructs a fresh statement bound to the
Connection — e.g. `qs.select()` builds a `SelectStatement(conn_)`. The
statement's `.query(...)` then **moves** the statement into the returned result
proxy, which holds it **by value**. The proxy and its statement are destroyed
together at the end of the full-expression:

```cpp
// Temporary SelectStatement is moved into the proxy returned by select();
// proxy + statement die at the ';' after execute().
auto rows = qs.select().execute();
```

The same pattern applies to update, insert, erase, distinct, aggregate, and
set-operation paths. Because the proxy owns the statement by value, no raw
`Statement*` is held anywhere across statements — the lifetime is purely
scope-bound.

The Connection cache is unaffected: each per-call statement still asks the
Connection for a compiled SQLite statement via `prepare_cached()`, so the
parse/plan cost is paid once per distinct SQL string.

## Performance Impact

The numbers below come from the Connection-level cache (the only cache that
ever mattered):

### UPDATE Operations

**Before caching**: 100K operations/sec
**After caching**: 12M peak operations/sec
**Speedup**: 100x+ improvement

### DELETE Operations

**Before caching**: 947K operations/sec
**After caching**: 21.6M operations/sec (73% of raw SQLite)
**Speedup**: 22.8x improvement

### SELECT Operations

**With caching**: 13.07M rows/sec (74% of raw SQLite)
**Cache hit rate**: Near 100% for repeated queries

## No statement type is locally cached

Earlier versions cached statement objects per QuerySet (L1) and prepared-handle
pointers per Statement (L2). **No statement type is cached at those levels
anymore.** Every operation — SELECT, INSERT, UPDATE, DELETE, JOIN — builds a
fresh statement per call and reuses only the Connection-level prepared
statement through `prepare_cached()`. There is also no longer a separate JOIN
statement cache (`cached_join_stmt_`); JOIN follows the same per-call path.

## Cache Invalidation

Cached statements pin the *compiled* form of a SQL string to a specific schema.
If the underlying schema changes (`ALTER TABLE`, `DROP TABLE`, table rename),
the cached prepared statements become stale. Storm exposes two explicit hooks
to drop them; Storm does **not** auto-detect DDL (see Issue #215 follow-ups).

| API | Scope | When to call |
|-----|-------|--------------|
| `Connection::clear_statement_cache()` | Whole connection | After a broad schema rebuild or to free memory |
| `Connection::clear_statement_cache(table)` | Entries referencing `table` | After targeted DDL — e.g., `ALTER TABLE persons ADD COLUMN …` |

`QuerySet::reset()` still clears the QuerySet's WHERE/JOIN/LIMIT/OFFSET query
state when reusing a QuerySet with a fresh filter chain. It no longer touches
any statement cache, because there is no per-QuerySet statement state to clear.

### Correct invalidation order

```cpp
// 1. Targeted DDL through the raw connection
conn->execute("ALTER TABLE persons ADD COLUMN nickname TEXT");

// 2. Drop the matching cache entries
conn->clear_statement_cache("persons");

// 3. Next operation re-prepares against the new schema
qs.select().execute();
```

Because statements are per-call temporaries that hold no raw pointers across
calls, there is nothing else to invalidate: clearing the Connection cache is
enough, and the next operation re-prepares.

### Pointer stability

The cache stores statements as `unordered_map<string, unique_ptr<Statement>>`
(not `unordered_map<string, Statement>`). The `unique_ptr` keeps each Statement
pinned in memory across map growth, so a `Statement*` returned by
`prepare_cached()` stays valid even if the map rehashes. This is now the central
lifetime guarantee for the cache.

Rehash stability is not the same as validity. The cache is bounded
(`statement_cache_capacity`, default 512) and evicts with CLOCK, so **a
`Statement*` is valid only until the next `prepare_cached()` on the same
`Connection`**: at capacity that call may evict the entry the pointer refers to
and destroy the `Statement`. `clear_statement_cache()` ends validity the same
way. On SQLite, `Connection::execute("BEGIN TRANSACTION" | "COMMIT" | "ROLLBACK")`
also goes through `prepare_cached()`, so `storm::begin()`, `txn->commit()` and a
guard's rollback count as "the next `prepare_cached()`" too. The pointer is a non-owning observer (C++ Core Guidelines R.3); holding it
raw is intentional on hot paths (see PERFORMANCE.md, "Raw Pointer Caching in Hot
Loops"), but the caller must not keep it across another `prepare_cached()`.
Code that needs two statements prepares the second one only after the last use
of the first, as `execute_chunked` does for the remainder statement (#728).

## Thread Safety

**Connection-level cache**: thread-safe via `std::shared_mutex` (Phase 2,
issue #271). `statement_cache_` on each `Connection` is guarded by a
`std::shared_mutex`:

- **Cache hit (hot path)** → `std::shared_lock`, so concurrent readers don't
  serialize.
- **Insert on miss + `clear_statement_cache()` / `clear_statement_cache(table)`**
  → `std::unique_lock`. On a miss the statement is prepared *outside* the lock
  (the expensive `sqlite3_prepare_v2` / `PQprepare` call); the lock is then taken
  only for the `try_emplace`, which also resolves the race where two threads
  prepare the same SQL at once. Both SQLite and PostgreSQL backends are covered;
  TSAN-clean under concurrent `prepare_cached` + clear traffic
  (`tests/db/test_statement_cache_threading.cpp`).

> **Important — pointer lifetime caveat.** The mutex makes the cache *map*
> operations race-free; it does **not** make a `Statement*` returned by
> `prepare_cached()` safe against a concurrent `clear_statement_cache()` that
> would destroy the pointed-to `Statement`. What guarantees that pointer's
> lifetime is the **exclusive-checkout invariant**: `ConnectionPool` hands each
> thread its own `Connection` between `checkout()` and `checkin()`, so no two
> threads ever touch one `Connection`'s cache at the same time. A debug-only
> assertion in `PoolCore` (gated behind `NDEBUG`) trips if a connection is
> checked out by two threads at once.

**Thread-local SQL caching**: Each thread has a separate compile-time SQL cache
(zero synchronization).

**Recommendation**: Per-thread QuerySet + Connection instances (or a
`ConnectionPool`, which enforces exclusive checkout).

## Why only one cache level (#214)

Issue #214 ran a controlled investigation
([results](../../superpowers/results/2026-05-30-cache-investigation-results.md))
across four cache configurations (all-3, no-L2, no-L1, L3-only) on multi-row
queries, bulk writes, and single-row tight loops.

**Finding:** L1 and L2 gave no measurable benefit on any workload. Every delta
sat at or below the measurement noise floor, including the single-row
`GetByPk` tight loop that was built specifically to expose statement-setup
cost (all four configs landed within 707±14 ns). The Connection cache plus the
optimizer already absorb the cost that L1/L2 were meant to save. This pointed
clearly to **Option C — collapse to a single cache level.**

### Accepted tradeoff

After the real deletion, the Core 8 Release benchmark was re-measured against
the develop baseline (both `-O3`, 20–30 reps). All categories stayed within
±5% noise **except one**:

- **Single-row UPDATE_PK (N:1) in a QuerySet-reuse loop: +7.5%** (855 ns vs
  795 ns), reproducible. INSERT/N:1 (+3.2%) and DELETE_PK/N:1 (+2.1%) trend the
  same way but stay under 5%.

This is an **accepted tradeoff** (decided with the maintainer). The extra cost
is one fresh statement construction (a `shared_ptr` refcount bump) plus the
Connection-cache hash lookup that the old L1+L2 path skipped — about ~60 ns on
a sub-microsecond operation, visible only in a tight single-row reuse loop. All
bulk and multi-row paths are clean. The simplicity of a single cache level
outweighs ~60 ns in a rare workload.

### Verification

The collapse was validated on the standard gates:

- **ASAN+UBSAN**: 1923/1923 pass, zero sanitizer errors — confirms the
  per-call / own-by-value lifetime is sound.
- **TSAN**: 1923/1923 pass, zero data races — the Connection `shared_mutex`
  remains the only shared mutable state.
- **Coverage**: 100.0% line (5922/5922).

## See Also

- [SELECT Queries](../../guide/features/SELECT_QUERIES.md) - How SELECT uses caching
- [CRUD Operations](../../guide/features/CRUD_OPERATIONS.md) - UPDATE/DELETE caching
- [SQL Generation](SQL_GENERATION.md) - What gets cached
- [#214 cache investigation results](../../superpowers/results/2026-05-30-cache-investigation-results.md)
