module;

export module storm_db_concept;

import std;

export namespace storm::db {

    // Error type for database operations.
    // Non-allocating: stores the message inline so construction is noexcept and Error can
    // be returned from noexcept catch blocks without risking bad_alloc (issue #316).
    struct Error {
        static constexpr std::size_t kMaxMessageLen = 255;

        int                                  code_{0};
        std::array<char, kMaxMessageLen + 1> message_buf_{};
        std::size_t                          message_len_{0};

        Error() noexcept = default;

        Error(int code, std::string_view msg) noexcept : code_{code} {
            const std::size_t n = msg.size() < kMaxMessageLen ? msg.size() : kMaxMessageLen;
            if (n > 0) {
                std::memcpy(message_buf_.data(), msg.data(), n);
            }
            message_buf_[n] = '\0';
            message_len_    = n;
        }

        [[nodiscard]] constexpr auto code() const noexcept -> int {
            return code_;
        }
        [[nodiscard]] constexpr auto message() const noexcept -> std::string_view {
            return std::string_view{message_buf_.data(), message_len_};
        }
    };

    // Forward declarations for concept requirements
    template <typename T> struct ConnectionTraits;

    template <typename T> struct StatementTraits;

    // Database connection concept
    template <typename T>
    concept DatabaseConnection = requires(T& conn, std::string_view sql) {
        // Type aliases required
        typename T::Error;
        typename T::Statement;

        // Connection state
        { conn.is_open() } -> std::same_as<bool>;

        // Statement preparation
        { conn.prepare(sql) } -> std::same_as<std::expected<typename T::Statement, typename T::Error>>;

        // SQL execution (simple queries)
        { conn.execute(sql) } -> std::same_as<std::expected<void, typename T::Error>>;
    };

    // Issue #273: per-Connection statement-cache configuration, shared by every
    // backend's open(). capacity 0 = unbounded (preserves pre-#273 behavior).
    struct StatementCacheConfig {
        std::size_t statement_cache_capacity = 512;
    };

    // Issue #410: SQLite journal mode. Lives here (backend-neutral) so the generic
    // PoolConfig can carry it without depending on the SQLite backend module.
    // Default leaves the engine default (rollback journal); WAL opts into
    // write-ahead logging. Honoured only by the SQLite backend; PG ignores it.
    enum class JournalMode : std::uint8_t { Default, WAL };

    // Snapshot of per-Connection cache statistics (Issue #273). Counters are
    // lifetime totals (not reset by clear_statement_cache); current_size is the
    // live entry count. Declared before CachedDatabaseConnection, which requires
    // cache_stats() to return it.
    struct CacheStats {
        std::uint64_t hits         = 0;
        std::uint64_t misses       = 0;
        std::uint64_t evictions    = 0;
        std::size_t   current_size = 0;
    };

    // Extended database connection concept with caching support
    template <typename T>
    concept CachedDatabaseConnection = DatabaseConnection<T> && requires(T& conn, std::string_view sql) {
        // Cached statement preparation
        { conn.prepare_cached(sql) } -> std::same_as<std::expected<typename T::Statement*, typename T::Error>>;

        // Cache management — clear-all and per-table (Issue #215)
        { conn.clear_statement_cache() } -> std::same_as<void>;
        { conn.clear_statement_cache(sql) } -> std::same_as<void>;
        { conn.cached_statement_count() } -> std::same_as<std::size_t>;

        // Cache statistics snapshot (Issue #273)
        { conn.cache_stats() } -> std::same_as<CacheStats>;
    };

    // ── Dialect-support concepts (Issue #477) ────────────────────────────────
    // Named gates for backend-dialect capability checks that were previously
    // done ad-hoc via anonymous `if constexpr (requires { ConnType::trait; })`
    // probes. Each models PRESENCE of a static trait, not its truth.

    // A connection whose backend uses the PostgreSQL SQL dialect. PG declares
    // `uses_pg_dialect`; SQLite does not — so this concept IS the dialect switch
    // (true for PG, false for SQLite), used to pick PG-specific SQL (NULLS
    // FIRST/LAST ordering, LIMIT ALL, the schema-generation Dialect enum).
    template <typename ConnType>
    concept SupportsPgDialect = requires {
        { ConnType::uses_pg_dialect } -> std::convertible_to<bool>;
    };

    // A connection that declares how it spells "unlimited rows" after OFFSET:
    // PG uses `LIMIT ALL`, SQLite uses `LIMIT -1`. Both backends declare the
    // trait, so this concept is an EXISTENCE probe — call sites still read the
    // bool VALUE to choose the spelling; a backend that declares neither falls
    // back to the SQLite-compatible form.
    template <typename ConnType>
    concept SupportsLimitAll = requires {
        { ConnType::supports_limit_all } -> std::convertible_to<bool>;
    };

    // A connection that can run an RAII transaction (storm::begin /
    // TransactionGuard, #415). Captures exactly the surface TransactionGuard
    // calls, so a mis-typed connection fails at the begin() call site with a
    // clear constraint violation instead of deep inside the guard body.
    template <typename ConnType>
    concept TransactionCapable = requires(ConnType& conn, std::string_view sql) {
        typename ConnType::Error;
        { conn.in_transaction() } -> std::convertible_to<bool>;
        conn.enter_transaction();
        conn.leave_transaction();
        { conn.execute(sql) } -> std::same_as<std::expected<void, typename ConnType::Error>>;
    };

    // Database statement concept.
    // Issue #206: covers the full bind/extract/control surface actually used by
    // orm/value_bind.cppm and orm/statements/extract.cppm, not just the minimal
    // insert-and-step path — so a non-compliant backend fails here, at the
    // static_assert boundary in sqlite.cppm / postgresql_connection.cppm, with a
    // clear concept violation instead of deep inside the binder/extractor.
    template <typename T>
    concept DatabaseStatement = requires(
            T&               stmt,
            int              int_val,
            std::int64_t     int64_val,
            double           double_val,
            std::string_view str_val,
            const void*      blob_ptr
    ) {
        // Type alias required
        typename T::Error;

        // Parameter binding (int_val doubles as the 1-based bind index)
        { stmt.bind_int(int_val, int_val) } -> std::same_as<std::expected<void, typename T::Error>>;
        { stmt.bind_text(int_val, str_val) } -> std::same_as<std::expected<void, typename T::Error>>;
        { stmt.bind_int64(int_val, int64_val) } -> std::same_as<std::expected<void, typename T::Error>>;
        { stmt.bind_double(int_val, double_val) } -> std::same_as<std::expected<void, typename T::Error>>;
        { stmt.bind_null(int_val) } -> std::same_as<std::expected<void, typename T::Error>>;
        {
            stmt.bind_blob(int_val, blob_ptr, static_cast<std::size_t>(int_val))
        } -> std::same_as<std::expected<void, typename T::Error>>;

        // Execution
        { stmt.execute() } -> std::same_as<std::expected<void, typename T::Error>>;
        { stmt.step() } -> std::same_as<std::expected<bool, typename T::Error>>; // true if row available
        { stmt.step_raw() } -> std::convertible_to<int>; // no std::expected overhead in hot loop

        // Statement management
        { stmt.reset() } -> std::same_as<void>;
        { stmt.finalize() } -> std::same_as<void>;

        // Column extraction (result-row reading; int_val doubles as the column index)
        { stmt.extract_int(int_val) } -> std::convertible_to<int>;
        { stmt.extract_int64(int_val) } -> std::convertible_to<std::int64_t>;
        { stmt.extract_double(int_val) } -> std::convertible_to<double>;
        { stmt.extract_float(int_val) } -> std::convertible_to<float>;
        { stmt.extract_bool(int_val) } -> std::convertible_to<bool>;
        { stmt.extract_text_ptr(int_val) } -> std::convertible_to<const unsigned char*>;
        { stmt.extract_bytes(int_val) } -> std::convertible_to<int>;
        { stmt.extract_blob_ptr(int_val) } -> std::convertible_to<const void*>;
        { stmt.is_null(int_val) } -> std::convertible_to<bool>;

        // Error reporting
        { stmt.get_error_message() } -> std::convertible_to<const char*>;

        // Step-return-code constants (checked at the boundary via step_raw())
        { T::ROW_AVAILABLE } -> std::convertible_to<int>;
        { T::NO_MORE_ROWS } -> std::convertible_to<int>;
    };

    // Database error concept
    template <typename T>
    concept DatabaseError = requires(const T& err) {
        { err.code() } -> std::integral;
        { err.message() } -> std::convertible_to<std::string_view>;
    };

    // Helper traits for type deduction
    template <DatabaseConnection T> struct ConnectionTraits<T> {
        using Error     = typename T::Error;
        using Statement = typename T::Statement;
    };

    template <DatabaseStatement T> struct StatementTraits<T> {
        using Error = typename T::Error;
    };

    // Transparent hash + equal for heterogeneous string_view lookup in caches.
    // Shared by every backend's StatementCache (sqlite.cppm, postgresql.cppm).
    struct string_hash {
        using is_transparent = void;
        using hash_type      = std::hash<std::string_view>;

        [[nodiscard]] auto operator()(std::string_view str) const noexcept -> std::size_t {
            return hash_type{}(str);
        }
        [[nodiscard]] auto operator()(const std::string& str) const noexcept -> std::size_t {
            return hash_type{}(str);
        }
    };

    struct string_equal {
        using is_transparent = void;

        [[nodiscard]] auto operator()(std::string_view lhs, std::string_view rhs) const noexcept -> bool {
            return lhs == rhs;
        }
    };

    // Issue #271: std::shared_mutex is neither movable nor copyable. Wrapping it
    // lets a Connection holding one keep its defaulted move operations: moving a
    // Connection just leaves each object with its own freshly-constructed mutex
    // (a Connection is only moved during construction, before any thread shares
    // it, so the source mutex is never contended at move time). The lock methods
    // delegate to the wrapped mutex.
    class MovableSharedMutex {
      public:
        MovableSharedMutex()  = default;
        ~MovableSharedMutex() = default;

        // Move = no-op: a moved-into Connection starts with a fresh, unlocked mutex.
        MovableSharedMutex(MovableSharedMutex&& /*other*/) noexcept {}
        auto operator=(MovableSharedMutex&& /*other*/) noexcept -> MovableSharedMutex& {
            return *this;
        }

        MovableSharedMutex(const MovableSharedMutex&)                    = delete;
        auto operator=(const MovableSharedMutex&) -> MovableSharedMutex& = delete;

        auto lock() -> void {
            mutex_.lock();
        }
        auto unlock() -> void {
            mutex_.unlock();
        }
        auto lock_shared() -> void {
            mutex_.lock_shared();
        }
        auto unlock_shared() -> void {
            mutex_.unlock_shared();
        }

      private:
        std::shared_mutex mutex_;
    };

    // Issue #273: CLOCK/second-chance LRU eviction + statistics on the single L3
    // statement cache. Each entry carries a reference bit set on a cache hit;
    // eviction sweeps the entries, clearing set bits (second chance) and evicting
    // the first unreferenced one. The hit path only flips an atomic bit under the
    // shared_lock — no structural mutation, so the #271 parallel-read hot path is
    // preserved.
    template <typename Stmt> struct CacheEntry {
        std::unique_ptr<Stmt> stmt;
        std::atomic<bool>     referenced{false}; // CLOCK second-chance bit
    };

    // Bundles the L3 cache map, its mutex, the stat counters, and the capacity.
    // Each Connection embeds one; both backends share the cache_* helpers that
    // operate on it. std::atomic is non-movable, so the move ops are explicit:
    // the map moves, the atomics reset to zero, capacity is copied — equivalent
    // to MovableSharedMutex's "fresh on move" contract (a Connection is only moved
    // during construction, before any thread shares it). Issue #273.
    template <typename Stmt> struct StatementCacheState {
        std::unordered_map<std::string, CacheEntry<Stmt>, string_hash, string_equal> map;
        mutable MovableSharedMutex                                                   mutex;
        std::atomic<std::uint64_t>                                                   hits{0};
        std::atomic<std::uint64_t>                                                   misses{0};
        std::atomic<std::uint64_t>                                                   evictions{0};
        std::size_t                                                                  capacity = 0; // 0 = unbounded

        StatementCacheState()  = default;
        ~StatementCacheState() = default;

        StatementCacheState(StatementCacheState&& other) noexcept
            : map(std::move(other.map)), mutex(std::move(other.mutex)), capacity(other.capacity) {}
        auto operator=(StatementCacheState&& other) noexcept -> StatementCacheState& {
            if (this != &other) {
                map      = std::move(other.map);
                mutex    = std::move(other.mutex);
                capacity = other.capacity;
                hits.store(0, std::memory_order_relaxed);
                misses.store(0, std::memory_order_relaxed);
                evictions.store(0, std::memory_order_relaxed);
            }
            return *this;
        }
        StatementCacheState(const StatementCacheState&)                    = delete;
        auto operator=(const StatementCacheState&) -> StatementCacheState& = delete;
    };

    // Identifier-character predicate (SQL word boundary): same set as `\w` in regex
    // ([A-Za-z0-9_]). Used by per-table cache invalidation to avoid clearing
    // "persons" entries when invalidating "person".
    [[nodiscard]] constexpr auto is_sql_ident_char(char c) noexcept -> bool {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    }

    [[nodiscard]] inline auto is_word_boundary_match(std::string_view sql, std::size_t pos, std::size_t len) noexcept
            -> bool {
        const bool left_ok  = pos == 0 || !is_sql_ident_char(sql[pos - 1]);
        const bool right_ok = pos + len == sql.size() || !is_sql_ident_char(sql[pos + len]);
        return left_ok && right_ok;
    }

    // Word-boundary table-name match in a SQL string. Identifier characters on
    // either side of the match disqualify it, so clearing "persons" does not
    // touch "person_addresses" or "persons_archive". Issue #215.
    [[nodiscard]] inline auto sql_references_table(std::string_view sql, std::string_view table) noexcept -> bool {
        if (table.empty() || sql.size() < table.size()) {
            return false;
        }
        for (std::size_t pos = 0; (pos = sql.find(table, pos)) != std::string_view::npos; pos += table.size()) {
            if (is_word_boundary_match(sql, pos, table.size())) {
                return true;
            }
        }
        return false;
    }

    // Issue #271/#273: shared L3-cache helpers. Both backends call cache_find_hit()
    // then cache_try_insert() around their own prepare step; the lock discipline,
    // CLOCK eviction, and statistics are identical across backends and live here.

    // Hot path: shared_lock find + reset. On a hit, sets the entry's CLOCK ref bit
    // and bumps `hits`; on a miss, bumps `misses`. Returns the cached Statement* on
    // a hit, nullptr on a miss (caller then prepares + inserts).
    //
    // LIFETIME INVARIANT (issue #357, finding A): the returned Stmt* outlives the
    // shared_lock released on return. Concurrent access to the cache from multiple
    // threads IS supported and tested — the shared_mutex serialises map structure
    // (#271; see StatementCacheThreadingTest, which pounds one Connection from 8
    // threads under TSAN). The actual contract is narrower than single-owner: the
    // returned Stmt* is valid only WITHIN the iteration that fetched it. A caller
    // must NOT retain it across a concurrent clear_statement_cache() / eviction,
    // which destroys the entry's unique_ptr<Stmt> and dangles the pointer. (The
    // per-thread-connection model trivially satisfies this; the bound is the
    // dereference window, not the number of threads.)
    template <typename Stmt>
    [[nodiscard]] auto cache_find_hit(StatementCacheState<Stmt>& state, std::string_view sql) -> Stmt* {
        std::shared_lock read_lock(state.mutex);
        auto             it = state.map.find(sql);
        if (it != state.map.end()) [[likely]] {
            it->second.referenced.store(true, std::memory_order_relaxed);
            state.hits.fetch_add(1, std::memory_order_relaxed);
            it->second.stmt->reset();
            return it->second.stmt.get();
        }
        state.misses.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    // CLOCK/second-chance eviction. Caller holds the unique_lock. Sweeps entries:
    // an entry whose ref bit is set gets it cleared (second chance) and is skipped;
    // the first entry with a clear bit is evicted. If a full pass clears every bit
    // without finding a victim, the next entry is evicted to guarantee progress.
    // Bumps `evictions`. Bounded to ~2 passes over a map of size <= capacity.
    template <typename Stmt> auto cache_evict_one(StatementCacheState<Stmt>& state) -> void {
        for (auto it = state.map.begin(); it != state.map.end(); ++it) {
            if (it->second.referenced.load(std::memory_order_relaxed)) {
                it->second.referenced.store(false, std::memory_order_relaxed);
                continue;
            }
            state.map.erase(it);
            state.evictions.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // Every bit was set; all are now cleared. Evict the first entry.
        if (!state.map.empty()) {
            state.map.erase(state.map.begin());
            state.evictions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Insert a freshly-prepared statement under a unique_lock. Evicts first if the
    // cache is at capacity (capacity 0 = unbounded). try_emplace keeps any entry a
    // racing thread inserted while we prepared (the lock was dropped during
    // prepare); the passed-in statement is then dropped by the caller. Returns the
    // live Statement* for the key.
    template <typename Stmt>
    [[nodiscard]] auto
    cache_try_insert(StatementCacheState<Stmt>& state, std::string_view sql, std::unique_ptr<Stmt> stmt) -> Stmt* {
        std::unique_lock write_lock(state.mutex);
        if (state.capacity != 0 && state.map.size() >= state.capacity && !state.map.contains(sql)) {
            cache_evict_one(state);
        }
        auto [it, inserted] = state.map.try_emplace(std::string(sql));
        if (inserted) {
            it->second.stmt = std::move(stmt);
        }
        return it->second.stmt.get();
    }

    // Drop every cached entry (unique_lock). Counters are lifetime totals and are
    // NOT reset here.
    template <typename Stmt> auto cache_clear_all(StatementCacheState<Stmt>& state) noexcept -> void {
        std::unique_lock write_lock(state.mutex);
        state.map.clear();
    }

    // Drop cached entries whose SQL references `table`, word-boundary aware
    // (unique_lock). Issue #215.
    template <typename Stmt> auto cache_clear_table(StatementCacheState<Stmt>& state, std::string_view table) -> void {
        std::unique_lock write_lock(state.mutex);
        std::erase_if(state.map, [table](const auto& entry) { return sql_references_table(entry.first, table); });
    }

    // Entry count (shared_lock).
    template <typename Stmt>
    [[nodiscard]] auto cache_count(const StatementCacheState<Stmt>& state) noexcept -> std::size_t {
        std::shared_lock read_lock(state.mutex);
        return state.map.size();
    }

    // Stats snapshot (shared_lock). Relaxed loads — the snapshot is advisory.
    template <typename Stmt>
    [[nodiscard]] auto cache_stats(const StatementCacheState<Stmt>& state) noexcept -> CacheStats {
        std::shared_lock read_lock(state.mutex);
        return CacheStats{
                state.hits.load(std::memory_order_relaxed),
                state.misses.load(std::memory_order_relaxed),
                state.evictions.load(std::memory_order_relaxed),
                state.map.size(),
        };
    }

} // namespace storm::db
