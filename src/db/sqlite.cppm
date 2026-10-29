module;

#include <sqlite3.h>

export module storm_db_sqlite;

import std;
import storm_db_concept;

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

    // Error type — shared across backends, defined in storm_db_concept (issue #316).
    using Error = storm::db::Error;

    // Forward declaration
    class Connection;

    // RAII wrapper for sqlite3_stmt
    class Statement {
        using StmtPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

      public:
        using Error = sqlite::Error;

        // OPTIMIZATION: Cache raw pointer to eliminate unique_ptr::get() overhead in hot loops
        explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt, sqlite3_finalize), raw_(stmt) {}

        // Destructor - unique_ptr handles cleanup via sqlite3_finalize
        ~Statement() = default;

        // Move semantics — explicit so the moved-from source nulls its cached
        // raw_. A defaulted move copies raw_ while stmt_ goes null, leaving the
        // source aliasing the statement now owned by the destination (issue
        // #354). Keep the cached raw_ for hot-loop performance (CLAUDE.md).
        Statement(Statement&& other) noexcept : stmt_(std::move(other.stmt_)), raw_(other.raw_) {
            other.raw_ = nullptr;
        }

        auto operator=(Statement&& other) noexcept -> Statement& {
            if (this != &other) {
                stmt_      = std::move(other.stmt_);
                raw_       = other.raw_;
                other.raw_ = nullptr;
            }
            return *this;
        }

        // Delete copy operations
        Statement(const Statement&)                    = delete;
        auto operator=(const Statement&) -> Statement& = delete;

        // DatabaseStatement concept implementation
        // All methods use template pattern for cross-module inlining
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_int(int index, int value) noexcept
                -> std::expected<void, Error> {
            const int rc = sqlite3_bind_int(raw_, index, value);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind integer parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_text(int index, std::string_view value) noexcept
                -> std::expected<void, Error> {
            const int rc =
                    sqlite3_bind_text(raw_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind text parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_int64(int index, std::int64_t value) noexcept
                -> std::expected<void, Error> {
            const int rc = sqlite3_bind_int64(raw_, index, value);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind int64 parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_double(int index, double value) noexcept
                -> std::expected<void, Error> {
            const int rc = sqlite3_bind_double(raw_, index, value);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind double parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_null(int index) noexcept -> std::expected<void, Error> {
            const int rc = sqlite3_bind_null(raw_, index);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind null parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto
        bind_blob(int index, const void* data, std::size_t size) noexcept // NOSONAR(cpp:S5008) - SQLite BLOB API
                -> std::expected<void, Error> {
            const int rc = sqlite3_bind_blob(raw_, index, data, static_cast<int>(size), SQLITE_TRANSIENT);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind blob parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto execute() noexcept -> std::expected<void, Error> {
            const int rc = sqlite3_step(raw_);
            if (rc != SQLITE_DONE) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to execute statement"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto step() noexcept -> std::expected<bool, Error> {
            const int rc = sqlite3_step(raw_);
            if (rc == SQLITE_ROW) [[likely]] {
                return true; // Row available
            }
            if (rc == SQLITE_DONE) {
                return false; // No more rows
            }
            return std::unexpected(Error{rc, "Failed to step statement"});
        }

        template <typename = void> __attribute__((always_inline)) auto reset() noexcept -> void {
            sqlite3_reset(raw_);
        }

        template <typename = void> __attribute__((always_inline)) auto finalize() noexcept -> void {
            stmt_.reset(); // Calls sqlite3_finalize via deleter
        }

        // Access raw handle for advanced operations
        // Returns cached raw pointer for zero overhead
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto handle() const noexcept -> sqlite3_stmt* {
            return raw_;
        }

        // === High-Performance Abstraction Layer ===
        // These methods provide database-agnostic interface while maintaining
        // zero-cost abstraction through aggressive inlining

        // Step with raw return value (no std::expected overhead in hot loop)
        // OPTIMIZATION: Template forces body visibility across modules, enabling inlining
        // Uses cached raw pointer to eliminate unique_ptr::get() overhead
        template <typename = void> [[nodiscard]] __attribute__((always_inline)) auto step_raw() noexcept -> int {
            return sqlite3_step(raw_);
        }

        // Column extraction methods - TEMPLATES for cross-module inlining
        // Use cached raw pointer for zero overhead
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_int(int col_index) const noexcept -> int {
            return sqlite3_column_int(raw_, col_index);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_int64(int col_index) const noexcept -> std::int64_t {
            return sqlite3_column_int64(raw_, col_index);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_double(int col_index) const noexcept -> double {
            return sqlite3_column_double(raw_, col_index);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_text_ptr(int col_index) const noexcept -> const
                unsigned char* {
            return sqlite3_column_text(raw_, col_index);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_bytes(int col_index) const noexcept -> int {
            return sqlite3_column_bytes(raw_, col_index);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_text_view(int col_index) const noexcept
                -> std::string_view {
            const unsigned char* text = sqlite3_column_text(raw_, col_index);
            if (text != nullptr) {
                const auto len = static_cast<std::size_t>(sqlite3_column_bytes(raw_, col_index));
                return {reinterpret_cast<const char*>(text), len};
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_bool(int col_index) const noexcept -> bool {
            return sqlite3_column_int(raw_, col_index) != 0;
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_float(int col_index) const noexcept -> float {
            return static_cast<float>(sqlite3_column_double(raw_, col_index));
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_blob_ptr(int col_index) const noexcept -> const
                void* { // NOSONAR(cpp:S5008) - SQLite BLOB API
            return sqlite3_column_blob(raw_, col_index);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto is_null(int col_index) const noexcept -> bool {
            return sqlite3_column_type(raw_, col_index) == SQLITE_NULL;
        }

        // Error message extraction
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto get_error_message() const noexcept -> const char* {
            return sqlite3_errmsg(sqlite3_db_handle(raw_));
        }

        // Returns SQL string with all bound parameters inlined (for debugging / SQL inspection)
        // Uses sqlite3_expanded_sql() which substitutes ? placeholders with actual bound values
        template <typename = void> [[nodiscard]] auto expanded_sql() const -> std::string {
            char* expanded = sqlite3_expanded_sql(raw_);
            if (expanded == nullptr) {
                return {};
            }
            std::string result(expanded);
            sqlite3_free(expanded);
            return result;
        }

        // Constants for return codes (make them constexpr for compile-time checks)
        static constexpr int ROW_AVAILABLE = SQLITE_ROW;
        static constexpr int NO_MORE_ROWS  = SQLITE_DONE;

      private:
        StmtPtr       stmt_;
        sqlite3_stmt* raw_; // Cached raw pointer for hot-path performance
    };

    class Connection {
        using SqlitePtr = std::unique_ptr<sqlite3, SqliteDeleter>;

      public:
        using Error     = sqlite::Error;
        using Statement = sqlite::Statement;

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
                -> std::expected<Connection, Error> {
            sqlite3*          raw_db = nullptr;
            const std::string db_path_str(db_path); // Ensure null-termination
            // Add SQLITE_OPEN_FULLMUTEX for serialized mode (thread-safe)
            const int rc = sqlite3_open_v2(
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

            return Connection{SqlitePtr{raw_db}, config};
        }

        // Destructor - unique_ptr handles cleanup via SqliteDeleter
        ~Connection() = default;

        // Move semantics (smart pointer handles cleanup). cache_mutex_ is a
        // MovableSharedMutex (Issue #271) so these can stay defaulted: a moved
        // Connection gets a fresh, unlocked mutex.
        Connection(Connection&&)                    = default;
        auto operator=(Connection&&) -> Connection& = default;

        // Delete copy operations
        Connection(const Connection&)                    = delete;
        auto operator=(const Connection&) -> Connection& = delete;

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
            const int         rc     = sqlite3_exec(db.get(), sql_str.c_str(), nullptr, nullptr, &errmsg);

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
        explicit Connection(SqlitePtr db_ptr, Config config) : db(std::move(db_ptr)) {
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
                const int rc = sqlite3_busy_timeout(raw_db, config.busy_timeout_ms);
                if (rc != SQLITE_OK) {
                    return std::unexpected(Error{rc, sqlite3_errmsg(raw_db)});
                }
            }

            if (config.journal_mode == JournalMode::WAL) {
                const int rc = sqlite3_exec(raw_db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
                if (rc != SQLITE_OK) {
                    return std::unexpected(Error{rc, sqlite3_errmsg(raw_db)});
                }
            }

            // Enforce referential integrity (#412). SQLite parses FK syntax always but
            // ignores it unless this PRAGMA is set, per connection — the docs recommend
            // applications turn it on. PG enforces FKs natively, so no equivalent there.
            if (const int rc = sqlite3_exec(raw_db, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
                rc != SQLITE_OK) {
                return std::unexpected(Error{rc, sqlite3_errmsg(raw_db)});
            }

            return {};
        }

        // Single source of truth for sqlite3_prepare_v2 + error wrapping.
        [[nodiscard]] auto prepare_raw(std::string_view sql) -> std::expected<Statement, Error> {
            sqlite3_stmt* stmt = nullptr;
            const int     rc   = sqlite3_prepare_v2(db.get(), sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
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

    // Verify concepts are satisfied
    static_assert(storm::db::DatabaseConnection<Connection>);
    static_assert(storm::db::CachedDatabaseConnection<Connection>);
    static_assert(storm::db::DatabaseStatement<Statement>);
    static_assert(storm::db::DatabaseError<Error>);

    // Dialect-support concepts (#477). SQLite uses its own dialect (no
    // uses_pg_dialect), declares supports_limit_all, and is transaction-capable.
    static_assert(!storm::db::SupportsPgDialect<Connection>);
    static_assert(storm::db::SupportsLimitAll<Connection>);
    static_assert(storm::db::TransactionCapable<Connection>);

} // namespace storm::db::sqlite
