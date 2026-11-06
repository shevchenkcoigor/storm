module;

#include <sqlite3.h>

export module storm_db_sqlite;

import std;
import storm_db_concept;
export import storm_db_sqlite_statement; // Error, RealApi, BasicStatement<Api> (#760)

export namespace storm::db::sqlite {

    // Cache size constants
    namespace cache {
        constexpr std::size_t STMT_CACHE_RESERVE = 32; // Initial statement cache capacity
    } // namespace cache

    // Issue #410: SQLite journal mode. Re-exported from the backend-neutral
    // definition in storm_db_concept so callers can spell storm::db::sqlite::WAL.
    using storm::db::JournalMode;

    // Issue #410: SQLite connection tuning. SQLite-specific (PostgreSQL keeps the
    // shared StatementCacheConfig). The statement_cache_capacity field name is kept
    // identical to StatementCacheConfig so existing designated-init call sites
    // (Connection::open(":memory:", {.statement_cache_capacity = N})) keep compiling.
    struct Config {
        std::size_t statement_cache_capacity = 512;                  // Issue #273: 0 = unbounded
        int         busy_timeout_ms          = 5000;                 // Issue #410: 0 = no wait (legacy)
        JournalMode journal_mode             = JournalMode::Default; // Issue #410: WAL opt-in
    };

    // Custom deleter for sqlite3
    struct SqliteDeleter {
        auto operator()(sqlite3* db) const noexcept -> void {
            if (db != nullptr) {
                sqlite3_close_v2(db);
            }
        }
    };

    template <typename Api> class BasicConnection {
        using SqlitePtr = std::unique_ptr<sqlite3, SqliteDeleter>;

      public:
        using Error     = sqlite::Error;
        using Statement = BasicStatement<Api>;

        // Dialect traits (version-gated via CMake — see cmake/db.cmake)
        static constexpr bool supports_limit_all = false;
        static constexpr bool supports_returning = true; // SQLite 3.35+ required
#ifdef STORM_SQLITE_STRICT_TABLES
        static constexpr bool supports_strict_tables = true; // SQLite 3.37+
#else
        static constexpr bool supports_strict_tables = false;
#endif

        // Issue #410: per-Connection tuning (cache capacity + busy_timeout + WAL).
        using Config = sqlite::Config;

        // Factory method with error handling and thread-safe flags
        [[nodiscard]] static auto open(std::string_view db_path, Config config = {})
                -> std::expected<BasicConnection, Error> {
            sqlite3*          raw_db = nullptr;
            const std::string db_path_str(db_path); // Ensure null-termination
            // Add SQLITE_OPEN_FULLMUTEX for serialized mode (thread-safe)
            const int rc = Api::open_v2(
                    db_path_str.c_str(),
                    &raw_db,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX,
                    nullptr
            );

            if (rc != SQLITE_OK) {
                const auto error = Error{rc, sqlite3_errmsg(raw_db)};
                if (raw_db != nullptr) {
                    sqlite3_close_v2(raw_db);
                }
                return std::unexpected(error);
            }

            // Issue #410: apply connection tuning (busy_timeout, journal mode) and #412:
            // referential-integrity PRAGMA before handing the handle out.
            if (auto tuned = apply_tuning(raw_db, config); !tuned.has_value()) {
                sqlite3_close_v2(raw_db);
                return std::unexpected(tuned.error());
            }

            return BasicConnection{SqlitePtr{raw_db}, config};
        }

        // Destructor - unique_ptr handles cleanup via SqliteDeleter
        ~BasicConnection() = default;

        // Move semantics (smart pointer handles cleanup). cache_mutex_ is a
        // MovableSharedMutex (Issue #271) so these can stay defaulted: a moved
        // Connection gets a fresh, unlocked mutex.
        BasicConnection(BasicConnection&&)                    = default;
        auto operator=(BasicConnection&&) -> BasicConnection& = default;

        // Delete copy operations
        BasicConnection(const BasicConnection&)                    = delete;
        auto operator=(const BasicConnection&) -> BasicConnection& = delete;

        // DatabaseConnection concept implementation
        [[nodiscard]] constexpr auto is_open() const noexcept -> bool {
            return static_cast<bool>(db);
        }

        [[nodiscard]] auto prepare(std::string_view sql) -> std::expected<Statement, Error> {
            if (!is_open()) {
                return std::unexpected(Error{SQLITE_MISUSE, "Connection not open"});
            }
            return prepare_raw(sql);
        }

        // Prepare statement with caching - reuses statements for identical SQL.
        // Hot path (cache hit) + insert lock discipline live in storm_db_concept's
        // cache_* helpers (Issue #271, shared with the PostgreSQL backend); only the
        // backend-specific prepare_raw() call differs. On a miss the lock is dropped
        // before prepare_raw() so the expensive syscall runs uncontended.
        // LIFETIME: the returned Statement* is a non-owning view valid only until the
        // next prepare_cached() on this Connection — at capacity that call may evict
        // the entry it points to (#728). Finish with it before preparing another.
        // execute("BEGIN TRANSACTION" | "COMMIT" | "ROLLBACK") below also goes through
        // here, so storm::begin(), commit() and a guard's rollback count as well.
        [[nodiscard]] auto prepare_cached(std::string_view sql) -> std::expected<Statement*, Error> {
            if (!is_open()) {
                return std::unexpected(Error{SQLITE_MISUSE, "Connection not open"});
            }

            if (auto* hit = storm::db::cache_find_hit(cache_, sql)) [[likely]] {
                return hit;
            }

            auto prepared = prepare_raw(sql);
            if (!prepared.has_value()) {
                return std::unexpected(prepared.error());
            }
            return storm::db::cache_try_insert(cache_, sql, std::make_unique<Statement>(std::move(*prepared)));
        }

        // Clear the entire statement cache (useful for memory management or after
        // major schema changes). Callers must not retain `Statement*` obtained from
        // a prior prepare_cached() across this call — the entries are destroyed.
        auto clear_statement_cache() noexcept -> void {
            storm::db::cache_clear_all(cache_); // Issue #271
        }

        // Issue #215: drop cached entries whose SQL references the given table.
        // Used after targeted DDL (ALTER TABLE persons …) when other tables'
        // cached statements should be preserved. Matching is word-boundary aware
        // so clearing "persons" does NOT touch "person_addresses".
        auto clear_statement_cache(std::string_view table) -> void {
            storm::db::cache_clear_table(cache_, table); // Issue #271
        }

        // Get cache statistics
        [[nodiscard]] auto cached_statement_count() const noexcept -> std::size_t {
            return storm::db::cache_count(cache_); // Issue #271
        }

        // Issue #273: snapshot of hit/miss/eviction counters + current size.
        [[nodiscard]] auto cache_stats() const noexcept -> storm::db::CacheStats {
            return storm::db::cache_stats(cache_);
        }

        // Pre-populate statement cache with common operations
        auto prepare_common_statements() -> void {
            // Common patterns that benefit from pre-compilation
            static const std::array<std::string_view, 3> common_patterns = {"BEGIN TRANSACTION", "COMMIT", "ROLLBACK"};

            for (const auto& sql : common_patterns) {
                std::ignore = prepare_cached(sql);
            }
        }

        // Execute SQL with error handling (optimized to use cached statements for common operations)
        [[nodiscard]] auto execute(std::string_view sql) -> std::expected<void, Error> {
            if (!is_open()) {
                return std::unexpected(Error{SQLITE_MISUSE, "Connection not open"});
            }

            // For common operations, try to use cached prepared statements for better performance
            static const std::unordered_set<std::string_view> cached_operations =
                    {"BEGIN TRANSACTION", "COMMIT", "ROLLBACK"};

            if (cached_operations.contains(sql)) {
                auto stmt_result = prepare_cached(sql);
                if (stmt_result.has_value()) {
                    return (*stmt_result)->execute();
                }
                // Fall through to regular execution if caching fails
            }

            // Regular execution for non-cached operations
            // sqlite3_exec requires null-terminated string
            const std::string sql_str(sql);
            char*             errmsg = nullptr;
            const int         rc     = Api::exec(db.get(), sql_str.c_str(), &errmsg);

            if (rc != SQLITE_OK) {
                const Error error{rc, errmsg != nullptr ? errmsg : "Unknown error"};
                sqlite3_free(errmsg);
                return std::unexpected(error);
            }

            return {};
        }

        [[nodiscard]] auto get() const noexcept -> sqlite3* {
            return db.get();
        }

        // Transaction-nesting state (#415). The public transaction guard calls
        // enter_transaction() after a successful BEGIN and leave_transaction()
        // after COMMIT/ROLLBACK; batch ops query in_transaction() and skip their
        // own inner BEGIN/COMMIT when an outer scope is already active (fixes #9).
        [[nodiscard]] constexpr auto in_transaction() const noexcept -> bool {
            return txn_depth_ > 0;
        }
        constexpr auto enter_transaction() noexcept -> void {
            ++txn_depth_;
        }
        constexpr auto leave_transaction() noexcept -> void {
            if (txn_depth_ > 0) {
                --txn_depth_;
            }
        }

      private:
        explicit BasicConnection(SqlitePtr db_ptr, Config config) : db(std::move(db_ptr)) {
            cache_.capacity = config.statement_cache_capacity; // Issue #273
            // Reserve to keep early inserts on the same rehash bucket. Pointer
            // stability across rehash is guaranteed by the unique_ptr in CacheEntry
            // (Issue #215).
            cache_.map.reserve(cache::STMT_CACHE_RESERVE);
        }

        // Issue #410: apply connection tuning (busy_timeout, journal_mode) to a
        // freshly opened raw handle. busy_timeout uses the dedicated C API;
        // journal_mode goes through a PRAGMA. WAL is silently ignored by SQLite on
        // :memory:/temp databases, so a non-WAL result there is not an error.
        [[nodiscard]] static auto apply_tuning(sqlite3* raw_db, const Config& config) -> std::expected<void, Error> {
            if (config.busy_timeout_ms > 0) {
                const int rc = Api::busy_timeout(raw_db, config.busy_timeout_ms);
                if (rc != SQLITE_OK) {
                    return std::unexpected(Error{rc, sqlite3_errmsg(raw_db)});
                }
            }

            if (config.journal_mode == JournalMode::WAL) {
                const int rc = Api::exec(raw_db, "PRAGMA journal_mode=WAL", nullptr);
                if (rc != SQLITE_OK) {
                    return std::unexpected(Error{rc, sqlite3_errmsg(raw_db)});
                }
            }

            // Enforce referential integrity (#412). SQLite parses FK syntax always but
            // ignores it unless this PRAGMA is set, per connection — the docs recommend
            // applications turn it on. PG enforces FKs natively, so no equivalent there.
            if (const int rc = Api::exec(raw_db, "PRAGMA foreign_keys = ON", nullptr); rc != SQLITE_OK) {
                return std::unexpected(Error{rc, sqlite3_errmsg(raw_db)});
            }

            return {};
        }

        // Single source of truth for sqlite3_prepare_v2 + error wrapping.
        [[nodiscard]] auto prepare_raw(std::string_view sql) -> std::expected<Statement, Error> {
            sqlite3_stmt* stmt = nullptr;
            const int     rc   = Api::prepare_v2(db.get(), sql.data(), static_cast<int>(sql.size()), &stmt);
            if (rc != SQLITE_OK) {
                return std::unexpected(Error{rc, sqlite3_errmsg(db.get())});
            }
            return Statement{stmt};
        }

        SqlitePtr db;
        // Issue #271/#273: the L3 cache, its shared_mutex, stat counters, and
        // capacity in one movable bundle. shared_lock on the hit path, unique_lock
        // for insert/clear/evict. mutable so const accessors (cached_statement_count,
        // cache_stats) can take a shared_lock.
        mutable storm::db::StatementCacheState<Statement> cache_;

        // Transaction-nesting depth (#415). 0 = autocommit; >0 = inside a guard.
        int txn_depth_ = 0;
    };

    // The production backend: the real sqlite3_* API (issue #760). A real class,
    // NOT `using Connection = BasicConnection<RealApi>`: a namespace-scope alias
    // exported from a module does not merge across the PCH + `import` paths, so
    // every `sqlite::Connection` spelling became "reference is ambiguous" (the
    // #464 / COMPILER_ISSUES.md section 11 failure class). A class merges fine.
    class Connection final : public BasicConnection<RealApi> {
        using Base = BasicConnection<RealApi>;

      public:
        explicit Connection(Base&& base) : Base(std::move(base)) {}

        [[nodiscard]] static auto open(std::string_view db_path, Config config = {})
                -> std::expected<Connection, Error> {
            auto opened = Base::open(db_path, config);
            if (!opened.has_value()) {
                return std::unexpected(opened.error());
            }
            return Connection{std::move(*opened)};
        }
    };

    // Verify concepts are satisfied
    static_assert(storm::db::DatabaseConnection<Connection>);
    static_assert(storm::db::CachedDatabaseConnection<Connection>);
    static_assert(storm::db::DatabaseStatement<BasicStatement<RealApi>>);
    static_assert(storm::db::DatabaseError<Error>);

    // Dialect-support concepts (#477). SQLite uses its own dialect (no
    // uses_pg_dialect), declares supports_limit_all, and is transaction-capable.
    static_assert(!storm::db::SupportsPgDialect<Connection>);
    static_assert(storm::db::SupportsLimitAll<Connection>);
    static_assert(storm::db::TransactionCapable<Connection>);

} // namespace storm::db::sqlite
