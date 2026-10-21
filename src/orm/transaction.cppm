module;

#include <meta>

export module storm_orm_transaction;

import std;
import storm_db_concept;

export namespace storm::orm::utilities {

    // ============================================================================
    // Transaction Guard - RAII-style transaction management
    // ============================================================================
    //
    // Usage:
    //   auto txn = TransactionGuard<ConnType>::begin(conn_);
    //   if (!txn) return std::unexpected(txn.error());
    //
    //   auto result = do_work();
    //   if (!result) return result;  // Destructor will ROLLBACK
    //
    //   return txn->commit();  // Explicit COMMIT, returns expected<void, Error>
    //
    // Destructor automatically ROLLBACKs if commit() was not called.
    // ============================================================================

    template <db::TransactionCapable ConnType> class TransactionGuard {
        using Error = typename ConnType::Error;

        std::shared_ptr<ConnType> conn_;
        bool                      committed_ = false;
        // #415/#9: a passive guard sits inside an already-open transaction. It
        // issued no BEGIN, owns no depth, and its commit()/rollback are no-ops —
        // the outer (real) guard remains the sole owner of the transaction. This
        // lets batch ops (which always TransactionGuard::begin) nest harmlessly.
        bool passive_ = false;

        explicit TransactionGuard(std::shared_ptr<ConnType> conn, bool passive) noexcept
            : conn_(std::move(conn)), passive_(passive) {}

      public:
        // Factory method - begins transaction, returns expected.
        // If the connection is already inside a transaction (an outer guard is
        // active), this returns a passive guard instead of issuing a nested BEGIN
        // — the fix for #9, so nested batch ops cooperate with the outer scope.
        [[nodiscard]] static auto begin(std::shared_ptr<ConnType> conn) noexcept
                -> std::expected<TransactionGuard, Error> {
            if (conn->in_transaction()) {
                return TransactionGuard(std::move(conn), /*passive=*/true);
            }
            if (auto result = conn->execute("BEGIN TRANSACTION"); !result) {
                return std::unexpected(result.error());
            }
            // Mark the connection as inside a transaction (#415) so nested batch
            // ops skip their own BEGIN/COMMIT (#9). Paired with leave_transaction()
            // on the single terminal path (commit() or rollback_if_needed()).
            conn->enter_transaction();
            return TransactionGuard(std::move(conn), /*passive=*/false);
        }

        // Move-only (no copying)
        TransactionGuard(const TransactionGuard&)                    = delete;
        auto operator=(const TransactionGuard&) -> TransactionGuard& = delete;

        TransactionGuard(TransactionGuard&& other) noexcept
            : conn_(std::move(other.conn_)), committed_(other.committed_), passive_(other.passive_) {
            // Neutralize the source so a moved-from guard can never attempt a DB op,
            // independent of std::move(shared_ptr) having nulled other.conn_.
            other.committed_ = true;
        }

        auto operator=(TransactionGuard&& other) noexcept -> TransactionGuard& {
            if (this != &other) {
                rollback_if_needed();
                conn_      = std::move(other.conn_);
                committed_ = other.committed_;
                passive_   = other.passive_;
                // Neutralize the source so a moved-from guard can never rollback.
                other.committed_ = true;
            }
            return *this;
        }

        // Destructor - rollback if not committed
        ~TransactionGuard() {
            rollback_if_needed();
        }

        // Explicit commit - must be called for successful transaction
        [[nodiscard]] auto commit() noexcept -> std::expected<void, Error> {
            if (!conn_ || committed_) {
                return {}; // Already committed or moved-from
            }

            if (passive_) {
                // Nested inside an outer transaction (#9): the outer guard owns the
                // real COMMIT/ROLLBACK. Just retire this guard with no SQL.
                committed_ = true;
                return {};
            }

            if (auto result = conn_->execute("COMMIT"); !result) {
                // Best-effort ROLLBACK. execute() is not noexcept (it builds a
                // std::string / inserts into the statement cache and can throw
                // std::bad_alloc under memory pressure); swallow any throw so it
                // never escapes this noexcept function and calls std::terminate.
                try {
                    (void)conn_->execute("ROLLBACK");
                } catch (...) { // NOSONAR(cpp:S2486) NOLINT(bugprone-empty-catch)
                }
                // Transaction is already rolled back; mark committed so the
                // destructor does not issue a second, redundant ROLLBACK.
                conn_->leave_transaction(); // #415: drop nesting depth.
                committed_ = true;
                return std::unexpected(result.error());
            }

            conn_->leave_transaction(); // #415: drop nesting depth.
            committed_ = true;
            return {};
        }

      private:
        auto rollback_if_needed() noexcept -> void {
            if (conn_ && !committed_ && passive_) {
                // Nested inside an outer transaction (#9): never ROLLBACK here —
                // that would abort the outer scope's work. Surfacing the failure
                // is the inner op's job (it returns std::unexpected); the outer
                // guard then rolls the whole transaction back.
                return;
            }
            if (conn_ && !committed_) {
                // Best-effort ROLLBACK during unwinding. execute() is not noexcept
                // (it builds a std::string / inserts into the statement cache and
                // can throw std::bad_alloc under memory pressure); swallow any
                // throw so it never escapes this noexcept function — a throw here
                // would call std::terminate (the throwing-destructor trap).
                try {
                    (void)conn_->execute("ROLLBACK");
                } catch (...) { // NOSONAR(cpp:S2486) NOLINT(bugprone-empty-catch)
                }
                // #415: drop nesting depth. Guarded by !committed_, so this runs
                // exactly once per guard (commit() also marks committed_).
                conn_->leave_transaction();
            }
        }
    };

} // namespace storm::orm::utilities

export namespace storm {

    template <typename ConnType> using TransactionGuard = orm::utilities::TransactionGuard<ConnType>;

    template <db::TransactionCapable ConnType>
    [[nodiscard]] auto begin(std::shared_ptr<ConnType> conn)
            -> std::expected<TransactionGuard<ConnType>, typename ConnType::Error> {
        return orm::utilities::TransactionGuard<ConnType>::begin(std::move(conn));
    }

    template <db::TransactionCapable ConnType, typename Body>
    [[nodiscard]] auto transaction(std::shared_ptr<ConnType> conn, Body&& body)
            -> std::invoke_result_t<Body&, TransactionGuard<ConnType>&> {
        using Ret = std::invoke_result_t<Body&, TransactionGuard<ConnType>&>;

        auto guard = orm::utilities::TransactionGuard<ConnType>::begin(std::move(conn));
        if (!guard) {
            return Ret(std::unexpect, guard.error());
        }

        Ret result = std::forward<Body>(body)(*guard);
        if (!result) {
            return result; // guard destructor ROLLBACKs on the way out.
        }

        if (auto commit_result = guard->commit(); !commit_result) {
            return Ret(std::unexpect, commit_result.error());
        }
        return result;
    }
} // namespace storm
