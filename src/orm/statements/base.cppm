module;

// Single cohesive class template; thresholds intentionally relaxed (see #264 finding).
// `duplicate` removed in #277 Phase 3 (bind_bulk_objects_impl + bind_expr_or_reset helpers; the
// for_each_field_name dedup now lives in storm_orm_statements_field_names, #434).

#include <meta>

export module storm_orm_statements_base;

import std;

import storm_db_concept;
import storm_orm_field_attr;
import storm_orm_utilities;
import storm_orm_statements_extract;
import storm_orm_statements_field_names;
import storm_orm_statements_orderby;
import storm_orm_where;

// Model-validation concepts (ModelWithPrimaryKey, PrimaryKeyType, ...) + the meta:: re-exports
// and relation/FK reflection helpers they rest on. Re-exported so every statement module that
// imports this one keeps seeing those names unchanged.
export import storm_orm_model_concepts;

export namespace storm::orm::statements {

    // Import utilities for compile-time string building
    using storm::orm::utilities::ConstexprString; // NOLINT(misc-unused-using-decls) — re-exported to statement modules

    // Shared reflection utilities for all statement types
    template <typename T>
        requires storm::meta::Entity<T> && ModelWithPrimaryKey<T> && PrimaryKeyType<T> && ModelStorageAnnotated<T> &&
                 ModelFkPoliciesValid<T> && ModelAnnotationsValid<T> && ModelMaxLengthValid<T> &&
                 ModelPrimaryKeyValid<T> && ModelTimestampPkValid<T> && ModelPrimaryKeyPartLimit<T>
    class BaseStatement {
      public:
        // Compile-time accessor for table name (used in SQL generation)
        static consteval auto get_table_name() -> std::string_view {
            return table_name_;
        }

      protected:
        // Helper to find primary key using storm::meta attributes
        static consteval auto find_primary_key_impl() -> std::meta::info {
            return meta::find_primary_key<T>(); // ModelWithPrimaryKey<T> guarantees a primary key exists
        }

        // True when `member` is a primary-key COLUMN — a PK annotation on a member that is
        // actually persisted. m2m / reverse_fk containers are relation members, never
        // columns, so a PK annotation on one must not enter the key: it would put a name
        // in PRIMARY KEY (...) that no column definition ever emits (the #485 class of
        // leak). ModelPrimaryKeyValid rejects that model outright; this keeps the helpers
        // below consistent with all_members_ regardless.
        static consteval auto is_primary_key_column(std::meta::info member) -> bool {
            return meta::is_primary_member(member) && !meta::is_relation_field(member);
        }

        // How many members make up the primary key (#500) — 1 for a single-PK model,
        // N >= 2 for a composite one. Sizes the primary_key_members_ array below, so it
        // must be a separate consteval function (an array bound needs the count before
        // the elements are computed).
        static consteval auto primary_key_count() -> std::size_t {
            std::size_t count = 0;
            for (const std::meta::info member : storm::meta::data_members_of_v<T>) {
                if (is_primary_key_column(member)) {
                    ++count;
                }
            }
            return count;
        }

        // Every primary-key member, in DECLARATION order — the order the table-level
        // PRIMARY KEY (a, b) clause emits, since column order is semantically
        // significant for the index it creates. For a single-PK model this is a
        // 1-element array whose sole element is primary_key_, so the existing
        // primary_key_ / pk_name_ statics (read by ~13 files) keep working unchanged.
        static consteval auto find_primary_key_members_impl() -> std::array<std::meta::info, primary_key_count()> {
            std::array<std::meta::info, primary_key_count()> members{};
            std::size_t                                      index = 0;
            for (const std::meta::info member : storm::meta::data_members_of_v<T>) {
                if (is_primary_key_column(member)) {
                    members[index++] = member;
                }
            }
            return members;
        }

        // FK field detection utilities — an fk<...> class-template annotation (#431).
        static consteval auto is_fk_field(std::meta::info member) -> bool {
            return meta::is_fk_field(member);
        }

      public:
        // True when `member` is ANY part of the primary key (#501) — the widened form of
        // the `member == primary_key_` test that the UPDATE and upsert SET-target gates
        // used. On a composite model that old test excluded only the FIRST part, leaving
        // every other part writable: `SET product_id=? WHERE order_id=? AND product_id=?`
        // would rewrite part of the very key it matches on. Identical to the old test on a
        // single-PK model (the array has one element). Public because UpdateGrammar and
        // UpsertGrammar are separate types, not subclasses.
        static consteval auto is_pk_member(std::meta::info member) -> bool {
            // A CALLED consteval any_of lambda reports as uncovered against the 100%
            // line-coverage gate; the plain loop does not.
            for (const std::meta::info pk : primary_key_members_) { // NOLINT(readability-use-anyofallof)
                if (pk == member) {
                    return true;
                }
            }
            return false;
        }

      protected:
        // Per-attribute predicates forward to the storm_orm_field_attr leaf (#421),
        // the single source of truth for each flag-annotation test.
        static consteval auto is_unique_field(std::meta::info member) -> bool {
            return meta::is_unique(member);
        }

        static consteval auto is_indexed_field(std::meta::info member) -> bool {
            return meta::is_indexed(member);
        }

        static consteval auto is_auto_create_field(std::meta::info member) -> bool {
            return meta::is_auto_create(member);
        }

        static consteval auto is_auto_update_field(std::meta::info member) -> bool {
            return meta::is_auto_update(member);
        }

        // True when `member` should be stamped with now() on this operation: auto_update
        // always; auto_create on INSERT only (IsUpdate=false). auto_create on UPDATE is
        // false here so it binds the object's stored value (preserving created_at).
        static consteval auto stamps_now(std::meta::info member, bool is_update) -> bool {
            return is_auto_update_field(member) || (is_auto_create_field(member) && !is_update);
        }

        // Check if a field needs an index (indexed, unique, or fk — but not primary key).
        // FK is now an fk<...> class-template annotation (#431), checked separately.
        // Every PK member is excluded, not just primary_key_: the table-level
        // PRIMARY KEY (a, b) of a composite key (#500) already indexes each part, so a
        // separate single-column CREATE INDEX on a part would be redundant.
        static consteval auto needs_index(std::meta::info member) -> bool {
            if (meta::is_primary_member(member)) {
                return false;
            }
            if (meta::is_fk_field(member)) {
                return true;
            }
            return meta::is_indexed(member) || meta::is_unique(member);
        }

        // Get database column name for FK field: User sender → "sender_id".
        // Routes through the canonical column-name writer (#422) so the "_id" suffix
        // stays single-sourced (consteval — std::format is not constant-evaluated).
        static consteval auto get_fk_column_name(std::meta::info member) -> std::string {
            std::string name;
            meta::append_column_name(name, member);
            return name;
        }

      public:
        // Find primary key of a FK type (unwraps std::optional<T> → T first). Public so
        // the free two-query join helpers (join.cppm / join_grammar.cppm, #398) can extract FK columns.
        template <typename FKType>
            requires ValidForeignKey<FKType>
        static consteval auto find_fk_primary_key() -> std::meta::info {
            using InnerType = utilities::optional_inner_type_t<FKType>;
            for (const std::meta::info member : storm::meta::data_members_of_v<InnerType>) {
                if (meta::is_primary_member(member)) {
                    return member;
                }
            }
            std::unreachable(); // never reached: requires ModelWithPrimaryKey<...> guarantees a primary key exists
        }

        // Widened form of find_fk_primary_key (#504): returns the FK target's FULL
        // primary-key member list — 1 element for a single-column target (matching
        // find_fk_primary_key exactly), N for a composite target. Existing single-FK
        // call sites keep using find_fk_primary_key unchanged (byte-identical splice
        // shape); this is used only by the new composite-FK bind/extract/JOIN paths.
        template <typename FKType>
            requires ValidForeignKey<FKType>
        static consteval auto fk_primary_key_count() -> std::size_t {
            using InnerType = utilities::optional_inner_type_t<FKType>;
            return BaseStatement<InnerType>::primary_key_column_count_;
        }

        template <typename FKType>
            requires ValidForeignKey<FKType>
        static consteval auto find_fk_primary_key_members()
                -> std::array<std::meta::info, fk_primary_key_count<FKType>()> {
            using InnerType = utilities::optional_inner_type_t<FKType>;
            return BaseStatement<InnerType>::primary_key_members_;
        }

      protected:
        // Number of PERSISTED fields. Relation container members (many-to-many #203,
        // reverse_fk #398) map to a separate query, not to a column, so they are
        // invisible to INSERT/SELECT/UPDATE/SCHEMA.
        static consteval auto get_field_count() -> std::size_t {
            std::size_t count = 0;
            for (const auto member : storm::meta::data_members_of_v<T>) {
                if (!meta::is_relation_field(member)) {
                    ++count;
                }
            }
            return count;
        }

        // Pre-compute all persisted field members at compile-time (relation members
        // filtered — m2m #203, reverse_fk #398).
        template <std::size_t N> static consteval auto get_all_field_members() -> std::array<std::meta::info, N> {
            std::array<std::meta::info, N> result{};
            std::size_t                    idx = 0;
            for (const auto member : storm::meta::data_members_of_v<T>) {
                if (!meta::is_relation_field(member) && idx < N) {
                    result[idx++] = member;
                }
            }
            return result;
        }

        // Field-name SQL grammar (#434): for_each_field_name + the size-calculators and
        // list-builders that produce "col1, col2, fk_id, ..." now live in
        // FieldNameGrammar<BaseStatement>. INSERT/SELECT call it directly; this class uses
        // it only to seed field_names_array_ below.
        using FieldNames = FieldNameGrammar<BaseStatement>;

      public:
        // Pre-computed field information - made public for QuerySet and JOIN optimization
        static constexpr auto field_count_ = get_field_count();
        // Makes the ModelWithPrimaryKey invariant explicit: a model with a primary-key
        // member always has >= 1 field, so the INSERT divides MAX_DB_VARIABLES / max(field_count_, placeholders)
        // can never divide by zero (issue #362, item A). Fires here if the concept is ever
        // loosened, instead of producing UB at the divide.
        static_assert(field_count_ >= 1, "A model must have at least one field (its primary key)");
        static constexpr auto           all_members_       = get_all_field_members<field_count_>();
        static constexpr auto           field_names_array_ = FieldNames::build_all_field_names_list();
        static inline const std::string field_names_       = std::string(field_names_array_);

        // True if T has any auto_create/auto_update field (#209). Gates the per-operation
        // system_clock::now() read so models with no timestamp fields pay zero overhead.
        static constexpr bool has_auto_timestamp_field_ = []() consteval {
            for (const auto m : all_members_) {
                if (is_auto_create_field(m) || is_auto_update_field(m)) {
                    return true;
                }
            }
            return false;
        }();

        // True if T has any many-to-many container field (#203/#391). Gates the
        // two-query eager-load path so models WITHOUT an m2m field never instantiate
        // it (scans raw members — m2m fields are filtered out of all_members_).
        static constexpr bool has_m2m_field_ = []() consteval {
            return std::ranges::any_of(storm::meta::data_members_of_v<T>, [](std::meta::info m) {
                return meta::is_m2m_field(m);
            });
        }();

        // True if T has any reverse_fk container field (#398). Gates the two-query
        // eager-load path the same way has_m2m_field_ does — a model without one
        // never instantiates the reverse-FK select machinery.
        static constexpr bool has_reverse_fk_field_ = []() consteval {
            return std::ranges::any_of(storm::meta::data_members_of_v<T>, [](std::meta::info m) {
                return meta::is_reverse_fk_field(m);
            });
        }();

        // ── PostgreSQL binary result format (#600 Phase 1) ────────────────────
        //
        // Can a statement returning EVERY column of T ask libpq for binary
        // results? The format flag is per statement, not per column, so this is
        // a whole-row AND: one column outside the decodable set puts the entire
        // statement back on the text path it has always used. False costs only
        // speed; true on the wrong model silently corrupts data, so every
        // uncertain shape answers false.
        //
        // Two exclusions live HERE rather than in the type-level predicate,
        // because neither is visible from the C++ type alone:
        //   - a storm::full_unsigned member is PG NUMERIC(20,0) while sharing
        //     std::uint64_t with a signed_storage member (plain BIGINT), so the
        //     two must be told apart by ANNOTATION;
        //   - an FK member's columns are the TARGET's key, not the member's own
        //     type (and a composite target spans N of them, #504). Classifying
        //     those is a straightforward widening, deferred — FK models stay on
        //     text, exactly as today.
        template <std::meta::info Member> static consteval auto member_pg_binary_safe() -> bool {
            if constexpr (is_fk_field(Member) || storm::meta::has_full_unsigned_attr(Member)) {
                return false;
            } else {
                using FieldType = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                return ColumnExtractor::is_pg_binary_safe_column_v<FieldType>;
            }
        }

        template <std::size_t... Is>
        static consteval auto pg_binary_safe_row_impl(std::index_sequence<Is...> /*unused*/) -> bool {
            return (member_pg_binary_safe<all_members_[Is]>() && ...);
        }

        static constexpr bool pg_binary_safe_row_ = pg_binary_safe_row_impl(std::make_index_sequence<field_count_>{});

        // Ask the backend for binary results, when the backend has that knob.
        // PostgreSQL-only by design (issue #600, design question 4): the
        // `requires` probe is false for SQLite — whose sqlite3_column_* API is
        // already native binary — so this compiles away there and neither
        // db::DatabaseStatement nor any generic call site has to know about it.
        //
        // Must be called AFTER prepare_cached (a cache hit reset()s the
        // statement, clearing the flag) and BEFORE the first step.
        template <typename Statement>
        __attribute__((always_inline)) static auto request_binary_results(Statement* stmt, bool binary) noexcept
                -> void {
            if constexpr (requires { stmt->set_result_binary(bool{}); }) {
                stmt->set_result_binary(binary);
            }
        }

        // One clock read per operation, but only for models that actually have a timestamp
        // field — otherwise the call compiles away entirely (no regression on plain models).
        [[nodiscard]] __attribute__((always_inline)) static auto batch_now() noexcept
                -> std::chrono::system_clock::time_point {
            if constexpr (has_auto_timestamp_field_) {
                return std::chrono::system_clock::now();
            } else {
                return {};
            }
        }

        // Reflection data - made public for JOIN statement access
        static constexpr auto primary_key_ = find_primary_key_impl();
        static constexpr auto pk_name_     = std::meta::identifier_of(primary_key_);
        static constexpr auto table_name_  = std::meta::identifier_of(^^T);

        // Composite primary key (#500). primary_key_members_ is the full PK list in
        // declaration order; primary_key_ / pk_name_ above stay the FIRST element, so
        // every single-PK caller is untouched. has_composite_pk_ branches the schema
        // generator (table-level PRIMARY KEY (a, b), #500), the by-key WHERE clause
        // (#501), the INSERT column/bind policy (#502), and the JOIN paths — FK ON
        // clauses, junction DDL, and the m2m/reverse-FK stitch key (#504).
        static constexpr auto primary_key_members_ = find_primary_key_members_impl();
        static constexpr bool has_composite_pk_    = primary_key_members_.size() > 1;

        // UUID primary key support (#507): true iff the single PK member has UUID type
        static consteval auto has_uuid_pk_() -> bool {
            if constexpr (has_composite_pk_ || primary_key_count() == 0) {
                return false; // composite or no PK
            } else {
                constexpr auto pk_type = std::meta::type_of(primary_key_);
                return std::meta::dealias(pk_type) == std::meta::dealias(^^storm::orm::utilities::UUID);
            }
        }

        // Caller-assigned integer primary key support: true iff the single PK
        // member carries storm::primary_assigned. Such a key is never DB-generated —
        // the caller always supplies the value — so INSERT must emit the column and
        // there is nothing to RETURNING.
        static consteval auto has_caller_assigned_pk_() -> bool {
            if constexpr (has_composite_pk_ || primary_key_count() == 0) {
                return false; // composite or no PK
            } else {
                return storm::meta::has_annotation_type<storm::meta::PrimaryAssigned>(primary_key_);
            }
        }

        // Does the DATABASE produce the key's value, so that RETURNING could hand
        // something back the caller does not already have?
        //
        // Only a single-column INTEGER key does. AUTOINCREMENT (SQLite) and
        // GENERATED ... AS IDENTITY (PostgreSQL) are single-integer-column
        // features, so the other key shapes are pure caller data:
        //   - a COMPOSITE key (#502) — every part is supplied by the caller;
        //   - a storm::UUID key (#572) — likewise, and worse than useless to
        //     RETURN: the column is TEXT/UUID, so extracting it as the int64 the
        //     RETURNING path expects yields 0 on SQLite and garbage or an error
        //     on PostgreSQL;
        //   - a caller-assigned integer key — likewise, the caller supplies
        //     the value and there is nothing new to echo back.
        //
        // One named predicate rather than the condition spelled out at each of
        // its decision points (insert's default_return_id/ReturnIdSupported,
        // UpsertGrammar's two RETURNING clauses and their two runners, plus
        // skips_pk_column/FieldNameGrammar::is_skipped_pk, which ask the same
        // question for the INSERT column list and bind order) — a disagreement
        // between them emits a RETURNING clause the runtime does not read, reads
        // one that was never emitted, or misaligns columns against values.
        //
        // ⚠ Written as a BLOCKLIST but load-bearing as an ALLOWLIST. It is
        // equivalent to "single-column INTEGER key" ONLY because
        // is_primary_key_typed_member's whitelist is closed: integrals + UUID +
        // caller-assigned, nothing else. Widen that whitelist — a std::string key,
        // a binary key — and this returns TRUE for the new type, re-emitting
        // RETURNING <text_pk> extracted as an int64: #572 verbatim, and it would
        // ship green because no in-tree model compiles the branch. Widen BOTH
        // together.
        static consteval auto pk_is_db_generated_() -> bool {
            return !has_composite_pk_ && !has_uuid_pk_() && !has_caller_assigned_pk_();
        }

        // How many columns the key spans, as a plain std::size_t (#501). Needed because
        // primary_key_members_ is an array of std::meta::info — a consteval-only type
        // that cannot be named at all in a runtime context, not even via .size(). The
        // bind path advances param_index by this at runtime.
        static constexpr std::size_t primary_key_column_count_ = primary_key_count();

      protected:
        // Index sequence utilities for compile-time field binding
        using field_indices_t = std::make_index_sequence<field_count_>;

        // Helper template for compile-time field binding with index sequence
        template <typename ConnType, typename Statement, std::size_t... Is>
        [[nodiscard]] static auto
        bind_all_fields_impl(Statement& stmt, const T& obj, std::index_sequence<Is...> /*unused*/) noexcept
                -> std::expected<void, typename ConnType::Error> {
            int                                           param_index = 1;
            std::expected<void, typename ConnType::Error> result{};
            const auto now = batch_now(); // shared by all fields of this object (compiles away if none)
            ((result = bind_field_at_index<ConnType, Is>(&stmt, obj, param_index, now), result.has_value()) && ...);
            return result;
        }

        // Helper template for INSERT binding (skips primary key for auto-increment)
        template <typename ConnType, typename Statement, std::size_t... Is>
        [[nodiscard]] static auto
        bind_non_pk_fields_impl(Statement& stmt, const T& obj, std::index_sequence<Is...> /*unused*/) noexcept
                -> std::expected<void, typename ConnType::Error> {
            int                                           param_index = 1;
            std::expected<void, typename ConnType::Error> result{};
            const auto now = batch_now(); // shared by all fields of this object (compiles away if none)
            ((result = bind_field_at_index<ConnType, Is, true>(&stmt, obj, param_index, now), result.has_value()) &&
             ...);
            return result;
        }

        // Does the caller's PK-skip policy exclude `member` from the bind order?
        // SkipAllPK covers every part of a composite key (#501) — the UPDATE policy,
        // whose SET clause omits the whole key. Plain SkipPK is the INSERT policy: it
        // skips the DB-generated key, which only a single-column PK can be — a
        // composite key has no auto-generation mechanism, so every part is caller
        // data and nothing is skipped (#502). The two policies coincide on a
        // single-PK model.
        template <bool SkipPK, bool SkipAllPK> static consteval auto skips_pk_column(std::meta::info member) -> bool {
            if (!SkipPK) {
                return false;
            }
            if (SkipAllPK) {
                return is_pk_member(member);
            }
            // INSERT omits the key column exactly when the DATABASE generates it —
            // the same question pk_is_db_generated_() answers for RETURNING (#502/
            // #507/#572), so it is read here rather than re-spelled. Kept in lockstep
            // with FieldNameGrammar::is_skipped_pk, which makes this decision for the
            // COLUMN LIST; a disagreement misaligns columns against values.
            return pk_is_db_generated_() && member == primary_key_;
        }

        // Unified field binder: binds a single field at compile-time index.
        // SkipPK=true skips primary key fields (for INSERT/UPDATE non-PK binding).
        // IsUpdate=true marks the UPDATE path so auto_create fields bind the object's
        // stored value instead of now() (#209). `now` is read once per operation by the
        // caller and threaded in so every row in a batch shares the same timestamp.
        // Auto-increments param_index on successful bind.
        //
        // SkipAllPK widens the skip from primary_key_ to EVERY primary-key member (#501),
        // which is what UPDATE needs: its SET clause omits the whole composite key, so the
        // bind order must too. INSERT keeps plain SkipPK, which since #502 skips NOTHING
        // on a composite model (see skips_pk_column: every key part is caller data).
        // No effect on single-PK models, where the two flags coincide.
        template <
                typename ConnType,
                std::size_t Index,
                bool        SkipPK    = false,
                bool        IsUpdate  = false,
                bool        SkipAllPK = false>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto bind_field_at_index(
                typename ConnType::Statement*         stmt,
                const T&                              obj,
                int&                                  param_index,
                std::chrono::system_clock::time_point now = {}
        ) noexcept -> std::expected<void, typename ConnType::Error> {
            constexpr auto member = all_members_[Index];

            // Auto-timestamp fields (#209) must be system_clock::time_point — binding now()
            // to any other type is a model error. Fires at the call site with a clear message.
            if constexpr (is_auto_create_field(member) || is_auto_update_field(member)) {
                static_assert(
                        ValidTimestampField<member>,
                        "auto_create / auto_update fields must be std::chrono::system_clock::time_point"
                );
            }

            // Compile-time PK skip for INSERT/UPDATE non-PK paths.
            if constexpr (skips_pk_column<SkipPK, SkipAllPK>(member)) {
                return {};
            }
            // Auto-timestamp (#209): stamp now() for auto_update (always) and auto_create
            // (INSERT only). auto_create on UPDATE is not stamped here and falls through to
            // the normal bind below, preserving the object's stored created_at.
            else if constexpr (stamps_now(member, IsUpdate)) {
                return bind_one<ConnType>(stmt, param_index, now);
            }
            // FK field - extract and bind the PK value from the foreign object
            else if constexpr (is_fk_field(member)) {
                return bind_fk_field_at_index<ConnType, Index>(stmt, obj, param_index);
            }
            // full_unsigned field (#436): order-preserving storage — bind a 20-char
            // zero-padded decimal string instead of the signed int64 hot path. uint64_max
            // (18446744073709551615) is 20 digits, so every value pads to the same width
            // and lexicographic order == numeric order (SQLite TEXT); PG NUMERIC(20,0)
            // parses the same string, leading zeros ignored. Optional binds NULL via the
            // optional<string> dispatch in bind_parameter_value. Slow path (format +
            // bind_text), paid only by full_unsigned fields.
            else if constexpr (storm::meta::has_full_unsigned_attr(member)) {
                using FieldType = std::remove_cvref_t<decltype(obj.[:member:])>;
                if constexpr (utilities::is_optional_v<FieldType>) {
                    return bind_one<ConnType>(
                            stmt, param_index, obj.[:member:]
                                    .has_value()
                                            ? std::optional<std::string>(std::format("{:020}", obj.[:member:].value()))
                                            : std::nullopt
                    );
                } else {
                    return bind_one<ConnType>(stmt, param_index, std::format("{:020}", obj.[:member:]));
                }
            } else {
                return bind_optional_or_uuid_pk_field<ConnType, Index>(stmt, obj, param_index);
            }
        }

        // Bind optional or UUID PK fields. Gates UUID PKs to reject empty.
        // The LCOV_EXCL pair that used to wrap this WHOLE function is gone (#565). It was
        // added when a UUID PK could not reach here at all; both arms are now genuinely
        // executed (the UUID branch 20x, its error return 4x, the else ~1.4M), and a stale
        // exclusion around live code is exactly what let three defects hide in this branch.
        // What remains excluded is only the declaration below and the closing brace: this
        // is always_inline, so lcov merges the inlined copies and the non-UUID
        // instantiations — which enter through the `else` and never materialise these two
        // as executable lines — win, reporting 0 hits on lines that hold no logic. Same
        // unhit-instantiation artifact as the m2m/BenchResult case; excluding the two
        // artifact lines is deliberate, unlike hiding the function body.
        template <typename ConnType, std::size_t Index>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto
        bind_optional_or_uuid_pk_field(typename ConnType::Statement* stmt, const T& obj, int& param_index) noexcept
                -> std::expected<void, typename ConnType::Error> {
            constexpr auto member = all_members_[Index]; // LCOV_EXCL_LINE
            if constexpr (is_pk_member(member) &&
                          std::meta::dealias(std::meta::type_of(member)) == ^^storm::orm::utilities::UUID) {
                // UUID PK: reject empty, no auto-generation. Non-PK UUID columns auto-generate via bind_uuid.
                // bind_pk_scalar_one binds through bind_uuid_pk and advances param_index only
                // on success — omitting the advance bound the next field over the key at the
                // same slot and left the last placeholder unset, a NOT NULL violation naming
                // the WRONG column. Unreachable before this change (the key was skipped).
                return bind_pk_scalar_one<ConnType>(stmt, param_index, obj.[:member:]);
            } else {
                return bind_one<ConnType>(stmt, param_index, obj.[:member:]);
            }
        } // LCOV_EXCL_LINE — see the always_inline note above

        // Bind one value at param_index and advance it on success. Shared tail used by the
        // plain-field and auto-timestamp branches of bind_field_at_index.
        template <typename ConnType>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto
        bind_one(typename ConnType::Statement* stmt, int& param_index, const auto& value) noexcept
                -> std::expected<void, typename ConnType::Error> {
            auto result = bind_value_by_type<ConnType>(*stmt, param_index, value);
            if (!result) {
                return std::unexpected(result.error());
            }
            ++param_index;
            return {};
        }

        // Bind N SQL NULLs starting at param_index, advancing it past all of them —
        // the composite-FK counterpart of a single stmt->bind_null() call, used when an
        // empty std::optional<CompositeFkTarget> must NULL out every target PK-part
        // column (#504). Extracted from bind_fk_field_at_index to keep that function's
        // cognitive complexity under the project's threshold.
        template <typename ConnType>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto
        bind_null_run(typename ConnType::Statement* stmt, int& param_index, std::size_t count) noexcept
                -> std::expected<void, typename ConnType::Error> {
            for (std::size_t i = 0; i < count; ++i) {
                auto null_result = stmt->bind_null(param_index);
                if (!null_result) {
                    return std::unexpected(null_result.error());
                }
                ++param_index;
            }
            return {};
        }

        // Extract and bind the foreign object's PK value (NULL for an empty optional FK).
        //
        // Widened for a composite FK target (#504): the utilities::is_optional_v /
        // is_fk_field dispatch stays exactly as it was for a SINGLE-column target
        // (fk_primary_key_count<FKType>() == 1, the pre-#504 code path, byte-for-byte
        // unchanged below); a composite target (> 1) routes through bind_fk_parts /
        // bind_null_run, which bind every part of the target's primary key and advance
        // param_index past all of them.
        template <typename ConnType, std::size_t Index>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto
        bind_fk_field_at_index(typename ConnType::Statement* stmt, const T& obj, int& param_index) noexcept
                -> std::expected<void, typename ConnType::Error> {
            constexpr auto member = all_members_[Index];
            using FKType          = std::remove_cvref_t<decltype(obj.[:member:])>;
            if constexpr (utilities::is_optional_v<FKType>) {
                // Optional FK: bind NULL(s) when empty, otherwise bind the inner PK value(s)
                if (!obj.[:member:].has_value()) {
                    if constexpr (fk_primary_key_count<FKType>() == 1) {
                        auto null_result = stmt->bind_null(param_index);
                        if (!null_result) {
                            return std::unexpected(null_result.error());
                        }
                        ++param_index;
                        return {};
                    } else {
                        return bind_null_run<ConnType>(stmt, param_index, fk_primary_key_count<FKType>());
                    }
                }
                if constexpr (fk_primary_key_count<FKType>() == 1) {
                    constexpr auto fk_pk_member = find_fk_primary_key<FKType>();
                    return bind_one<ConnType>(stmt, param_index, obj.[:member:].value().[:fk_pk_member:]);
                } else {
                    return bind_fk_parts<ConnType, FKType>(stmt, obj.[:member:].value(), param_index);
                }
            } else {
                if constexpr (fk_primary_key_count<FKType>() == 1) {
                    constexpr auto fk_pk_member = find_fk_primary_key<FKType>();
                    // #608: an FK field that is itself a composite-PK part (the
                    // association-table shape) must reject an unset UUID target key
                    // the same way a bare UUID PK does (bind_optional_or_uuid_pk_field)
                    // — otherwise this falls into bind_one -> bind_value_by_type ->
                    // utilities::bind_uuid, which AUTO-GENERATES a fresh random UUID
                    // and fabricates a foreign key instead of erroring at bind time.
                    if constexpr (is_pk_member(member)) {
                        return bind_pk_scalar_one<ConnType>(stmt, param_index, obj.[:member:].[:fk_pk_member:]);
                    } else {
                        return bind_one<ConnType>(stmt, param_index, obj.[:member:].[:fk_pk_member:]);
                    }
                } else {
                    return bind_fk_parts<ConnType, FKType>(stmt, obj.[:member:], param_index);
                }
            }
        }

        // Bind one part of a composite FK target's primary key at `param_index`,
        // advancing it on success. `Member` is an NTTP (not a runtime array
        // element): a std::meta::info captured by a runtime lambda closure is a
        // "consteval-only type used outside a constant expression" hard error in
        // this compiler, so each part must be named as a template argument, the
        // same discipline bind_one_pk_part uses for composite-PK binding (#501).
        //
        // The target's key part may ITSELF be an FK — the canonical association-table
        // shape, one hop further out than #501's own FK-part handling: here the part
        // belongs to the FK TARGET's key rather than to T's own. Such a part stores the
        // referenced row's key, so bind THAT, exactly as bind_one_pk_part does and
        // matching the value the "<part>_id" column holds (#536). Binding the whole
        // struct is not a wrong value but a hard error — it fails BindableType — which
        // is why this shape did not compile at all before.
        template <typename ConnType, std::meta::info Member>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto
        bind_one_fk_part(typename ConnType::Statement* stmt, const auto& fk_obj, int& param_index) noexcept
                -> std::expected<void, typename ConnType::Error> {
            auto result = [&] {
                if constexpr (is_fk_field(Member)) {
                    using PartFKType = std::remove_cvref_t<decltype(fk_obj.[:Member:])>;
                    return bind_value_by_type<ConnType>(
                            *stmt, param_index, fk_obj.[:Member:].[:find_fk_primary_key<PartFKType>():]
                    );
                } else {
                    return bind_value_by_type<ConnType>(*stmt, param_index, fk_obj.[:Member:]);
                }
            }();
            if (result.has_value()) {
                ++param_index;
            }
            return result;
        }

        // Bind every part of the FK target's primary key, in declaration order,
        // advancing param_index past all of them. Only reached for a composite
        // target (fk_primary_key_count<FKType>() > 1) — see bind_fk_field_at_index.
        // Index-sequence fold over the FUNCTION TEMPLATE's own parameter pack
        // (Is), not a runtime lambda closure — see bind_one_fk_part above.
        template <typename ConnType, typename FKType, std::size_t... Is>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto bind_fk_parts_impl(
                typename ConnType::Statement* stmt,
                const auto&                   fk_obj,
                int&                          param_index,
                std::index_sequence<Is...> /*unused*/
        ) noexcept -> std::expected<void, typename ConnType::Error> {
            constexpr auto                                members = find_fk_primary_key_members<FKType>();
            std::expected<void, typename ConnType::Error> result{};
            ((result = bind_one_fk_part<ConnType, members[Is]>(stmt, fk_obj, param_index), result.has_value()) && ...);
            return result;
        }

        template <typename ConnType, typename FKType>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto
        bind_fk_parts(typename ConnType::Statement* stmt, const auto& fk_obj, int& param_index) noexcept
                -> std::expected<void, typename ConnType::Error> {
            return bind_fk_parts_impl<ConnType, FKType>(
                    stmt, fk_obj, param_index, std::make_index_sequence<fk_primary_key_count<FKType>()>{}
            );
        }

        // Bulk binding scaffolding: iterate objects, fold the per-field bind over the
        // index sequence, return on first error. Bound to bind_field_at_index<...>
        // through the SkipPrimaryKey template parameter so a single body serves
        // both the all-fields and non-PK variants.
        template <bool SkipPrimaryKey, typename ConnType, typename Statement, typename ContainerType, std::size_t... Is>
        [[nodiscard]] static auto bind_bulk_objects_impl(
                Statement& stmt, const ContainerType& objects, std::index_sequence<Is...> /*unused*/
        ) noexcept -> std::expected<void, typename ConnType::Error> {
            int param_index = 1;
            // One now() per batch, reused for every row (#209): all rows share the same
            // created_at/updated_at, matching the issue's batch-timestamp intent.
            const auto now = batch_now();

            for (const auto& obj : objects) {
                std::expected<void, typename ConnType::Error> result{};
                ((result = bind_field_at_index<ConnType, Is, SkipPrimaryKey>(&stmt, obj, param_index, now),
                  result.has_value()) &&
                 ...);
                if (!result) {
                    return result;
                }
            }
            return {};
        }

        // Helper for bulk binding multiple objects with index sequence
        template <typename ConnType, typename Statement, typename ContainerType, std::size_t... Is>
        [[nodiscard]] static auto bind_all_objects_bulk_impl(
                Statement& stmt, const ContainerType& objects, std::index_sequence<Is...> seq
        ) noexcept -> std::expected<void, typename ConnType::Error> {
            return bind_bulk_objects_impl<false, ConnType, Statement, ContainerType, Is...>(stmt, objects, seq);
        }

        // Helper for bulk INSERT binding (skips PK for auto-increment)
        template <typename ConnType, typename Statement, typename ContainerType, std::size_t... Is>
        [[nodiscard]] static auto bind_non_pk_objects_bulk_impl(
                Statement& stmt, const ContainerType& objects, std::index_sequence<Is...> seq
        ) noexcept -> std::expected<void, typename ConnType::Error> {
            return bind_bulk_objects_impl<true, ConnType, Statement, ContainerType, Is...>(stmt, objects, seq);
        }

      public:
        // Common batch operation thresholds. Public since #501: EraseGrammar derives the
        // bulk-DELETE chunk size from it and is a separate type, not a subclass. It is a
        // backend constant rather than model state, so exposing it leaks nothing.
        static constexpr std::size_t MAX_DB_VARIABLES = 999;

      protected:
        // Adaptive threshold calculation based on batch size and field count
        // Returns the optimal threshold for deciding between bulk SQL and individual inserts
        static constexpr auto calculate_adaptive_threshold(std::size_t batch_size, std::size_t max_bulk_size)
                -> std::size_t {
            using utilities::batch::FALLBACK_BATCH_SIZE;
            using utilities::batch::SMALL_THRESHOLD;

            // For very small batches, always use bulk SQL up to the SQLite limit
            if (batch_size <= SMALL_THRESHOLD) {
                return max_bulk_size;
            }

            // Calculate safe thresholds based on max_bulk_size (which already accounts for field count)
            // max_bulk_size = 999 / field_count, so we scale our thresholds accordingly

            // For small-medium batches, use bulk SQL if safe
            // Use 50% of max_bulk_size as the sweet spot for bulk operations
            const std::size_t bulk_sweet_spot = std::max(FALLBACK_BATCH_SIZE, max_bulk_size / 2);

            if (batch_size <= bulk_sweet_spot) {
                return bulk_sweet_spot; // Use bulk SQL - most efficient
            }

            // For medium batches, try to use bulk if within 80% of SQLite limit
            const std::size_t bulk_max_safe = (max_bulk_size * 4) / 5; // 80% of max

            if (batch_size <= bulk_max_safe) {
                return bulk_max_safe; // Push bulk SQL to near SQLite limit
            }

            // For large batches (>80% of SQLite limit), use individual inserts with transaction
            // Avoids hitting SQLite variable limits and better memory usage
            return FALLBACK_BATCH_SIZE; // Force individual insert path - safe for any field count
        }

        // Common binding utilities for different types
        // Delegates to unified bind_parameter_value in utilities
        template <typename ConnType>
        [[nodiscard]] static auto
        bind_value_by_type(typename ConnType::Statement& stmt, int param_index, const auto& value) noexcept
                -> std::expected<void, typename ConnType::Error> {
            return utilities::bind_parameter_value<typename ConnType::Statement, typename ConnType::Error>(
                    stmt, param_index, value
            );
        }

        // Bind every primary-key value of `obj`, in declaration order, starting at
        // `param_index` and advancing it past the last part (#501). The bind order matches
        // the column order append_pk_where_clause emits, which is what keeps the
        // placeholders and the values aligned.
        //
        // The index sequence is over primary_key_members_ rather than a runtime loop
        // because each part may be a DIFFERENT type (int + std::string is the canonical
        // composite key), so the bind has to be dispatched per part at compile time. An FK
        // part binds the referenced object's own PK value, matching how the "<name>_id"
        // column is written.
        // Each part's placeholder is `param_index + Is` — a compile-time offset from the
        // start of the key, so no running counter is threaded through the fold. On a
        // single-PK model this collapses to exactly one `bind(stmt, param_index, value)`
        // call, identical to the hand-written bind it replaced.
        template <typename ConnType, std::size_t... Is>
        [[nodiscard]] __attribute__((always_inline)) static auto bind_pk_values_impl(
                typename ConnType::Statement& stmt, const T& obj, int param_index, std::index_sequence<Is...> /*unused*/
        ) noexcept -> std::expected<void, typename ConnType::Error> {
            std::expected<void, typename ConnType::Error> result{};
            ((result = bind_one_pk_part<ConnType, primary_key_members_[Is]>(
                      stmt, obj, param_index + static_cast<int>(Is)
              ),
              result.has_value()) &&
             ...);
            return result;
        }

        // Bind a single PK-part VALUE at `index`, without advancing it. A UUID value is
        // routed through bind_uuid_pk rather than bind_value_by_type (#573): the generic
        // path falls into utilities::bind_uuid, which AUTO-GENERATES a fresh random UUID
        // when the value is empty — correct for a non-PK UUID column, wrong for a KEY. An
        // unset key would silently bind a random UUID into the WHERE clause, match zero
        // rows, and report success. bind_uuid_pk rejects the empty value instead, the same
        // guard INSERT already applies to a bare (non-FK) UUID PK member via
        // bind_optional_or_uuid_pk_field — INSERT of a composite key whose FK PART targets a
        // UUID key now routes through the matching guard in bind_fk_field_at_index (#608),
        // via the bind_pk_scalar_one wrapper below that both call sites share. The dispatch
        // is `if constexpr` on the value's own type, so a non-UUID value compiles to exactly
        // the bind_value_by_type call this replaces.
        template <typename ConnType, typename ValueType>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_pk_scalar(typename ConnType::Statement& stmt, int index, const ValueType& value) noexcept
                -> std::expected<void, typename ConnType::Error> {
            if constexpr (std::is_same_v<ValueType, storm::orm::utilities::UUID>) {
                return utilities::bind_uuid_pk<typename ConnType::Statement, typename ConnType::Error>(
                        stmt, index, value
                );
            } else {
                return bind_value_by_type<ConnType>(stmt, index, value);
            }
        }

        // bind_pk_scalar at `param_index`, advancing it on success — the shared tail of
        // bind_optional_or_uuid_pk_field (a bare UUID PK) and bind_fk_field_at_index (an FK
        // PK-part, #608), the two sites that reach a PK-guarded value through a *runtime*
        // param_index rather than bind_pk_values_impl's compile-time per-part offset. Mirrors
        // bind_one's shape (bind, then advance only on success) so a missing key still leaves
        // param_index untouched on the error path, same as every other guarded bind.
        template <typename ConnType>
        [[nodiscard]] __attribute__((always_inline)) static constexpr auto
        bind_pk_scalar_one(typename ConnType::Statement* stmt, int& param_index, const auto& value) noexcept
                -> std::expected<void, typename ConnType::Error> {
            auto result = bind_pk_scalar<ConnType>(*stmt, param_index, value);
            if (!result) {
                return result;
            }
            ++param_index;
            return {};
        }

        // Bind one primary-key part at `index`. An FK part (the canonical
        // association-table shape) stores the referenced row's key, so bind THAT, not the
        // whole object — the same value the "<name>_id" column holds.
        //
        // Takes `index` BY VALUE and does not advance it: the caller computes each part's
        // offset as a compile-time constant (see bind_pk_values_impl). Flat code, no
        // lambda and no dead increment on the last part — this sits directly in the
        // single-row UPDATE/DELETE hot path, where nested lambdas have cost ~3-4% before.
        template <typename ConnType, std::meta::info Member>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_one_pk_part(typename ConnType::Statement& stmt, const T& obj, int index) noexcept
                -> std::expected<void, typename ConnType::Error> {
            if constexpr (is_fk_field(Member)) {
                using FKType = std::remove_cvref_t<decltype(obj.[:Member:])>;
                return bind_pk_scalar<ConnType>(stmt, index, obj.[:Member:].[:find_fk_primary_key<FKType>():]);
            } else {
                return bind_pk_scalar<ConnType>(stmt, index, obj.[:Member:]);
            }
        }

        // Bind the whole primary key of `obj` starting at `param_index`, advancing it past
        // the key. The entry point erase.cppm and update.cppm call; single-PK models bind
        // exactly one value, as before.
        //
        // The advance is a single add of a compile-time constant, applied once here rather
        // than per-part inside the fold — on the single-PK path the caller never reads it
        // back, so it folds away entirely.
        template <typename ConnType>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_pk_values(typename ConnType::Statement& stmt, const T& obj, int& param_index) noexcept
                -> std::expected<void, typename ConnType::Error> {
            auto result = bind_pk_values_impl<ConnType>(
                    stmt, obj, param_index, std::make_index_sequence<primary_key_column_count_>{}
            );
            if (result.has_value()) {
                param_index += static_cast<int>(primary_key_column_count_);
            }
            return result;
        }

        // =====================================================================
        // COLUMN EXTRACTION HELPERS - Moved here from SelectStatement so that
        // constexpr access to all_members_[Index] happens in base.cppm context
        // (avoids P2996 experimental compiler limitation in select.cppm module)
        // =====================================================================

        // Extract optional FK column: set nullopt when NULL, otherwise extract inner PK.
        // Single-column target only (fk_primary_key_count<FieldType>() == 1) — the
        // pre-#504 code, widened only to read from col_idx (the ACTUAL SQL column
        // position) rather than Index (the member's position in all_members_): the two
        // diverge once any earlier member is a composite-FK member consuming N>1 columns
        // (#504). Index still drives the MEMBER lookup (member = all_members_[Index]) —
        // that part is correct and unchanged; only the column POSITION moves to the
        // threaded col_idx parameter, matching extract_fk_parts/extract_optional_fk_parts.
        template <std::size_t Index, typename Statement, typename FieldType>
        __attribute__((always_inline)) static void
        extract_optional_fk_column(Statement* stmt, T& obj, int col_idx) noexcept {
            constexpr auto member       = all_members_[Index];
            using InnerFKType           = utilities::optional_inner_type_t<FieldType>;
            constexpr auto fk_pk_member = find_fk_primary_key<FieldType>();
            using PKType                = std::remove_cvref_t<decltype(std::declval<InnerFKType>().[:fk_pk_member:])>;
            if (stmt->is_null(col_idx)) {
                obj.[:member:] = std::nullopt;
            } else {
                InnerFKType fk_inner{};
                fk_inner.[:fk_pk_member:] = ColumnExtractor::extract_column_value<PKType>(stmt, col_idx);
                obj.[:member:]            = std::move(fk_inner);
            }
        }

        // Extract one part of a composite FK target's primary key from column
        // col_idx into fk_obj.[:Member:]. `Member` is an NTTP — see bind_one_fk_part
        // for why a std::meta::info cannot instead be captured by a runtime lambda
        // closure in this compiler.
        //
        // An FK part is the exact mirror of bind_one_fk_part's FK branch (#536): the
        // column holds the REFERENCED row's key, so extract that key's own type and
        // store it into the part's referenced-object member, leaving the rest of that
        // object default-constructed — the same single-level, key-only reconstruction
        // extract_optional_fk_column performs for a single-column FK. Extracting
        // PartType directly would ask ColumnExtractor for a whole model struct.
        template <std::meta::info Member, typename Statement>
        __attribute__((always_inline)) static void
        extract_one_fk_part(Statement* stmt, auto& fk_obj, int col_idx) noexcept {
            using PartType = std::remove_cvref_t<decltype(fk_obj.[:Member:])>;
            if constexpr (is_fk_field(Member)) {
                constexpr auto part_pk_member = find_fk_primary_key<PartType>();
                using PartPKType = std::remove_cvref_t<decltype(std::declval<PartType>().[:part_pk_member:])>;
                fk_obj.[:Member:].[:part_pk_member:] = ColumnExtractor::template extract_column_value<PartPKType>(
                                                             stmt, col_idx
                                                     );
            } else {
                fk_obj.[:Member:] = ColumnExtractor::template extract_column_value<PartType>(stmt, col_idx);
            }
        }

        // Extract every part of a composite FK target's primary key from N
        // consecutive columns starting at col_idx, in declaration order (#504).
        // Single-column targets never reach this — see extract_column_fast's dispatch.
        // Index-sequence fold over the FUNCTION TEMPLATE's own parameter pack (Is),
        // not a runtime lambda closure — see extract_one_fk_part above.
        template <typename Statement, typename FieldType, std::size_t... Is>
        __attribute__((always_inline)) static void extract_fk_parts_impl(
                Statement* stmt, auto& fk_obj, int col_idx, std::index_sequence<Is...> /*unused*/
        ) noexcept {
            constexpr auto members = find_fk_primary_key_members<FieldType>();
            (extract_one_fk_part<members[Is], Statement>(stmt, fk_obj, col_idx + static_cast<int>(Is)), ...);
        }

        template <typename Statement, typename FieldType>
        __attribute__((always_inline)) static void
        extract_fk_parts(Statement* stmt, auto& fk_obj, int col_idx) noexcept {
            extract_fk_parts_impl<Statement, FieldType>(
                    stmt, fk_obj, col_idx, std::make_index_sequence<fk_primary_key_count<FieldType>()>{}
            );
        }

        // Extract an optional composite FK: NULL in the first of the N columns means
        // no match (mirrors the single-column NULL-FK convention) → nullopt; otherwise
        // every part is extracted from its own column (#504).
        template <typename Statement, typename FieldType>
        __attribute__((always_inline)) static void
        extract_optional_fk_parts(Statement* stmt, auto& obj_field, int col_idx) noexcept {
            using InnerFKType = utilities::optional_inner_type_t<FieldType>;
            if (stmt->is_null(col_idx)) {
                obj_field = std::nullopt;
            } else {
                InnerFKType fk_inner{};
                extract_fk_parts<Statement, InnerFKType>(stmt, fk_inner, col_idx);
                obj_field = std::move(fk_inner);
            }
        }

        // Extract single column into obj at compile-time member index, reading from
        // col_idx (the ACTUAL SQL column position, threaded by reference through the
        // fold below) rather than Index directly: a composite-FK member (#504) spans
        // more than one SQL column, so member index and column position diverge from
        // that member onward. col_idx is advanced by exactly the columns this member
        // consumed — 1 for every pre-#504 shape, fk_primary_key_count<FieldType>()
        // for a composite FK.
        template <std::size_t Index, typename Statement>
        __attribute__((always_inline)) static void extract_column_fast(Statement* stmt, T& obj, int& col_idx) noexcept {
            if constexpr (Index < field_count_) {
                constexpr auto member = all_members_[Index];
                using FieldType       = std::remove_cvref_t<decltype(obj.[:member:])>;
                if constexpr (is_fk_field(member)) {
                    if constexpr (utilities::is_optional_v<FieldType>) {
                        if constexpr (fk_primary_key_count<FieldType>() == 1) {
                            extract_optional_fk_column<Index, Statement, FieldType>(stmt, obj, col_idx);
                            ++col_idx;
                        } else {
                            extract_optional_fk_parts<Statement, FieldType>(stmt, obj.[:member:], col_idx);
                            col_idx += static_cast<int>(fk_primary_key_count<FieldType>());
                        }
                    } else {
                        obj.[:member:] = FieldType{};
                        if constexpr (fk_primary_key_count<FieldType>() == 1) {
                            constexpr auto fk_pk_member = find_fk_primary_key<FieldType>();
                            using PKType = std::remove_cvref_t<decltype(obj.[:member:].[:fk_pk_member:])>;
                            obj.[:member:].[:fk_pk_member:] = ColumnExtractor::extract_column_value<PKType>(
                                                                    stmt, col_idx
                                                            );
                            ++col_idx;
                        } else {
                            extract_fk_parts<Statement, FieldType>(stmt, obj.[:member:], col_idx);
                            col_idx += static_cast<int>(fk_primary_key_count<FieldType>());
                        }
                    }
                } else if constexpr (storm::meta::has_full_unsigned_attr(member)) {
                    extract_full_unsigned_into<member, FieldType>(stmt, obj, col_idx);
                    ++col_idx;
                } else {
                    obj.[:member:] = ColumnExtractor::extract_column_value<FieldType>(stmt, col_idx);
                    ++col_idx;
                }
            }
        }

        // Expand index sequence and extract each column, threading the actual SQL
        // column position through col_idx (#504: diverges from the member index once
        // a composite-FK member has been consumed).
        template <typename Statement, std::size_t... Is>
        __attribute__((always_inline)) static void
        extract_all_columns_impl(Statement* stmt, T& obj, std::index_sequence<Is...> /*unused*/) noexcept {
            int col_idx = 0;
            ((extract_column_fast<Is>(stmt, obj, col_idx)), ...);
        }

        // Entry point: extract all columns into obj using field_indices_t
        template <typename Statement>
        __attribute__((always_inline)) static void extract_all_columns(Statement* stmt, T& obj) noexcept {
            extract_all_columns_impl(stmt, obj, field_indices_t{});
        }

        // =====================================================================
        // SQL CLAUSE HELPERS - Shared across SELECT, DISTINCT, AGGREGATE
        // =====================================================================

        // LCOV_EXCL_START — PostgreSQL-only; covered by CI PG tests, not local SQLite mock
        // Helper: Adapt ORDER BY SQL for PostgreSQL NULL ordering semantics
        // Adds NULLS FIRST after ASC and NULLS LAST after DESC to match SQLite behavior
        static void adapt_order_by_for_pg(std::string& adapted) {
            using namespace std::string_view_literals;
            // Token lengths derived from the literals so the offsets can never drift (ES.45).
            static constexpr auto kAsc        = " ASC"sv;
            static constexpr auto kDesc       = " DESC"sv;
            static constexpr auto kNulls      = " NULLS"sv; // already-adapted guard prefix
            static constexpr auto kNullsFirst = " NULLS FIRST"sv;
            static constexpr auto kNullsLast  = " NULLS LAST"sv;

            std::size_t pos = 0;
            while ((pos = adapted.find(kAsc, pos)) != std::string::npos) {
                std::size_t const after = pos + kAsc.size();
                if (adapted.substr(after, kNulls.size()) != kNulls) {
                    adapted.insert(after, kNullsFirst);
                }
                pos = after + kNullsFirst.size();
            }
            pos = 0;
            while ((pos = adapted.find(kDesc, pos)) != std::string::npos) {
                std::size_t const after = pos + kDesc.size();
                if (adapted.substr(after, kNulls.size()) != kNulls) {
                    adapted.insert(after, kNullsLast);
                }
                pos = after + kNullsLast.size();
            }
        }
        // LCOV_EXCL_STOP

      public:
        // ORDER BY / LIMIT / OFFSET appenders are public so the free two-query join
        // helpers (join.cppm, #398) can assemble Q1/Q2 base clauses for any model.

        // Helper: Append ORDER BY clause to SQL from wrapper
        // NOTE: ORDER BY must come before LIMIT/OFFSET in SQLite
        // For PostgreSQL, adds NULLS FIRST/LAST to match SQLite semantics
        template <typename ConnTypeForDialect = void>
        __attribute__((always_inline)) static void
        append_order_by(std::string& sql, const std::optional<OrderByWrapper>& order_by_wrapper) {
            if (order_by_wrapper.has_value() && !order_by_wrapper->empty()) {
                const auto& order_sql = order_by_wrapper->get_order_by_sql();
                if constexpr (db::SupportsPgDialect<ConnTypeForDialect>) {
                    std::string adapted = order_sql; // LCOV_EXCL_LINE — PG-only
                    adapt_order_by_for_pg(adapted);  // LCOV_EXCL_LINE — PG-only
                    sql += adapted;                  // LCOV_EXCL_LINE — PG-only
                } else {
                    sql += order_sql;
                }
            }
        }

        // Helper: Append LIMIT/OFFSET clauses to SQL
        // NOTE: SQLite requires LIMIT when using OFFSET, so we use LIMIT -1 (meaning unlimited)
        // PostgreSQL uses LIMIT ALL for unlimited rows
        template <typename ConnTypeForDialect = void>
        __attribute__((always_inline)) static void
        append_limit_offset(std::string& sql, const std::optional<int>& limit, const std::optional<int>& offset) {
            if (limit.has_value()) {
                sql += " LIMIT ";
                sql += std::to_string(limit.value());
            } else if (offset.has_value()) {
                // Need LIMIT when using OFFSET
                if constexpr (db::SupportsLimitAll<ConnTypeForDialect>) {
                    if constexpr (ConnTypeForDialect::supports_limit_all) {
                        sql += " LIMIT ALL";
                    } else {
                        sql += " LIMIT -1";
                    }
                } else {
                    sql += " LIMIT -1"; // Default: SQLite-compatible
                }
            }

            if (offset.has_value()) {
                sql += " OFFSET ";
                sql += std::to_string(offset.value());
            }
        }

      protected:
        // Helper: Bind WHERE expression parameters to statement
        // Returns std::expected<void, Error> - resets statement on failure
        // Helper: bind a WHERE-style expression and convert the result. On
        // failure, reset the statement and propagate as std::unexpected.
        // Used by both bind_where_params (starts at param_index = 1) and
        // bind_having_params (continues from WHERE's last index).
        template <typename Statement, typename Error>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_expr_or_reset(Statement* stmt_ptr, const orm::where::ExpressionVariantPtr& expr, int& param_index)
                -> std::expected<void, Error> {
            auto bind_result = orm::where::bind_params_direct<Statement, Error>(*expr, stmt_ptr, param_index);
            if (!bind_result) [[unlikely]] {
                stmt_ptr->reset();
                return std::unexpected(bind_result.error());
            }
            return {};
        }

        template <typename Statement, typename Error>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_where_params(Statement* stmt_ptr, const orm::where::ExpressionVariantPtr& where_expr)
                -> std::expected<void, Error> {
            int param_index = 1;
            return bind_expr_or_reset<Statement, Error>(stmt_ptr, where_expr, param_index);
        }

        // Helper: Bind HAVING expression parameters to statement
        // param_index continues from WHERE's last index (or starts at 1 if no WHERE)
        template <typename Statement, typename Error>
        [[nodiscard]] __attribute__((always_inline)) static auto
        bind_having_params(Statement* stmt_ptr, const orm::where::ExpressionVariantPtr& having_expr, int& param_index)
                -> std::expected<void, Error> {
            return bind_expr_or_reset<Statement, Error>(stmt_ptr, having_expr, param_index);
        }
    };

} // namespace storm::orm::statements
