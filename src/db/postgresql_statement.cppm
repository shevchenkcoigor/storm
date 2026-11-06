module;

// `duplicate` removed in #277 Phase 3 (bind_text_value(idx, std::string) helper shared by
// bind_int/bind_int64/bind_text).

#include <libpq-fe.h>

export module storm_db_postgresql_statement;

import std;
import storm_db_postgresql_error;

export namespace storm::db::postgresql {

    // RAII wrapper emulating step-based iteration over PGresult*. `Api` is the libpq call
    // policy (#787): RealApi (storm_db_postgresql_connection) in production, a fake in tests.
    template <typename Api> class BasicStatement { // NOSONAR(cpp:S1448) - RAII + step iteration over PGresult
      public:
        using Error = postgresql::Error;

        // Constants for step return codes
        static constexpr int         ROW_AVAILABLE    = 1;
        static constexpr int         NO_MORE_ROWS     = 0;
        static constexpr std::size_t MAX_DB_VARIABLES = 999; // matches BaseStatement::MAX_DB_VARIABLES

        explicit BasicStatement(PGconn* conn, std::string stmt_name) : conn_(conn), stmt_name_(std::move(stmt_name)) {
            param_values_.reserve(MAX_DB_VARIABLES);
            param_ptrs_.reserve(MAX_DB_VARIABLES);
            param_lengths_.reserve(MAX_DB_VARIABLES);
            param_formats_.reserve(MAX_DB_VARIABLES);
        }

        // Destructor — DEALLOCATEs the server-side prepared statement (issue #417).
        // SQLite finalizes on eviction via its unique_ptr deleter; PG must do the
        // same here or the server-side `_storm_N` statement leaks for the
        // connection's lifetime when the bounded L3 cache evicts this entry.
        // finalize() is best-effort and noexcept-safe, and clears stmt_name_ so a
        // moved-from Statement (whose name was swapped away) does nothing.
        ~BasicStatement() {
            finalize();
        }

        // Move semantics — swap-based to keep the field list in one place.
        // `other` ends up in a default-constructed-equivalent state; its destructor
        // sees result_==nullptr and does nothing.
        BasicStatement(BasicStatement&& other) noexcept {
            swap(*this, other);
        }

        auto operator=(BasicStatement&& other) noexcept -> BasicStatement& {
            if (this != &other) {
                clear_result();
                swap(*this, other);
            }
            return *this;
        }

        friend auto swap(BasicStatement& first, BasicStatement& second) noexcept -> void {
            using std::swap;
            swap(first.conn_, second.conn_);
            swap(first.stmt_name_, second.stmt_name_);
            swap(first.original_sql_, second.original_sql_);
            swap(first.result_, second.result_);
            swap(first.current_row_, second.current_row_);
            swap(first.total_rows_, second.total_rows_);
            swap(first.param_values_, second.param_values_);
            swap(first.param_ptrs_, second.param_ptrs_);
            swap(first.param_lengths_, second.param_lengths_);
            swap(first.param_formats_, second.param_formats_);
            swap(first.param_count_, second.param_count_);
            swap(first.blob_buffer_, second.blob_buffer_);
            swap(first.blob_decoded_size_, second.blob_decoded_size_);
            swap(first.blob_decoded_col_, second.blob_decoded_col_);
            swap(first.binary_results_, second.binary_results_);
        }

        // Delete copy operations
        BasicStatement(const BasicStatement&)                    = delete;
        auto operator=(const BasicStatement&) -> BasicStatement& = delete;

        // Shared core for all text-based bind paths. Propagates OOM as Error rather than
        // throwing through noexcept — see issue #316.
        [[nodiscard]] __attribute__((always_inline)) auto bind_text_value(int index, std::string_view value) noexcept
                -> std::expected<void, Error> {
            try {
                if (auto slot = ensure_param_slot(index); !slot) { // LCOV_EXCL_LINE — defensive OOM, reserved vector
                    return std::unexpected(slot.error());          // LCOV_EXCL_LINE
                } // LCOV_EXCL_LINE
                param_values_[index - 1].assign(value);
                update_param_ptrs(index);
                return {};
            } catch (...) { // LCOV_EXCL_LINE — defensive: assign() bad_alloc unreachable, fixed-size reserved vector
                return std::unexpected(Error{-1, "out of memory binding parameter"}); // LCOV_EXCL_LINE
            } // LCOV_EXCL_LINE
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_int(int index, int value) noexcept
                -> std::expected<void, Error> {
            return bind_int64(index, static_cast<std::int64_t>(value));
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_text(int index, std::string_view value) noexcept
                -> std::expected<void, Error> {
            return bind_text_value(index, value);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_int64(int index, std::int64_t value) noexcept
                -> std::expected<void, Error> {
            std::array<char, 20> buf{}; // max int64: 19 digits + sign (no null needed)
            // to_chars never fails for an int64 into a 20-byte buffer; ptr marks the end.
            const char* ptr = std::to_chars(buf.data(), buf.data() + buf.size(), value).ptr;
            return bind_text_value(index, std::string_view{buf.data(), ptr});
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_double(int index, double value) noexcept
                -> std::expected<void, Error> {
            // snprintf with %.17g for full double precision (17 significant digits)
            std::array<char, 32> buf{};
            std::snprintf(buf.data(), buf.size(), "%.17g", value);
            return bind_text_value(index, std::string_view{buf.data()});
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto bind_null(int index) noexcept -> std::expected<void, Error> {
            // LCOV_EXCL_START — defensive OOM path; param_values_ is reserved to MAX_DB_VARIABLES
            if (auto slot = ensure_param_slot(index); !slot) {
                return std::unexpected(slot.error());
            }
            // LCOV_EXCL_STOP
            param_values_[index - 1].clear();
            param_ptrs_[index - 1]    = nullptr; // NULL parameter
            param_lengths_[index - 1] = 0;
            param_formats_[index - 1] = 0;
            return {};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto
        bind_blob(int index, const void* data, std::size_t size) noexcept // NOSONAR(cpp:S5008) - PostgreSQL BLOB API
                -> std::expected<void, Error> {
            // LCOV_EXCL_START — defensive OOM path; param_values_ is reserved to MAX_DB_VARIABLES
            if (auto slot = ensure_param_slot(index); !slot) {
                return std::unexpected(slot.error());
            }
            // LCOV_EXCL_STOP
            try {
                if (data != nullptr && size > 0) {
                    // Store binary data as-is, use binary format
                    param_values_[index - 1]
                            .assign(static_cast<const char*>(data), size); // NOSONAR(cpp:S5008) - Binary data
                } else {
                    // Empty blob - use empty string (non-NULL, zero-length)
                    param_values_[index - 1].clear();
                }
            } catch (...) { // LCOV_EXCL_LINE — defensive: assign() bad_alloc unreachable, fixed-size reserved vector
                return std::unexpected(Error{-1, "out of memory binding blob"}); // LCOV_EXCL_LINE
            } // LCOV_EXCL_LINE
            param_ptrs_[index - 1]    = param_values_[index - 1].c_str();
            param_lengths_[index - 1] = static_cast<int>(size);
            param_formats_[index - 1] = 1; // Binary format
            return {};
        }

        // Request binary (true) or text (false) RESULT format for the next
        // execute() (#600). Text stays the default: the flag is per STATEMENT, not
        // per column, so only a caller that has proved every selected column is a
        // plain byte reinterpretation may flip it, AFTER prepare_cached() (which
        // reset()s on a cache hit, clearing this) and BEFORE the first step().
        // PG-only — SQLite's column API is already binary, so the ORM finds this
        // via a `requires` probe instead of db::DatabaseStatement growing a knob.
        template <typename = void> __attribute__((always_inline)) auto set_result_binary(bool binary) noexcept -> void {
            binary_results_ = binary;
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto execute() noexcept -> std::expected<void, Error> {
            clear_result();

            // Rebuild param_ptrs_ from param_values_ to ensure pointers are valid
            rebuild_param_ptrs();

            result_ = Api::exec_prepared(
                    conn_,
                    stmt_name_.c_str(),
                    param_count_,
                    param_count_ > 0 ? param_ptrs_.data() : nullptr,
                    param_count_ > 0 ? param_lengths_.data() : nullptr,
                    param_count_ > 0 ? param_formats_.data() : nullptr,
                    binary_results_ ? kBinaryFormat : kTextFormat // #600
            );

            const ExecStatusType status = PQresultStatus(result_);
            if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) [[unlikely]] {
                const char* msg = Api::error_message(conn_);
                clear_result();
                return std::unexpected(Error{static_cast<int>(status), msg});
            }

            total_rows_  = PQntuples(result_);
            current_row_ = -1; // Before first row
            return {};
        }

        // Step through results - returns true if row available, false if done
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto step() noexcept -> std::expected<bool, Error> {
            if (result_ == nullptr) {
                // Auto-execute if not yet executed
                auto exec_result = execute();
                if (!exec_result.has_value()) {
                    return std::unexpected(exec_result.error());
                }
            }

            ++current_row_;
            if (current_row_ < total_rows_) [[likely]] {
                return true; // Row available
            }
            return false;
        }

        // Step with raw return value (no std::expected overhead)
        template <typename = void> [[nodiscard]] __attribute__((always_inline)) auto step_raw() noexcept -> int {
            if (result_ == nullptr) {
                auto exec_result = execute();
                if (!exec_result.has_value()) {
                    return -1; // Error
                }
            }

            ++current_row_;
            if (current_row_ < total_rows_) [[likely]] {
                return ROW_AVAILABLE;
            }
            return NO_MORE_ROWS;
        }

        // Reset statement for reuse - clear params and result.
        // Also drops back to text result format (#600): a cached statement is reset
        // on every cache hit, so the next caller starts from the safe default.
        template <typename = void> __attribute__((always_inline)) auto reset() noexcept -> void {
            clear_result();
            clear_params();
            binary_results_ = false;
        }

        template <typename = void> __attribute__((always_inline)) auto finalize() noexcept -> void {
            if (conn_ != nullptr && !stmt_name_.empty()) {
                // PostgreSQL identifiers are bounded by NAMEDATALEN (63 chars + NUL by spec),
                // so a stack buffer for "DEALLOCATE <name>" can never overflow and never throws.
                std::array<char, 11 + 64 + 1> buf{};
                std::snprintf(buf.data(), buf.size(), "DEALLOCATE %s", stmt_name_.c_str());
                PGresult* res = Api::exec(conn_, buf.data());
                if (res != nullptr) {
                    PQclear(res);
                }
                stmt_name_.clear(); // std::string::clear is noexcept
            }
            clear_result();
        }

        // Access raw handle (PGresult) for advanced operations
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto handle() const noexcept -> PGresult* {
            return result_;
        }

        // Append a quoted parameter value to result, escaping single quotes
        auto append_quoted_param(std::string& result, std::size_t idx) const -> void {
            if (param_ptrs_[idx] == nullptr) {
                result += "NULL";
                return;
            }
            result += '\'';
            for (const char c : param_values_[idx]) {
                if (c == '\'') {
                    result += "''";
                } else {
                    result += c;
                }
            }
            result += '\'';
        }

        // Returns SQL string with all bound parameters inlined (for debugging / SQL inspection)
        // Substitutes ? placeholders with quoted param_values_ strings
        template <typename = void> [[nodiscard]] auto expanded_sql() const -> std::string {
            std::string result;
            result.reserve(original_sql_.size() + (static_cast<std::size_t>(param_count_) * 8));
            int  param_idx       = 0;
            bool in_single_quote = false;
            bool in_double_quote = false;
            for (const char ch : original_sql_) {
                if (ch == '\'' && !in_double_quote) {
                    in_single_quote = !in_single_quote;
                    result += ch;
                } else if (ch == '"' && !in_single_quote) {
                    in_double_quote = !in_double_quote;
                    result += ch;
                } else if (ch == '?' && !in_single_quote && !in_double_quote) {
                    if (param_idx < param_count_) {
                        append_quoted_param(result, static_cast<std::size_t>(param_idx));
                        ++param_idx;
                    } else {
                        result += ch;
                    }
                } else {
                    result += ch;
                }
            }
            return result;
        }

        // Store original SQL (with ? placeholders) for expanded_sql()
        auto set_original_sql(std::string sql) -> void {
            original_sql_ = std::move(sql);
        }

        // Column extraction methods
        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_int(int col_index) const noexcept -> int {
            if (binary_results_) {
                return static_cast<int>(decode_binary_int(col_index));
            }
            const char* val = PQgetvalue(result_, current_row_, col_index);
            return static_cast<int>(std::strtol(val, nullptr, 10)); // NOSONAR - known-valid DB integer string
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_int64(int col_index) const noexcept -> std::int64_t {
            if (binary_results_) {
                return decode_binary_int(col_index);
            }
            const char* val = PQgetvalue(result_, current_row_, col_index);
            return std::strtoll(val, nullptr, 10);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_double(int col_index) const noexcept -> double {
            if (binary_results_) {
                return decode_binary_floating(col_index);
            }
            const char* val = PQgetvalue(result_, current_row_, col_index);
            return std::strtod(val, nullptr);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_text_ptr(int col_index) const noexcept -> const
                unsigned char* {
            return reinterpret_cast<const unsigned char*>(PQgetvalue(result_, current_row_, col_index));
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_bytes(int col_index) noexcept -> int {
            // If we just decoded a blob for this column, return decoded size
            if (blob_decoded_col_ == col_index) {
                return static_cast<int>(blob_decoded_size_);
            }
            return PQgetlength(result_, current_row_, col_index);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_text_view(int col_index) const noexcept
                -> std::string_view {
            if (PQgetisnull(result_, current_row_, col_index) != 0) {
                return {};
            }
            const char* text = PQgetvalue(result_, current_row_, col_index);
            const auto  len  = static_cast<std::size_t>(PQgetlength(result_, current_row_, col_index));
            return {text, len};
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_bool(int col_index) const noexcept -> bool {
            const char* val = PQgetvalue(result_, current_row_, col_index);
            if (binary_results_) {
                // One byte, 0x00/0x01. libpq NUL-terminates every value, so a
                // zero-length NULL reads '\0' → false rather than off the end.
                return val[0] != 0;
            }
            // PostgreSQL returns 't'/'f' for boolean, or '1'/'0'
            return val[0] == 't' || val[0] == '1';
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_float(int col_index) const noexcept -> float {
            if (binary_results_) {
                return static_cast<float>(decode_binary_floating(col_index));
            }
            const char* val = PQgetvalue(result_, current_row_, col_index);
            return std::strtof(val, nullptr);
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto extract_blob_ptr(int col_index) noexcept -> const
                void* { // NOSONAR(cpp:S5008) - PostgreSQL BLOB API uses void*
            blob_decoded_col_ = col_index;
            if (PQgetisnull(result_, current_row_, col_index) != 0) {
                blob_decoded_size_ = 0;
                return nullptr;
            }
            if (binary_results_) {
                // BYTEA's binary form IS the raw bytes: no "\x" envelope, no copy.
                // Clearing blob_decoded_col_ routes extract_bytes() to PQgetlength,
                // already the exact byte count of what is returned here.
                blob_decoded_col_  = -1;
                blob_decoded_size_ = 0;
                return PQgetvalue(result_, current_row_, col_index);
            }
            const char* hex_str = PQgetvalue(result_, current_row_, col_index);
            const int   hex_len = PQgetlength(result_, current_row_, col_index);
            try {
                // PG hex format starts with "\x" prefix
                if (hex_len >= 2 && hex_str[0] == '\\' && hex_str[1] == 'x') {
                    const int binary_len = (hex_len - 2) / 2;
                    blob_buffer_.resize(static_cast<std::size_t>(binary_len));
                    for (int i = 0; i < binary_len;
                         ++i) { // NOSONAR(cpp:S6022) - unsigned char required for uint8_t blob API compatibility
                        const char hi = hex_str[2 + (i * 2)];
                        const char lo = hex_str[2 + (i * 2) + 1];
                        blob_buffer_[static_cast<std::size_t>(i)] =
                                static_cast<unsigned char>((hex_digit(hi) << 4) | hex_digit(lo)); // NOSONAR(cpp:S6022)
                    }
                    blob_decoded_size_ = static_cast<std::size_t>(binary_len);
                } else {
                    blob_buffer_
                            .assign(reinterpret_cast<const unsigned char*>(hex_str),
                                    reinterpret_cast<const unsigned char*>(hex_str) + hex_len);
                    blob_decoded_size_ = static_cast<std::size_t>(hex_len);
                }
            } catch (...) {             // LCOV_EXCL_LINE — defensive: hex decode bad_alloc, blob_buffer_ resize/assign
                blob_decoded_size_ = 0; // LCOV_EXCL_LINE
                return nullptr;         // LCOV_EXCL_LINE
            } // LCOV_EXCL_LINE
            return blob_buffer_.data();
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto is_null(int col_index) const noexcept -> bool {
            return PQgetisnull(result_, current_row_, col_index) != 0;
        }

        template <typename = void>
        [[nodiscard]] __attribute__((always_inline)) auto get_error_message() const noexcept -> const char* {
            return Api::error_message(conn_);
        }

      private:
        // Binary result decoding (#600). PG sends NETWORK byte order, so each
        // fixed-width read is a byte load + byteswap on a little-endian host;
        // float4/8 send raw IEEE-754 bits, hence bit_cast, not a conversion.
        static constexpr int         kTextFormat   = 0;
        static constexpr int         kBinaryFormat = 1;
        static constexpr std::size_t kInt16Bytes   = 2;
        static constexpr std::size_t kInt32Bytes   = 4;
        static constexpr std::size_t kInt64Bytes   = 8;

        // Load sizeof(UInt) network-order bytes at `val` as a host-order unsigned.
        template <typename UInt> [[nodiscard]] static auto load_network(const char* val) noexcept -> UInt {
            UInt raw{};
            std::memcpy(&raw, val, sizeof(UInt));
            if constexpr (std::endian::native == std::endian::little) {
                return std::byteswap(raw);
            } else {
                return raw; // LCOV_EXCL_LINE — no big-endian host in CI
            }
        }

        // Width comes from the SERVER, not from the caller's C++ type: `unsigned
        // int` still maps to PG BIGINT (#603 — needs 8 bytes of signed range), so
        // extract_int() — the 32-bit-and-smaller extractor — routinely faces an
        // 8-byte int8. A fixed 4-byte read would return the high half (0 for
        // small positives) and look plausible.
        // A NULL is zero-length and decodes to 0, as strtoll("") does on text.
        [[nodiscard]] auto decode_binary_int(int col_index) const noexcept -> std::int64_t {
            const char* val = PQgetvalue(result_, current_row_, col_index);
            switch (static_cast<std::size_t>(PQgetlength(result_, current_row_, col_index))) {
            case kInt64Bytes:
                return static_cast<std::int64_t>(load_network<std::uint64_t>(val));
            case kInt32Bytes:
                return static_cast<std::int32_t>(load_network<std::uint32_t>(val));
            case kInt16Bytes:
                return static_cast<std::int16_t>(load_network<std::uint16_t>(val));
            default:
                return 0;
            }
        }

        // Same width-from-the-server rule for float8/float4; one decoder returning
        // double, with extract_float() narrowing exactly as text-path strtof() did.
        [[nodiscard]] auto decode_binary_floating(int col_index) const noexcept -> double {
            const char* val = PQgetvalue(result_, current_row_, col_index);
            switch (static_cast<std::size_t>(PQgetlength(result_, current_row_, col_index))) {
            case kInt64Bytes:
                return std::bit_cast<double>(load_network<std::uint64_t>(val));
            case kInt32Bytes:
                return static_cast<double>(std::bit_cast<float>(load_network<std::uint32_t>(val)));
            default:
                return 0.0;
            }
        }

        static constexpr auto hex_digit(char ch) noexcept -> unsigned char {
            if (ch >= '0' && ch <= '9') {
                return static_cast<unsigned char>(ch - '0');
            }
            if (ch >= 'a' && ch <= 'f') {
                return static_cast<unsigned char>(ch - 'a' + 10);
            }
            if (ch >= 'A' && ch <= 'F') {
                return static_cast<unsigned char>(ch - 'A' + 10);
            }
            return 0;
        }

        auto clear_result() noexcept -> void {
            if (result_ != nullptr) {
                PQclear(result_);
                result_ = nullptr;
            }
            current_row_ = -1;
            total_rows_  = 0;
        }

        auto clear_params() noexcept -> void {
            param_values_.clear();
            param_ptrs_.clear();
            param_lengths_.clear();
            param_formats_.clear();
            param_count_ = 0;
        }

        // Grows the param slot vectors to cover `index`. The constructor reserve()'s capacity
        // to MAX_DB_VARIABLES, so resize never reallocates in practice; the try/catch covers the
        // theoretically-possible bad_alloc and propagates it through std::expected — issue #316.
        [[nodiscard]] auto ensure_param_slot(int index) noexcept -> std::expected<void, Error> {
            try {
                const auto idx = static_cast<std::size_t>(index);
                if (idx > param_values_.size()) {
                    param_values_.resize(idx);
                    param_ptrs_.resize(idx, nullptr);
                    param_lengths_.resize(idx, 0);
                    param_formats_.resize(idx, 0); // Text format by default
                }
                param_count_ = std::max(param_count_, index);
                return {};
            } catch (...) { // LCOV_EXCL_LINE — defensive: resize() bad_alloc, reserved to MAX_DB_VARIABLES
                return std::unexpected(Error{-1, "out of memory growing param slots"}); // LCOV_EXCL_LINE
            } // LCOV_EXCL_LINE
        }

        auto update_param_ptrs(int index) noexcept -> void {
            const auto idx      = static_cast<std::size_t>(index - 1);
            param_ptrs_[idx]    = param_values_[idx].c_str();
            param_lengths_[idx] = static_cast<int>(param_values_[idx].size());
            param_formats_[idx] = 0; // Text format
        }

        auto rebuild_param_ptrs() noexcept -> void {
            for (int i = 0; i < param_count_; ++i) {
                const auto idx = static_cast<std::size_t>(i);
                if (param_ptrs_[idx] != nullptr) {
                    // Non-NULL param (text or binary) - update pointer
                    // (may have been invalidated by param_values_ vector resize)
                    param_ptrs_[idx] = param_values_[idx].c_str();
                }
                // NULL params: param_ptrs_ already nullptr, no update needed
            }
        }

        PGconn*     conn_ = nullptr; // Borrowed reference
        std::string stmt_name_;
        std::string original_sql_; // Original SQL with ? placeholders (for expanded_sql())
        PGresult*   result_      = nullptr;
        int         current_row_ = -1;
        int         total_rows_  = 0;

        // Parameter accumulation for PQexecPrepared
        std::vector<std::string> param_values_;
        std::vector<const char*> param_ptrs_;
        std::vector<int>         param_lengths_;
        std::vector<int>         param_formats_;
        int                      param_count_ = 0;

        // BYTEA decode buffer (reused across extract_blob_ptr calls)
        std::vector<unsigned char> blob_buffer_;
        std::size_t                blob_decoded_size_ = 0;
        int                        blob_decoded_col_  = -1;

        // Whether the next execute() asks libpq for binary results (#600). A bool,
        // not libpq's 0/1 int — converted only at the PQexecPrepared call site.
        bool binary_results_ = false;
    };

} // namespace storm::db::postgresql
