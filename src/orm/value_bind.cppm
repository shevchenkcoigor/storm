module;

// Value binding (#671): the UUID type, optional/chrono traits, chrono_conv and the
// bind_parameter_value dispatcher, split out of storm_orm_utilities, which
// re-exports this module so existing importers are unchanged.

#include <uuid.h>

export module storm_orm_value_bind;

import std;

export namespace storm::orm::utilities {

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

} // namespace storm::orm::utilities

// Hash specialization for UUID to enable unordered_map<UUID, T*>
// LCOV_EXCL_START — used by m2m stitch maps at runtime in integration tests, not direct unit coverage
template <> struct std::hash<storm::orm::utilities::UUID> {
    [[nodiscard]] auto operator()(const storm::orm::utilities::UUID& uuid) const noexcept -> std::size_t {
        return std::hash<std::string_view>{}(uuid.value);
    }
};
// LCOV_EXCL_STOP
