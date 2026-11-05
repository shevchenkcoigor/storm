module;

#include <sqlite3.h>

export module storm_db_sqlite_statement;

import std;
import storm_db_concept;

// Split out of storm_db_sqlite (#760): the sqlite3_* API policy and the RAII
// statement wrapper, so the connection module stays focused and under the
// file-size ceiling.
export namespace storm::db::sqlite {

    // Error type — shared across backends, defined in storm_db_concept (issue #316).
    using Error = storm::db::Error;

    // Issue #760: compile-time API policy for the fallible sqlite3_* calls. Every
    // member is a static forwarder, so BasicStatement<RealApi>/BasicConnection<RealApi>
    // compile to the same direct sqlite3_* calls as before (no virtual, no
    // std::function). A test substitutes a fake policy to inject SQLITE_NOMEM,
    // I/O errors etc. without LD_PRELOAD. Calls that cannot meaningfully fail
    // (column_*, errmsg, finalize, close) stay direct.
    // The hot members (step, bind_*) are `template <typename = void>` on purpose: a
    // non-template function defined in a named module is not inlined into importing
    // TUs even with always_inline (clang only imports bodies of always_inline
    // template instantiations), which would add a call per bind/step (#760 review).
    // `Connection` is the only production spelling; BasicConnection<Api> exists so
    // tests can substitute a fake Api.
    struct RealApi {
        static auto open_v2(const char* path, sqlite3** db, int flags, const char* vfs) noexcept -> int {
            return sqlite3_open_v2(path, db, flags, vfs);
        }
        static auto busy_timeout(sqlite3* db, int timeout_ms) noexcept -> int {
            return sqlite3_busy_timeout(db, timeout_ms);
        }
        static auto exec(sqlite3* db, const char* sql, char** errmsg) noexcept -> int {
            return sqlite3_exec(db, sql, nullptr, nullptr, errmsg);
        }
        static auto prepare_v2(sqlite3* db, const char* sql, int len, sqlite3_stmt** out) noexcept -> int {
            return sqlite3_prepare_v2(db, sql, len, out, nullptr);
        }
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) static auto step(sqlite3_stmt* stmt) noexcept -> int {
            return sqlite3_step(stmt);
        }
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_int(sqlite3_stmt* stmt, int idx, int value) noexcept -> int {
            return sqlite3_bind_int(stmt, idx, value);
        }
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_int64(sqlite3_stmt* stmt, int idx, std::int64_t value) noexcept -> int {
            return sqlite3_bind_int64(stmt, idx, value);
        }
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_double(sqlite3_stmt* stmt, int idx, double value) noexcept -> int {
            return sqlite3_bind_double(stmt, idx, value);
        }
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_text(sqlite3_stmt* stmt, int idx, std::string_view value) noexcept -> int {
            return sqlite3_bind_text(stmt, idx, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        }
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) static auto bind_null(sqlite3_stmt* stmt, int idx) noexcept
                -> int {
            return sqlite3_bind_null(stmt, idx);
        }
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) static auto bind_blob(
                sqlite3_stmt* stmt, int idx, const void* data, std::size_t size
        ) noexcept // NOSONAR(cpp:S5008) - SQLite BLOB API
                -> int {
            return sqlite3_bind_blob(stmt, idx, data, static_cast<int>(size), SQLITE_TRANSIENT);
        }
        static auto expanded_sql(sqlite3_stmt* stmt) noexcept -> char* {
            return sqlite3_expanded_sql(stmt);
        }
    };

    // RAII wrapper for sqlite3_stmt
    template <typename Api> class BasicStatement {
        using StmtPtr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

      public:
        using Error = sqlite::Error;

        // OPTIMIZATION: Cache raw pointer to eliminate unique_ptr::get() overhead in hot loops
        explicit BasicStatement(sqlite3_stmt* stmt) : stmt_(stmt, sqlite3_finalize), raw_(stmt) {}

        // Destructor - unique_ptr handles cleanup via sqlite3_finalize
        ~BasicStatement() = default;

        // Move semantics — explicit so the moved-from source nulls its cached
        // raw_. A defaulted move copies raw_ while stmt_ goes null, leaving the
        // source aliasing the statement now owned by the destination (issue
        // #354). Keep the cached raw_ for hot-loop performance (CLAUDE.md).
        BasicStatement(BasicStatement&& other) noexcept : stmt_(std::move(other.stmt_)), raw_(other.raw_) {
            other.raw_ = nullptr;
        }

        auto operator=(BasicStatement&& other) noexcept -> BasicStatement& {
            if (this != &other) {
                stmt_      = std::move(other.stmt_);
                raw_       = other.raw_;
                other.raw_ = nullptr;
            }
            return *this;
        }

        // Delete copy operations
        BasicStatement(const BasicStatement&)                    = delete;
        auto operator=(const BasicStatement&) -> BasicStatement& = delete;

        // DatabaseStatement concept implementation
        // All methods use template pattern for cross-module inlining
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_int(int index, int value) noexcept
                -> std::expected<void, Error> {
            const int rc = Api::bind_int(raw_, index, value);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind integer parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_text(int index, std::string_view value) noexcept
                -> std::expected<void, Error> {
            const int rc = Api::bind_text(raw_, index, value);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind text parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_int64(int index, std::int64_t value) noexcept
                -> std::expected<void, Error> {
            const int rc = Api::bind_int64(raw_, index, value);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind int64 parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_double(int index, double value) noexcept
                -> std::expected<void, Error> {
            const int rc = Api::bind_double(raw_, index, value);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind double parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_null(int index) noexcept -> std::expected<void, Error> {
            const int rc = Api::bind_null(raw_, index);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind null parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto
        bind_blob(int index, const void* data, std::size_t size) noexcept // NOSONAR(cpp:S5008) - SQLite BLOB API
                -> std::expected<void, Error> {
            const int rc = Api::bind_blob(raw_, index, data, size);
            if (rc != SQLITE_OK) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to bind blob parameter"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto execute() noexcept -> std::expected<void, Error> {
            const int rc = Api::step(raw_);
            if (rc != SQLITE_DONE) [[unlikely]] {
                return std::unexpected(Error{rc, "Failed to execute statement"});
            }
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto step() noexcept -> std::expected<bool, Error> {
            const int rc = Api::step(raw_);
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
            return Api::step(raw_);
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
            char* expanded = Api::expanded_sql(raw_);
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

} // namespace storm::db::sqlite
