module;

#include <libpq-fe.h>

export module storm_db_postgresql_connection;

import std;
import storm_db_concept;
import storm_db_postgresql_error;
import storm_db_postgresql_statement;

export namespace storm::db::postgresql {

    // Custom deleter for PGconn
    struct PGconnDeleter {
        auto operator()(PGconn* conn) const noexcept -> void {
            if (conn != nullptr) {
                PQfinish(conn);
            }
        }
    };

    class Connection {
        using PGconnPtr = std::unique_ptr<PGconn, PGconnDeleter>;

        // Cache size constants
        static constexpr std::size_t STMT_CACHE_RESERVE = 32;

      public:
        using Error     = postgresql::Error;
        using Statement = postgresql::Statement;

        // Dialect traits
        static constexpr bool supports_limit_all = true;
        static constexpr bool supports_returning = true;
        static constexpr bool uses_pg_dialect    = true;

        // Issue #273: per-Connection cache configuration (capacity 0 = unbounded).
        using Config = storm::db::StatementCacheConfig;

        // Pre-populate statement cache with common operations (stub for PostgreSQL)
        auto prepare_common_statements() -> void {
            // PostgreSQL doesn't need pre-populated common statements
            // Transaction management uses direct PQexec
        }

        // Factory method with error handling
        [[nodiscard]] static auto open(std::string_view conninfo, Config config = {})
                -> std::expected<Connection, Error> {
            const std::string conninfo_str(conninfo); // Ensure null-termination
            PGconn*           raw_conn = PQconnectdb(conninfo_str.c_str());

            if (raw_conn == nullptr) {
                return std::unexpected(Error{-1, "Failed to allocate PGconn"});
            }

            if (PQstatus(raw_conn) != CONNECTION_OK) {
                const auto        status = static_cast<int>(PQstatus(raw_conn));
                const std::string msg    = PQerrorMessage(raw_conn);
                PQfinish(raw_conn);
                return std::unexpected(Error{status, msg});
            }

            return Connection{PGconnPtr{raw_conn}, config};
        }

        // Destructor - unique_ptr handles cleanup via PGconnDeleter
        ~Connection() = default;

        // Move semantics. cache_mutex_ is a MovableSharedMutex (Issue #271) so
        // these can stay defaulted: a moved Connection gets a fresh mutex.
        Connection(Connection&&)                    = default;
        auto operator=(Connection&&) -> Connection& = default;

        // Delete copy operations
        Connection(const Connection&)                    = delete;
        auto operator=(const Connection&) -> Connection& = delete;

        // Connection state
        [[nodiscard]] constexpr auto is_open() const noexcept -> bool {
            return static_cast<bool>(conn_);
        }

        // Prepare a statement - translates ? placeholders to $1, $2, ...
        [[nodiscard]] auto prepare(std::string_view sql) -> std::expected<Statement, Error> {
            auto stmt_name = prepare_pg_statement(sql);
            if (!stmt_name) {
                return std::unexpected(stmt_name.error());
            }
            return Statement{conn_.get(), std::move(*stmt_name)};
        }

        // Prepare with caching - reuses prepared statements for identical SQL.
        // Lock discipline lives in storm_db_concept's cache_* helpers (Issue #271,
        // shared with the SQLite backend); only the backend-specific prepare step
        // (prepare_pg_statement + set_original_sql) differs. On a miss the lock is
        // dropped before PQprepare so the expensive syscall runs uncontended.
        // LIFETIME: the returned Statement* is a non-owning view valid only until the
        // next prepare_cached() on this Connection — at capacity that call may evict
        // the entry it points to (#728). Finish with it before preparing another.
        [[nodiscard]] auto prepare_cached(std::string_view sql) -> std::expected<Statement*, Error> {
            if (!is_open()) {
                return std::unexpected(Error{-1, "Connection not open"});
            }

            if (auto* hit = storm::db::cache_find_hit(cache_, sql)) [[likely]] {
                return hit;
            }

            auto stmt_name = prepare_pg_statement(sql);
            if (!stmt_name) {
                return std::unexpected(stmt_name.error());
            }
            auto new_stmt = std::make_unique<Statement>(conn_.get(), std::move(*stmt_name));
            new_stmt->set_original_sql(std::string(sql)); // Store original SQL for expanded_sql()
            return storm::db::cache_try_insert(cache_, sql, std::move(new_stmt));
        }

        // Clear the entire statement cache (useful for memory management or after
        // major schema changes). Callers must not retain `Statement*` obtained from
        // a prior prepare_cached() across this call — the entries are destroyed.
        auto clear_statement_cache() noexcept -> void {
            storm::db::cache_clear_all(cache_); // Issue #271
        }

        // Issue #215: drop cached entries whose SQL references the given table.
        // Word-boundary aware so clearing "persons" does NOT touch
        // "person_addresses" or "persons_archive".
        auto clear_statement_cache(std::string_view table) -> void {
            storm::db::cache_clear_table(cache_, table); // Issue #271
        }

        [[nodiscard]] auto cached_statement_count() const noexcept -> std::size_t {
            return storm::db::cache_count(cache_); // Issue #271
        }

        // Issue #273: snapshot of hit/miss/eviction counters + current size.
        [[nodiscard]] auto cache_stats() const noexcept -> storm::db::CacheStats {
            return storm::db::cache_stats(cache_);
        }

        // Execute SQL directly (simple queries without parameters)
        [[nodiscard]] auto execute(std::string_view sql) -> std::expected<void, Error> {
            if (!is_open()) {
                return std::unexpected(Error{-1, "Connection not open"});
            }

            const std::string sql_str(sql); // Ensure null-termination
            PGresult*         res = PQexec(conn_.get(), sql_str.c_str());

            if (res == nullptr) {
                return std::unexpected(Error{-1, PQerrorMessage(conn_.get())});
            }

            const ExecStatusType status = PQresultStatus(res);
            if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
                const std::string msg = PQerrorMessage(conn_.get());
                PQclear(res);
                return std::unexpected(Error{static_cast<int>(status), msg});
            }

            PQclear(res);
            return {};
        }

        [[nodiscard]] auto get() const noexcept -> PGconn* {
            return conn_.get();
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
        explicit Connection(PGconnPtr conn_ptr, Config config) : conn_(std::move(conn_ptr)) {
            cache_.capacity = config.statement_cache_capacity; // Issue #273
            cache_.map.reserve(STMT_CACHE_RESERVE);
        }

        // A `?` opening one of these literal/comment regions is copied verbatim
        // until the region's terminator, so any `?` inside is never rewritten to
        // a $N placeholder. Each skipper starts at the opener and returns the
        // index just past the terminator. See translate_placeholders (#418).

        // Single- or double-quoted string starting at sql[pos] (a ' or "). Inside
        // a single-quoted string a backslash escapes the next char (covers the
        // E'...' escape-string form, e.g. E'it\'s'); double-quoted identifiers do
        // not. Copies through the closing quote.
        static auto skip_quoted(std::string_view sql, std::size_t pos, std::string& out) -> std::size_t {
            const char quote = sql[pos];
            out += sql[pos++];
            while (pos < sql.size()) {
                const char ch = sql[pos];
                out += ch;
                ++pos;
                if (quote == '\'' && ch == '\\' && pos < sql.size()) {
                    out += sql[pos++]; // escaped char — never a terminator
                } else if (ch == quote) {
                    break;
                }
            }
            return pos;
        }

        // Dollar-quoted body starting at sql[pos] (a '$'). Reads the tag up to the
        // next '$' ($$ or $name$), then copies through the matching closing tag.
        // If no closing '$' forms a valid tag, treats '$' as an ordinary char.
        static auto skip_dollar_quote(std::string_view sql, std::size_t pos, std::string& out) -> std::size_t {
            const std::size_t tag_close = sql.find('$', pos + 1);
            if (tag_close == std::string_view::npos) {
                out += sql[pos];
                return pos + 1;
            }
            const std::string_view tag = sql.substr(pos, tag_close - pos + 1); // includes both $
            const std::size_t      end = sql.find(tag, tag_close + 1);
            if (end == std::string_view::npos) {
                out += sql[pos];
                return pos + 1; // unterminated — treat opener as a plain char
            }
            out += sql.substr(pos, end - pos + tag.size());
            return end + tag.size();
        }

        // Line comment (-- ...) starting at sql[pos]. Copies through end of line.
        static auto skip_line_comment(std::string_view sql, std::size_t pos, std::string& out) -> std::size_t {
            const std::size_t newline = sql.find('\n', pos);
            const std::size_t end     = (newline == std::string_view::npos) ? sql.size() : newline + 1;
            out += sql.substr(pos, end - pos);
            return end;
        }

        // C-style block comment starting at sql[pos] (the slash-star opener).
        // PostgreSQL block comments nest, so track depth across nested openers
        // and closers and only stop when depth returns to zero.
        static auto skip_block_comment(std::string_view sql, std::size_t pos, std::string& out) -> std::size_t {
            int depth = 0;
            while (pos < sql.size()) {
                if (sql.substr(pos, 2) == "/*") {
                    ++depth;
                    out += "/*";
                    pos += 2;
                } else if (sql.substr(pos, 2) == "*/") {
                    --depth;
                    out += "*/";
                    pos += 2;
                    if (depth == 0) {
                        break;
                    }
                } else {
                    out += sql[pos++];
                }
            }
            return pos;
        }

        // Translate ? placeholders to $1, $2, ... for PostgreSQL.
        //
        // Supported input contract (#418): general SQL. `?` is rewritten to a $N
        // placeholder only when it is NOT inside a single-/double-quoted string,
        // an E'...' escape string, a $tag$ dollar-quoted body, or a SQL comment
        // (line comments and C-style block comments) — those regions are copied
        // verbatim by the skip_* helpers above. Storm itself only emits plain `?`
        // SQL today; the richer handling is here for future raw-SQL / UDF (#89) /
        // FTS (#208) paths.
        [[nodiscard]] static auto translate_placeholders(std::string_view sql) -> std::string {
            std::string result;
            result.reserve(sql.size() + 16); // Extra space for $N expansions
            int         param_index = 0;
            std::size_t pos         = 0;

            while (pos < sql.size()) {
                const char ch = sql[pos];
                if (ch == '\'' || ch == '"') {
                    pos = skip_quoted(sql, pos, result);
                } else if (ch == '$') {
                    pos = skip_dollar_quote(sql, pos, result);
                } else if (sql.substr(pos, 2) == "--") {
                    pos = skip_line_comment(sql, pos, result);
                } else if (sql.substr(pos, 2) == "/*") {
                    pos = skip_block_comment(sql, pos, result);
                } else if (ch == '?') {
                    ++param_index;
                    result += '$';
                    result += std::to_string(param_index);
                    ++pos;
                } else {
                    result += ch;
                    ++pos;
                }
            }

            return result;
        }

        // Generate unique prepared statement names
        [[nodiscard]] auto next_stmt_name() -> std::string {
            return std::format("_storm_{}", stmt_counter_++);
        }

        // Shared body of prepare() and prepare_cached(): translates placeholders,
        // calls PQprepare, returns the generated statement name on success.
        [[nodiscard]] auto prepare_pg_statement(std::string_view sql) -> std::expected<std::string, Error> {
            if (!is_open()) {
                return std::unexpected(Error{-1, "Connection not open"});
            }
            const std::string pg_sql    = translate_placeholders(sql);
            std::string       stmt_name = next_stmt_name();
            PGresult*         res       = PQprepare(conn_.get(), stmt_name.c_str(), pg_sql.c_str(), 0, nullptr);
            if (res == nullptr || PQresultStatus(res) != PGRES_COMMAND_OK) {
                const std::string msg = PQerrorMessage(conn_.get());
                if (res != nullptr) {
                    PQclear(res);
                }
                return std::unexpected(Error{-1, msg});
            }
            PQclear(res);
            return stmt_name;
        }

        PGconnPtr conn_;
        // Issue #271/#273: L3 cache + mutex + stat counters + capacity in one
        // movable bundle. mutable so const accessors take a shared_lock.
        mutable storm::db::StatementCacheState<Statement> cache_;
        int                                               stmt_counter_ = 0;
        // Transaction-nesting depth (#415). 0 = autocommit; >0 = inside a guard.
        int txn_depth_ = 0;
    };

    // Verify concepts are satisfied
    static_assert(storm::db::DatabaseConnection<Connection>);
    static_assert(storm::db::CachedDatabaseConnection<Connection>);
    static_assert(storm::db::DatabaseStatement<Statement>);
    static_assert(storm::db::DatabaseError<Error>);

    // Dialect-support concepts (#477). PostgreSQL uses the PG dialect, declares
    // supports_limit_all, and is transaction-capable.
    static_assert(storm::db::SupportsPgDialect<Connection>);
    static_assert(storm::db::SupportsLimitAll<Connection>);
    static_assert(storm::db::TransactionCapable<Connection>);

} // namespace storm::db::postgresql
