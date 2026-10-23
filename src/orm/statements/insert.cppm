module;

// Single cohesive class template; thresholds intentionally relaxed (see #264 finding).
// `duplicate` removed in #277 Phase 3 (shared consteval SQL builder + to_sql_impl).

#include <meta>

export module storm_orm_statements_insert;

import std;

import storm_orm_statements_base;
import storm_orm_statements_insert_grammar;
import storm_orm_statements_upsert_grammar;
import storm_orm_fields; // selector_info — #518 proxy selectors
import storm_orm_utilities;
import storm_orm_transaction;
import storm_db_concept;
import storm_db_default_connection;

export namespace storm::orm::statements {

    // Import utilities for code convenience
    using storm::orm::utilities::BulkSQLCache;
    using storm::orm::utilities::ConstexprString;
    using storm::orm::utilities::TransactionGuard;

    // Controls whether INSERT returns the auto-generated ID(s)
    enum class ReturnId : bool { No = false, Yes = true };

    // Configuration options for batch INSERT operations
    struct InsertOptions {
        std::optional<std::size_t> batch_size = std::nullopt; // nullopt = automatic (999/field_count)
    };

    // Plain insert() defaults to the void/no-RETURNING path on any model whose
    // key is caller data — composite (#502) or storm::UUID (#572), see
    // BaseStatement::pk_is_db_generated_() for why neither can be RETURNed — and
    // keeps returning the generated id on a single-integer-PK model.
    template <typename T> consteval auto default_return_id() -> ReturnId {
        return BaseStatement<T>::pk_is_db_generated_() ? ReturnId::Yes : ReturnId::No;
    }

    // An explicit ReturnId::Yes on such a model is rejected at the call site:
    // unconstrained it would emit "RETURNING <first part>" (composite) or
    // "RETURNING <uuid_pk>" (UUID) — silently lossy either way. ReturnId::No
    // stays valid on every model shape, so generic code that spells it out keeps
    // compiling. (Named concept per the #472/#477/#478 precedent; the #413
    // auto-DEFAULT caveat was checked and does not fire — that DEFAULT is
    // recovered from a C++ default-member-initializer, a value already in the
    // caller's object.)
    template <typename T, ReturnId R>
    concept ReturnIdSupported = (R == ReturnId::No) || BaseStatement<T>::pk_is_db_generated_();

    template <typename T, storm::db::DatabaseConnection ConnType> class BasicInsertStatement;

    // Upsert proxy chain (#205): lifted OUT of InsertStatement so their sql()/execute()/to_sql()
    // methods don't count toward InsertStatement's method total (cpp:S1448 — mirrors the
    // UpdateStatement proxy split in #438). on_conflict<Target...>() returns a ConflictTarget,
    // whose update<SetCols...>()/nothing() are the two terminal upsert proxies.
    //
    // ConflictTargetTag<Target...> carries the conflict-target columns as a TYPE (not a pack)
    // so UpdateUpsertQuery can be a class template with exactly one variadic NTTP pack
    // (SetCols...). A class template cannot declare two `std::meta::info...` packs — both
    // would need independent deduction/specification, which the language does not support —
    // so Target... must be wrapped before it reaches a second pack's scope.
    template <std::meta::info... Target> struct ConflictTargetTag {};

    namespace detail {

        // DO UPDATE terminal proxy. TargetTag is a ConflictTargetTag<Target...> instantiation.
        template <typename T, storm::db::DatabaseConnection ConnType, typename TargetTag, std::meta::info... SetCols>
        struct UpdateUpsertQuery;

        template <
                typename T,
                storm::db::DatabaseConnection ConnType,
                std::meta::info... Target,
                std::meta::info... SetCols>
        struct UpdateUpsertQuery<T, ConnType, ConflictTargetTag<Target...>, SetCols...> {
            using Error = typename ConnType::Error;
            BasicInsertStatement<T, ConnType> stmt;
            const T&                          obj;
            // DO UPDATE always touches a row, but only a DB-generated key has a
            // value to RETURN (#502/#503/#572), so the proxy resolves to void on
            // a composite- or UUID-PK model instead of the id.
            [[nodiscard]] auto execute() {
                return stmt.template upsert_update_runner<Target...>().template run<SetCols...>(obj);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                const std::string sql = UpsertGrammar<T>::template update_sql<Target...>(
                        UpsertGrammar<T>::template build_excluded_set_clause<SetCols...>()
                );
                return stmt.template to_sql_with<SetCols...>(sql, obj);
            }
            [[nodiscard]] auto sql() -> std::string {
                return UpsertGrammar<T>::template update_sql<Target...>(
                        UpsertGrammar<T>::template build_excluded_set_clause<SetCols...>()
                );
            }
        };

        // DO NOTHING terminal proxy.
        template <typename T, storm::db::DatabaseConnection ConnType, std::meta::info... Target>
        struct NothingUpsertQuery {
            using Error = typename ConnType::Error;
            BasicInsertStatement<T, ConnType> stmt;
            const T&                          obj;
            // A key that is not DB-generated (#502/#503/#572) omits RETURNING, so
            // there is no id to report and no way to tell "inserted" from
            // "skipped" — the proxy resolves to void on a composite- or UUID-PK
            // model (documented as unavailable, per #502's DoD) instead of
            // optional<int64_t>.
            [[nodiscard]] auto execute() { // NOSONAR(cpp:S1659)
                return stmt.template execute_upsert_nothing<Target...>(obj);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.template to_sql_with<>(UpsertGrammar<T>::template nothing_sql<Target...>(), obj);
            }
            [[nodiscard]] auto sql() -> std::string {
                return UpsertGrammar<T>::template nothing_sql<Target...>();
            }
        };

        // Intermediate: carries the conflict target, offers update<>/nothing().
        template <typename T, storm::db::DatabaseConnection ConnType, std::meta::info... Target> struct ConflictTarget {
            BasicInsertStatement<T, ConnType> stmt;
            const T&                          obj;
            template <auto... S>
                requires UpsertSettable<T, storm::meta::selector_info<S>()...>
            [[nodiscard]] auto update() && -> UpdateUpsertQuery<
                    T,
                    ConnType,
                    ConflictTargetTag<Target...>,
                    storm::meta::selector_info<S>()...> {
                return {std::move(stmt), obj};
            }
            [[nodiscard]] auto nothing() && -> NothingUpsertQuery<T, ConnType, Target...> {
                return {std::move(stmt), obj};
            }
        };

    } // namespace detail

    // Statement class for ORM insert operations
    template <typename T, storm::db::DatabaseConnection ConnType>
    class BasicInsertStatement : private BaseStatement<T> {
        friend class BaseStatement<T>; // Allow BaseStatement to access protected/private members
        using Base       = BaseStatement<T>;
        using InsertG    = InsertGrammar<T>;
        using UpsertG    = UpsertGrammar<T>;
        using Connection = ConnType;
        using Error      = typename ConnType::Error;
        using Statement  = typename ConnType::Statement;

        // Result of a bulk insert: the generated ids with ReturnId::Yes, void otherwise.
        // Declared before the public proxies because it appears in their return types.
        template <ReturnId R>
        using BulkResult =
                std::expected<std::conditional_t<R == ReturnId::Yes, std::vector<std::int64_t>, void>, Error>;

        // Bulk INSERT SQL for `count` rows, with or without " RETURNING <pk>" (with caching).
        // Returns const reference to avoid expensive string copy. The function-local
        // thread_local cache is per instantiation, so the RETURNING and plain variants
        // (R = Yes / No) keep separate caches without needing two functions.
        template <ReturnId R> static auto get_bulk_insert_sql(std::size_t count) -> const std::string& {
            static thread_local BulkSQLCache cache;

            if (const auto* cached = cache.find(count)) {
                return *cached;
            }

            std::string sql = InsertG::build_bulk_insert_body(count);
            if constexpr (R == ReturnId::Yes) {
                sql += InsertG::returning_suffix;
            }
            cache.insert(count, std::move(sql));
            return *cache.find(count); // Guaranteed to exist after insert
        }

        // Rows per bulk statement: the bind-variable limit divided by field_count_.
        // Dividing by InsertG::placeholders_count() instead would be tighter for a DB-generated PK
        // (it is not bound) and exact for a composite-FK member (N placeholders, one
        // field), but it grew the chunk from 249 to 333 rows on the INSERT benchmark model
        // and measured ~2.6% slower at N=1000. Until that is resolved, a model with a
        // composite-FK member can still overshoot the parameter limit.
        // Clamped to [1, max_allowed]: a caller-supplied 0 would never advance the chunk
        // loop (offset += 0, see #359), and the upper bound caps at the parameter limit.
        static auto effective_batch_size(const std::optional<InsertOptions>& opts) -> std::size_t {
            constexpr std::size_t max_allowed =
                    std::max<std::size_t>(1, Base::MAX_DB_VARIABLES / std::max<std::size_t>(1, Base::field_count_));
            const std::size_t requested = opts.value_or(InsertOptions{}).batch_size.value_or(max_allowed);
            return std::clamp<std::size_t>(requested, 1, max_allowed);
        }

        template <ReturnId R> static auto __attribute__((always_inline)) single_insert_sql() -> const std::string& {
            if constexpr (R == ReturnId::Yes) {
                return InsertG::insert_returning_sql_string;
            } else {
                return InsertG::insert_sql_string;
            }
        }

      public:
        explicit BasicInsertStatement(
                std::shared_ptr<ConnType> conn = storm::db::BasicDefaultConnection<ConnType>::get()
        )
            : conn_(std::move(conn)) {}

        // Single-row insert proxy. R selects RETURNING (execute() yields the generated id)
        // or the plain void path. on_conflict() starts the upsert chain (#205):
        // INSERT ... ON CONFLICT (Target...) DO {UPDATE|NOTHING}; it is rvalue-only
        // because it consumes the statement.
        template <ReturnId R> struct SingleInsertQuery {
            BasicInsertStatement stmt;
            const T&             obj;

            using Result = std::
                    conditional_t<R == ReturnId::Yes, std::expected<std::int64_t, Error>, std::expected<void, Error>>;

            [[nodiscard]] auto execute() -> Result {
                return stmt.template execute_single<R>(obj);
            }

            template <auto... S>
                requires ConflictTargetUnique<T, storm::meta::selector_info<S>()...>
            [[nodiscard]] auto
            on_conflict() && -> detail::ConflictTarget<T, ConnType, storm::meta::selector_info<S>()...> {
                return {std::move(stmt), obj};
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.template to_sql<R>(obj);
            }
            [[nodiscard]] static auto sql() -> std::string {
                return single_insert_sql<R>();
            }
        };

        // Bulk insert proxy. R = Yes: execute() yields the generated ids; R = No: void.
        template <ReturnId R> struct BulkInsertQuery {
            BasicInsertStatement         stmt;
            std::span<const T>           objects;
            std::optional<InsertOptions> opts;

            [[nodiscard]] auto execute() -> BulkResult<R> {
                return stmt.template execute_batched<R>(objects, opts);
            }
            [[nodiscard]] auto to_sql() -> std::expected<std::string, Error> {
                return stmt.template to_sql_bulk<R>(objects);
            }
            [[nodiscard]] auto sql() -> std::string {
                return get_bulk_insert_sql<R>(objects.size());
            }
        };

        // Single-row query. R defaults per model shape (default_return_id<T>()).
        template <ReturnId R = default_return_id<T>()>
            requires ReturnIdSupported<T, R>
        [[nodiscard]] auto query(const T& obj [[clang::lifetimebound]]) -> SingleInsertQuery<R> {
            return {std::move(*this), obj};
        }

        // Bulk query. Unlike the single-row overload, R deliberately defaults to No on
        // every model: ids from a bulk insert are opt-in via query<ReturnId::Yes>(span).
        //
        // LIFETIME CONTRACT (issue #357, finding B): the returned BulkInsertQuery holds a
        // std::span<const T> aliasing the caller's container. `[[clang::lifetimebound]]`
        // (+ -Werror=dangling) catches the inline-temporary case at compile time, but a
        // span built from a container that dies before .execute()/.to_sql() runs still
        // dangles silently at runtime. Treat the proxy as single-expression-use: keep the
        // backing container alive until the terminal call completes.
        template <ReturnId R = ReturnId::No>
            requires ReturnIdSupported<T, R>
        [[nodiscard]] auto
        query(std::span<const T> objects [[clang::lifetimebound]], std::optional<InsertOptions> opts = std::nullopt)
                -> BulkInsertQuery<R> {
            return {std::move(*this), objects, opts};
        }

        // Returns SQL string with bound parameters inlined for a single INSERT (for debugging)
        template <ReturnId R> [[nodiscard]] auto to_sql(const T& obj) -> std::expected<std::string, Error> {
            return to_sql_impl(single_insert_sql<R>(), [&](Statement& s) { return bind_single(s, obj); });
        }

        // Returns SQL string with bound parameters inlined for a bulk INSERT, with or
        // without RETURNING (for debugging)
        template <ReturnId R>
        [[nodiscard]] __attribute__((cold)) auto to_sql_bulk(std::span<const T> objects)
                -> std::expected<std::string, Error> {
            if (objects.empty()) {
                return std::string{};
            }
            return to_sql_impl(get_bulk_insert_sql<R>(objects.size()), [&](Statement& s) {
                return bind_bulk(s, objects);
            });
        }

        // Returns SQL string with bound parameters inlined for an upsert (DO UPDATE/DO NOTHING)
        // statement (for debugging). Generalizes to_sql_impl to bind the non-PK VALUES fields
        // plus the auto_update now() tail (#209) for any auto_update column not in SetCols —
        // empty SetCols (the DO NOTHING proxy) binds just the non-PK fields.
        template <std::meta::info... SetCols>
        [[nodiscard]] __attribute__((cold)) auto to_sql_with(const std::string& sql, const T& obj)
                -> std::expected<std::string, Error> {
            return to_sql_impl(sql, [&](Statement& s) -> std::expected<void, Error> {
                if (auto bound = bind_single(s, obj); !bound) {
                    return std::unexpected(bound.error());
                }
                return bind_upsert_auto_updates<SetCols...>(&s);
            });
        }

        // Batch insert (single statement, or chunked inside one transaction when the rows
        // exceed the batch size). R = Yes collects the generated ids in input order.
        template <ReturnId R>
        [[nodiscard]] auto execute_batched(std::span<const T> objects, std::optional<InsertOptions> opts = std::nullopt)
                -> BulkResult<R> {
            if (objects.empty()) {
                return BulkResult<R>{};
            }

            const std::size_t batch = effective_batch_size(opts);
            if (objects.size() <= batch) {
                return execute_bulk<R>(objects); // fits in one bulk SQL
            }
            return execute_chunked_bulk<R>(objects, batch);
        }

        // Single INSERT; R = Yes appends RETURNING <pk> and yields the id.
        template <ReturnId R>
        [[nodiscard]] __attribute__((hot)) auto execute_single(const T& obj) noexcept -> std::
                conditional_t<R == ReturnId::Yes, std::expected<std::int64_t, Error>, std::expected<void, Error>> {
            auto prepared = prepare_and_bind(single_insert_sql<R>(), obj);
            if (!prepared) [[unlikely]] {
                return std::unexpected(prepared.error());
            }
            Statement* stmt = *prepared;

            // step_raw() avoids std::expected overhead on the hot path
            const int rc = stmt->step_raw();
            if constexpr (R == ReturnId::Yes) {
                if (rc == Statement::ROW_AVAILABLE) [[likely]] {
                    const auto id = stmt->extract_int64(0);
                    stmt->reset();
                    return id;
                }
                stmt->reset();
                if (rc == Statement::NO_MORE_ROWS) [[unlikely]] {
                    return std::int64_t{0}; // No row returned (shouldn't happen with RETURNING)
                }
                return make_error(rc, stmt);
            } else {
                stmt->reset();
                if (rc == Statement::NO_MORE_ROWS) [[likely]] {
                    return {};
                }
                return make_error(rc, stmt);
            }
        }

        // Step a prepared statement to completion and collapse it to void: a plain
        // NO_MORE_ROWS is success, anything else is an Error. Used by the upsert paths
        // (execute_upsert_nothing, UpsertUpdateRunner::run) on a model whose key is not
        // DB-generated — there is no RETURNING to extract, only "did it execute".
        [[nodiscard]] static __attribute__((always_inline)) auto finish_void_step(Statement* stmt)
                -> std::expected<void, Error> {
            const int rc = stmt->step_raw();
            stmt->reset();
            if (rc == Statement::NO_MORE_ROWS) {
                return {};
            }
            return make_error(rc, stmt);
        }

        // Step a RETURNING statement that may legitimately yield no row (DO NOTHING):
        // a row means "inserted" (value = new id), no row means "skipped" (nullopt).
        [[nodiscard]] static __attribute__((always_inline)) auto finish_optional_id_step(Statement* stmt)
                -> std::expected<std::optional<std::int64_t>, Error> {
            const int                   rc = stmt->step_raw();
            std::optional<std::int64_t> out;
            if (rc == Statement::ROW_AVAILABLE) {
                out = stmt->extract_int64(0);
            }
            stmt->reset();
            if (rc == Statement::ROW_AVAILABLE || rc == Statement::NO_MORE_ROWS) {
                return out;
            }
            return make_error(rc, stmt);
        }

        // Upsert DO NOTHING — RETURNING yields the new id, or no row when skipped.
        //
        // Only a DB-generated key has an id to report (#503/#572): nothing_sql()
        // omits RETURNING for a composite or storm::UUID key, so there is no row
        // to distinguish "inserted" from "skipped" — that signal is unavailable
        // there (documented in #502's DoD), and this resolves to
        // std::expected<void, Error>. Branch on the same predicate the grammar
        // uses, or this reads a row the statement never asked for.
        template <std::meta::info... Target>
        [[nodiscard]] __attribute__((hot)) auto execute_upsert_nothing(const T& obj) {
            using Result = std::conditional_t<
                    Base::pk_is_db_generated_(),
                    std::expected<std::optional<std::int64_t>, Error>,
                    std::expected<void, Error>>;

            auto prepared = prepare_and_bind(UpsertG::template nothing_sql<Target...>(), obj);
            if (!prepared) {
                return Result(std::unexpected(prepared.error()));
            }
            Statement* stmt = *prepared;
            if constexpr (!Base::pk_is_db_generated_()) {
                return finish_void_step(stmt);
            } else {
                return finish_optional_id_step(stmt);
            }
        }

        // Upsert DO UPDATE — always touches a row, so RETURNING always yields the id.
        // The SET clause is "col=excluded.col, ..." for SetCols, plus an auto_update
        // tail (#209) for any auto_update column not already listed.
        //
        // NOTE (#205, found while wiring the on_conflict() proxy in Task 5): a SINGLE
        // function template cannot take two variadic NTTP packs — `template <info...
        // Target, info... SetCols>` compiles, but ALL explicitly-supplied arguments bind
        // to the first pack and the second is silently always empty (verified: GCC/Clang
        // both do this — no diagnostic). The original two-pack signature here was never
        // caught because Task 4 shipped it with zero call sites. Splitting into Target
        // (this function's pack) + SetCols (the nested run<>() pack) mirrors the proven
        // ConflictTarget<Target...>::update<SetCols...>() shape and resolves both packs
        // unambiguously.
        // Only a DB-generated key has an id to return (#503/#572): update_sql()
        // omits RETURNING for a composite or storm::UUID key, so run() resolves
        // to std::expected<void, Error> there instead of the generated id.
        template <std::meta::info... Target> struct UpsertUpdateRunner {
            BasicInsertStatement*                                    self;
            template <std::meta::info... SetCols> [[nodiscard]] auto run(const T& obj) {
                using Result = std::conditional_t<
                        Base::pk_is_db_generated_(),
                        std::expected<std::int64_t, Error>,
                        std::expected<void, Error>>;

                const std::string sql = UpsertG::template update_sql<Target...>(
                        UpsertG::template build_excluded_set_clause<SetCols...>()
                );
                auto stmt_result = self->conn_->prepare_cached(sql);
                if (!stmt_result) {
                    return Result(std::unexpected(stmt_result.error()));
                }
                Statement* stmt = *stmt_result;
                // (1) bind VALUES params (non-PK fields, same as plain INSERT).
                if (auto bind_result = self->bind_all_fields(*stmt, obj); !bind_result) {
                    return Result(std::unexpected(bind_result.error()));
                }
                // (2) bind the trailing auto_update now() params (excluded.col targets bind nothing).
                if (auto bind_result = self->template bind_upsert_auto_updates<SetCols...>(stmt); !bind_result) {
                    return Result(std::unexpected(bind_result.error()));
                }

                if constexpr (!Base::pk_is_db_generated_()) {
                    return BasicInsertStatement::finish_void_step(stmt);
                } else {
                    const int rc = stmt->step_raw();
                    if (rc == Statement::ROW_AVAILABLE) {
                        const std::int64_t id = stmt->extract_int64(0);
                        stmt->reset();
                        return Result(id);
                    }
                    stmt->reset();
                    return Result(BasicInsertStatement::make_error(rc, stmt));
                }
            }
        };

        // Entry point: fixes Target... via UpsertUpdateRunner; callers then supply
        // SetCols... via run<>(). Used by the detail::UpdateUpsertQuery proxy.
        template <std::meta::info... Target>
        [[nodiscard]] __attribute__((always_inline)) auto upsert_update_runner() -> UpsertUpdateRunner<Target...> {
            return {this};
        }

      protected: // Changed to protected so BaseStatement can access
        // Bind non-PK fields for INSERT (skips primary key for auto-increment)
        [[nodiscard]] __attribute__((always_inline)) auto bind_all_fields(Statement& stmt, const T& obj) noexcept
                -> std::expected<void, Error> {
            return bind_single(stmt, obj);
        }

        // Execute ONE bulk INSERT with multiple VALUES clauses.
        // NOTE: No transaction wrapper needed - a single INSERT statement is already atomic.
        // R = Yes appends RETURNING <pk> and collects every returned id.
        template <ReturnId R>
        [[nodiscard]] __attribute__((hot)) auto execute_bulk(std::span<const T> objects) -> BulkResult<R> {
            // prepare_cached reuses prepared statements across iterations
            auto stmt_res = conn_->prepare_cached(get_bulk_insert_sql<R>(objects.size()));
            if (!stmt_res) {
                return std::unexpected(stmt_res.error());
            }
            Statement* stmt = *stmt_res;

            if (auto bound = bind_bulk(*stmt, objects); !bound) {
                return std::unexpected(bound.error());
            }

            if constexpr (R == ReturnId::Yes) {
                std::vector<std::int64_t> ids;
                ids.reserve(objects.size());

                int rc = 0;
                while ((rc = stmt->step_raw()) == Statement::ROW_AVAILABLE) {
                    ids.push_back(stmt->extract_int64(0));
                }
                stmt->reset();

                if (rc != Statement::NO_MORE_ROWS) {
                    return make_error(rc, stmt);
                }
                return ids;
            } else {
                return stmt->execute();
            }
        }

        // Execute CHUNKED bulk inserts with a given chunk size.
        // NOTE: Transaction wrapper required - multiple INSERT statements need atomicity.
        template <ReturnId R>
        [[nodiscard]] auto execute_chunked_bulk(std::span<const T> objects, std::size_t chunk_max) -> BulkResult<R> {
            auto txn = TransactionGuard<ConnType>::begin(conn_);
            if (!txn) {
                return std::unexpected(txn.error());
            }

            [[maybe_unused]] std::vector<std::int64_t> all_ids;
            if constexpr (R == ReturnId::Yes) {
                all_ids.reserve(objects.size());
            }

            for (std::size_t offset = 0; offset < objects.size(); offset += chunk_max) {
                const std::size_t chunk_size = std::min(chunk_max, objects.size() - offset);

                auto chunk_result = execute_bulk<R>(objects.subspan(offset, chunk_size));
                if (!chunk_result) {
                    return std::unexpected(chunk_result.error()); // ~TransactionGuard auto-rollbacks
                }
                if constexpr (R == ReturnId::Yes) {
                    all_ids.insert(all_ids.end(), chunk_result->begin(), chunk_result->end());
                }
            }

            if (auto commit_result = txn->commit(); !commit_result) {
                return std::unexpected(commit_result.error());
            }
            if constexpr (R == ReturnId::Yes) {
                return all_ids;
            } else {
                return {};
            }
        }

      private:
        // Error path shared by every step_raw() failure, kept out of line so the hot
        // paths stay compact.
        [[nodiscard]] static __attribute__((cold, noinline)) auto make_error(int rc, Statement* stmt)
                -> std::unexpected<Error> {
            return std::unexpected(Error{rc, stmt->get_error_message()});
        }

        // Bind now() for each unlisted auto_update column (#209), in declaration order,
        // starting right after the VALUES placeholders (excluded.col SET targets need
        // no params; only the trailing "col=?" auto_update tail does). Mirrors
        // UpdateStatement::bind_unlisted_auto_updates (update.cppm).
        template <std::meta::info... SetCols>
        [[nodiscard]] auto bind_upsert_auto_updates(Statement* stmt) noexcept -> std::expected<void, Error> {
            int        param_index = static_cast<int>(InsertG::placeholders_count()) + 1; // 1-based, after VALUES
            const auto now         = Base::batch_now();
            return bind_upsert_auto_updates_impl<SetCols...>(stmt, param_index, now, typename Base::field_indices_t{});
        }

        template <std::meta::info... SetCols, std::size_t... Is>
        [[nodiscard]] auto bind_upsert_auto_updates_impl(
                Statement*                            stmt,
                int&                                  param_index,
                std::chrono::system_clock::time_point now,
                std::index_sequence<Is...> /*unused*/
        ) noexcept -> std::expected<void, Error> {
            std::expected<void, Error> result{};
            (
                    [&] {
                        if constexpr (UpsertG::template is_unlisted_auto_update<SetCols...>(Base::all_members_[Is])) {
                            if (result.has_value()) {
                                result = Base::template bind_one<ConnType>(stmt, param_index, now);
                            }
                        }
                    }(),
                    ...
            );
            return result;
        }

        // Prepare (L3-cached) the given INSERT SQL and bind the object's non-PK
        // fields. Both single-row execute paths and the DO NOTHING upsert share this
        // so the prepare + bind + error-check sequence lives in one place.
        [[nodiscard]] __attribute__((always_inline)) auto
        prepare_and_bind(const std::string& sql, const T& obj) noexcept -> std::expected<Statement*, Error> {
            auto stmt_result = conn_->prepare_cached(sql);
            if (!stmt_result) [[unlikely]] {
                return std::unexpected(stmt_result.error());
            }
            Statement* stmt        = *stmt_result;
            auto       bind_result = Base::template bind_non_pk_fields_impl<ConnType, Statement>(
                    *stmt, obj, typename Base::field_indices_t()
            );
            if (!bind_result) [[unlikely]] {
                return std::unexpected(bind_result.error());
            }
            return stmt;
        }

        // Shared body of the to_sql* debug helpers: prepare the given SQL,
        // delegate binding to the caller, then return the expanded SQL string.
        template <typename BindFn>
        [[nodiscard]] __attribute__((cold)) auto to_sql_impl(const std::string& sql, BindFn&& bind_fn)
                -> std::expected<std::string, Error> {
            auto stmt_result = conn_->prepare_cached(sql);
            if (!stmt_result) {
                return std::unexpected(stmt_result.error());
            }
            auto* stmt        = *stmt_result;
            auto  bind_result = std::forward<BindFn>(bind_fn)(*stmt);
            if (!bind_result) {
                return std::unexpected(bind_result.error());
            }
            return stmt->expanded_sql();
        }

        // Binders shared between the single-row and bulk paths.
        [[nodiscard]] static __attribute__((always_inline)) auto bind_single(Statement& stmt, const T& obj)
                -> std::expected<void, Error> {
            return Base::template bind_non_pk_fields_impl<ConnType, Statement>(
                    stmt, obj, typename Base::field_indices_t()
            );
        }

        [[nodiscard]] static __attribute__((always_inline)) auto bind_bulk(Statement& stmt, std::span<const T> objects)
                -> std::expected<void, Error> {
            return Base::template bind_non_pk_objects_bulk_impl<ConnType, Statement>(
                    stmt, objects, typename Base::field_indices_t()
            );
        }

        std::shared_ptr<ConnType> conn_;
    };

} // namespace storm::orm::statements
