module;

// Boilerplate-pattern duplicates accepted (see #264 finding).

// Unused, but load-bearing: without a non-empty global module fragment clang-tidy
// segfaults on this re-export module (same crash as storm.cppm, #665/#671).
#include <meta>

export module storm_orm_utilities;

import std;
// Value-binding helpers + UUID and StitchKey live in their own leaf modules (#671);
// re-exported so every existing importer of storm_orm_utilities is unchanged.
export import storm_orm_value_bind;
export import storm_orm_stitch_key;

export namespace storm::orm::utilities {

    // ============================================================================
    // SQL Fragment Length Constants (for compile-time SQL size calculations)
    // ============================================================================

    namespace sql_len {
        // SELECT-related lengths
        constexpr std::size_t SELECT          = 7;  // "SELECT "
        constexpr std::size_t SELECT_DISTINCT = 16; // "SELECT DISTINCT "
        constexpr std::size_t DISTINCT        = 9;  // "DISTINCT "
        constexpr std::size_t FROM            = 6;  // " FROM "

        // INSERT-related lengths
        constexpr std::size_t INSERT_INTO = 12; // "INSERT INTO "
        constexpr std::size_t VALUES_OPEN = 10; // ") VALUES ("

        // UPDATE-related lengths
        constexpr std::size_t UPDATE = 7; // "UPDATE "
        constexpr std::size_t SET    = 5; // " SET "

        // DELETE-related lengths
        constexpr std::size_t DELETE_FROM = 12; // "DELETE FROM "

        // WHERE-related lengths
        constexpr std::size_t WHERE             = 7;  // " WHERE "
        constexpr std::size_t IN_OPEN           = 5;  // " IN ("
        constexpr std::size_t IN_CLAUSE         = 10; // " IN (" + buffer for placeholders base
        constexpr std::size_t AND               = 5;  // " AND "
        constexpr std::size_t EQ_PARAM_AND      = 8;  // " = ? AND "
        constexpr std::size_t LIKE_PATTERN      = 8;  // " LIKE ?"
        constexpr std::size_t BETWEEN_PATTERN   = 17; // " BETWEEN ? AND ?"
        constexpr std::size_t IS_NOT_NULL_AND   = 17; // " IS NOT NULL AND "
        constexpr std::size_t LOGICAL_OP_PARENS = 8;  // "(" + " AND " or " OR " + ")"

        // JOIN-related lengths
        constexpr std::size_t ON_EQUALS = 5; // " ON " + " = " partial

        // Misc lengths
        constexpr std::size_t SMALL_BUFFER  = 10;  // Small buffer/padding
        constexpr std::size_t MEDIUM_BUFFER = 30;  // Medium reserve hint
        constexpr std::size_t LARGE_BUFFER  = 50;  // Large reserve hint
        constexpr std::size_t XL_BUFFER     = 100; // Extra large reserve hint
    } // namespace sql_len

    // ============================================================================
    // Buffer Size Constants
    // ============================================================================

    namespace buffer_size {
        constexpr std::size_t SQL_SMALL  = 512;  // Small SQL buffer
        constexpr std::size_t SQL_MEDIUM = 1024; // Medium SQL buffer (default)
        constexpr std::size_t SQL_LARGE  = 4096; // Large SQL buffer (for complex queries)

        constexpr std::size_t CACHE_DEFAULT = 8;  // Default SQL cache size
        constexpr std::size_t STMT_CACHE    = 32; // Statement cache reserve size
    } // namespace buffer_size

    // ============================================================================
    // Batch Operation Constants
    // ============================================================================

    namespace batch {
        constexpr std::size_t SMALL_THRESHOLD     = 10; // Batch size considered "small"
        constexpr std::size_t FALLBACK_BATCH_SIZE = 50; // Safe fallback for any field count
    } // namespace batch

    // Numeric constants
    namespace numeric {
        constexpr std::size_t MAX_SINGLE_DIGIT = 9; // Maximum single digit value (0-9)
        // Decimal digits needed to render any std::size_t (2^64 - 1 has 20 digits).
        constexpr std::size_t MAX_DECIMAL_DIGITS = 20;

        // Number of decimal digits required to render `value` (digits_of(0) == 1).
        // Used by compile-time SQL size estimators to reserve exact alias width.
        constexpr auto digits_of(std::size_t value) -> std::size_t { // LCOV_EXCL_LINE — compile-time only
            std::size_t digits = 1;                                  // LCOV_EXCL_LINE
            while (value > MAX_SINGLE_DIGIT) {                       // LCOV_EXCL_LINE
                value /= 10;                                         // LCOV_EXCL_LINE
                ++digits;                                            // LCOV_EXCL_LINE
            } // LCOV_EXCL_LINE
            return digits; // LCOV_EXCL_LINE
        } // LCOV_EXCL_LINE
    } // namespace numeric

    // ============================================================================
    // Collation Support
    // ============================================================================

    enum class Collate : std::uint8_t { None, Binary, NoCase, RTrim };

    constexpr auto collate_to_sql(Collate col) noexcept -> std::string_view {
        using enum Collate;
        switch (col) {
        case None:
            return "";
        case Binary:
            return " COLLATE BINARY";
        case NoCase:
            return " COLLATE NOCASE";
        case RTrim:
            return " COLLATE RTRIM";
        }
        return ""; // LCOV_EXCL_LINE - unreachable: all enum values handled above
    }

    // ============================================================================
    // SQL String Building Utilities
    // ============================================================================

    // Length-counting sink for the .append(const char*) / .append(string_view) subset of
    // ConstexprString's interface (the ClauseSizer
    // technique, schema.cppm). Run a writer against it first to learn the exact byte count,
    // then size a ConstexprString from that — ConstexprString truncates SILENTLY on
    // overflow, so a hand-picked fixed capacity turns a wide model into broken SQL.
    struct LengthCounter {
        std::size_t len = 0;

        consteval void append(const char* str) {
            while (*str != '\0') {
                ++len;
                ++str;
            }
        }
        consteval void append(std::string_view str) {
            len += str.size();
        }
    };

    // Compile-time string utility for SQL generation and constexpr parsing
    template <std::size_t N> struct ConstexprString { // LCOV_EXCL_LINE — compile-time only
        std::array<char, N> data{};
        std::size_t         len = 0;

        constexpr ConstexprString() = default;

        explicit constexpr ConstexprString(const char* str) {
            std::size_t i = 0;
            while (str[i] != '\0' && i < N - 1) {
                data[i] = str[i];
                ++i;
            }
            len       = i;
            data[len] = '\0';
        }

        // LCOV_EXCL_START — constexpr-only methods: evaluated at compile time, no runtime code generated
        // Read access
        constexpr std::string_view view() const {
            return {data.data(), len};
        }
        constexpr const char* c_str() const {
            return data.data();
        }
        constexpr bool empty() const {
            return len == 0;
        }

        constexpr bool operator==(std::string_view other) const {
            return view() == other;
        }
        constexpr bool operator!=(std::string_view other) const {
            return view() != other;
        }

        constexpr auto append(const char* str) -> void {
            std::size_t i = 0;
            while (str[i] != '\0' && len < N - 1) {
                data[len] = str[i];
                ++len;
                ++i;
            }
            data[len] = '\0';
        }

        constexpr auto append(const std::string_view& str) -> void {
            for (char c : str) {
                if (len < N - 1) {
                    data[len] = c;
                    ++len;
                }
            }
            data[len] = '\0';
        }

        // Append another ConstexprString
        template <std::size_t M> constexpr auto append(const ConstexprString<M>& other) -> void {
            for (std::size_t i = 0; i < other.len && len < N - 1; ++i) {
                data[len] = other.data[i];
                ++len;
            }
            data[len] = '\0';
        }

        // Operator+= overloads (for convenient syntax)
        constexpr auto operator+=(const char* str) -> ConstexprString& {
            append(str);
            return *this;
        }

        constexpr auto operator+=(const std::string_view& str) -> ConstexprString& {
            append(str);
            return *this;
        }

        template <std::size_t M> constexpr auto operator+=(const ConstexprString<M>& other) -> ConstexprString& {
            append(other);
            return *this;
        }

        // Append the full decimal representation of an unsigned value for
        // compile-time number formatting (handles multi-digit values, e.g. 10).
        constexpr auto append_uint(std::size_t value) -> void {
            if (value <= numeric::MAX_SINGLE_DIGIT) {
                if (len < N - 1) {
                    data[len] = '0' + static_cast<char>(value);
                    ++len;
                    data[len] = '\0';
                }
                return;
            }
            // Build digits in reverse, then append in order.
            std::array<char, numeric::MAX_DECIMAL_DIGITS> buf{};
            std::size_t                                   count = 0;
            while (value > 0) {
                buf[count++] = static_cast<char>('0' + (value % 10));
                value /= 10;
            }
            for (std::size_t i = count; i > 0 && len < N - 1; --i) {
                data[len] = buf[i - 1];
                ++len;
            }
            data[len] = '\0';
        }
        // LCOV_EXCL_STOP

        // Runtime conversion to std::string. Intentionally IMPLICIT (C.164 reviewed,
        // #454): the INSERT/SQL builders rely on `std::string += ConstexprString` and
        // pass these buffers where a std::string is expected; explicit breaks the build.
        operator std::string() const {
            return std::string(data.data(), len);
        } // NOLINT(google-explicit-constructor)
    };

    // Generic thread-local SQL cache template
    template <typename KeyType = std::size_t, std::size_t CACHE_SIZE = buffer_size::CACHE_DEFAULT> struct SQLCache {
        struct Entry {
            KeyType     key{};
            std::string sql;
            // Distinguishes a populated slot from an unused one, so a legitimately
            // empty value (or key == KeyType{}) is not mistaken for a free slot.
            bool occupied = false;
        };

        std::array<Entry, CACHE_SIZE> entries{};
        std::size_t                   next_slot = 0; // For round-robin replacement

        // Returns a pointer to the cached SQL for `key`, or nullptr if absent.
        //
        // Lifetime: the returned pointer is valid only until the next insert() or
        // clear() on this thread. A subsequent insert() may reuse the same slot via
        // round-robin replacement (entry.sql = std::move(sql)), invalidating the
        // pointed-to contents. Consume it immediately, or copy the std::string if you
        // need to hold it across a later insert/clear. The cache is thread_local.
        auto find(const KeyType& key) const -> const std::string* {
            for (const auto& entry : entries) {
                if (entry.occupied && entry.key == key) {
                    return &entry.sql;
                }
            }
            return nullptr;
        }

        auto insert(KeyType key, std::string sql) -> void {
            // Try to find an unused slot first
            for (auto& entry : entries) {
                if (!entry.occupied) {
                    entry.key      = std::move(key);
                    entry.sql      = std::move(sql);
                    entry.occupied = true;
                    return;
                }
            }
            entries[next_slot].key      = std::move(key);
            entries[next_slot].sql      = std::move(sql);
            entries[next_slot].occupied = true;
            next_slot                   = (next_slot + 1) % CACHE_SIZE;
        }

        auto clear() -> void {
            for (auto& entry : entries) {
                entry.key = KeyType{};
                entry.sql.clear();
                entry.occupied = false;
            }
            next_slot = 0;
        }
    };

    // Specialized type aliases for common use cases
    using BulkSQLCache = SQLCache<std::size_t, buffer_size::CACHE_DEFAULT>;

} // namespace storm::orm::utilities

export namespace storm {
    using UUID = orm::utilities::UUID;
} // namespace storm
