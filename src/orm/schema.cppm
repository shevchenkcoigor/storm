module;

// `duplicate` removed in #277 Phase 3 (shared append_index_sql helper).

#include <meta>

export module storm_orm_schema;

import std;

import storm_orm_statements_base;
import storm_orm_indexes;
import storm_orm_utilities;
import storm_orm_field_attr;
import storm_db_concept;
import storm_db_default_connection;

export namespace storm::orm::schema {

    // SQL dialect for compile-time schema generation
    enum class Dialect : std::uint8_t { SQLite, PostgreSQL };

    // Import utilities for compile-time string building
    using storm::orm::utilities::ConstexprString;

    namespace detail {

        // Storage class — which SQL primitive the C++ inner type maps to. The set is
        // closed; unknown types fall through to `Fallback` (TEXT, untyped).
        enum class StorageClass : std::uint8_t {
            Bool,
            Integer,
            Double,
            Float,
            Text,
            Date,
            DateTime,
            Uuid,
            Blob,
            Fallback,
        };

        // Strip std::optional<T> to T for storage classification. Non-optionals pass
        // through unchanged.
        template <typename T>
        using col_inner_t = std::conditional_t<utilities::is_optional_v<T>, utilities::optional_inner_type_t<T>, T>;

        // PostgreSQL integer column width (#603): a C++ integer-stored field maps to
        // the narrowest PG integer type that can hold its full value range, instead of
        // always BIGINT. SQLite ignores this (dynamic typing — one "INTEGER" storage
        // class for every width already).
        //   Small   -> SMALLINT (2 bytes,  -32768..32767)
        //   Regular -> INTEGER  (4 bytes,  -2147483648..2147483647)
        //   Big     -> BIGINT   (8 bytes,  -9223372036854775808..9223372036854775807)
        // An unsigned N-byte source needs a signed column wide enough for its full
        // 0..2^(8N)-1 range, which is one size class up from a signed N-byte source
        // (a 1-byte unsigned still fits Small, same as signed, since 255 <= 32767) —
        // up to 8 bytes, where it saturates at Big: an 8-byte unsigned source (a
        // storm::signed_storage uint64_t, #436) has no wider signed class to promote
        // to, so it keeps the pre-#603 BIGINT and the caller's existing 0..INT64_MAX
        // contract. A full 0..2^64-1 range never reaches this dispatcher at all —
        // storm::full_unsigned routes through a separate NUMERIC(20,0)/TEXT type.
        enum class IntWidth : std::uint8_t { Small, Regular, Big };

        template <typename T> consteval auto integer_width_of() -> IntWidth {
            using enum IntWidth;
            if constexpr (utilities::is_chrono_duration_v<T>) {
                return integer_width_of<typename T::rep>();
            } else if constexpr (std::is_enum_v<T>) {
                return integer_width_of<std::underlying_type_t<T>>();
            } else if constexpr (std::is_signed_v<T>) {
                if constexpr (sizeof(T) <= 2) {
                    return Small;
                } else if constexpr (sizeof(T) <= 4) {
                    return Regular;
                } else {
                    return Big;
                }
            } else {
                if constexpr (sizeof(T) <= 1) {
                    return Small;
                } else if constexpr (sizeof(T) <= 2) {
                    return Regular;
                } else {
                    return Big;
                }
            }
        }

        // Source-type predicates per StorageClass. Combining them as a single named
        // constexpr bool keeps the dispatcher below to one branch per storage class.
        template <typename T>
        constexpr bool is_integer_source_v =
                std::is_same_v<T, int> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, short> ||
                std::is_same_v<T, unsigned int> || std::is_same_v<T, unsigned short> || std::is_same_v<T, long> ||
                std::is_same_v<T, unsigned long> || std::is_same_v<T, long long> ||
                std::is_same_v<T, unsigned long long> || std::is_same_v<T, signed char> ||
                std::is_same_v<T, unsigned char> || std::is_same_v<T, char> || std::is_enum_v<T> ||
                utilities::is_chrono_duration_v<T>;

        template <typename T>
        constexpr bool is_text_source_v = std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
                                          std::is_same_v<T, std::filesystem::path>;

        template <typename T>
        constexpr bool is_blob_source_v =
                std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<unsigned char>> ||
                std::is_same_v<T, std::vector<std::byte>>;

        // Classify a C++ type (already stripped of optional<>) into one StorageClass.
        // Compile-time only.
        template <typename T> consteval auto storage_class_of() -> StorageClass {
            using enum StorageClass;
            if constexpr (std::is_same_v<T, bool>) {
                return Bool;
            } else if constexpr (is_integer_source_v<T>) {
                return Integer;
            } else if constexpr (std::is_same_v<T, double>) {
                return Double;
            } else if constexpr (std::is_same_v<T, float>) {
                return Float;
            } else if constexpr (is_text_source_v<T>) {
                return Text;
            } else if constexpr (std::is_same_v<T, std::chrono::year_month_day>) {
                return Date;
            } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                return DateTime;
            } else if constexpr (std::is_same_v<T, utilities::UUID>) {
                return Uuid;
            } else if constexpr (is_blob_source_v<T>) {
                return Blob;
            } else {
                return Fallback;
            }
        }

        // Per-storage-class type-name helpers. Each one returns the bare type string
        // (no NOT NULL) for the given dialect. Splitting these out keeps every helper
        // at one or two if-constexpr branches.
        template <Dialect D> consteval auto bool_type() -> std::string_view {
            return D == Dialect::PostgreSQL ? "BOOLEAN" : "INTEGER";
        }
        // Width-aware integer type (#603). SQLite keeps a single "INTEGER" for every
        // width (dynamic typing already stores any value regardless of declared
        // affinity); PostgreSQL picks SMALLINT/INTEGER/BIGINT per IntWidth. No default
        // for W — every caller of THIS function must know its width; the load-bearing
        // "default to Big for callers that predate #603" lives one level up, on
        // bare_type_for/not_null_type_for/sql_type_for, which every pre-#603 call site
        // actually goes through.
        template <Dialect D, IntWidth W> consteval auto integer_type() -> std::string_view {
            // [width][pg] → type string; avoids branch-clone (SQLite's "INTEGER" and PG's
            // Regular-width "INTEGER" are coincidentally identical text, not a real clone).
            constexpr std::array<std::array<std::string_view, 2>, 3> types     = {{
                    {"INTEGER", "SMALLINT"},
                    {"INTEGER", "INTEGER"},
                    {"INTEGER", "BIGINT"},
            }};
            constexpr auto                                           width_idx = std::to_underlying(W);
            constexpr auto                                           pg = static_cast<int>(D == Dialect::PostgreSQL);
            return types[width_idx][pg];
        }
        template <Dialect D> consteval auto double_type() -> std::string_view {
            return D == Dialect::PostgreSQL ? "DOUBLE PRECISION" : "REAL";
        }
        template <Dialect D> consteval auto date_type() -> std::string_view {
            return D == Dialect::PostgreSQL ? "DATE" : "TEXT";
        }
        template <Dialect D> consteval auto datetime_type() -> std::string_view {
            return D == Dialect::PostgreSQL ? "TIMESTAMP" : "TEXT";
        }
        template <Dialect D> consteval auto uuid_type() -> std::string_view {
            return D == Dialect::PostgreSQL ? "UUID" : "TEXT";
        }
        template <Dialect D> consteval auto blob_type() -> std::string_view {
            return D == Dialect::PostgreSQL ? "BYTEA" : "BLOB";
        }

        // Order-preserving full-range unsigned-64 storage (#436): PostgreSQL
        // NUMERIC(20,0) holds the whole 0..2^64-1 range; SQLite TEXT stores a
        // zero-padded 20-char decimal so lexicographic order == numeric order.
        template <Dialect D> consteval auto full_unsigned_type() -> std::string_view {
            return D == Dialect::PostgreSQL ? "NUMERIC(20,0)" : "TEXT";
        }
        template <Dialect D> consteval auto full_unsigned_type_nn() -> std::string_view {
            return D == Dialect::PostgreSQL ? "NUMERIC(20,0) NOT NULL" : "TEXT NOT NULL";
        }

        // NOT NULL counterparts (same dialect dispatch). Splitting them keeps each helper
        // at one ternary, so the outer storage-class dispatcher only counts the one branch
        // per class.
        template <Dialect D> consteval auto bool_type_nn() -> std::string_view {
            return D == Dialect::PostgreSQL ? "BOOLEAN NOT NULL" : "INTEGER NOT NULL";
        }
        template <Dialect D, IntWidth W> consteval auto integer_type_nn() -> std::string_view {
            // [width][pg] → type string; see integer_type's own comment on why the two
            // "INTEGER NOT NULL" cells are not a branch-clone.
            constexpr std::array<std::array<std::string_view, 2>, 3> types     = {{
                    {"INTEGER NOT NULL", "SMALLINT NOT NULL"},
                    {"INTEGER NOT NULL", "INTEGER NOT NULL"},
                    {"INTEGER NOT NULL", "BIGINT NOT NULL"},
            }};
            constexpr auto                                           width_idx = std::to_underlying(W);
            constexpr auto                                           pg = static_cast<int>(D == Dialect::PostgreSQL);
            return types[width_idx][pg];
        }
        template <Dialect D> consteval auto double_type_nn() -> std::string_view {
            return D == Dialect::PostgreSQL ? "DOUBLE PRECISION NOT NULL" : "REAL NOT NULL";
        }
        template <Dialect D> consteval auto date_type_nn() -> std::string_view {
            return D == Dialect::PostgreSQL ? "DATE NOT NULL" : "TEXT NOT NULL";
        }
        template <Dialect D> consteval auto datetime_type_nn() -> std::string_view {
            return D == Dialect::PostgreSQL ? "TIMESTAMP NOT NULL" : "TEXT NOT NULL";
        }
        template <Dialect D> consteval auto uuid_type_nn() -> std::string_view {
            return D == Dialect::PostgreSQL ? "UUID NOT NULL" : "TEXT NOT NULL";
        }

        // Pre-baked type strings keyed by StorageClass × Dialect. sql_type_for picks
        // one of these (or its NOT NULL counterpart) per call. W defaults to Big so
        // every call site written before #603 keeps emitting BIGINT unless it opts
        // into a narrower width explicitly.
        template <StorageClass C, Dialect D, IntWidth W = IntWidth::Big>
        consteval auto bare_type_for() -> std::string_view {
            using enum StorageClass;
            if constexpr (C == Bool) {
                return bool_type<D>();
            } else if constexpr (C == Integer) {
                return integer_type<D, W>();
            } else if constexpr (C == Double) {
                return double_type<D>();
            } else if constexpr (C == Float) {
                return "REAL";
            } else if constexpr (C == Text) {
                return "TEXT";
            } else if constexpr (C == Date) {
                return date_type<D>();
            } else if constexpr (C == DateTime) {
                return datetime_type<D>();
            } else if constexpr (C == Uuid) {
                return uuid_type<D>();
            } else if constexpr (C == Blob) {
                return blob_type<D>();
            } else {
                static_assert(C == Fallback, "bare_type_for: unhandled StorageClass");
                return "TEXT";
            }
        }

        template <StorageClass C, Dialect D, IntWidth W = IntWidth::Big>
        consteval auto not_null_type_for() -> std::string_view {
            using enum StorageClass;
            if constexpr (C == Bool) {
                return bool_type_nn<D>();
            } else if constexpr (C == Integer) {
                return integer_type_nn<D, W>();
            } else if constexpr (C == Double) {
                return double_type_nn<D>();
            } else if constexpr (C == Float) {
                return "REAL NOT NULL";
            } else if constexpr (C == Text) {
                return "TEXT NOT NULL";
            } else if constexpr (C == Date) {
                return date_type_nn<D>();
            } else if constexpr (C == DateTime) {
                return datetime_type_nn<D>();
            } else if constexpr (C == Uuid) {
                return uuid_type_nn<D>();
            } else {
                // Blob and Fallback are always nullable-shaped; no NOT NULL variant needed.
                static_assert(
                        C != StorageClass::Blob && C != StorageClass::Fallback,
                        "not_null_type_for: storage class has no NOT NULL variant"
                );
                return {};
            }
        }

        // Pick the bare or NOT-NULL variant for (StorageClass, Dialect, Nullable).
        // Blob and Fallback always use the bare variant (matches the prior catch-all
        // behaviour for std::vector<...> fields).
        template <StorageClass C, Dialect D, bool Nullable, IntWidth W = IntWidth::Big>
        consteval auto sql_type_for() -> std::string_view {
            if constexpr (Nullable) {
                return bare_type_for<C, D, W>();
            } else {
                return not_null_type_for<C, D, W>();
            }
        }

        // Append the decimal representation of a signed integral value (handles
        // the leading '-'; the magnitude reuses ConstexprString::append_uint).
        template <typename SqlT> consteval void append_signed(SqlT& col, std::int64_t value) {
            if (value < 0) {
                col.append("-");
                // -(min) overflows in two's complement; widen through unsigned.
                col.append_uint(static_cast<std::uint64_t>(-(value + 1)) + 1U);
            } else {
                col.append_uint(static_cast<std::uint64_t>(value));
            }
        }

        // Append a compile-time decimal rendering of a double. The values are
        // programmer-written literals (e.g. 0.0, 3.14, -2.5), not arbitrary
        // doubles, so a fixed-precision render with trailing-zero trimming (but
        // always keeping one fractional digit) is sufficient and round-trips
        // these literals. std::to_chars is not constexpr for double in this
        // toolchain, so the digits are extracted by hand.
        template <typename SqlT> consteval void append_double(SqlT& col, double value) {
            if (value < 0) {
                col.append("-");
                value = -value;
            }
            constexpr std::uint64_t scale = 1000000U; // 6 fractional digits
            const auto              whole = static_cast<std::uint64_t>(value);
            // Scale the fraction (always >= 0 here, value was made non-negative
            // above) and round half-up: truncate, then bump when the dropped part
            // is >= 0.5. Avoids the cast-of-(x+0.5) pattern, which mis-rounds only
            // for negative inputs — not possible here.
            const double        frac_scaled = (value - static_cast<double>(whole)) * static_cast<double>(scale);
            const auto          frac_trunc  = static_cast<std::uint64_t>(frac_scaled);
            const std::uint64_t scaled_frac =
                    frac_trunc + ((frac_scaled - static_cast<double>(frac_trunc)) >= 0.5 ? 1U : 0U);
            // Carry a rounded-up fraction (e.g. 0.9999995) into the whole part.
            const std::uint64_t carry = scaled_frac / scale;
            std::uint64_t       frac  = scaled_frac % scale;
            col.append_uint(whole + carry);
            col.append(".");
            // Render exactly 6 fractional digits, then trim trailing zeros while
            // keeping at least one digit after the point.
            std::array<char, 6> digits{};
            for (std::size_t i = 6; i > 0; --i) {
                digits[i - 1] = static_cast<char>('0' + (frac % 10));
                frac /= 10;
            }
            std::size_t last = 5;
            while (last > 0 && digits[last] == '0') {
                --last;
            }
            for (std::size_t i = 0; i <= last; ++i) {
                col.append(std::string_view{&digits[i], 1});
            }
        }

        // Append a SQL single-quoted text literal, escaping embedded quotes by
        // doubling them ("O'Brien" -> 'O''Brien'). Same escaping in both dialects.
        template <typename SqlT> consteval void append_sql_text(SqlT& col, std::string_view value) {
            col.append("'");
            for (char c : value) {
                if (c == '\'') {
                    col.append("''");
                } else {
                    col.append(std::string_view{&c, 1});
                }
            }
            col.append("'");
        }

        // Emit the " DEFAULT <value>" clause for a NOT NULL column that carries a
        // C++ default-member-initializer, recovering the value from a
        // default-constructed Owner (P2996 exposes whether a default exists, not
        // its value). Covers bool/int/int64/double/float/string; other storage
        // classes (date, blob, uuid, fallback) emit nothing and keep their prior
        // shape. Generalizes the bool-only #344 clause — see issue #413. Without
        // this, `ALTER TABLE ... ADD COLUMN <x> NOT NULL` is rejected on a
        // populated table ("Cannot add a NOT NULL column with default value NULL").
        // True when Owner can be default-constructed in a constant expression —
        // the prerequisite for recovering a field's default value. Models with a
        // non-literal member (e.g. an m2m `plf::hive<...>`/`std::vector<...>`
        // field) are not literal types; their scalar defaults cannot be read at
        // compile time, so they keep the no-DEFAULT shape (these are the
        // relationship-bearing models, not the plain scalar tables that ALTER ADD
        // COLUMN migrations target).
        template <typename Owner>
        constexpr bool owner_constexpr_constructible = requires { typename std::bool_constant<(Owner{}, true)>; };

        // Render the bool DEFAULT literal for the dialect (split out so the
        // dispatcher below stays at a single branch per storage class).
        template <Dialect D, typename SqlT> consteval void append_bool_default(SqlT& col, bool value) {
            if constexpr (D == Dialect::PostgreSQL) {
                col.append(value ? " DEFAULT TRUE" : " DEFAULT FALSE");
            } else {
                col.append(value ? " DEFAULT 1" : " DEFAULT 0");
            }
        }

        template <typename Owner, std::meta::info Member, Dialect D, typename SqlT>
        consteval void append_default_clause(SqlT& col) {
            using enum StorageClass;
            using FieldType            = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
            constexpr StorageClass cls = storage_class_of<FieldType>();
            if constexpr (!std::meta::has_default_member_initializer(Member) || !owner_constexpr_constructible<Owner>) {
                return;
            } else {
                constexpr Owner def{};
                if constexpr (cls == Bool) {
                    append_bool_default<D>(col, def.[:Member:]);
                } else if constexpr (cls == Integer) {
                    col.append(" DEFAULT ");
                    append_signed(col, static_cast<std::int64_t>(def.[:Member:]));
                } else if constexpr (cls == Double || cls == Float) {
                    col.append(" DEFAULT ");
                    append_double(col, static_cast<double>(def.[:Member:]));
                } else if constexpr (cls == Text) {
                    col.append(" DEFAULT ");
                    append_sql_text(col, std::string_view{def.[:Member:]});
                }
            }
        }

        // A length-counting sink mirroring the ConstexprString append/append_uint
        // interface every DDL writer here uses. Running the same writer against it yields
        // the exact rendered length, so a buffer budget stays in lock-step with the
        // emitted SQL (no hand-maintained constant): build_create_table_sql (#682) and
        // build_junction_sql size their whole statement this way.
        struct ClauseSizer {
            std::size_t    len = 0;
            consteval void append(const char* str) {
                while (*str != '\0') {
                    ++len;
                    ++str;
                }
            }
            consteval void append(std::string_view str) {
                len += str.size();
            }
            consteval void append_uint(std::uint64_t value) {
                ++len; // at least one digit
                while (value > utilities::numeric::MAX_SINGLE_DIGIT) {
                    value /= 10;
                    ++len;
                }
            }
        };

        // Emit the type portion for a max_length<N> text field (#493).
        //   PostgreSQL: "VARCHAR(N)[ NOT NULL]" — a real bounded type.
        //   SQLite:     "TEXT[ NOT NULL]" — the bound is a trailing CHECK, appended
        //               later by append_max_length_check (so the DEFAULT clause can sit
        //               between the type and the CHECK).
        // Nullable fields omit NOT NULL; the SQLite CHECK passes on NULL either way.
        template <Dialect D, bool Nullable, typename SqlT>
        consteval void append_max_length_type(SqlT& col, std::size_t bound) {
            if constexpr (D == Dialect::PostgreSQL) {
                col.append("VARCHAR(");
                col.append_uint(bound);
                col.append(")");
            } else {
                col.append("TEXT");
            }
            if constexpr (!Nullable) {
                col.append(" NOT NULL");
            }
        }

        // Append the SQLite length CHECK for a max_length<N> text field:
        //   " CHECK(length(<name>) <= N)". No-op on PostgreSQL (VARCHAR(N) already bounds
        //   the column). Passes when the value is NULL (standard SQL), so nullable+bounded
        //   works correctly.
        template <Dialect D, typename SqlT>
        consteval void append_max_length_check(SqlT& col, std::string_view name, std::size_t bound) {
            if constexpr (D == Dialect::PostgreSQL) {
                return;
            } else {
                col.append(" CHECK(length(");
                col.append(name);
                col.append(") <= ");
                col.append_uint(bound);
                col.append(")");
            }
        }

        // Append the table-level PRIMARY KEY (a, b) of a composite-PK model (#500), in
        // declaration order — column order is semantically significant for the index the
        // key creates. Emitted once, after every column, exactly like the junction-table
        // DDL. No-op for a single-PK model, whose key is column-level.
        //
        // Parts are written with the canonical column-name writer (#422), not
        // identifier_of: an FK part's column is "<name>_id", and naming the bare member
        // would emit a PRIMARY KEY over a column that does not exist. A composite key
        // whose parts are FKs is the canonical reason composite keys exist, so this path
        // is load-bearing, not defensive.
        //
        // Free-standing (like the appenders above) rather than a SchemaStatement method:
        // it needs only the PK member list, not the class, and the class is at its method
        // ceiling (S1448).
        template <typename SqlT, typename MembersT>
        consteval void append_composite_pk_clause(SqlT& sql, const MembersT& members, bool is_composite) {
            if (!is_composite) {
                return;
            }
            sql.append(",\n    PRIMARY KEY (");
            bool first_part = true;
            for (const auto member : members) {
                if (!first_part) {
                    sql.append(", ");
                }
                storm::meta::append_column_name(sql, member);
                first_part = false;
            }
            sql.append(")");
        }

        // Map a C++ field type to its SQL column definition string for the given dialect.
        // Returns the column type portion (after the column name).
        // Two-axis dispatch:
        //   1. col_inner_t<FieldType>   strips std::optional<> to the inner type
        //   2. storage_class_of<…>()    classifies the inner type into one of the
        //                               StorageClass tags
        //   3. sql_type_for<C, D, N>()  returns the SQL fragment for that
        //                               (storage class, dialect, nullable) triple
        // The nullable flag drives the `NOT NULL` suffix; Blob is always nullable-shaped
        // (matches the prior catch-all behaviour for vector<...> fields).
        template <typename FieldType, Dialect D = Dialect::SQLite> consteval auto sql_col_def() -> std::string_view {
            using Inner                = col_inner_t<FieldType>;
            constexpr StorageClass cls = storage_class_of<Inner>();
            constexpr bool         nullable =
                    utilities::is_optional_v<FieldType> || cls == StorageClass::Blob || cls == StorageClass::Fallback;
            // Width-aware only for Integer: the field OWNS its storage here (never
            // mirrors another model's primary key), so its declared C++ type's width
            // is exactly the width the column should have (#603).
            if constexpr (cls == StorageClass::Integer) {
                return sql_type_for<cls, D, nullable, integer_width_of<Inner>()>();
            } else {
                return sql_type_for<cls, D, nullable>();
            }
        }

        // ---- Composite-FK column/constraint DDL (#504) ----
        // A composite-FK member (fk<> whose target has a multi-column primary key, #500)
        // cannot be expressed as one column-level "<name>_id ... REFERENCES ..." the way a
        // single-column FK is (append_fk_column_def): SQLite/PG column-level REFERENCES only
        // ever names one column. Instead: one plain column PER target PK part
        // (append_composite_fk_columns, "<name>_<part> <TYPE> [NOT NULL], ..."), plus a
        // SEPARATE table-level FOREIGN KEY (...) REFERENCES <Related>(...) constraint tying
        // them back to the target (append_composite_fk_constraint) — the same two-piece shape
        // append_composite_pk_clause already uses for a composite PRIMARY KEY. Free-standing
        // functions (not SchemaStatement methods): SchemaStatement is at its S1448 method
        // ceiling (see append_composite_pk_clause's own comment), and these need only the FK
        // member's own reflection plus its target's PK member list — the target's own
        // BaseStatement<RelatedType>, not SchemaStatement<T>'s Base.

        // The C++ type a composite key part is STORED as. A plain part stores its own
        // declared type; an FK part stores the REFERENCED row's key, so its storage type
        // is that key's type, not the whole related struct (which has no storage class at
        // all — it would fall through to StorageClass::Fallback and trip
        // not_null_type_for's static_assert from several levels down inside a sizer,
        // naming neither the model nor the field). Same is_fk_field routing that
        // BaseStatement::bind_one_pk_part (#501) and TwoQueryJoinBase::append_pk_part_to_key
        // (#504 Task 8) already use for the identical question. Single-level, matching the
        // ValidForeignKey precedent (#412): the referenced key is taken as-is, never
        // resolved recursively if it is ITSELF an FK.
        template <std::meta::info PartMember> consteval auto pk_part_storage_type() -> std::meta::info {
            if constexpr (storm::orm::statements::meta::is_fk_field(PartMember)) {
                using FkFieldType = std::remove_cvref_t<typename[:std::meta::type_of(PartMember):]>;
                using InnerType   = storm::orm::utilities::optional_inner_type_t<FkFieldType>;
                return std::meta::type_of(storm::orm::statements::BaseStatement<InnerType>::primary_key_);
            } else {
                return std::meta::type_of(PartMember);
            }
        }

        // True when Model's own primary key is the single-column, DB-generated shape
        // (not composite, not UUID) — append_single_pk_column_def hardcodes THAT shape's
        // column to BIGINT on PostgreSQL / INTEGER on SQLite, UNCONDITIONALLY, ignoring
        // the PK member's own declared C++ type (every model in this tree spells its PK
        // as plain `int`, yet gets a PG BIGINT column). Any column that MIRRORS such a
        // PK — a junction side column, a composite-FK part column pointing at it — must
        // mirror that pinned width, not the width computed from the declared C++ type,
        // or the mirroring column's type disagrees with the actual PK column and
        // PostgreSQL rejects the FOREIGN KEY (SQLite accepts the mismatch silently).
        // A composite or UUID PK is never pinned: its own column is typed by the same
        // width-aware sql_col_def a mirroring column also resolves through, so both
        // sides agree by construction — no pin needed.
        template <typename Model> consteval auto has_pinned_bigint_pk() -> bool {
            using ModelBase = storm::orm::statements::BaseStatement<Model>;
            return !ModelBase::has_composite_pk_ && !ModelBase::has_uuid_pk_();
        }

        // True when PartMember (one part of a composite target's PK) resolves, via
        // pk_part_storage_type above, to ANOTHER model's primary key — and that other
        // model's PK is the pinned-BIGINT shape has_pinned_bigint_pk describes. A plain
        // (non-FK) composite part is never pinned: it stores its own declared type
        // directly, and its own column (emitted through the regular-column path,
        // sql_col_def) is already width-aware from that same declared type, so a
        // mirroring column agrees by construction.
        template <std::meta::info PartMember> consteval auto part_pinned_bigint() -> bool {
            if constexpr (storm::orm::statements::meta::is_fk_field(PartMember)) {
                using FkFieldType = std::remove_cvref_t<typename[:std::meta::type_of(PartMember):]>;
                using InnerType   = storm::orm::utilities::optional_inner_type_t<FkFieldType>;
                return has_pinned_bigint_pk<InnerType>();
            } else {
                return false;
            }
        }

        // Whether junction column Is of SideType is pinned (see has_pinned_bigint_pk /
        // part_pinned_bigint above): a single-column side pins on the SIDE's own PK
        // shape (there is only one column, and it is not itself an FK field); a
        // composite side pins per-part via the FK-or-not rule, since only a part that
        // is itself an FK can mirror ANOTHER model's pinned PK — a composite side's own
        // plain part is never pinned. `if constexpr` (not a ternary) so only the
        // reachable branch is instantiated.
        template <typename SideType, std::size_t Is> consteval auto junction_side_pinned() -> bool {
            using SideBase = storm::orm::statements::BaseStatement<SideType>;
            if constexpr (SideBase::has_composite_pk_) {
                return part_pinned_bigint<SideBase::primary_key_members_[Is]>();
            } else {
                return has_pinned_bigint_pk<SideType>();
            }
        }

        // Append ONE column of a composite-FK target's part: "<name>_<part> <TYPE> [NOT
        // NULL]" (omits NOT NULL for a nullable FK member, matching append_fk_column_def's
        // own [nullable] dispatch). `PartMember` is the TARGET's own primary-key-part
        // reflection (e.g. OrderLineWithShipments::order_id) — its STORED type drives the
        // SQL storage CLASS (Integer/Text/...) via the same storage_class_of dispatch
        // sql_col_def uses internally. "Stored" rather than "declared" because a part may
        // ITSELF be an FK (the canonical association-table shape), in which case the
        // column holds the REFERENCED row's key: pk_part_storage_type resolves that,
        // exactly as the junction DDL and the bind/extract paths do for the same question
        // (#536). Reading the declared type instead typed such a column from the whole
        // related STRUCT — StorageClass::Fallback, emitted as a nullable TEXT — so the
        // table-level FOREIGN KEY clause compared TEXT against the target's BIGINT key
        // and PostgreSQL rejected the CREATE TABLE outright (SQLite, which does not type-
        // check FK targets, accepted it and then silently never matched). Single-level,
        // matching the FKFieldOf/ValidForeignKey precedent (#412): the referenced key is
        // taken as-is, never resolved recursively if it is itself an FK.
        // `FkMember` is the FK member's own reflection (source of the "<name>_" prefix).
        //
        // Nullability is NOT delegated to sql_col_def<PartType, D>(): that dispatch reads
        // is_optional_v<PartType>, and PartType is the TARGET's own PK-part type — a
        // primary-key part is NEVER std::optional (PKs can't be nullable), so that call
        // would always return the NOT-NULL variant regardless of whether the FK MEMBER
        // itself is nullable. The single decision point for nullability must be
        // FkFieldType's own optionality (mirrors append_fk_column_def's fk_nullable flag,
        // which reads storm::orm::utilities::is_optional_v<FieldType> where FieldType is
        // the FK member's type, not the target's) — so this calls sql_type_for directly
        // with that flag, rather than sql_col_def.
        template <std::meta::info FkMember, std::meta::info PartMember, Dialect D, typename SqlT>
        consteval void append_composite_fk_part_column(SqlT& col) {
            using FkFieldType          = std::remove_cvref_t<typename[:std::meta::type_of(FkMember):]>;
            using PartType             = std::remove_cvref_t<typename[:pk_part_storage_type<PartMember>():]>;
            constexpr StorageClass cls = storage_class_of<PartType>();
            constexpr bool         nullable =
                    utilities::is_optional_v<FkFieldType> || cls == StorageClass::Blob || cls == StorageClass::Fallback;
            col.append(std::meta::identifier_of(FkMember));
            col.append("_");
            col.append(std::meta::identifier_of(PartMember));
            col.append(" ");
            // Width-aware (#603) unless this part mirrors another model's pinned
            // single-column PK (part_pinned_bigint) — that PK's own column ignores its
            // declared C++ width entirely, so a mirroring column must too.
            if constexpr (cls == StorageClass::Integer && !part_pinned_bigint<PartMember>()) {
                col.append(sql_type_for<cls, D, nullable, integer_width_of<PartType>()>());
            } else {
                col.append(sql_type_for<cls, D, nullable>());
            }
        }

        // The target's PK member list, as a template-argument-usable constexpr value
        // (NOT a runtime array reference, even a const-ref one): std::meta::info is a
        // consteval-only type, and `targets[Is]` used as another function's template
        // argument (see append_composite_fk_part_column below) must itself be a
        // constant expression — a std::array PARAMETER is not, even when the caller
        // only ever passes a constexpr local (the parameter's constexpr-ness does not
        // propagate through the call). Shared by append_composite_fk_columns_impl and
        // append_composite_fk_constraint_impl below so the RelatedBase/targets lookup
        // is written once; matches the discipline base.cppm's
        // bind_fk_parts_impl/extract_fk_parts_impl already use for the identical shape.
        template <typename RelatedType> consteval auto composite_fk_target_members() {
            return storm::orm::statements::BaseStatement<RelatedType>::primary_key_members_;
        }

        template <typename RelatedType, std::meta::info Member, Dialect D, typename SqlT, std::size_t... Is>
        consteval void append_composite_fk_columns_impl(SqlT& col, std::index_sequence<Is...> /*unused*/) {
            constexpr auto targets = composite_fk_target_members<RelatedType>();
            (
                    [&] {
                        if constexpr (Is > 0) {
                            col.append(", ");
                        }
                        append_composite_fk_part_column<Member, targets[Is], D>(col);
                    }(),
                    ...
            );
        }

        // Append every column a composite-FK member emits, comma-joined: one column per
        // target PK part (#504). Sits in append_column_def's FK branch alongside the
        // single-column append_fk_column_def.
        template <typename RelatedType, std::meta::info Member, Dialect D, typename SqlT>
        consteval void append_composite_fk_columns(SqlT& col) {
            using RelatedBase = storm::orm::statements::BaseStatement<RelatedType>;
            append_composite_fk_columns_impl<RelatedType, Member, D>(
                    col, std::make_index_sequence<RelatedBase::primary_key_column_count_>{}
            );
        }

        template <typename RelatedType, std::meta::info Member, Dialect D, typename SqlT, std::size_t... Is>
        consteval void append_composite_fk_constraint_impl(
                SqlT& sql, std::string_view related_name, std::index_sequence<Is...> /*unused*/
        ) {
            constexpr auto targets = composite_fk_target_members<RelatedType>();
            sql.append(",\n    FOREIGN KEY (");
            (
                    [&] {
                        if constexpr (Is > 0) {
                            sql.append(", ");
                        }
                        sql.append(std::meta::identifier_of(Member));
                        sql.append("_");
                        sql.append(std::meta::identifier_of(targets[Is]));
                    }(),
                    ...
            );
            sql.append(") REFERENCES ");
            sql.append(related_name);
            sql.append("(");
            (
                    [&] {
                        if constexpr (Is > 0) {
                            sql.append(", ");
                        }
                        storm::meta::append_column_name(sql, targets[Is]);
                    }(),
                    ...
            );
            sql.append(")");
            if constexpr (constexpr auto action = storm::orm::statements::meta::fk_on_delete_action_of(Member);
                          action.has_value()) {
                sql.append(" ON DELETE ");
                sql.append(storm::orm::statements::meta::ref_action_sql(action.value()));
            }
        }

        // Table-level "FOREIGN KEY (<name>_a, <name>_b) REFERENCES <Related>(a, b) [ON
        // DELETE <action>]" for a composite-FK member (#504) — the multi-column counterpart
        // of append_fk_column_def's inline column-level REFERENCES, emitted once per
        // composite-FK member after all columns (same placement as
        // append_composite_pk_clause). The LOCAL side names the columns
        // append_composite_fk_columns just defined ("<name>_<part>"); the TARGET side names
        // the target's own PK part columns via append_column_name (correct even if a target
        // part is itself an FK, whose own column is "<part>_id").
        template <typename RelatedType, std::meta::info Member, Dialect D, typename SqlT>
        consteval void append_composite_fk_constraint(SqlT& sql) {
            using RelatedBase = storm::orm::statements::BaseStatement<RelatedType>;
            append_composite_fk_constraint_impl<RelatedType, Member, D>(
                    sql,
                    std::meta::identifier_of(std::meta::dealias(^^RelatedType)),
                    std::make_index_sequence<RelatedBase::primary_key_column_count_>{}
            );
        }

        // ---- Auto-junction table DDL (#203, widened for composite PKs in #504) ----
        //
        // The auto-junction was fixed at two columns, "<Owner>_id" and "<Related>_id",
        // with both sides' FK clauses hardcoded to "REFERENCES <Side>(id)". All three
        // parts of that shape break when a side's primary key spans several columns
        // (#500): one column cannot carry an N-part key, the PRIMARY KEY clause must
        // list every column or the junction rejects legitimate distinct pairs, and
        // "(id)" names a column a composite-PK model does not have. (SQLite does not
        // validate FK target columns at CREATE TABLE time so it accepted the broken
        // DDL silently; PostgreSQL rejected it, which is why such a model could not be
        // created there at all.)
        //
        // Naming follows the composite-FK convention (#504 Task 3): a single-PK side
        // keeps the exact legacy "<Side>_id" spelling, a composite side emits
        // "<Side>_<part>" per part. The single-PK spelling is a hardcoded special case,
        // NOT a degenerate instance of the general rule — same discipline as
        // append_fk_column_names, so the byte-identical guarantee for existing junctions
        // cannot drift.
        //
        // Free functions rather than SchemaStatement methods: that class is at its
        // S1448 method ceiling, and these need both sides' own BaseStatement, not
        // SchemaStatement<T>'s.

        // Column-name writer for one junction side, part `Is`. Single-PK sides ignore
        // `Is` entirely (they have exactly one part, index 0). The composite branch
        // routes the part through append_column_name (#422), so an FK key part
        // (`[[= storm::primary_part]][[= storm::fk<>]] Person warehouse` — the canonical
        // association-table shape) emits "<Side>_warehouse_id", matching the column
        // append_junction_side_target_list names on the FK clause's target side.
        // Naming the bare member would produce a junction column referencing a column
        // that does not exist on the target.
        template <typename SideType, std::size_t Is, typename SqlT>
        consteval void append_junction_side_column_name(SqlT& sql, std::string_view side_name) {
            using SideBase = storm::orm::statements::BaseStatement<SideType>;
            sql.append(side_name);
            if constexpr (SideBase::has_composite_pk_) {
                sql.append("_");
                storm::meta::append_column_name(sql, SideBase::primary_key_members_[Is]);
            } else {
                sql.append("_id");
            }
        }

        // "<Side>_<part> <TYPE> NOT NULL" for one junction column. A junction column is
        // always NOT NULL (a junction row with a partial key is meaningless), matching
        // the pre-#504 behaviour.
        //
        // The type comes from the part's OWN STORED type (int -> INTEGER/BIGINT,
        // storm::UUID -> UUID/TEXT, string -> TEXT, an FK part -> the referenced key's
        // type) via the same storage_class_of dispatch every other column uses, for
        // EVERY side shape — composite (#504) and single-PK alike. The single-PK branch
        // used to hardcode the integer type, so an m2m between storm::UUID-PK models
        // (#507) got an INTEGER/BIGINT column carrying a "REFERENCES <Side>(<uuid_pk>)"
        // clause pointing at a UUID/TEXT key: PostgreSQL rejects that CREATE TABLE
        // outright (FK type mismatch), SQLite accepts it and stores mismatched
        // affinities silently — the #536/#519 asymmetry (#565). The per-field FK path
        // (append_fk_column_def) has always derived the type from the target PK member;
        // this is the same rule, one path later.
        //
        // primary_key_members_[0] IS primary_key_ for a single-PK model, so the two
        // shapes need no separate branch at all, and an integer-PK side still renders
        // exactly "INTEGER NOT NULL"/"BIGINT NOT NULL" — byte-identical.
        template <typename SideType, std::size_t Is, Dialect D, typename SqlT>
        consteval void append_junction_side_column_def(SqlT& sql, std::string_view side_name) {
            using SideBase = storm::orm::statements::BaseStatement<SideType>;
            using PartType =
                    std::remove_cvref_t<typename[:pk_part_storage_type<SideBase::primary_key_members_[Is]>():]>;
            constexpr StorageClass cls    = storage_class_of<PartType>();
            constexpr bool         pinned = junction_side_pinned<SideType, Is>();
            append_junction_side_column_name<SideType, Is>(sql, side_name);
            sql.append(" ");
            if constexpr (cls == StorageClass::Integer && !pinned) {
                sql.append(sql_type_for<cls, D, /*Nullable=*/false, integer_width_of<PartType>()>());
            } else {
                sql.append(sql_type_for<cls, D, /*Nullable=*/false>());
            }
        }

        // How many junction columns a side contributes: 1 for a single-column PK,
        // N for a composite one.
        template <typename SideType> consteval auto junction_side_column_count() -> std::size_t {
            return storm::orm::statements::BaseStatement<SideType>::primary_key_column_count_;
        }

        template <typename SideType, Dialect D, typename SqlT, std::size_t... Is>
        consteval void append_junction_side_column_defs_impl(
                SqlT& sql, std::string_view side_name, std::index_sequence<Is...> /*unused*/
        ) {
            (
                    [&] {
                        if constexpr (Is > 0) {
                            sql.append(",\n    ");
                        }
                        append_junction_side_column_def<SideType, Is, D>(sql, side_name);
                    }(),
                    ...
            );
        }

        // Every column definition this side contributes, ",\n    "-joined.
        template <typename SideType, Dialect D, typename SqlT>
        consteval void append_junction_side_column_defs(SqlT& sql, std::string_view side_name) {
            append_junction_side_column_defs_impl<SideType, D>(
                    sql, side_name, std::make_index_sequence<junction_side_column_count<SideType>()>{}
            );
        }

        template <typename SideType, typename SqlT, std::size_t... Is>
        consteval void append_junction_side_column_list_impl(
                SqlT& sql, std::string_view side_name, std::index_sequence<Is...> /*unused*/
        ) {
            (
                    [&] {
                        if constexpr (Is > 0) {
                            sql.append(", ");
                        }
                        append_junction_side_column_name<SideType, Is>(sql, side_name);
                    }(),
                    ...
            );
        }

        // Every column NAME this side contributes, ", "-joined — used by both the
        // PRIMARY KEY clause and the local side of the FOREIGN KEY clause, so those two
        // can never disagree about which columns exist.
        template <typename SideType, typename SqlT>
        consteval void append_junction_side_column_list(SqlT& sql, std::string_view side_name) {
            append_junction_side_column_list_impl<SideType>(
                    sql, side_name, std::make_index_sequence<junction_side_column_count<SideType>()>{}
            );
        }

        template <typename SideType, typename SqlT, std::size_t... Is>
        consteval void append_junction_side_target_list_impl(SqlT& sql, std::index_sequence<Is...> /*unused*/) {
            using SideBase = storm::orm::statements::BaseStatement<SideType>;
            (
                    [&] {
                        if constexpr (Is > 0) {
                            sql.append(", ");
                        }
                        storm::meta::append_column_name(sql, SideBase::primary_key_members_[Is]);
                    }(),
                    ...
            );
        }

        // The TARGET side of this side's FOREIGN KEY clause: the referenced model's own
        // primary-key column names. Composite sides only (#504); the single-PK side is
        // written by append_junction_fk below, which reads that side's real PK identifier
        // (#519). Routed through append_column_name (#422) so an FK part emits "<part>_id".
        template <typename SideType, typename SqlT> consteval void append_junction_side_target_list(SqlT& sql) {
            append_junction_side_target_list_impl<SideType>(
                    sql, std::make_index_sequence<junction_side_column_count<SideType>()>{}
            );
        }

        // "FOREIGN KEY (<local cols>) REFERENCES <Side>(<target cols>) ON DELETE <action>"
        // for one junction side (#412).
        template <typename SideType, typename SqlT>
        consteval void append_junction_fk(SqlT& sql, std::string_view side_name, std::string_view action) {
            using SideBase = storm::orm::statements::BaseStatement<SideType>;
            sql.append("FOREIGN KEY (");
            append_junction_side_column_list<SideType>(sql, side_name);
            sql.append(") REFERENCES ");
            sql.append(side_name);
            sql.append("(");
            if constexpr (SideBase::has_composite_pk_) {
                append_junction_side_target_list<SideType>(sql);
            } else {
                // This side's REAL PK member identifier (#519), not the literal "id" the
                // pre-#504 code emitted: an m2m whose owner or related PK is named anything
                // else produced junction DDL naming a nonexistent column (unexecutable
                // CREATE TABLE — the #506 failure class). pk_name_ is the same source
                // join.cppm's ON clauses read, so DDL and the eager load cannot drift.
                // The junction's OWN column stays "<Side>_id" (append_junction_side_column_name):
                // that is the junction's column, not the referenced model's.
                sql.append(SideBase::pk_name_);
            }
            sql.append(") ON DELETE ");
            sql.append(action);
        }

        // The whole junction CREATE TABLE. Single sink-agnostic writer: the size
        // companion below runs THIS function against ClauseSizer, so the budget is
        // measured from the emitting code itself and cannot drift from it.
        template <typename OwnerType, typename RelatedType, Dialect D, typename SqlT>
        consteval void append_junction_sql(SqlT& sql, std::string_view action) {
            constexpr auto owner_name   = std::meta::identifier_of(^^OwnerType);
            constexpr auto related_name = std::meta::identifier_of(std::meta::dealias(^^RelatedType));
            sql.append("CREATE TABLE ");
            sql.append(owner_name);
            sql.append("_");
            sql.append(related_name);
            sql.append(" (\n    ");
            append_junction_side_column_defs<OwnerType, D>(sql, owner_name);
            sql.append(",\n    ");
            append_junction_side_column_defs<RelatedType, D>(sql, related_name);
            sql.append(",\n    PRIMARY KEY (");
            append_junction_side_column_list<OwnerType>(sql, owner_name);
            sql.append(", ");
            append_junction_side_column_list<RelatedType>(sql, related_name);
            sql.append("),\n    ");
            append_junction_fk<OwnerType>(sql, owner_name, action);
            sql.append(",\n    ");
            append_junction_fk<RelatedType>(sql, related_name, action);
            sql.append("\n)");
        }

        // Exact byte size of append_junction_sql's output, measured by rendering it into
        // the counting sink — the same technique every other exact-size companion in this
        // file uses.
        template <typename OwnerType, typename RelatedType, Dialect D>
        consteval auto junction_sql_size(std::string_view action) -> std::size_t {
            ClauseSizer sizer;
            append_junction_sql<OwnerType, RelatedType, D>(sizer, action);
            return sizer.len;
        }

        // ---- Composite-FK helpers (#504) ----
        //
        // These live here rather than on SchemaStatement so that class stays under the
        // 35-method ceiling (S1448); they are the same shape as every other `detail`
        // helper in this file, with the model's BaseStatement passed in as `Base`
        // (it supplies all_members_/field_count_) instead of being the enclosing class.

        // True when Base::all_members_[Index] is an FK field whose target has a COMPOSITE
        // primary key (#500/#504) — the shared gate every composite-FK DDL helper below
        // (index sizing AND emission) branches on, so the "is this member a composite FK" check
        // is written exactly once. Index-templated (not a runtime-loop predicate): naming
        // the target's own BaseStatement<RelatedType> needs Index as a template argument,
        // which a runtime loop variable cannot supply.
        template <typename Base, std::size_t Index> consteval auto has_composite_fk_target() -> bool {
            constexpr auto member = Base::all_members_[Index];
            if constexpr (statements::meta::is_fk_field(member)) {
                using FieldType   = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
                using RelatedType = col_inner_t<FieldType>;
                return storm::orm::statements::BaseStatement<RelatedType>::has_composite_pk_;
            } else {
                return false;
            }
        }

        // The (optional-unwrapped) related type of an FK member at Index — only valid to
        // instantiate under an `if constexpr (statements::meta::is_fk_field(all_members_[Index]))`
        // guard, paired with has_composite_fk_target<Base, Index>() at every call site so the
        // two never drift.
        template <typename Base, std::size_t Index>
        using composite_fk_related_t =
                col_inner_t<std::remove_cvref_t<typename[:std::meta::type_of(Base::all_members_[Index]):]>>;

        // A composite-FK member's index (#504) names N columns ("<name>_a, <name>_b, ...")
        // instead of the single "<name>_id" index_sql_buffer's (name+3+2)*2 term budgets
        // for; this is the per-member extra, 0 for every other member.
        template <typename Base, std::size_t Index> consteval auto composite_fk_index_extra_len() -> std::size_t {
            if constexpr (has_composite_fk_target<Base, Index>()) {
                using RelatedBase       = storm::orm::statements::BaseStatement<composite_fk_related_t<Base, Index>>;
                constexpr auto targets  = RelatedBase::primary_key_members_;
                const auto     name_len = std::meta::identifier_of(Base::all_members_[Index]).size();
                std::size_t    total    = 0;
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    total += name_len + 1 + std::meta::identifier_of(targets[i]).size() + 2; // "_part, "
                }
                return total * 2; // once for the idx name, once for the column list
            } else {
                return 0;
            }
        }

        // Sum of composite_fk_index_extra_len across every field — the whole extra
        // index_sql_buffer must add for composite-FK members. Zero for models without one.
        template <typename Base> consteval auto composite_fk_index_extra_size() -> std::size_t {
            return [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                std::size_t total = 0;
                ((total += composite_fk_index_extra_len<Base, Is>()), ...);
                return total;
            }(std::make_index_sequence<Base::field_count_>{});
        }

    } // namespace detail

    template <typename T> class SchemaStatement : private storm::orm::statements::BaseStatement<T> {
        using Base = storm::orm::statements::BaseStatement<T>;

        // True when the model's PK opted into the SQLite never-reuse guarantee
        // via storm::primary_autoincrement (#379). Drives the AUTOINCREMENT keyword.
        static consteval auto pk_wants_autoincrement() -> bool {
            return storm::meta::is_primary_autoincrement(Base::primary_key_);
        }

        // Append an FK column definition: "<name>_id INTEGER/BIGINT [NOT NULL] REFERENCES
        // <Related>(<pk>) [ON DELETE <action>]". The REFERENCES clause enforces referential
        // integrity (#412): the related table is the FK field's C++ type identifier
        // (optional-unwrapped) and the related PK is the target model's actual PK member
        // identifier (#506, not the literal "id"). An unannotated FK emits plain REFERENCES
        // (= ON DELETE NO ACTION / RESTRICT, the SQL default); an fk<RefAction::...> policy
        // (#431) appends the chosen ON DELETE clause.
        // Extracted from append_column_def to keep it single-purpose.
        template <std::meta::info Member, Dialect D, typename SqlT>
        static consteval void append_fk_column_def(SqlT& col) {
            using FieldType   = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
            using RelatedType = detail::col_inner_t<FieldType>;
            col.append(std::meta::identifier_of(Member));
            // The FK column's storage type mirrors the referenced model's PK type (#507):
            // an FK onto a storm::UUID PK is a UUID/TEXT column, not INTEGER/BIGINT. Derive
            // it from the target PK member so DDL and the runtime FK bind agree.
            constexpr auto referenced_pk_member = Base::template find_fk_primary_key<FieldType>();
            constexpr bool ref_is_uuid =
                    std::meta::dealias(std::meta::type_of(referenced_pk_member)) == ^^storm::orm::utilities::UUID;
            constexpr auto fk_nullable = static_cast<int>(storm::orm::utilities::is_optional_v<FieldType>);
            if constexpr (ref_is_uuid) {
                col.append("_id ");
                // detail::-qualified: SchemaStatement sits OUTSIDE namespace detail, so
                // the unqualified name does not resolve. Both UUID branches in this class
                // are if constexpr-guarded, so the error only fires once a model actually
                // has a UUID key — which nothing in-tree did until #565's junction tests.
                col.append(detail::uuid_type<D>());
                if constexpr (fk_nullable == 0) {
                    col.append(" NOT NULL");
                }
            } else {
                // [nullable][pg] → fk column suffix; avoids branch-clone and nested ternary
                constexpr std::array<std::array<std::string_view, 2>, 2> fk_suffixes = {{
                        {"_id INTEGER NOT NULL", "_id BIGINT NOT NULL"},
                        {"_id INTEGER", "_id BIGINT"},
                }};
                constexpr auto fk_pg = static_cast<int>(D == Dialect::PostgreSQL);
                col.append(fk_suffixes[fk_nullable][fk_pg]);
            }
            col.append(" REFERENCES ");
            col.append(std::meta::identifier_of(std::meta::dealias(^^RelatedType)));
            col.append("(");
            col.append(std::meta::identifier_of(referenced_pk_member));
            col.append(")");
            // Per-FK ON DELETE policy (#431): emitted only when the field's fk<RefAction>
            // is a non-default action, so bare fk<>/fk<Restrict> stay byte-identical.
            if constexpr (constexpr auto action = statements::meta::fk_on_delete_action_of(Member);
                          action.has_value()) {
                col.append(" ON DELETE ");
                col.append(statements::meta::ref_action_sql(action.value()));
            }
        }

        // Append a regular (non-PK, non-FK, non-full_unsigned) column definition, with the
        // optional UNIQUE constraint. Handles the max_length<N> text bound (#493): PG swaps
        // TEXT for VARCHAR(N); SQLite keeps TEXT and appends a trailing
        // CHECK(length(<name>) <= N). SQLite emission order is
        //   <name> TEXT [NOT NULL] [DEFAULT v] [CHECK(...)] [UNIQUE]
        // — the DEFAULT sits between the type and the CHECK, matching #413. Extracted from
        // append_column_def to keep the regular and unique branches from duplicating.
        template <std::meta::info Member, Dialect D, bool Unique, typename SqlT>
        static consteval void append_regular_column_def(SqlT& col) {
            using FieldType = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
            col.append(std::meta::identifier_of(Member));
            col.append(" ");
            if constexpr (constexpr auto bound = storm::meta::max_length_of(Member); bound.has_value()) {
                constexpr bool nullable = storm::orm::utilities::is_optional_v<FieldType>;
                detail::append_max_length_type<D, nullable>(col, bound.value());
                detail::append_default_clause<T, Member, D>(col);
                detail::append_max_length_check<D>(col, std::meta::identifier_of(Member), bound.value());
            } else {
                col.append(detail::sql_col_def<FieldType, D>());
                detail::append_default_clause<T, Member, D>(col);
            }
            if constexpr (Unique) {
                col.append(" UNIQUE");
            }
        }

        // Append the column-level primary-key definition of a SINGLE-PK model. PG ignores
        // the SQLite-only AUTOINCREMENT opt-in (#379). SQLite default: plain INTEGER
        // PRIMARY KEY auto-assigns ids (aliases rowid) without the per-insert
        // sqlite_sequence cost; AUTOINCREMENT (never-reuse) is opt-in via
        // storm::primary_autoincrement.
        //
        // The column name is the PK member's own identifier (#506), not the literal "id" —
        // a model whose PK is not called "id" now gets DDL that agrees with the DML side
        // (base.cppm's pk_name_), which has always used the real identifier.
        template <Dialect D, typename SqlT> static consteval void append_single_pk_column_def(SqlT& col) {
            col.append(Base::pk_name_);
            // A storm::UUID PK is client-generated (no AUTOINCREMENT/IDENTITY): emit the
            // dialect UUID/TEXT type followed by PRIMARY KEY (#507). Integer PKs keep the
            // existing DB-generated identity/autoincrement DDL, byte-identical.
            if constexpr (Base::has_uuid_pk_()) {
                col.append(" ");
                col.append(detail::uuid_type<D>()); // see append_fk_column_def on the qualification
                col.append(" PRIMARY KEY");
            } else if constexpr (D == Dialect::PostgreSQL) {
                // This BIGINT is hardcoded, not width-derived from the PK member's own
                // declared C++ type (#603) — three OTHER places assume that and must be
                // kept in agreement if this literal ever changes: has_pinned_bigint_pk
                // (this branch is exactly its true case for a non-composite, non-UUID
                // PK), part_pinned_bigint (the composite/junction-part sibling), and
                // append_fk_column_def's own hardcoded fk_suffixes table (the per-field
                // FK column, which mirrors this PK's type by an independent literal).
                static_assert(
                        detail::has_pinned_bigint_pk<T>(),
                        "append_single_pk_column_def's hardcoded PG BIGINT disagrees with "
                        "has_pinned_bigint_pk for this model's PK shape"
                );
                col.append(" BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY");
            } else if constexpr (pk_wants_autoincrement()) {
                col.append(" INTEGER PRIMARY KEY AUTOINCREMENT");
            } else {
                col.append(" INTEGER PRIMARY KEY");
            }
        }

        // Append the column definition for the field at compile-time index straight into
        // the CREATE TABLE sink (#682) — no per-column buffer, and no separate size
        // estimate: build_create_table_sql measures the whole statement by running this
        // same writer against the counting sink first, so it cannot truncate.
        template <std::size_t Index, Dialect D, typename SqlT> static consteval void append_column_def(SqlT& col) {
            constexpr auto member = Base::all_members_[Index];

            // Column-level primary key — SINGLE-PK models only. A composite key (#500) is
            // emitted once as a table-level PRIMARY KEY (a, b) clause after all columns
            // (see append_composite_pk_clause), so each of its parts falls through to the
            // regular-column branch below: a part genuinely IS an ordinary column here.
            // The has_composite_pk_ guard also keeps composite models on the table-level
            // clause path rather than this single-column one. Autoincrement cannot reach a
            // composite model at all: ModelPrimaryKeyValid rejects primary_autoincrement
            // combined with primary_part, since SQL cannot spell an autoincrementing column
            // inside a multi-column key on either backend.
            if constexpr (!Base::has_composite_pk_ && Base::all_members_[Index] == Base::primary_key_) {
                append_single_pk_column_def<D>(col);
            }
            // FK field — "<name>_id INTEGER/BIGINT [NOT NULL] REFERENCES <Related>(<pk>)" (#412),
            // where <pk> is the target's real PK member identifier (#506).
            // A composite-PK target (#504) has no single PK column to reference, so it
            // takes a DIFFERENT shape: one plain column per target PK part here
            // (append_composite_fk_columns), plus a table-level FOREIGN KEY (...) constraint
            // appended after every column (append_create_table_sql, mirroring the composite-PK clause).
            // Checked BEFORE the single-column FK branch: append_fk_column_def's
            // find_fk_primary_key names one column, which a composite target does not have.
            else if constexpr (detail::has_composite_fk_target<Base, Index>()) {
                detail::append_composite_fk_columns<detail::composite_fk_related_t<Base, Index>, member, D>(col);
            } else if constexpr (Base::is_fk_field(member)) {
                append_fk_column_def<member, D>(col);
            }
            // full_unsigned field (#436) — order-preserving NUMERIC(20,0)/TEXT, regardless
            // of the C++ uint64 type. Checked before the type-driven branches because the
            // storage class comes from the annotation, not storage_class_of<FieldType>.
            else if constexpr (storm::meta::has_full_unsigned_attr(member)) {
                using FieldType = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
                col.append(std::meta::identifier_of(member));
                col.append(" ");
                if constexpr (storm::orm::utilities::is_optional_v<FieldType>) {
                    col.append(detail::full_unsigned_type<D>());
                } else {
                    col.append(detail::full_unsigned_type_nn<D>());
                }
            }
            // Unique field — same as regular but with UNIQUE constraint
            else if constexpr (Base::is_unique_field(member)) {
                append_regular_column_def<member, D, /*Unique=*/true>(col);
            }
            // Regular field
            else {
                append_regular_column_def<member, D, /*Unique=*/false>(col);
            }
        }

        // Write the full CREATE TABLE SQL into any sink with the ConstexprString/ClauseSizer
        // `.append` interface, using an index sequence fold over the columns.
        template <Dialect D, typename SqlT, std::size_t... Is>
        static consteval void append_create_table_sql(SqlT& sql, std::index_sequence<Is...> /*unused*/) {
            sql.append("CREATE TABLE ");
            sql.append(Base::table_name_);
            sql.append(" (\n");

            bool first = true;
            (([&] {
                 if (!first) {
                     sql.append(",\n");
                 }
                 sql.append("    ");
                 append_column_def<Is, D>(sql);
                 first = false;
             }()),
             ...);

            detail::append_composite_pk_clause(sql, Base::primary_key_members_, Base::has_composite_pk_);
            // One table-level FOREIGN KEY (...) constraint per composite-FK member (#504),
            // in declaration order — the multi-column counterpart of the column-level
            // REFERENCES a single-column FK gets inline in append_column_def.
            (
                    [&] {
                        if constexpr (detail::has_composite_fk_target<Base, Is>()) {
                            detail::append_composite_fk_constraint<
                                    detail::composite_fk_related_t<Base, Is>,
                                    Base::all_members_[Is],
                                    D>(sql);
                        }
                    }(),
                    ...
            );
            sql.append("\n)");
        }

        // Exact rendered length of the CREATE TABLE SQL: the writer run against the
        // counting sink, so the buffer is measured by the SAME code that fills it.
        template <Dialect D> static consteval auto create_table_sql_len() -> std::size_t {
            detail::ClauseSizer sizer;
            append_create_table_sql<D>(sizer, std::make_index_sequence<Base::field_count_>{});
            return sizer.len;
        }

        template <Dialect D = Dialect::SQLite> static consteval auto build_create_table_sql() {
            // +1 for ConstexprString's NUL terminator (its append stops at len < N - 1),
            // as in build_junction_sql.
            constexpr std::size_t size = create_table_sql_len<D>() + 1;
            ConstexprString<size> sql;
            append_create_table_sql<D>(sql, std::make_index_sequence<Base::field_count_>{});
            // Backstop (#361): ConstexprString::append drops overflow SILENTLY, so a sink
            // that ever counted differently from the writer would truncate the DDL with no
            // diagnostic. In a consteval context a hit fails the build instead.
            if (sql.len != size - 1) {
                std::unreachable();
            }
            return sql;
        }

        // =====================================================================
        // INDEX SQL GENERATION
        // =====================================================================

        // Index SQL buffer sized from reflected lengths (#355)
        // so a long table/field name can never silently truncate. Sums EVERY field
        // name x2 (+"_id"+", " worst case), strictly bounding both the single-field
        // and composite builders; 64 covers table*2 + "CREATE UNIQUE INDEX ... ON
        // (...)" scaffolding + '\0'. Correct-by-construction — no truncation possible.
        //
        // A composite-FK member's index (#504) names N columns ("<name>_a, <name>_b, ...")
        // instead of the single "<name>_id" the (name+3+2)*2 term budgets for — folded in
        // via detail::composite_fk_index_extra_size<Base>(), 0 for every non-composite member.
        static consteval auto index_sql_buffer() -> std::size_t {
            std::size_t names = 0;
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                names += (std::meta::identifier_of(Base::all_members_[i]).size() + 3 + 2) * 2;
            }
            std::size_t composite_extra = detail::composite_fk_index_extra_size<Base>();
            return (Base::table_name_.size() * 2) + names + composite_extra + 64;
        }
        static constexpr std::size_t INDEX_SQL_BUFFER = index_sql_buffer();

        // Append the shared tail of a CREATE INDEX statement:
        //   <prefix>idx_<table>_<field><suffix> ON <table>(<field><column_suffix>)
        // The three callers differ only in the leading keyword ("CREATE INDEX" vs
        // "CREATE UNIQUE INDEX") and in whether FK fields get the "_id" decoration.
        template <typename SqlT>
        static consteval void append_index_sql(
                SqlT& sql, std::string_view prefix, std::string_view field_name, std::string_view column_suffix
        ) {
            sql.append(prefix);
            sql.append(Base::table_name_);
            sql.append("_");
            sql.append(field_name);
            sql.append(column_suffix);
            sql.append(" ON ");
            sql.append(Base::table_name_);
            sql.append("(");
            sql.append(field_name);
            sql.append(column_suffix);
            sql.append(")");
        }

        // Append "CREATE INDEX IF NOT EXISTS idx_<table>_<name> ON <table>(<name>_a,
        // <name>_b, ...)" for a composite-FK member (#504) — the multi-column
        // counterpart of append_index_sql's single "<name>_id" column. All N target PK
        // parts share one index (matching the composite-PK table-level PRIMARY KEY's own
        // shape, rather than one single-column index per part).
        template <std::size_t Index, typename SqlT> static consteval void append_composite_fk_index_sql(SqlT& sql) {
            using RelatedBase      = storm::orm::statements::BaseStatement<detail::composite_fk_related_t<Base, Index>>;
            constexpr auto member  = Base::all_members_[Index];
            constexpr auto targets = RelatedBase::primary_key_members_;
            sql.append("CREATE INDEX IF NOT EXISTS idx_");
            sql.append(Base::table_name_);
            sql.append("_");
            sql.append(std::meta::identifier_of(member));
            sql.append(" ON ");
            sql.append(Base::table_name_);
            sql.append("(");
            for (std::size_t i = 0; i < targets.size(); ++i) {
                if (i > 0) {
                    sql.append(", ");
                }
                sql.append(std::meta::identifier_of(member));
                sql.append("_");
                sql.append(std::meta::identifier_of(targets[i]));
            }
            sql.append(")");
        }

        // Build CREATE INDEX SQL for a single field at compile-time
        template <std::size_t Index> static consteval auto build_create_index_sql() {
            ConstexprString<INDEX_SQL_BUFFER> sql;
            constexpr auto                    member = Base::all_members_[Index];

            if constexpr (!Base::needs_index(member)) {
                return sql;
            } else if constexpr (Base::is_unique_field(member)) {
                append_index_sql(sql, "CREATE UNIQUE INDEX IF NOT EXISTS idx_", std::meta::identifier_of(member), "");
            } else if constexpr (detail::has_composite_fk_target<Base, Index>()) {
                append_composite_fk_index_sql<Index>(sql);
            } else if constexpr (Base::is_fk_field(member)) {
                append_index_sql(sql, "CREATE INDEX IF NOT EXISTS idx_", std::meta::identifier_of(member), "_id");
            } else {
                append_index_sql(sql, "CREATE INDEX IF NOT EXISTS idx_", std::meta::identifier_of(member), "");
            }
            return sql;
        }

        // Count how many fields need indexes at compile-time
        static consteval auto count_indexes() -> std::size_t {
            std::size_t count = 0;
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                if (Base::needs_index(Base::all_members_[i])) {
                    ++count;
                }
            }
            return count;
        }

        // Collect all index SQL strings into a vector at runtime (from constexpr data)
        template <std::size_t... Is>
        static auto build_index_sql_vector(std::index_sequence<Is...> /*unused*/) -> std::vector<std::string> {
            std::vector<std::string> result;
            result.reserve(count_indexes());
            (([&] {
                 constexpr auto sql = build_create_index_sql<Is>();
                 if constexpr (sql.len > 0) {
                     result.emplace_back(std::string(sql));
                 }
             }()),
             ...);
            return result;
        }

        // Build CREATE INDEX SQL for a single composite index type at compile-time
        template <typename IdxType> static consteval auto build_composite_index_sql() {
            ConstexprString<INDEX_SQL_BUFFER> sql;
            if constexpr (IdxType::unique) {
                sql.append("CREATE UNIQUE INDEX IF NOT EXISTS idx_");
            } else {
                sql.append("CREATE INDEX IF NOT EXISTS idx_");
            }
            sql.append(Base::table_name_);
            for (std::size_t i = 0; i < IdxType::fields.size(); ++i) {
                sql.append("_");
                sql.append(std::meta::identifier_of(IdxType::fields[i]));
            }
            sql.append(" ON ");
            sql.append(Base::table_name_);
            sql.append("(");
            for (std::size_t i = 0; i < IdxType::fields.size(); ++i) {
                if (i > 0) {
                    sql.append(", ");
                }
                storm::meta::append_column_name(sql, IdxType::fields[i]); // #422 — emits FK "_id"
            }
            sql.append(")");
            return sql;
        }

        // Collect composite index SQL strings into a vector
        template <typename Tuple, std::size_t... Is>
        static auto build_composite_index_sql_vector(std::index_sequence<Is...> /*unused*/)
                -> std::vector<std::string> {
            std::vector<std::string> result;
            ((result.emplace_back(std::string(build_composite_index_sql<std::tuple_element_t<Is, Tuple>>()))), ...);
            return result;
        }

        static auto build_all_index_sql() -> std::vector<std::string> {
            auto result         = build_index_sql_vector(std::make_index_sequence<Base::field_count_>{});
            using CompositeIdxs = storm::indexes_t<T>;
            if constexpr (std::tuple_size_v<CompositeIdxs> > 0) {
                auto composite = build_composite_index_sql_vector<CompositeIdxs>(
                        std::make_index_sequence<std::tuple_size_v<CompositeIdxs>>{}
                );
                result
                        .insert(result.end(),
                                std::make_move_iterator(composite.begin()),
                                std::make_move_iterator(composite.end()));
            }
            return result;
        }

        // Pre-computed index SQL strings
        static inline const std::vector<std::string> index_sql_strings_ = build_all_index_sql();

        // =====================================================================
        // AUTO-JUNCTION TABLE for many_to_many fields without a through model (#203)
        // =====================================================================

        // Scans RAW members — all_members_ excludes m2m fields by design.
        // A model may carry several auto-junction m2m fields (#392), each one
        // producing its own junction table.
        static constexpr std::size_t m2m_auto_count_ = []() consteval {
            std::size_t count = 0;
            for (auto member : std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())) {
                count += statements::meta::is_m2m_auto(member) ? 1U : 0U;
            }
            return count;
        }();

        static constexpr auto m2m_auto_members_ = []() consteval {
            std::array<std::meta::info, m2m_auto_count_> members{};
            std::size_t                                  idx = 0;
            for (auto member : std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())) {
                if (statements::meta::is_m2m_auto(member)) {
                    members[idx++] = member;
                }
            }
            return members;
        }();

        // Junction DDL: <T>_<Related> (<T>_id, <Related>_id, PRIMARY KEY (both),
        // FOREIGN KEY each side ON DELETE <action>). The composite PK rejects duplicate
        // relation pairs and doubles as the index; the FKs enforce referential integrity (#412).
        // The ON DELETE action is CASCADE unless the m2m field carries
        // many_to_many<RefAction::...> (#431), which overrides BOTH sides.
        //
        // Composite-PK sides (#504): a side whose model has a multi-column primary key
        // (#500) contributes ONE junction column PER key part instead of a single
        // "<Side>_id" — see detail::append_junction_side_column_name for the naming and the
        // reason the single-PK spelling stays a hardcoded special case rather than a
        // degenerate instance of the general rule.
        template <Dialect D, std::meta::info Member> static consteval auto build_junction_sql() {
            constexpr auto related = statements::meta::related_type_from_container(std::meta::type_of(Member));
            constexpr std::string_view action =
                    statements::meta::ref_action_sql(statements::meta::m2m_junction_on_delete_of(Member));
            using RelatedType = typename[:related:];
            // Exact budget: the writer is run against the counting sink (ClauseSizer),
            // so the buffer is measured from the SAME code that fills it. The pre-#504
            // "5x name length + 256" heuristic had no term scaling with PK part count or
            // part-identifier length, and ConstexprString truncates SILENTLY on overflow.
            //
            // The +1 is ConstexprString's NUL terminator: its append stops at
            // `len < N - 1`, so a buffer of exactly the rendered length holds one byte
            // FEWER than it needs and silently drops the final character. Caught by the
            // single-PK byte-identical regression test, which lost its closing ")".
            constexpr std::size_t size = detail::junction_sql_size<T, RelatedType, D>(action) + 1;
            ConstexprString<size> sql;
            detail::append_junction_sql<T, RelatedType, D>(sql, action);
            return sql;
        }

        // Rewrites "CREATE TABLE " → "CREATE TABLE IF NOT EXISTS " (idempotent DDL).
        static auto with_if_not_exists(std::string sql) -> std::string {
            const std::string create_prefix = "CREATE TABLE ";
            if (sql.starts_with(create_prefix)) {
                sql.insert(create_prefix.size(), "IF NOT EXISTS ");
            }
            return sql;
        }

      public:
        // True when T has a many_to_many field with an auto-generated junction table.
        static constexpr bool has_m2m_junction_ = m2m_auto_count_ > 0;

        // Pre-computed junction CREATE TABLE SQL statements for the given
        // dialect — one per auto-junction m2m field, in declaration order
        // (#203 single relation; #392 several).
        template <Dialect D = Dialect::SQLite>
        static auto junction_table_sqls() -> const std::vector<std::string>&
            requires has_m2m_junction_
        {
            static const std::vector<std::string> sqls = []() {
                std::vector<std::string> out;
                out.reserve(m2m_auto_count_);
                [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                    (out.emplace_back(std::string(build_junction_sql<D, m2m_auto_members_[Is]>())), ...);
                }(std::make_index_sequence<m2m_auto_count_>{});
                return out;
            }();
            return sqls;
        }

        // Pre-computed junction CREATE TABLE SQL for the first (often only)
        // auto-junction m2m field (#203).
        template <Dialect D = Dialect::SQLite>
        static auto junction_table_sql() -> const std::string&
            requires has_m2m_junction_
        {
            return junction_table_sqls<D>()[0];
        }
        // Return the pre-computed CREATE TABLE SQL for the given dialect.
        //
        // Function-local per dialect, NOT a pair of class-scope static constexpr members
        // (#670): in practice Clang evaluates a static constexpr data member's initializer
        // together with the class, so every TU that instantiated SchemaStatement<T> for any
        // reason paid the consteval DDL build for BOTH dialects. Here a dialect's DDL is
        // generated only in a TU that asks for it. The SQL is still generated at compile
        // time (the constexpr local); the std::string copy is made on first call (a
        // thread-safe function-local static, like junction_table_sqls) instead of during
        // unordered dynamic initialization at startup.
        template <Dialect D = Dialect::SQLite> static auto create_table_sql() -> const std::string& {
            static const std::string sql = [] {
                static constexpr auto arr = build_create_table_sql<D>();
                return std::string(arr);
            }();
            return sql;
        }

        // Return pre-computed CREATE INDEX SQL statements for all indexed/unique/FK fields.
        static auto create_index_sql() -> const std::vector<std::string>& {
            return index_sql_strings_;
        }

        // Execute all CREATE INDEX IF NOT EXISTS statements on the given connection.
        // Defaults to the calling thread's default connection for ConnType.
        template <db::DatabaseConnection ConnType>
        [[nodiscard]] static auto
        create_indexes_if_not_exist(std::shared_ptr<ConnType> conn = db::BasicDefaultConnection<ConnType>::get())
                -> std::expected<void, typename ConnType::Error> {
            for (const auto& sql : index_sql_strings_) {
                auto result = conn->execute(sql);
                if (!result) {
                    return result;
                }
            }
            return {};
        }

        // Create each FK-referenced parent table before T's own table (#412). With
        // always-on REFERENCES a table cannot be created before its FK target exists
        // (PG rejects it; SQLite tolerated it). `done` is the set of already-created
        // table names — it makes the recursion idempotent and cycle-safe (self-FK,
        // mutual FK). Index fold so each FK target type is recursed at compile time.
        template <db::DatabaseConnection ConnType>
        [[nodiscard]] static auto
        create_fk_parents(std::shared_ptr<ConnType> conn, std::set<std::string, std::less<>>& done)
                -> std::expected<void, typename ConnType::Error> {
            std::expected<void, typename ConnType::Error> result{};
            [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                (
                        [&] {
                            if constexpr (Base::is_fk_field(Base::all_members_[Is])) {
                                using FieldType =
                                        std::remove_cvref_t<typename[:std::meta::type_of(Base::all_members_[Is]):]>;
                                using RelatedType = detail::col_inner_t<FieldType>;
                                result            = result.and_then([&] {
                                    return SchemaStatement<RelatedType>::create_table_if_not_exists(conn, done);
                                });
                            }
                        }(),
                        ...
                );
            }(std::make_index_sequence<Base::field_count_>{});
            return result;
        }

        // Create the related model's table for each auto-junction m2m field (#412). The
        // junction FK-references both T (already created) and the related model, so the
        // related table must exist before the junction DDL runs. Recurses through the same
        // `done` cycle guard. Only instantiated when has_m2m_junction_.
        template <db::DatabaseConnection ConnType>
        [[nodiscard]] static auto
        create_m2m_related_tables(std::shared_ptr<ConnType> conn, std::set<std::string, std::less<>>& done)
                -> std::expected<void, typename ConnType::Error>
            requires has_m2m_junction_
        {
            std::expected<void, typename ConnType::Error> result{};
            [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                (
                        [&] {
                            using Container =
                                    std::remove_cvref_t<typename[:std::meta::type_of(m2m_auto_members_[Is]):]>;
                            using RelatedType = statements::meta::m2m_related_t<Container>;
                            result            = result.and_then([&] {
                                return SchemaStatement<RelatedType>::create_table_if_not_exists(conn, done);
                            });
                        }(),
                        ...
                );
            }(std::make_index_sequence<m2m_auto_count_>{});
            return result;
        }

        // Execute CREATE TABLE IF NOT EXISTS on the given connection (recursion overload).
        // `done` tracks tables already created in this call tree (cycle/idempotency guard).
        template <db::DatabaseConnection ConnType>
        [[nodiscard]] static auto
        create_table_if_not_exists(std::shared_ptr<ConnType> conn, std::set<std::string, std::less<>>& done)
                -> std::expected<void, typename ConnType::Error> {
            constexpr Dialect dialect = db::SupportsPgDialect<ConnType> ? Dialect::PostgreSQL : Dialect::SQLite;

            // Already created in this call tree — break FK cycles and avoid redundant DDL.
            if (!done.insert(std::string(Base::table_name_)).second) {
                return {};
            }

            // FK-referenced parent tables first, then T's own table (#412).
            auto result = create_fk_parents(conn, done).and_then([&conn] {
                return conn->execute(with_if_not_exists(create_table_sql<dialect>()));
            });
            result      = result.and_then([&conn] { return create_indexes_if_not_exist(conn); });

            // Auto-generated junction tables for many_to_many fields — one per
            // auto-junction field (#203 Phase 1; #392 several relations). Each junction
            // FK-references both T and the related model, so the related tables must
            // exist first (#412).
            if constexpr (has_m2m_junction_) {
                result = result.and_then([&conn, &done] { return create_m2m_related_tables(conn, done); });
                for (const auto& junction_sql : junction_table_sqls<dialect>()) {
                    result = result.and_then([&conn, &junction_sql] {
                        return conn->execute(with_if_not_exists(junction_sql));
                    });
                }
            }
            return result;
        }

        // Execute CREATE TABLE IF NOT EXISTS on the given connection.
        // Selects the appropriate dialect at compile time. FK-referenced parent tables
        // are created first so always-on referential integrity (#412) is satisfied.
        // Defaults to the calling thread's default connection for ConnType.
        template <db::DatabaseConnection ConnType>
        [[nodiscard]] static auto
        create_table_if_not_exists(std::shared_ptr<ConnType> conn = db::BasicDefaultConnection<ConnType>::get())
                -> std::expected<void, typename ConnType::Error> {
            std::set<std::string, std::less<>> done;
            return create_table_if_not_exists(conn, done);
        }
    };

} // namespace storm::orm::schema

export namespace storm {

    // Returns the compile-time generated CREATE TABLE SQL for model T and dialect D.
    // D defaults to SQLite regardless of backend: pass Dialect::PostgreSQL for PG DDL.
    template <typename T, orm::schema::Dialect D = orm::schema::Dialect::SQLite>
    [[nodiscard]] auto create_table_sql() -> const std::string& {
        return orm::schema::SchemaStatement<T>::template create_table_sql<D>();
    }

    // Returns the pre-computed CREATE INDEX SQL statements for model T.
    template <typename T> [[nodiscard]] auto create_index_sql() -> const std::vector<std::string>& {
        return orm::schema::SchemaStatement<T>::create_index_sql();
    }
} // namespace storm
