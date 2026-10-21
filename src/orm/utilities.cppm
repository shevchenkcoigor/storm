module;

// Boilerplate-pattern duplicates accepted (see #264 finding).

#include <cassert>
#include <meta>
#include <uuid.h>

export module storm_orm_utilities;

import std;

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
    // Parameter Binding Utilities
    // ============================================================================

    // Helper trait to detect std::optional types
    template <typename T> struct is_optional : std::false_type {};
    template <typename TValue> struct is_optional<std::optional<TValue>> : std::true_type {};
    template <typename TValue> constexpr bool is_optional_v = is_optional<TValue>::value;

    // Unwrap std::optional<T> → T; pass-through for non-optional types
    template <typename T> struct optional_inner_type {
        using type = T;
    };
    template <typename TValue> struct optional_inner_type<std::optional<TValue>> {
        using type = TValue;
    };
    template <typename T> using optional_inner_type_t = typename optional_inner_type<T>::type;

    // Helper trait to detect std::chrono::duration types
    template <typename T> struct is_chrono_duration : std::false_type {};
    template <typename Rep, typename Period>
    struct is_chrono_duration<std::chrono::duration<Rep, Period>> : std::true_type {};
    template <typename T> constexpr bool is_chrono_duration_v = is_chrono_duration<T>::value;

    // UUID type — thin wrapper for RFC 4122 UUID strings stored as TEXT in SQLite
    struct UUID {
        std::string value;

        UUID() = default;
        explicit UUID(std::string v) : value(std::move(v)) {}
        explicit UUID(const char* v) : value(v) {}
        explicit UUID(std::string_view v) : value(v) {}

        auto operator==(const UUID&) const -> bool = default;
        // Intentionally IMPLICIT (C.164, #454): is_text_source_v routes UUID to the
        // text-bind path via is_convertible_v<UUID, string_view>; explicit breaks it.
        operator std::string_view() const noexcept {
            return value;
        } // NOLINT(google-explicit-constructor)

        // Validate UUID using stduuid library (checks RFC 4122 compliance)
        [[nodiscard]] static auto is_valid(std::string_view sv) -> bool {
            return uuids::uuid::from_string(sv).has_value();
        }

        // Generate a random RFC 4122 v4 UUID using stduuid
        static auto generate() -> UUID {
            static thread_local std::mt19937 gen{std::random_device{}()};
            uuids::uuid_random_generator     gen_uuid{gen};
            return UUID{uuids::to_string(gen_uuid())};
        }
    };

    // ============================================================================
    // Chrono / String Conversion Helpers (ISO-8601 format)
    // ============================================================================

    namespace chrono_conv {

        inline auto parse_int(std::string_view sv) -> int {
            int val = 0;
            for (char const c : sv) {
                val = (val * 10) + (c - '0');
            }
            return val;
        }

        // year_month_day → "YYYY-MM-DD"
        inline auto ymd_to_string(std::chrono::year_month_day ymd) -> std::string {
            return std::format(
                    "{:04d}-{:02d}-{:02d}",
                    static_cast<int>(ymd.year()),
                    static_cast<unsigned>(ymd.month()),
                    static_cast<unsigned>(ymd.day())
            );
        }

        // "YYYY-MM-DD" → year_month_day
        inline auto string_to_ymd(std::string_view s) -> std::chrono::year_month_day {
            return std::chrono::year_month_day{
                    std::chrono::year{parse_int(s.substr(0, 4))},
                    std::chrono::month{static_cast<unsigned>(parse_int(s.substr(5, 2)))},
                    std::chrono::day{static_cast<unsigned>(parse_int(s.substr(8, 2)))}
            };
        }

        // system_clock::time_point → "YYYY-MM-DD HH:MM:SS"
        inline auto tp_to_string(std::chrono::system_clock::time_point tp) -> std::string {
            auto dp            = std::chrono::floor<std::chrono::days>(tp);
            auto ymd           = std::chrono::year_month_day{dp};
            auto time_since_dp = std::chrono::floor<std::chrono::seconds>(tp) - dp;
            auto h             = std::chrono::duration_cast<std::chrono::hours>(time_since_dp);
            auto m             = std::chrono::duration_cast<std::chrono::minutes>(time_since_dp - h);
            auto s             = time_since_dp - h - m;
            return std::format(
                    "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                    static_cast<int>(ymd.year()),
                    static_cast<unsigned>(ymd.month()),
                    static_cast<unsigned>(ymd.day()),
                    static_cast<int>(h.count()),
                    static_cast<int>(m.count()),
                    static_cast<int>(s.count())
            );
        }

        // "YYYY-MM-DD HH:MM:SS" → system_clock::time_point
        inline auto string_to_tp(std::string_view s) -> std::chrono::system_clock::time_point {
            auto ymd = std::chrono::year_month_day{
                    std::chrono::year{parse_int(s.substr(0, 4))},
                    std::chrono::month{static_cast<unsigned>(parse_int(s.substr(5, 2)))},
                    std::chrono::day{static_cast<unsigned>(parse_int(s.substr(8, 2)))}
            };
            auto dp = std::chrono::sys_days{ymd};
            return dp + std::chrono::hours{parse_int(s.substr(11, 2))} +
                   std::chrono::minutes{parse_int(s.substr(14, 2))} + std::chrono::seconds{parse_int(s.substr(17, 2))};
        }

    } // namespace chrono_conv

    // ---- Source-type predicates for bind_parameter_value ------------------------------
    // Group every supported C++ field type by the bind_* backend call it dispatches to.
    // Collapsing the source-type fan-out into named constexpr bools lets the top-level
    // dispatcher carry one branch per storage class instead of per source type.

    template <typename T>
    constexpr bool is_int_source_v =
            std::is_same_v<T, int> || std::is_same_v<T, short> || std::is_same_v<T, unsigned short> ||
            std::is_same_v<T, unsigned int> || std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char> ||
            std::is_same_v<T, char>;

    template <typename T>
    constexpr bool is_int64_source_v = std::is_same_v<T, std::int64_t> || std::is_same_v<T, long> ||
                                       std::is_same_v<T, long long> || std::is_same_v<T, std::uint64_t> ||
                                       std::is_same_v<T, unsigned long> || std::is_same_v<T, unsigned long long>;

    template <typename T> constexpr bool is_integer_source_v = is_int_source_v<T> || is_int64_source_v<T>;

    template <typename T>
    constexpr bool is_blob_source_v =
            std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<unsigned char>> ||
            std::is_same_v<T, std::vector<std::byte>>;

    // ---- Per-storage-class bind helpers -----------------------------------------------
    // Each helper handles one storage class. bind_parameter_value dispatches to exactly
    // one helper per ValueType. Keeping them small + non-template-on-ValueType where
    // possible lets the compiler fold them back into the call site.

    template <typename T, typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_int_like(StmtType& stmt, int param_index, T value) -> std::expected<void, ErrorType> {
        if constexpr (is_int64_source_v<T>) {
            return stmt.bind_int64(param_index, static_cast<std::int64_t>(value));
        } else {
            static_assert(is_int_source_v<T>, "bind_int_like: caller must pre-check storage class");
            return stmt.bind_int(param_index, static_cast<int>(value));
        }
    }

    template <typename T, typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_enum(StmtType& stmt, int param_index, T value) -> std::expected<void, ErrorType> {
        using Underlying = std::underlying_type_t<T>;
        if constexpr (sizeof(Underlying) <= sizeof(int)) {
            return stmt.bind_int(param_index, static_cast<int>(static_cast<Underlying>(value)));
        } else {
            return stmt.bind_int64(param_index, static_cast<std::int64_t>(static_cast<Underlying>(value)));
        }
    }

    template <typename T, typename StmtType, typename ErrorType> // NOSONAR(cpp:S6024) — already a free function;
                                                                 // SonarCloud's C++ parser misclassifies
                                                                 // namespace-scope module templates
    [[nodiscard]] auto bind_blob_like(StmtType& stmt, int param_index, const T& value)
            -> std::expected<void, ErrorType> {
        if (value.empty()) {
            return stmt.bind_blob(param_index, nullptr, 0);
        }
        return stmt.bind_blob(param_index, value.data(), value.size());
    }

    template <typename T>
    constexpr bool is_chrono_text_v =
            std::is_same_v<T, std::chrono::year_month_day> || std::is_same_v<T, std::chrono::system_clock::time_point>;

    // Bind a chrono value. Date/datetime go through stmt.bind_text with the canonical
    // serialisation; durations get bound as the raw integer count.
    template <typename T, typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_chrono(StmtType& stmt, int param_index, const T& value) -> std::expected<void, ErrorType> {
        if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
            return stmt.bind_text(param_index, std::string_view{chrono_conv::ymd_to_string(value)});
        } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
            return stmt.bind_text(param_index, std::string_view{chrono_conv::tp_to_string(value)});
        } else {
            static_assert(is_chrono_duration_v<T>, "bind_chrono: caller must pre-check storage class");
            return stmt.bind_int64(param_index, static_cast<std::int64_t>(value.count()));
        }
    }

    template <typename T> constexpr bool is_chrono_source_v = is_chrono_text_v<T> || is_chrono_duration_v<T>;

    template <typename T> constexpr bool is_floating_source_v = std::is_same_v<T, double> || std::is_same_v<T, float>;

    template <typename T, typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_floating_like(StmtType& stmt, int param_index, T value) -> std::expected<void, ErrorType> {
        if constexpr (std::is_same_v<T, double>) {
            return stmt.bind_double(param_index, value);
        } else {
            static_assert(std::is_same_v<T, float>, "bind_floating_like: caller must pre-check storage class");
            return stmt.bind_double(param_index, static_cast<double>(value));
        }
    }

    // Bind a boolean as INTEGER 0/1. Pulled out so the dispatcher carries one branch
    // for "bool" rather than one + one for the ternary.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_bool(StmtType& stmt, int param_index, bool value) -> std::expected<void, ErrorType> {
        return stmt.bind_int(param_index, value ? 1 : 0);
    }

    template <typename T>
    constexpr bool is_text_source_v =
            std::is_same_v<T, std::filesystem::path> || std::is_convertible_v<T, std::string_view>;

    // ---- BindableType concept (#473) --------------------------------------------------
    // The set of field/operand types bind_parameter_value can actually dispatch. Composed
    // from the same is_*_source_v predicates the dispatcher branches on, so the concept
    // cannot silently drift from the binder. Constraining the binder and the WHERE operand
    // path with this makes an unsupported type fail at a clear boundary instead of deep
    // inside the dispatcher.

    // Non-optional storage classes the dispatcher can bind directly.
    template <typename T>
    constexpr bool is_bindable_scalar_v =
            is_integer_source_v<T> || std::is_enum_v<T> || std::is_same_v<T, bool> || is_floating_source_v<T> ||
            is_chrono_source_v<T> || is_blob_source_v<T> || std::is_same_v<T, UUID> || is_text_source_v<T>;

    // is_bindable_v<T>: T is directly bindable, or T is std::optional<U> with U bindable
    // (recurses through optional_inner_type_t, matching bind_optional_value's recursion).
    template <typename T>
    constexpr bool is_bindable_v =
            is_optional_v<T> ? is_bindable_scalar_v<optional_inner_type_t<T>> : is_bindable_scalar_v<T>;

    template <typename T>
    concept BindableType = is_bindable_v<T>;

    // Bind a text-shaped value via stmt.bind_text. filesystem::path needs an extra
    // .string() step; everything else converts directly to string_view.
    template <typename T, typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_text_value(StmtType& stmt, int param_index, const T& value)
            -> std::expected<void, ErrorType> {
        if constexpr (std::is_same_v<T, std::filesystem::path>) {
            return stmt.bind_text(param_index, std::string_view{value.string()});
        } else {
            return stmt.bind_text(param_index, std::string_view{value});
        }
    }

    // Helper: validate UUID format. Returns error if invalid, otherwise binds the text.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto validate_and_bind_uuid_text(StmtType& stmt, int param_index, std::string_view uuid_value)
            -> std::expected<void, ErrorType> {
        if (!UUID::is_valid(uuid_value)) {
            return std::unexpected(ErrorType{-1, std::format("Invalid UUID format: '{}'", uuid_value)});
        }
        return stmt.bind_text(param_index, uuid_value);
    }

    // UUID bind: auto-generate if empty, validate if provided. Pulled out so the top-level
    // dispatcher only sees one branch for this slow-path case.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_uuid(StmtType& stmt, int param_index, const UUID& value) -> std::expected<void, ErrorType> {
        if (value.value.empty()) {
            auto generated = UUID::generate();
            return validate_and_bind_uuid_text<StmtType, ErrorType>(stmt, param_index, generated.value);
        }
        return validate_and_bind_uuid_text<StmtType, ErrorType>(stmt, param_index, value.value);
    }

    // Shared "reject empty" UUID bind body — bind_uuid_pk and bind_uuid_where differ only in
    // which position rejected the value, so only the message differs.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto
    bind_uuid_reject_empty(StmtType& stmt, int param_index, const UUID& value, const char* empty_message)
            -> std::expected<void, ErrorType> {
        if (value.value.empty()) {
            return std::unexpected(ErrorType{-1, empty_message});
        }
        return validate_and_bind_uuid_text<StmtType, ErrorType>(stmt, param_index, value.value);
    }

    // UUID PK bind: reject empty (no auto-generation for primary keys).
    // PK UUIDs must be caller-supplied via UUID::generate() — they are never DB-generated.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_uuid_pk(StmtType& stmt, int param_index, const UUID& value)
            -> std::expected<void, ErrorType> {
        return bind_uuid_reject_empty<StmtType, ErrorType>(
                stmt, param_index, value, "Primary key UUID must be explicitly set; auto-generation not allowed for PKs"
        );
    }

    // UUID WHERE/HAVING-position bind: reject empty, same rationale as bind_uuid_pk but for
    // a FILTER comparison rather than a key bind — the column being compared may or may not be
    // a primary key (#609), so the message doesn't claim it is.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_uuid_where(StmtType& stmt, int param_index, const UUID& value)
            -> std::expected<void, ErrorType> {
        return bind_uuid_reject_empty<StmtType, ErrorType>(
                stmt,
                param_index,
                value,
                "UUID comparison value must be explicitly set; auto-generation not allowed in a WHERE/HAVING clause"
        );
    }

    // Forward declaration so bind_optional_value can recurse into bind_parameter_value.
    // Constrained on BindableType (#473) so an unsupported operand is rejected at this
    // boundary with a clear constraint message instead of deep inside the dispatcher.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_parameter_value(StmtType& stmt, int param_index, const auto& value) noexcept
            -> std::expected<void, ErrorType>
        requires BindableType<std::decay_t<decltype(value)>>;

    // Bind a std::optional<T>: NULL if empty, otherwise recurse on the contained value.
    // Pulled out so the dispatcher carries one branch for "optional" rather than one + the
    // inner empty check.
    template <typename StmtType, typename ErrorType, typename Opt>
    [[nodiscard]] auto bind_optional_value(StmtType& stmt, int param_index, const Opt& value)
            -> std::expected<void, ErrorType> {
        if (!value.has_value()) {
            return stmt.bind_null(param_index);
        }
        return bind_parameter_value<StmtType, ErrorType>(stmt, param_index, *value);
    }

    // ---- Top-level dispatcher ---------------------------------------------------------
    // Generic parameter binding — unified implementation for WHERE and CRUD statements.
    // No dependency on entity type T; pure type dispatch on the value type. One branch
    // per storage class; each branch either calls a stmt.bind_* directly or delegates to
    // one of the helpers above.
    // chrono conversions / vector::resize can throw bad_alloc on the rare path;
    // accepted as terminate-on-OOM in the bind hot path (issue #262).
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_parameter_value(StmtType& stmt, int param_index, const auto& value) noexcept
            -> std::expected<void, ErrorType>
        requires BindableType<std::decay_t<decltype(value)>>
    {
        using ValueType = std::decay_t<decltype(value)>;
        if constexpr (is_optional_v<ValueType>) {
            return bind_optional_value<StmtType, ErrorType>(stmt, param_index, value);
        } else if constexpr (std::is_same_v<ValueType, bool>) {
            return bind_bool<StmtType, ErrorType>(stmt, param_index, value);
        } else if constexpr (is_integer_source_v<ValueType>) {
            return bind_int_like<ValueType, StmtType, ErrorType>(stmt, param_index, value);
        } else if constexpr (std::is_enum_v<ValueType>) {
            return bind_enum<ValueType, StmtType, ErrorType>(stmt, param_index, value);
        } else if constexpr (is_floating_source_v<ValueType>) {
            return bind_floating_like<ValueType, StmtType, ErrorType>(stmt, param_index, value);
        } else if constexpr (is_chrono_source_v<ValueType>) {
            return bind_chrono<ValueType, StmtType, ErrorType>(stmt, param_index, value);
        } else if constexpr (is_blob_source_v<ValueType>) {
            return bind_blob_like<ValueType, StmtType, ErrorType>(stmt, param_index, value);
        } else if constexpr (std::is_same_v<ValueType, UUID>) {
            return bind_uuid<StmtType, ErrorType>(stmt, param_index, value);
        } else {
            static_assert(is_text_source_v<ValueType>, "bind_parameter_value: BindableType arm exhaustion");
            return bind_text_value<ValueType, StmtType, ErrorType>(stmt, param_index, value);
        }
        // No trailing unsupported-type branch: the BindableType<> constraint (#473)
        // guarantees exactly one arm above matches, so an unbindable type is rejected
        // at the call site rather than reaching a runtime std::unexpected here.
    }

    // WHERE/HAVING-position parameter bind (#609): same dispatch as bind_parameter_value except a
    // UUID rejects an empty value instead of auto-generating one — auto-generation is only
    // meaningful in an INSERT/SET position, never in a filter comparison. Mirrors
    // BaseStatement::bind_pk_scalar's if-constexpr shape (#573, statements/base.cppm).
    // The else branch also covers std::optional<UUID> (falling through to the auto-generating
    // bind_optional_value -> bind_uuid): currently unreachable, since ExpressionVariant has no
    // ComparisonExpr<std::optional<T>>/InExpression<std::optional<T>> arm for any T, so no
    // filter-position operand can ever be optional. Revisit this branch if that changes.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_filter_value(StmtType& stmt, int param_index, const auto& value) noexcept
            -> std::expected<void, ErrorType>
        requires BindableType<std::decay_t<decltype(value)>>
    {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, UUID>) {
            return bind_uuid_where<StmtType, ErrorType>(stmt, param_index, value);
        } else {
            return bind_parameter_value<StmtType, ErrorType>(stmt, param_index, value);
        }
    }

    // ============================================================================
    // SQL String Building Utilities
    // ============================================================================

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

    // ============================================================================
    // Stitch Key (#504)
    // ============================================================================

    // The m2m/reverse-FK two-query eager-load stitches Q2 rows to their Q1 owner
    // through a hash map keyed on the owner's primary key. A single-column PK key
    // was a bare std::int64_t; a composite PK needs a multi-value key that still
    // crosses the M2MRelation type-erased vtable (ErasedStatementPtr — no T in
    // scope there), so it can't be a template parameter. StitchKey is a
    // fixed-size inline byte buffer built by appending one part at a time:
    // append_int64 for integral PK parts (8 bytes, matching today's exact
    // single-PK layout so that case is byte-identical), append_string for text
    // parts (hashed to 8 bytes — the stitch only needs equality for map lookup,
    // never the original text back, so storing a hash instead of the bytes
    // avoids capping composite key width by string length). CAPACITY=32 covers
    // every composite PK in the codebase today (max 3 parts before #504 ships);
    // a 4th part (int64 or string-hash) still fits exactly (4 x 8 = 32) but is a
    // hard ceiling with zero slack — a 5th part overflows.
    //
    // That ceiling is enforced at COMPILE TIME, at the model (#537). The only
    // guard used to be the assert in append_word, which NDEBUG compiles out —
    // i.e. it was absent from exactly the Release configuration where the
    // overrun would matter. ModelPrimaryKeyPartLimit<T> (base.cppm) now rejects a
    // model with more parts than MAX_PARTS, so append_word cannot be reached with
    // a full buffer and the assert is a redundant backstop rather than the guard.
    //
    // CAPACITY was NOT simply grown to make room: it is load-bearing for
    // performance. #504 measured a wider key costing ~4% on the stitch hot path,
    // which is why single-PK models bypass this class entirely for a bare
    // std::uint64_t. Folding parts beyond the 4th was also rejected in #504 —
    // two distinct composite keys colliding in the fold is a MIS-STITCH (rows
    // attached to the wrong owner), not a recoverable map collision.
    class StitchKey {
      public:
        static constexpr std::size_t CAPACITY = 32;

        // How many parts fit. Every appender writes exactly one 8-byte word (see
        // append_word), so this is the capacity in words. DERIVED rather than
        // written as a literal 4 so the compile-time model gate that reads it
        // tracks CAPACITY automatically: changing one cannot leave the other
        // stale, which is the failure mode that would reintroduce the overrun.
        static constexpr std::size_t MAX_PARTS = CAPACITY / sizeof(std::uint64_t);

        void append_int64(std::int64_t v) noexcept {
            append_word(static_cast<std::uint64_t>(v));
        }

        // Both loops below read whole 8-byte words, so every appender must write
        // exactly 8. append_int64 does by construction; append_string does only
        // while size_t is 64-bit, so assert it rather than leave it to a comment.
        static_assert(sizeof(std::size_t) == sizeof(std::uint64_t), "StitchKey assumes a 64-bit std::size_t");

        void append_string(std::string_view v) noexcept {
            append_word(std::hash<std::string_view>{}(v));
        }

        // Word-wise compare, inline. This runs on EVERY hash-map probe of the m2m
        // stitch, where the bare std::int64_t key it replaced compared in one
        // inlined instruction, so the two obvious spellings both cost real time:
        // std::ranges::equal over a span lowers to a byte-at-a-time loop
        // (movzbl/cmp/jne per byte), and std::memcmp lowers to a PLT call into
        // libc's bcmp — perf attributed 11.7% of the fanout200 profile to that
        // call alone. Comparing whole words keeps it branch-light and callable-free.
        // Safe because len_ is always a multiple of 8 (see hash()).
        friend auto operator==(const StitchKey& lhs, const StitchKey& rhs) noexcept -> bool {
            if (lhs.len_ != rhs.len_) {
                return false;
            }
            for (std::size_t off = 0; off + sizeof(std::uint64_t) <= lhs.len_; off += sizeof(std::uint64_t)) {
                std::uint64_t lhs_word = 0;
                std::uint64_t rhs_word = 0;
                std::memcpy(&lhs_word, lhs.bytes_.data() + off, sizeof(lhs_word));
                std::memcpy(&rhs_word, rhs.bytes_.data() + off, sizeof(rhs_word));
                if (lhs_word != rhs_word) {
                    return false;
                }
            }
            return true;
        }

        // Word-wise FNV-1a. NOT std::hash<string_view>: libc++ routes that to
        // murmur2/cityhash, which perf showed running once per Q1 owner AND once
        // per Q2 related row, where the bare std::int64_t key this replaced used
        // the identity hash.
        //
        // Reads whole words, which is safe because len_ is always a multiple of 8:
        // the only writer is append_word, which is private and takes a whole
        // std::uint64_t, so a part of any other width is not expressible. Were one
        // ever added, its tail would fall outside this loop and contribute nothing
        // to the hash — not a correctness bug (operator== still separates such
        // keys, so the map resolves it) but a silent collision cliff.
        [[nodiscard]] auto hash() const noexcept -> std::size_t {
            constexpr std::uint64_t FNV_OFFSET = 1469598103934665603ULL;
            constexpr std::uint64_t FNV_PRIME  = 1099511628211ULL;
            std::uint64_t           h          = FNV_OFFSET;
            for (std::size_t off = 0; off + sizeof(std::uint64_t) <= len_; off += sizeof(std::uint64_t)) {
                std::uint64_t word = 0;
                std::memcpy(&word, bytes_.data() + off, sizeof(word));
                h = (h ^ word) * FNV_PRIME;
            }
            return static_cast<std::size_t>(h);
        }

        // The single word of a ONE-part key (#504 perf). A single-column PK is the
        // overwhelmingly common shape, and for it this whole class is a 33-byte
        // wrapper around one std::uint64_t: the map can be keyed on that word
        // directly, recovering libc++'s identity std::hash<integral> and a
        // one-instruction inlined operator== in place of hash<StitchKey>'s FNV loop
        // and this class's out-of-line word compare. SelectStatement narrows to this
        // when Base::primary_key_column_count_ == 1; composite models keep the full
        // key. Only meaningful for len_ == sizeof(std::uint64_t) — asserted, since a
        // composite key read through here would silently stitch on its first part
        // alone (a real mis-stitch, not just a collision).
        [[nodiscard]] auto first_word() const noexcept -> std::uint64_t {
            assert(len_ == sizeof(std::uint64_t) && "StitchKey::first_word on a non-single-part key");
            std::uint64_t word = 0;
            std::memcpy(&word, bytes_.data(), sizeof(word));
            return word;
        }

      private:
        // Takes a whole word rather than a (pointer, size) pair: the "every part is
        // exactly 8 bytes" invariant that hash() and operator== both read whole words
        // on is then enforced by the signature instead of resting on each caller
        // passing sizeof correctly.
        //
        // The assert is a BACKSTOP, not the guard (#537). It is compiled out under
        // NDEBUG — precisely the Release build — so it never protected the case it
        // names. ModelPrimaryKeyPartLimit<T> rejects an over-wide model at compile
        // time, before any statement class instantiates, so this is now unreachable
        // for every model that exists; it is retained to catch a direct caller
        // (the unit tests append parts by hand, bypassing the model gate).
        void append_word(std::uint64_t word) noexcept {
            assert(len_ + sizeof(word) <= CAPACITY && "StitchKey: append exceeds fixed CAPACITY");
            std::memcpy(bytes_.data() + len_, &word, sizeof(word));
            len_ += sizeof(word);
        }

        std::array<std::byte, CAPACITY> bytes_{};
        std::size_t                     len_ = 0;
    };

} // namespace storm::orm::utilities

// Hash specialization for UUID to enable unordered_map<UUID, T*>
// LCOV_EXCL_START — used by m2m stitch maps at runtime in integration tests, not direct unit coverage
template <> struct std::hash<storm::orm::utilities::UUID> {
    [[nodiscard]] auto operator()(const storm::orm::utilities::UUID& u) const noexcept -> std::size_t {
        return std::hash<std::string_view>{}(u.value);
    }
};
// LCOV_EXCL_STOP

// std::hash<StitchKey> must live in namespace std (import std; provides no macro
// or ADL escape hatch for specializing it inside storm::). Delegates to the
// member hash(), which covers the occupied byte range only (mirroring
// operator=='s len_-bounded comparison).
export template <> struct std::hash<storm::orm::utilities::StitchKey> {
    auto operator()(const storm::orm::utilities::StitchKey& k) const noexcept -> std::size_t {
        return k.hash();
    }
};

export namespace storm {
    using UUID = orm::utilities::UUID;
} // namespace storm
