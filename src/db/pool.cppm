module;

// `duplicate` removed in #277 Phase 3 (count_entries(pred) helper shared by available() / in_use() counters).

#include <cassert> // Issue #271: assert() macro — import std; cannot deliver macros

export module storm_db_pool;

import std;
import storm_db_concept;

export namespace storm::db {

    struct PoolConfig {
        int         min_connections          = 1;       // Pre-created on init
        int         max_connections          = 10;      // Hard upper bound
        int         checkout_timeout_ms      = 5000;    // 0 = fail immediately
        int         shutdown_timeout_ms      = 5000;    // Issue #733: shutdown() wait bound (0 = don't wait)
        int         max_lifetime_ms          = 1800000; // 30 min (0 = never recycle)
        int         idle_timeout_ms          = 600000;  // 10 min (0 = never expire idle)
        bool        validate_on_checkout     = true;    // is_open() check before handing out
        std::size_t statement_cache_capacity = 512;     // Issue #273: 0 = unbounded
        // Issue #410: SQLite connection tuning propagated to each pooled connection.
        // Ignored by backends (PostgreSQL) whose Config lacks these fields.
        int         busy_timeout_ms = 5000;                 // Issue #410: 0 = no wait (legacy)
        JournalMode journal_mode    = JournalMode::Default; // Issue #410: WAL opt-in
    };

    // Forward declaration
    template <CachedDatabaseConnection ConnType> class ConnectionPool;

    namespace detail {

        using Clock     = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        // Issue #410: build the backend-specific open() Config from a PoolConfig.
        // The SQLite-only tuning fields are set only when the backend's Config
        // actually has them (detected via requires), so this stays compilable for
        // PostgreSQL whose Config is just StatementCacheConfig. Also dedupes the
        // two identical inline braced-inits in try_grow()/create_entry().
        template <CachedDatabaseConnection ConnType>
        [[nodiscard]] auto make_conn_config(const PoolConfig& pool_config) -> typename ConnType::Config {
            typename ConnType::Config cfg{};
            cfg.statement_cache_capacity = pool_config.statement_cache_capacity;
            if constexpr (requires { cfg.busy_timeout_ms; }) {
                cfg.busy_timeout_ms = pool_config.busy_timeout_ms;
                cfg.journal_mode    = pool_config.journal_mode;
            }
            return cfg;
        }

        template <CachedDatabaseConnection ConnType> struct PoolEntry {
            std::unique_ptr<ConnType> conn;
            bool                      in_use       = false;
            TimePoint                 created_at   = Clock::now();
            TimePoint                 last_used_at = Clock::now();
#ifndef NDEBUG
            // Issue #271: debug-only single-owner tracking. Records which thread
            // currently holds this connection between checkout and checkin so a
            // concurrent double-checkout (which would let two threads share one
            // Connection's L3 cache) trips an assertion. Release builds pay
            // nothing — the field and its checks compile out.
            std::thread::id owner{};
#endif
        };

        // Issue #271: debug-only single-owner assertions on checkout/checkin.
        // No-ops in Release (NDEBUG). Called under the pool mutex_, so reading
        // and stamping `owner` needs no extra synchronization.
        template <typename Entry> auto debug_claim_owner([[maybe_unused]] Entry& entry) -> void {
#ifndef NDEBUG
            assert(entry.owner == std::thread::id{} && "Connection checked out by two threads at once (#271)");
            entry.owner = std::this_thread::get_id();
#endif
        }

        template <typename Entry> auto debug_release_owner([[maybe_unused]] Entry& entry) -> void {
#ifndef NDEBUG
            assert(entry.owner == std::this_thread::get_id() &&
                   "Connection checked in by a thread that did not own it (#271)");
            entry.owner = std::thread::id{};
#endif
        }

        template <CachedDatabaseConnection ConnType> class PoolCore {
          public:
            using Error = typename ConnType::Error;

            PoolCore(std::string conninfo, PoolConfig config) : conninfo_(std::move(conninfo)), config_(config) {
                entries_.reserve(static_cast<std::size_t>(config_.max_connections));
            }

            [[nodiscard]] auto init() -> std::expected<void, Error> {
                for (int i = 0; i < config_.min_connections; ++i) {
                    auto result = create_entry();
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                }
                return {};
            }

            auto checkout() -> std::expected<ConnType*, Error> {
                std::unique_lock lock(mutex_);

                if (shutdown_) {
                    return std::unexpected(Error{-1, "Pool is shut down"});
                }
                evict_expired();
                if (auto* entry = find_idle(); entry != nullptr) {
                    return entry;
                }
                if (auto grown = try_grow(lock); grown.has_value()) {
                    return *grown;
                }
                if (config_.checkout_timeout_ms == 0) {
                    return std::unexpected(Error{-1, "Pool exhausted (timeout=0)"});
                }
                auto deadline = Clock::now() + std::chrono::milliseconds(config_.checkout_timeout_ms);
                return wait_for_idle(lock, deadline);
            }

            auto checkin(ConnType* conn) -> void {
                std::lock_guard lock(mutex_);
                auto            it =
                        std::ranges::find_if(entries_, [conn](const auto& entry) { return entry.conn.get() == conn; });
                // LCOV_EXCL_START — a handle's pointer is always in entries_ (in-use entries are never erased)
                if (it == entries_.end()) {
                    return;
                }
                // LCOV_EXCL_STOP
                debug_release_owner(*it); // Issue #271
                it->in_use = false;
                if (shutdown_) {
                    // Issue #733: a return after shutdown() began (possibly after it timed
                    // out) closes the connection now rather than parking it until the core dies.
                    entries_.erase(it);
                    cv_.notify_all(); // every concurrent shutdown() waiter, not just one
                    return;
                }
                it->last_used_at = Clock::now();
                cv_.notify_one();
            }

            [[nodiscard]] auto size() const -> int {
                std::lock_guard lock(mutex_);
                return static_cast<int>(entries_.size());
            }

            // Count entries matching a predicate. The available() / in_use()
            // counters used to repeat the lock + loop + count body verbatim;
            // they now differ only in which entries pass the predicate.
            template <typename Pred> [[nodiscard]] auto count_entries(Pred pred) const -> int {
                std::lock_guard lock(mutex_);
                return static_cast<int>(std::ranges::count_if(entries_, pred));
            }

            [[nodiscard]] auto available() const -> int {
                return count_entries([](const auto& entry) { return !entry.in_use; });
            }

            [[nodiscard]] auto in_use() const -> int {
                return count_entries([](const auto& entry) { return entry.in_use; });
            }

            // Issue #733: bounded wait. Returns true when every connection came back
            // and the pool was torn down; false when `shutdown_timeout_ms` elapsed first
            // (a leaked handle, or the calling thread's own DefaultConnection slot —
            // which nobody else can return). On timeout only IDLE entries are released:
            // a connection still checked out is never destroyed under its owner, and its
            // late checkin finds the entry still present.
            [[nodiscard]] auto shutdown() -> bool {
                std::unique_lock lock(mutex_);
                shutdown_ = true;
                cv_.notify_all();

                auto all_returned = [this] {
                    return std::ranges::none_of(entries_, [](const auto& entry) { return entry.in_use; });
                };
                const bool drained =
                        cv_.wait_for(lock, std::chrono::milliseconds(config_.shutdown_timeout_ms), all_returned);
                if (drained) {
                    entries_.clear();
                } else {
                    std::erase_if(entries_, [](const auto& entry) { return !entry.in_use; });
                }
                return drained;
            }

          private:
            // Remove expired idle connections (called under lock)
            auto evict_expired() -> void {
                if (config_.max_lifetime_ms <= 0 && config_.idle_timeout_ms <= 0) {
                    return;
                }

                auto now = Clock::now();
                auto it  = entries_.begin();
                while (it != entries_.end()) {
                    if (it->in_use) {
                        ++it;
                        continue;
                    }

                    bool expired = false;
                    if (config_.max_lifetime_ms > 0) {
                        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->created_at).count();
                        expired  = age > config_.max_lifetime_ms;
                    }
                    if (!expired && config_.idle_timeout_ms > 0) {
                        auto idle =
                                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->last_used_at).count();
                        expired = idle > config_.idle_timeout_ms;
                    }

                    if (expired) {
                        it = entries_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // Find an idle connection, validating if configured.
            // Stale connections (is_open() == false) are erased rather than replaced
            // under the lock — the caller grows the pool outside the lock if needed.
            auto find_idle() -> ConnType* {
                auto it = entries_.begin();
                while (it != entries_.end()) {
                    if (it->in_use) {
                        ++it;
                        continue;
                    }
                    if (config_.validate_on_checkout && !it->conn->is_open()) {
                        it = entries_.erase(it);
                        continue;
                    }
                    it->in_use = true;
                    debug_claim_owner(*it); // Issue #271
                    return it->conn.get();
                }
                return nullptr;
            }

            // Try to add a new connection to the pool. Releases `lock` while opening
            // (which can be slow) and re-acquires it before returning.
            //
            // Return value semantics:
            //   - has_value() == true  → terminal result: success (ConnType*) OR error
            //   - has_value() == false → caller should fall through to wait/timeout path
            //                            (pool was already at max when called, or
            //                             reached max via another thread during open())
            auto try_grow(std::unique_lock<std::mutex>& lock) -> std::optional<std::expected<ConnType*, Error>> {
                if (static_cast<int>(entries_.size()) >= config_.max_connections) {
                    return std::nullopt;
                }
                lock.unlock();
                auto result = ConnType::open(conninfo_, make_conn_config<ConnType>(config_));
                lock.lock();

                if (shutdown_) {
                    return std::expected<ConnType*, Error>{std::unexpected(Error{-1, "Pool is shut down"})};
                }
                if (!result) {
                    return std::expected<ConnType*, Error>{std::unexpected(result.error())};
                }
                // Re-check: another thread may have grown the pool while we were unlocked.
                //
                // Both arms are race-only, reachable solely when another thread reaches
                // max_connections during the open() window above. The find_idle() arm is
                // covered deterministically by MockPoolTest.Checkout_RaceMaxReachedWithIdle
                // (#544) — before that test existed it was hit only incidentally, by the
                // 10-thread scramble in Checkout_RaceConditionMaxReached, which made the CI
                // 100%-line-coverage gate flake at 99.9% on exactly these two lines. If they
                // ever show up uncovered again, suspect that test, not a real coverage gap.
                if (static_cast<int>(entries_.size()) >= config_.max_connections) {
                    if (auto* idle = find_idle(); idle != nullptr) {
                        return std::expected<ConnType*, Error>{idle};
                    }
                    // Race-only fall-through: pool hit max during unlock + all slots in
                    // use. Covered by MockPoolTest.Checkout_RaceMaxReachedNoIdle (#544);
                    // was LCOV_EXCL_LINE until that test made it deterministic.
                    return std::nullopt;
                }
                auto now = Clock::now();
                entries_.emplace_back(std::make_unique<ConnType>(std::move(result.value())), true, now, now);
                debug_claim_owner(entries_.back()); // Issue #271
                return std::expected<ConnType*, Error>{entries_.back().conn.get()};
            }

            // Block on cv_ until an idle entry is available, deadline hits, or shutdown.
            auto wait_for_idle(std::unique_lock<std::mutex>& lock, TimePoint deadline)
                    -> std::expected<ConnType*, Error> {
                while (true) {
                    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                        return std::unexpected(Error{-1, "Pool checkout timed out"});
                    }
                    if (shutdown_) {
                        return std::unexpected(Error{-1, "Pool is shut down"});
                    }
                    evict_expired();
                    if (auto* found = find_idle(); found != nullptr) {
                        return found;
                    }
                }
            }

            auto create_entry() -> std::expected<void, Error> {
                auto result = ConnType::open(conninfo_, make_conn_config<ConnType>(config_));
                if (!result) {
                    return std::unexpected(result.error());
                }
                auto now = Clock::now();
                entries_.emplace_back(std::make_unique<ConnType>(std::move(result.value())), false, now, now);
                return {};
            }

            std::string                      conninfo_;
            PoolConfig                       config_;
            std::vector<PoolEntry<ConnType>> entries_;
            mutable std::mutex               mutex_;
            std::condition_variable          cv_;
            bool                             shutdown_ = false;
        };

    } // namespace detail

    template <CachedDatabaseConnection ConnType> class ConnectionPool {
      public:
        using Error = typename ConnType::Error;

        [[nodiscard]] static auto create(std::string_view conninfo, PoolConfig config = {})
                -> std::expected<ConnectionPool, Error> {
            if (config.min_connections < 0 || config.max_connections <= 0 ||
                config.min_connections > config.max_connections || config.checkout_timeout_ms < 0 ||
                config.shutdown_timeout_ms < 0 || config.max_lifetime_ms < 0 || config.idle_timeout_ms < 0) {
                return std::unexpected(Error{-1, "Invalid pool config: min >= 0, max > 0, min <= max, timeouts >= 0"});
            }

            auto core        = std::make_shared<detail::PoolCore<ConnType>>(std::string(conninfo), config);
            auto init_result = core->init();
            if (!init_result) {
                return std::unexpected(init_result.error());
            }

            return ConnectionPool{std::move(core)};
        }

        [[nodiscard]] auto // NOSONAR(cpp:S1659) — single declaration, template return type not multiple identifiers
        checkout() -> std::expected<std::shared_ptr<ConnType>, Error> {
            auto result = core_->checkout();
            if (!result) {
                return std::unexpected(result.error());
            }

            ConnType* raw = result.value();
            // Capture shared_ptr to PoolCore — keeps pool alive until all connections returned.
            // This prevents use-after-free: PoolCore owns connection memory via unique_ptr,
            // so it must outlive all checked-out shared_ptr<ConnType> handles.
            std::shared_ptr<detail::PoolCore<ConnType>> core_ref = core_;

            return std::shared_ptr<ConnType>(raw, [core_ref](ConnType* p) { core_ref->checkin(p); });
        }

        [[nodiscard]] auto size() const -> int {
            return core_->size();
        }

        [[nodiscard]] auto available() const -> int {
            return core_->available();
        }

        [[nodiscard]] auto in_use() const -> int {
            return core_->in_use();
        }

        // Issue #733: false = timed out with connections still checked out (see PoolCore::shutdown).
        [[nodiscard]] auto shutdown() -> bool {
            return core_->shutdown();
        }

      private:
        explicit ConnectionPool(std::shared_ptr<detail::PoolCore<ConnType>> core) : core_(std::move(core)) {}

        std::shared_ptr<detail::PoolCore<ConnType>> core_;
    };

} // namespace storm::db
