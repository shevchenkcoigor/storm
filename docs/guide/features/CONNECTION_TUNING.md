# Connection Tuning (SQLite)

Storm opens SQLite connections with sensible concurrency defaults and exposes a
small set of per-connection tuning knobs (issue #410). These knobs live on the
SQLite `Config` and propagate through `PoolConfig` to every pooled connection.

## Why this matters

SQLite returns `SQLITE_BUSY` the instant a second connection tries to write
while another holds the write lock — unless a **busy timeout** is set. With the
historical default (`busy_timeout = 0`), any two pooled connections touching the
same database file would collide immediately under write contention. Storm now
defaults to a non-zero busy timeout so writers **wait** for the lock instead of
failing instantly.

## The knobs

```cpp
struct Config {                 // storm::db::sqlite::Config
    std::size_t statement_cache_capacity = 512;                  // #273: 0 = unbounded
    int         busy_timeout_ms          = 5000;                 // #410: 0 = no wait (legacy)
    JournalMode journal_mode             = JournalMode::Default; // #410: WAL opt-in
};

enum class JournalMode { Default, WAL };   // storm::db::JournalMode
```

`JournalMode` itself is deliberately declared **backend-neutral**, in `storm::db` (`src/db/concept.cppm`),
so the generic `PoolConfig` can carry it without depending on the SQLite backend module. SQLite's
`Config` and the examples below reach it via `using storm::db::JournalMode;` re-exposed from
`storm::db::sqlite`.

| Field | Default | Effect |
|---|---|---|
| `busy_timeout_ms` | `5000` | Milliseconds a writer waits for a held lock before giving up with `SQLITE_BUSY`. `0` restores the legacy "fail immediately" behaviour. Applied via `sqlite3_busy_timeout()`. |
| `journal_mode` | `Default` | `WAL` opts into write-ahead logging (`PRAGMA journal_mode=WAL`), which materially improves concurrent read/write throughput on file-backed databases. `Default` leaves the engine default (rollback journal). |

### Single connection

```cpp
using storm::db::sqlite::Connection;
using storm::db::sqlite::JournalMode;

// Defaults: 5s busy timeout, rollback journal.
auto conn = Connection::open("app.db");

// Custom timeout + WAL.
auto tuned = Connection::open("app.db", {.busy_timeout_ms = 10000, .journal_mode = JournalMode::WAL});

// Opt out of waiting entirely (legacy behaviour).
auto legacy = Connection::open("app.db", {.busy_timeout_ms = 0});
```

The tuning is applied once, in `open()`, immediately after the handle is created
and before it is handed out. It does **not** touch the query hot path (prepare /
bind / step), so it costs nothing per query.

## Pooled SQLite concurrency

`PoolConfig` carries the same knobs and propagates them to every connection the
pool creates (alongside `statement_cache_capacity`):

```cpp
using storm::db::ConnectionPool;
using storm::db::sqlite::Connection;
using storm::db::sqlite::JournalMode;

auto pool = ConnectionPool<Connection>::create(
        "app.db",
        {.min_connections = 2,
         .max_connections = 8,
         .busy_timeout_ms = 5000,
         .journal_mode    = JournalMode::WAL});
```

**Recommended setup for a shared file database under a pool:** enable WAL and
keep a non-zero `busy_timeout_ms`. WAL lets readers proceed concurrently with a
single writer; the busy timeout absorbs brief writer-vs-writer contention by
waiting instead of erroring. Together they turn the common "two pooled
connections hit the same file" case from an immediate `SQLITE_BUSY` into a short
wait.

### Notes

- **`:memory:` and temp databases ignore WAL.** SQLite cannot use WAL on an
  in-memory or temporary database; a `JournalMode::WAL` request there is
  silently ignored by the engine — `open()` still succeeds and reports the
  engine's actual journal mode.
- **Backend scope.** These knobs are SQLite-specific. The PostgreSQL backend's
  `Config` is just `StatementCacheConfig`; `PoolConfig`'s SQLite tuning fields
  are ignored when the pool is instantiated over a PostgreSQL connection.
- **WAL persists in the database file.** Once a file database is switched to WAL
  it stays in WAL across reopens until explicitly changed; re-requesting WAL is
  idempotent.

## Pool shutdown (`shutdown_timeout_ms`)

`ConnectionPool::shutdown()` waits for checked-out connections to come back, but
only up to `PoolConfig::shutdown_timeout_ms` (default `5000`; `0` = don't wait).
It returns `bool` (`[[nodiscard]]`): `true` means every connection was returned and
the pool is torn down; `false` means the wait timed out and some are still checked out.

On `false`, idle connections are already released, and a connection still held is
never destroyed under its owner — it is closed when its handle is finally returned.
Do not delete the database file or stop the server on `false`.

A thread whose own `DefaultConnection` slot holds a checkout can never return it
while blocked in `shutdown()`; call `DefaultConnection<...>::clear()` first, or
`shutdown()` will wait out the full timeout and return `false`.

See also [Statement Caching](../../internals/architecture/STATEMENT_CACHING.md) for the
`statement_cache_capacity` knob that shares the same `Config`.
