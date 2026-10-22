module;

// Column value extraction (#434): the storage-class predicates and extract_* helpers that
// turn a result-row column into a typed C++ value. Split out of BaseStatement — this logic
// depends only on the Statement's extract_* API and utilities, never on the model type T,
// so it is a stateless free utility. Keeping it here lets BaseStatement stay cohesive
// (cpp:S1448) without scattering the dispatch.

#include <meta>

export module storm_orm_statements_extract;

import std;

import storm_orm_utilities;

export namespace storm::orm::statements {

    // Stateless column extractor. All members are static templates keyed on FieldType /
    // Statement; there is no per-model state, so a single type serves every statement class.
    struct ColumnExtractor {
        // ---- Storage-class predicates ---------------------------------------------------
        // Each predicate groups the source types that share one extract_* backend call.
        // Used by extract_column_value() to dispatch by storage class, not by source type.

        template <typename FT>
        static constexpr bool is_int_stored_v =
                std::is_same_v<FT, int> || std::is_same_v<FT, short> || std::is_same_v<FT, unsigned short> ||
                std::is_same_v<FT, unsigned int> || std::is_same_v<FT, signed char> ||
                std::is_same_v<FT, unsigned char> || std::is_same_v<FT, char>;

        template <typename FT>
        static constexpr bool is_int64_stored_v =
                std::is_same_v<FT, std::int64_t> || std::is_same_v<FT, long> || std::is_same_v<FT, long long> ||
                std::is_same_v<FT, std::uint64_t> || std::is_same_v<FT, unsigned long> ||
                std::is_same_v<FT, unsigned long long>;

        template <typename FT> static constexpr bool is_integer_stored_v = is_int_stored_v<FT> || is_int64_stored_v<FT>;

        template <typename FT>
        static constexpr bool is_floating_stored_v = std::is_same_v<FT, double> || std::is_same_v<FT, float>;

        template <typename FT>
        static constexpr bool is_blob_stored_v =
                std::is_same_v<FT, std::vector<std::uint8_t>> || std::is_same_v<FT, std::vector<unsigned char>> ||
                std::is_same_v<FT, std::vector<std::byte>>;

        template <typename FT>
        static constexpr bool is_text_stored_v = std::is_same_v<FT, std::chrono::year_month_day> ||
                                                 std::is_same_v<FT, std::chrono::system_clock::time_point> ||
                                                 std::is_same_v<FT, std::filesystem::path> ||
                                                 std::is_same_v<FT, utilities::UUID> || std::is_same_v<FT, std::string>;

        // PostgreSQL binary-result-format safety (#600 Phase 1). libpq's result
        // format is a per-STATEMENT flag, so a statement may only ask for binary
        // when EVERY column it returns decodes as a plain byte reinterpretation:
        // a network-order fixed width (BOOLEAN/BIGINT/REAL/DOUBLE PRECISION) or
        // raw bytes (TEXT/BYTEA).
        //
        // Deliberately EXCLUDES year_month_day / time_point / UUID even though
        // they share is_text_stored_v's C++ extraction path: on PostgreSQL those
        // are native DATE / TIMESTAMP / UUID columns whose binary wire form is
        // NOT their text form (int32 days, int64 microseconds, 16 raw bytes), so
        // extract_text_like would parse binary garbage as a date string. Also
        // excluded, but only reachable at the MEMBER level: a storm::full_unsigned
        // member is PG NUMERIC(20,0) (base-10000 digit groups) while sharing the
        // very same C++ type as a signed_storage member — see BaseStatement's
        // member_pg_binary_safe.
        //
        // Enums and chrono durations are SMALLINT/INTEGER/BIGINT on PostgreSQL
        // (width-matched to their underlying/rep type, #603) and would decode
        // correctly, but are left out of Phase 1's tested surface.
        template <typename FT>
        static constexpr bool is_pg_binary_safe_v =
                std::is_same_v<FT, bool> || is_int_stored_v<FT> || is_int64_stored_v<FT> || is_floating_stored_v<FT> ||
                is_blob_stored_v<FT> || std::is_same_v<FT, std::string> || std::is_same_v<FT, std::filesystem::path>;

        // Column-level form of the above: a nullable column is decoded by
        // unwrapping the optional and reading its inner type (extract_column_value
        // checks is_null first, and PQgetisnull is format-independent), so the
        // inner type is what decides.
        template <typename FT>
        static constexpr bool is_pg_binary_safe_column_v = is_pg_binary_safe_v<utilities::optional_inner_type_t<FT>>;

        // ---- Storage-class extraction helpers --------------------------------------------
        // Each helper handles one storage class. extract_column_value() dispatches to
        // exactly one helper per FieldType. All marked always_inline so the call site
        // collapses to the same machine code as the inlined branch did before.

        template <typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto read_text_view(Statement* stmt, int col_idx) noexcept
                -> std::string_view {
            const unsigned char* text = stmt->extract_text_ptr(col_idx);
            if (text == nullptr) {
                return {};
            }
            return std::string_view(reinterpret_cast<const char*>(text), stmt->extract_bytes(col_idx));
        }

        // Read an integer column and static_cast it to Target, choosing the 32- vs 64-bit
        // backend by UseInt32. Shared by the int-stored and enum dispatch paths.
        template <typename Target, bool UseInt32, typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto read_int_sized(Statement* stmt, int col_idx) noexcept
                -> Target {
            if constexpr (UseInt32) {
                return static_cast<Target>(stmt->extract_int(col_idx));
            } else {
                return static_cast<Target>(stmt->extract_int64(col_idx));
            }
        }

        template <typename FieldType, typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto extract_int_like(Statement* stmt, int col_idx) noexcept
                -> FieldType {
            static_assert(
                    is_int_stored_v<FieldType> || is_int64_stored_v<FieldType>,
                    "extract_int_like: caller must pre-check storage class"
            );
            return read_int_sized<FieldType, is_int_stored_v<FieldType>>(stmt, col_idx);
        }

        template <typename FieldType, typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto
        extract_floating_like(Statement* stmt, int col_idx) noexcept -> FieldType {
            if constexpr (std::is_same_v<FieldType, double>) {
                return stmt->extract_double(col_idx);
            } else {
                static_assert(
                        std::is_same_v<FieldType, float>, "extract_floating_like: caller must pre-check storage class"
                );
                return stmt->extract_float(col_idx);
            }
        }

        template <typename FieldType, typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto extract_enum(Statement* stmt, int col_idx) noexcept
                -> FieldType {
            using Underlying = std::underlying_type_t<FieldType>;
            return read_int_sized<FieldType, (sizeof(Underlying) <= sizeof(int))>(stmt, col_idx);
        }

        template <typename FieldType, typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto
        extract_text_like(Statement* stmt, int col_idx) noexcept -> FieldType {
            const std::string_view view = read_text_view(stmt, col_idx);
            if constexpr (std::is_same_v<FieldType, std::chrono::year_month_day>) {
                // LCOV_EXCL_START — NOT NULL column, defensive fallback for empty text
                if (view.empty()) {
                    return FieldType{};
                }
                // LCOV_EXCL_STOP
                return utilities::chrono_conv::string_to_ymd(view);
            } else if constexpr (std::is_same_v<FieldType, std::chrono::system_clock::time_point>) {
                // LCOV_EXCL_START — NOT NULL column, defensive fallback for empty text
                if (view.empty()) {
                    return FieldType{};
                }
                // LCOV_EXCL_STOP
                return utilities::chrono_conv::string_to_tp(view);
            } else if constexpr (std::is_same_v<FieldType, std::filesystem::path>) {
                // LCOV_EXCL_START — NOT NULL column, defensive fallback for empty text
                if (view.empty()) {
                    return FieldType{};
                }
                // LCOV_EXCL_STOP
                return std::filesystem::path(std::string(view));
            } else if constexpr (std::is_same_v<FieldType, utilities::UUID>) {
                // LCOV_EXCL_START — NOT NULL column, defensive fallback for empty text
                if (view.empty()) {
                    return FieldType{};
                }
                // LCOV_EXCL_STOP
                return utilities::UUID{std::string(view)};
            } else {
                static_assert(std::is_same_v<FieldType, std::string>, "extract_text_like: unhandled text storage type");
                return FieldType(view);
            }
        }

        template <typename FieldType, typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto
        extract_blob_like(Statement* stmt, int col_idx) noexcept -> FieldType {
            const void* blob = stmt->extract_blob_ptr(col_idx);
            const int   size = stmt->extract_bytes(col_idx);
            if (blob == nullptr || size <= 0) {
                return FieldType{};
            }
            using Byte       = typename FieldType::value_type;
            const auto* data = static_cast<const Byte*>(blob);
            return FieldType(data, data + size);
        }

        // Shared column extraction utility — returns value of specified type from given
        // column index. Dispatches by storage class to one of the extract_* helpers above.
        // Supported FieldType groups: optional<T>, bool, int-stored ints, int64-stored
        // ints, enum, double, float, chrono duration, text-stored types, blob-stored types.
        template <typename FieldType, typename Statement>
        [[nodiscard]] __attribute__((always_inline)) static auto
        extract_column_value(Statement* stmt, int col_idx) noexcept -> FieldType {
            if constexpr (utilities::is_optional_v<FieldType>) {
                using InnerType = typename FieldType::value_type;
                if (stmt->is_null(col_idx)) {
                    return std::nullopt;
                }
                return FieldType{extract_column_value<InnerType>(stmt, col_idx)};
            } else if constexpr (std::is_same_v<FieldType, bool>) {
                return stmt->extract_bool(col_idx);
            } else if constexpr (is_integer_stored_v<FieldType>) {
                return extract_int_like<FieldType>(stmt, col_idx);
            } else if constexpr (std::is_enum_v<FieldType>) {
                return extract_enum<FieldType>(stmt, col_idx);
            } else if constexpr (is_floating_stored_v<FieldType>) {
                return extract_floating_like<FieldType>(stmt, col_idx);
            } else if constexpr (utilities::is_chrono_duration_v<FieldType>) {
                return FieldType{static_cast<typename FieldType::rep>(stmt->extract_int64(col_idx))};
            } else if constexpr (is_text_stored_v<FieldType>) {
                return extract_text_like<FieldType>(stmt, col_idx);
            } else if constexpr (is_blob_stored_v<FieldType>) {
                return extract_blob_like<FieldType>(stmt, col_idx);
            } else {
                // FieldType-dependent false so the assertion only fires when this
                // branch is selected (i.e. when none of the supported groups matched).
                static_assert(
                        !std::is_same_v<FieldType, FieldType>,
                        "Unsupported field type for column extraction. Supported types: "
                        "int, int64_t, long, short, char, unsigned variants, enum, "
                        "float, double, bool, std::string, chrono types, "
                        "filesystem::path, UUID, std::optional<T>, "
                        "std::vector<uint8_t>, std::vector<std::byte>"
                );
            }
        }
    };

    // Parse a decimal string to uint64 for full_unsigned extraction (#436); leading
    // zeros from the SQLite zero-padded TEXT (or PG NUMERIC) are ignored. Defensive 0
    // on an unexpected empty/garbage value — the column is NOT NULL decimal by
    // construction. Free function (no model state) so it stays off BaseStatement's
    // method count (cpp:S1448).
    inline auto parse_full_unsigned(std::string_view text) noexcept -> std::uint64_t {
        std::uint64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    }

    // Extract a full_unsigned column (#436) into obj.[:Member:]: parse the decimal text
    // (SQLite zero-padded TEXT or PG NUMERIC) back to uint64; nullopt on a NULL optional.
    // Free function template (Member NTTP) so it stays off BaseStatement's method count
    // (cpp:S1448) and keeps extract_column_fast's nesting under the limit (cpp:S134).
    template <std::meta::info Member, typename FieldType, typename Obj, typename Statement>
    __attribute__((always_inline)) void extract_full_unsigned_into(Statement* stmt, Obj& obj, int col_idx) noexcept {
        if constexpr (utilities::is_optional_v<FieldType>) {
            obj.[:Member:] = stmt->is_null(col_idx)
                                     ? std::optional<std::uint64_t>(std::nullopt)
                                     : parse_full_unsigned(ColumnExtractor::read_text_view(stmt, col_idx));
        } else {
            obj.[:Member:] = parse_full_unsigned(ColumnExtractor::read_text_view(stmt, col_idx));
        }
    }
} // namespace storm::orm::statements
