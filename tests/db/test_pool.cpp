// LINT-EXCLUDE-FILE: file-size, duplicate
// Pre-existing structural debt — large test file with repeated TEST() boilerplate
// across many backend types. Tracked under issue #264 Phase 1.
#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length,cppcoreguidelines-special-member-functions) // GTest fixtures use protected connstr_; tests use short connection names c1/c2/c3; MockConfigGuard is RAII-only. Pre-existing; tracked under #262/#264.

import storm_defaults;
import storm_db_concept;
import storm_db_default_connection;
import storm_db_pool;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
using storm::db::ConnectionPool;

// ============================================================================
// Mock connection for testing pool error/edge-case paths
// ============================================================================

namespace {
    using storm::db::DefaultConnection;

    // Controls MockPoolConnection behavior.
    //
    // INVARIANT: every field except open_call_count must be written only while no pool
    // thread is live (i.e. before the first std::thread is spawned, or after join()).
    // The threaded tests rely on the std::thread constructor to publish those writes;
    // mutating one while a thread is parked inside open() is a data race. open_call_count
    // is exempt — open() bumps it from every opening thread, so it is atomic (TSAN runs
    // on every PR).
    struct MockConfig {
        bool             open_should_fail = false;
        bool             is_open_returns  = true;
        int              open_fail_after  = -1; // fail after N successful opens (-1 = never)
        std::atomic<int> open_call_count  = 0;
        int              open_delay_ms    = 0;       // artificial delay in open() to test race conditions
        std::latch*      open_entered     = nullptr; // counted down on entry to open() (lock already released)
        std::latch*      open_release     = nullptr; // if set, open() blocks on it before returning
        // Which open() call the open_entered/open_release handshake applies to (1-based;
        // 0 = every call). Needed when a test must let a SECOND thread open a connection
        // while the first is parked inside open() — see Checkout_RaceMaxReachedWithIdle.
        int open_gate_call = 0;
    };

    inline auto mock_config() -> MockConfig& {
        static MockConfig cfg; // NOSONAR
        return cfg;
    }

} // namespace

struct MockStatement {
    struct Error {
        int         code_;
        std::string message_;

        [[nodiscard]] constexpr auto code() const noexcept -> int {
            return code_;
        }
        [[nodiscard]] constexpr auto message() const noexcept -> std::string_view {
            return message_;
        }
    };

    static auto bind_int(int /*unused*/, int /*unused*/) -> std::expected<void, Error> {
        return {};
    }
    static auto bind_text(int /*unused*/, std::string_view /*unused*/) -> std::expected<void, Error> {
        return {};
    }
    static auto execute() -> std::expected<void, Error> {
        return {};
    }
    static auto step() -> std::expected<bool, Error> {
        return false;
    }
    auto reset() -> void {}
    auto finalize() -> void {}
};

struct MockPoolConnection {
    struct Error {
        int         code_;
        std::string message_;

        [[nodiscard]] constexpr auto code() const noexcept -> int {
            return code_;
        }
        [[nodiscard]] constexpr auto message() const noexcept -> std::string_view {
            return message_;
        }
    };
    using Statement = MockStatement;
    using Config    = storm::db::StatementCacheConfig; // Issue #410: matches real backends' Config alias

    [[nodiscard]] static auto open(std::string_view /*unused*/, Config /*unused*/ = {})
            -> std::expected<MockPoolConnection, Error> {
        auto& cfg = mock_config();
        // open_call_count is read (not just written) by the gate below, and open() runs
        // concurrently on several threads in the race tests — increment atomically and
        // keep this call's own 1-based number.
        int const this_call = ++cfg.open_call_count;
        // The handshake applies to every open() by default, or to one specific call when
        // open_gate_call is set — a test that needs a second thread to open a connection
        // while the first is parked in open() must not park the second one too.
        bool const gated = cfg.open_gate_call == 0 || cfg.open_gate_call == this_call;
        // Signal that open() was entered — the caller has already released the pool
        // lock at this point, so a waiter can deterministically acquire it (e.g. to
        // call shutdown() before this open() returns). See Checkout_ShutdownDuringGrow.
        if (gated && cfg.open_entered != nullptr) {
            cfg.open_entered->count_down();
        }
        // Optional handshake: block until the test releases us (e.g. after it has run
        // shutdown() while we hold no lock). Deterministic alternative to open_delay_ms.
        if (gated && cfg.open_release != nullptr) {
            cfg.open_release->wait();
        }
        if (cfg.open_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.open_delay_ms));
        }
        if (cfg.open_should_fail) {
            return std::unexpected(Error{-1, "Mock open failure"});
        }
        if (cfg.open_fail_after >= 0 && this_call > cfg.open_fail_after) {
            return std::unexpected(Error{-1, "Mock open failure after threshold"});
        }
        return MockPoolConnection{};
    }

    [[nodiscard]] auto is_open() const -> bool {
        return is_open_;
    }

    static auto prepare(std::string_view /*unused*/) -> std::expected<Statement, Error> {
        return Statement{};
    }
    static auto execute(std::string_view /*unused*/) -> std::expected<void, Error> {
        return {};
    }
    auto prepare_cached(std::string_view /*unused*/) -> std::expected<Statement*, Error> {
        return &cached_stmt_;
    }
    auto                      clear_statement_cache() -> void {}
    auto                      clear_statement_cache(std::string_view /*unused*/) -> void {}
    [[nodiscard]] static auto cached_statement_count() -> std::size_t {
        return 0;
    }
    [[nodiscard]] static auto cache_stats() -> storm::db::CacheStats { // Issue #273
        return {};
    }

    bool is_open_ = true;

  private:
    Statement cached_stmt_;
};

// Guard to reset mock config after each test. Field-by-field rather than
// `= MockConfig{}` because the atomic member deletes MockConfig's copy-assignment
// operator, so the struct is no longer assignable at all.
struct MockConfigGuard {
    static auto reset() -> void {
        auto& cfg            = mock_config();
        cfg.open_should_fail = false;
        cfg.is_open_returns  = true;
        cfg.open_fail_after  = -1;
        cfg.open_call_count.store(0);
        cfg.open_delay_ms  = 0;
        cfg.open_entered   = nullptr;
        cfg.open_release   = nullptr;
        cfg.open_gate_call = 0;
    }
    MockConfigGuard() {
        reset();
    }
    ~MockConfigGuard() {
        reset();
    }
};

namespace {

    template <typename ConnType> auto get_pool_connstr() -> std::string {
        return storm::test::get_connection_string<ConnType>();
    }

    template <typename ConnType> auto skip_if_unavailable() -> bool {
        return !storm::test::backend_available<ConnType>();
    }

} // namespace

// ============================================================================
// Construction Tests
// ============================================================================

template <typename ConnType> class PoolCreateTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }
    std::string connstr_;
};

TYPED_TEST_SUITE(PoolCreateTest, DatabaseTypes);

TYPED_TEST(PoolCreateTest, CreatePool_ValidConfig) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 2, .max_connections = 5});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();
    EXPECT_EQ(pool->size(), 2);
    EXPECT_EQ(pool->available(), 2);
    EXPECT_EQ(pool->in_use(), 0);
}

TYPED_TEST(PoolCreateTest, CreatePool_BadConfig_MinGreaterThanMax) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 10, .max_connections = 5});
    EXPECT_FALSE(pool.has_value());
}

TYPED_TEST(PoolCreateTest, CreatePool_BadConfig_NegativeMin) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = -1, .max_connections = 5});
    EXPECT_FALSE(pool.has_value());
}

TYPED_TEST(PoolCreateTest, CreatePool_BadConfig_ZeroMax) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 0, .max_connections = 0});
    EXPECT_FALSE(pool.has_value());
}

TYPED_TEST(PoolCreateTest, CreatePool_ZeroMin) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 0, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();
    EXPECT_EQ(pool->size(), 0);
    EXPECT_EQ(pool->available(), 0);
}

TYPED_TEST(PoolCreateTest, CreatePool_InvalidConnstring) {
    if constexpr (storm::test::is_postgresql<TypeParam>()) {
        auto pool = ConnectionPool<TypeParam>::create(
                "host=invalid_host_that_does_not_exist port=99999 dbname=nonexistent connect_timeout=1",
                {.min_connections = 1, .max_connections = 5}
        );
        EXPECT_FALSE(pool.has_value());
    } else {
        // SQLite :memory: always succeeds; verify the API works
        auto pool = ConnectionPool<TypeParam>::create(":memory:", {.min_connections = 1, .max_connections = 5});
        EXPECT_TRUE(pool.has_value());
    }
}

// ============================================================================
// Checkout/Checkin Tests
// ============================================================================

template <typename ConnType> class PoolCheckoutTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }
    std::string connstr_;
};

TYPED_TEST_SUITE(PoolCheckoutTest, DatabaseTypes);

TYPED_TEST(PoolCheckoutTest, Checkout_ReturnsValid) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto conn = pool->checkout();
    ASSERT_TRUE(conn.has_value()) << conn.error().message();
    EXPECT_TRUE((*conn)->is_open());
    EXPECT_EQ(pool->in_use(), 1);
}

TYPED_TEST(PoolCheckoutTest, Checkout_ReturnOnRelease) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    {
        auto conn = pool->checkout();
        ASSERT_TRUE(conn.has_value());
        EXPECT_EQ(pool->in_use(), 1);
        EXPECT_EQ(pool->available(), 0);
    } // conn released — custom deleter returns to pool
    EXPECT_EQ(pool->in_use(), 0);
    EXPECT_EQ(pool->available(), 1);
}

TYPED_TEST(PoolCheckoutTest, Checkout_Multiple) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 5});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto c1 = pool->checkout();
    auto c2 = pool->checkout();
    auto c3 = pool->checkout();
    ASSERT_TRUE(c1.has_value());
    ASSERT_TRUE(c2.has_value());
    ASSERT_TRUE(c3.has_value());
    EXPECT_EQ(pool->in_use(), 3);
    EXPECT_EQ(pool->size(), 3);
}

TYPED_TEST(PoolCheckoutTest, Checkout_CachePreserved) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    TypeParam* raw_ptr = nullptr;
    {
        auto conn = pool->checkout();
        ASSERT_TRUE(conn.has_value());
        raw_ptr   = conn->get();
        auto stmt = raw_ptr->prepare_cached("SELECT 1");
        ASSERT_TRUE(stmt.has_value());
        EXPECT_GE(raw_ptr->cached_statement_count(), 1U);
    }
    // Re-checkout — should get same connection with cache intact
    auto conn2 = pool->checkout();
    ASSERT_TRUE(conn2.has_value());
    EXPECT_EQ(conn2->get(), raw_ptr);
    EXPECT_GE(conn2->get()->cached_statement_count(), 1U);
}

TYPED_TEST(PoolCheckoutTest, Checkout_GrowsOnDemand) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 0, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();
    EXPECT_EQ(pool->size(), 0);

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());
    EXPECT_EQ(pool->size(), 1);

    auto c2 = pool->checkout();
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(pool->size(), 2);
}

TYPED_TEST(PoolCheckoutTest, Checkout_ReusesReturnedConnection) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 1});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    TypeParam const* first_raw = nullptr;
    {
        auto c1 = pool->checkout();
        ASSERT_TRUE(c1.has_value());
        first_raw = c1->get();
    }
    auto c2 = pool->checkout();
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(c2->get(), first_raw); // Same connection reused
}

// ============================================================================
// Exhaustion & Timeout Tests
// ============================================================================

template <typename ConnType> class PoolExhaustionTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }
    std::string connstr_;
};

TYPED_TEST_SUITE(PoolExhaustionTest, DatabaseTypes);

TYPED_TEST(PoolExhaustionTest, Checkout_ExhaustedImmediate) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 1, .checkout_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());
    auto c2 = pool->checkout();
    EXPECT_FALSE(c2.has_value());
}

TYPED_TEST(PoolExhaustionTest, Checkout_WaitsForReturn) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 1, .checkout_timeout_ms = 2000}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());

    std::atomic<bool> got_connection{false};
    std::latch        worker_started{1};
    std::thread       worker([&pool, &got_connection, &worker_started] {
        worker_started.count_down();
        auto c         = pool->checkout();
        got_connection = c.has_value();
    });

    // Deterministic: wait until the worker is at checkout(), then release c1. The
    // worker either finds the freed conn via find_idle() or is woken by checkin's
    // notify_one() — both yield success (#307, replaces a 100ms sync sleep).
    worker_started.wait();
    c1->reset(); // Releases the shared_ptr, triggering checkin
    worker.join();
    EXPECT_TRUE(got_connection);
}

TYPED_TEST(PoolExhaustionTest, Checkout_NeverExceedsMax) {
    constexpr int max  = 3;
    auto          pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = max, .checkout_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    // Checkout all max connections
    std::vector<std::shared_ptr<TypeParam>> conns;
    for (int i = 0; i < max; ++i) {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value()) << "Failed on checkout " << i;
        conns.push_back(std::move(*c));
    }
    EXPECT_EQ(pool->size(), max);
    EXPECT_EQ(pool->in_use(), max);
    EXPECT_EQ(pool->available(), 0);

    // Next checkout must fail — pool must not grow beyond max
    auto overflow = pool->checkout();
    EXPECT_FALSE(overflow.has_value());
    EXPECT_EQ(pool->size(), max); // Still max, not max+1
}

TYPED_TEST(PoolExhaustionTest, Checkout_WaitsAndReusesReturned) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 1, .checkout_timeout_ms = 2000}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());
    TypeParam const* original_raw = c1->get();

    std::atomic<TypeParam*> worker_raw{nullptr};
    std::latch              worker_started{1};
    std::thread             worker([&pool, &worker_raw, &worker_started] {
        worker_started.count_down();
        auto c = pool->checkout();
        if (c.has_value()) {
            worker_raw = c->get();
        }
    });

    // Deterministic: wait until the worker is at checkout(), then release c1. With
    // max=1 only one connection ever exists, so the worker gets the SAME one back
    // (reuse, not new) under every interleaving (#307, replaces a 100ms sync sleep).
    worker_started.wait();
    c1->reset();
    worker.join();

    EXPECT_EQ(worker_raw.load(), original_raw); // Reused, not new
    EXPECT_EQ(pool->size(), 1);                 // Never grew beyond max
}

TYPED_TEST(PoolExhaustionTest, Checkout_TimesOut) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 1, .checkout_timeout_ms = 50}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());

    auto start = std::chrono::steady_clock::now();
    auto c2    = pool->checkout();
    auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    EXPECT_FALSE(c2.has_value());
    EXPECT_GE(elapsed, 40); // Allow some tolerance
}

// ============================================================================
// Shutdown Tests
// ============================================================================

template <typename ConnType> class PoolShutdownTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }
    std::string connstr_;
};

TYPED_TEST_SUITE(PoolShutdownTest, DatabaseTypes);

TYPED_TEST(PoolShutdownTest, Shutdown_RejectsNew) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    pool->shutdown();
    auto conn = pool->checkout();
    EXPECT_FALSE(conn.has_value());
}

TYPED_TEST(PoolShutdownTest, Shutdown_WaitsForReturns) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto conn = pool->checkout();
    ASSERT_TRUE(conn.has_value());

    std::atomic<bool> shutdown_done{false};
    std::latch        shutdown_started{1};
    std::thread       shutdown_thread([&pool, &shutdown_done, &shutdown_started] {
        shutdown_started.count_down();
        pool->shutdown();
        shutdown_done = true;
    });

    // Deterministic: shutdown() blocks in cv_.wait until in_use == 0, so shutdown_done
    // cannot become true while `conn` is held — regardless of how far the thread has
    // progressed. The latch only confirms the thread is running before we assert
    // (#307, replaces a 50ms sync sleep).
    shutdown_started.wait();
    EXPECT_FALSE(shutdown_done);

    conn->reset(); // Return connection
    shutdown_thread.join();
    EXPECT_TRUE(shutdown_done);
}

// ============================================================================
// Thread Safety Tests (designed to trigger TSAN)
// ============================================================================

template <typename ConnType> class PoolThreadSafetyTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }
    std::string connstr_;
};

TYPED_TEST_SUITE(PoolThreadSafetyTest, DatabaseTypes);

TYPED_TEST(PoolThreadSafetyTest, ConcurrentCheckoutCheckin) {
    // SQLite :memory: creates isolated DBs, use shared cache for pooling test
    std::string const connstr = [this]() -> std::string {
        if constexpr (storm::test::is_sqlite<TypeParam>()) {
            return "file::memory:?cache=shared";
        } else {
            return this->connstr_;
        }
    }();

    auto pool = ConnectionPool<TypeParam>::create(
            connstr, {.min_connections = 2, .max_connections = 5, .checkout_timeout_ms = 5000}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    constexpr int            num_threads    = 10;
    constexpr int            num_iterations = 100;
    std::atomic<int>         success_count{0};
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&pool, &success_count] {
            for (int i = 0; i < num_iterations; ++i) {
                auto conn = pool->checkout();
                if (conn.has_value()) {
                    ++success_count;
                }
                // shared_ptr release triggers checkin
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count, num_threads * num_iterations);
    EXPECT_EQ(pool->in_use(), 0);
}

// ============================================================================
// QuerySet Integration Tests
// ============================================================================

template <typename ConnType> class PoolQuerySetTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }

    auto TearDown() -> void override {
        DefaultConnection<ConnType>::clear();
    }

    std::string connstr_;
};

TYPED_TEST_SUITE(PoolQuerySetTest, DatabaseTypes);

TYPED_TEST(PoolQuerySetTest, QuerySet_FromPool) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto result = DefaultConnection<TypeParam>::set(*pool);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_TRUE((DefaultConnection<TypeParam>::has()));
    EXPECT_EQ(pool->in_use(), 1);
}

TYPED_TEST(PoolQuerySetTest, QuerySet_SetDefaultConnection_PoolExhausted) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 1, .checkout_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    // Exhaust the pool
    auto held = pool->checkout();
    ASSERT_TRUE(held.has_value());

    // DefaultConnection::set should fail — pool exhausted
    auto result = DefaultConnection<TypeParam>::set(*pool);
    EXPECT_FALSE(result.has_value());
}

TYPED_TEST(PoolQuerySetTest, QuerySet_ClearReturnsToPool) {
    auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto result = DefaultConnection<TypeParam>::set(*pool);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(pool->in_use(), 1);

    DefaultConnection<TypeParam>::clear();
    EXPECT_EQ(pool->in_use(), 0);
    EXPECT_EQ(pool->available(), 1);
}

// ============================================================================
// Pool Handle Copy Semantics
// ============================================================================

template <typename ConnType> class PoolHandleTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }
    std::string connstr_;
};

TYPED_TEST_SUITE(PoolHandleTest, DatabaseTypes);

TYPED_TEST(PoolHandleTest, CopyHandle_SharesCore) {
    auto pool1 = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool1.has_value()) << pool1.error().message();

    auto pool2 = *pool1; // Copy handle
    auto conn  = pool1->checkout();
    ASSERT_TRUE(conn.has_value());
    EXPECT_EQ(pool2.in_use(), 1); // Same underlying core
}

// ============================================================================
// Connection Lifetime & Idle Timeout Tests
// ============================================================================

template <typename ConnType> class PoolLifetimeTest : public ::testing::Test {
  protected:
    auto SetUp() -> void override {
        if (skip_if_unavailable<ConnType>()) {
            GTEST_SKIP() << "Backend unavailable";
        }
        connstr_ = get_pool_connstr<ConnType>();
    }
    std::string connstr_;
};

TYPED_TEST_SUITE(PoolLifetimeTest, DatabaseTypes);

TYPED_TEST(PoolLifetimeTest, MaxLifetime_EvictsExpiredOnCheckout) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 3, .max_lifetime_ms = 100, .idle_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    {
        auto c1 = pool->checkout();
        ASSERT_TRUE(c1.has_value());
        // Populate statement cache to prove this is the "old" connection
        auto stmt = c1->get()->prepare_cached("SELECT 1");
        ASSERT_TRUE(stmt.has_value());
        EXPECT_GE(c1->get()->cached_statement_count(), 1U);
    }
    EXPECT_EQ(pool->size(), 1);

    // Wait for lifetime to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Next checkout evicts expired connection, grows a fresh one
    auto c2 = pool->checkout();
    ASSERT_TRUE(c2.has_value());
    // Fresh connection has empty statement cache — proves it's new
    EXPECT_EQ(c2->get()->cached_statement_count(), 0U);
}

TYPED_TEST(PoolLifetimeTest, IdleTimeout_EvictsIdleOnCheckout) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 2, .max_connections = 5, .max_lifetime_ms = 0, .idle_timeout_ms = 100}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();
    EXPECT_EQ(pool->size(), 2);

    // Checkout and return one connection to reset its last_used_at
    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
    }

    // Wait for idle timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Next checkout evicts idle connections, then grows a new one
    auto c = pool->checkout();
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE((*c)->is_open());
    // Pool shrank (expired idle removed) then grew by 1
    EXPECT_EQ(pool->size(), 1);
}

TYPED_TEST(PoolLifetimeTest, NoExpiry_ConnectionsLiveForever) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 3, .max_lifetime_ms = 0, .idle_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    TypeParam const* first_raw = nullptr;
    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
        first_raw = c->get();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto c2 = pool->checkout();
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(c2->get(), first_raw); // Same connection — no expiry
}

TYPED_TEST(PoolLifetimeTest, InUseConnections_NotEvicted) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 2, .max_lifetime_ms = 100, .idle_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());

    // Wait for lifetime to expire while connection is checked out
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // c1 is still in use — should NOT be evicted
    EXPECT_EQ(pool->in_use(), 1);
    EXPECT_TRUE((*c1)->is_open());
}

TYPED_TEST(PoolLifetimeTest, MaxLifetime_ClosedAfterReturnAndRecheckout) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 1, .max_lifetime_ms = 100, .idle_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    // Checkout and hold past max_lifetime
    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
        auto stmt = c->get()->prepare_cached("SELECT 1");
        ASSERT_TRUE(stmt.has_value());
        EXPECT_GE(c->get()->cached_statement_count(), 1U);

        // Hold connection past its max lifetime
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    } // checkin — connection is back in pool but expired

    // Next checkout should evict the expired connection and create a fresh one
    auto c2 = pool->checkout();
    ASSERT_TRUE(c2.has_value());
    EXPECT_EQ(c2->get()->cached_statement_count(), 0U); // Fresh connection — old was closed
    EXPECT_EQ(pool->size(), 1);                         // Pool didn't grow, it replaced
}

TYPED_TEST(PoolLifetimeTest, BothEnabled_IdleTimeoutFiresFirst) {
    // idle_timeout=100ms fires before max_lifetime=500ms
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 3, .max_lifetime_ms = 500, .idle_timeout_ms = 100}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
        // Populate statement cache to prove this is the "old" connection
        auto stmt = c->get()->prepare_cached("SELECT 1");
        ASSERT_TRUE(stmt.has_value());
        EXPECT_GE(c->get()->cached_statement_count(), 1U);
    }

    // Wait past idle timeout but within max lifetime
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Old connection evicted by idle timeout → pool grew a fresh one
    auto c2 = pool->checkout();
    ASSERT_TRUE(c2.has_value());
    // Fresh connection has empty statement cache
    EXPECT_EQ(c2->get()->cached_statement_count(), 0U);
}

TYPED_TEST(PoolLifetimeTest, InUseConnections_NotEvictedByIdleTimeout) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 2, .max_lifetime_ms = 0, .idle_timeout_ms = 100}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());

    // Wait past idle timeout while connection is checked out
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // c1 is in use — must NOT be evicted
    EXPECT_EQ(pool->in_use(), 1);
    EXPECT_TRUE((*c1)->is_open());
}

TYPED_TEST(PoolLifetimeTest, ActiveUse_ResetsIdleTimer) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 1, .max_connections = 1, .max_lifetime_ms = 0, .idle_timeout_ms = 150}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();

    TypeParam const* raw = nullptr;
    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
        raw = c->get();
    } // checkin resets last_used_at

    // Wait 100ms (within 150ms idle timeout)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Checkout + checkin resets the idle timer
    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
        EXPECT_EQ(c->get(), raw); // Same connection, not expired yet
    }

    // Wait another 100ms (within 150ms since last checkin)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Still alive — timer was reset by the second checkin
    auto c3 = pool->checkout();
    ASSERT_TRUE(c3.has_value());
    EXPECT_EQ(c3->get(), raw); // Same connection survives
}

TYPED_TEST(PoolLifetimeTest, MixedExpiry_OnlyExpiredEvicted) {
    auto pool = ConnectionPool<TypeParam>::create(
            this->connstr_, {.min_connections = 2, .max_connections = 3, .max_lifetime_ms = 100, .idle_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value()) << pool.error().message();
    EXPECT_EQ(pool->size(), 2);

    // Checkout and return first connection (it starts aging)
    TypeParam const* old_raw = nullptr;
    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
        old_raw = c->get();
    }

    // Wait for first connection to exceed max_lifetime
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Add a fresh connection by checking out both slots + one new
    auto c1 = pool->checkout(); // May get old or new — eviction runs first
    ASSERT_TRUE(c1.has_value());

    // The old connection should have been evicted, c1 is a fresh one
    // Pool grew after evicting the expired idle entry
    EXPECT_TRUE((*c1)->is_open());
}

TYPED_TEST(PoolHandleTest, PoolDestroyed_ConnectionStillWorks) {
    // The custom deleter captures shared_ptr<PoolCore>, keeping PoolCore alive
    // even after all ConnectionPool handles are destroyed.
    // Connection remains valid because PoolCore (and its PoolEntry unique_ptrs) survive.
    std::shared_ptr<TypeParam> surviving_conn;
    {
        auto pool = ConnectionPool<TypeParam>::create(this->connstr_, {.min_connections = 1, .max_connections = 3});
        ASSERT_TRUE(pool.has_value()) << pool.error().message();
        auto conn = pool->checkout();
        ASSERT_TRUE(conn.has_value());
        surviving_conn = std::move(*conn);
    } // pool handle destroyed, but PoolCore stays alive via surviving_conn's deleter
    EXPECT_TRUE(surviving_conn->is_open());
}

// ============================================================================
// Mock Pool Tests (cover error/edge-case paths unreachable with SQLite)
// ============================================================================

class MockPoolTest : public ::testing::Test {
  public:
    MockConfigGuard guard;
};

// Lines 276-277, 53-54, 244-245: create() fails when init()/create_entry()/open() fails
TEST_F(MockPoolTest, Create_FailsWhenOpenFails) {
    mock_config().open_should_fail = true;
    auto pool                      = ConnectionPool<MockPoolConnection>::create("mock://db", {.min_connections = 1});
    EXPECT_FALSE(pool.has_value());
    EXPECT_EQ(pool.error().code_, -1);
}

// Lines 87-88: checkout grow path — open() fails during growth
TEST_F(MockPoolTest, Checkout_GrowFails) {
    mock_config().open_fail_after = 1; // First open (init) succeeds, second (grow) fails
    auto pool = ConnectionPool<MockPoolConnection>::create("mock://db", {.min_connections = 1, .max_connections = 3});
    ASSERT_TRUE(pool.has_value());

    auto c1 = pool->checkout(); // Gets pre-created connection
    ASSERT_TRUE(c1.has_value());

    auto c2 = pool->checkout(); // Triggers grow, open() fails
    EXPECT_FALSE(c2.has_value());
}

// Lines 118-119: shutdown during wait loop
TEST_F(MockPoolTest, Checkout_ShutdownDuringWait) {
    auto pool = ConnectionPool<MockPoolConnection>::create(
            "mock://db", {.min_connections = 1, .max_connections = 1, .checkout_timeout_ms = 5000}
    );
    ASSERT_TRUE(pool.has_value());

    auto c1 = pool->checkout();
    ASSERT_TRUE(c1.has_value());

    // Deterministic sequence (no timing races, fixes #300):
    //   1. Waiter starts and counts down `waiter_started` immediately before calling
    //      checkout(). Main blocks on that latch — guarantees the waiter is at (or
    //      microseconds away from) cv_.wait_until before we proceed.
    //   2. Main spawns shutdown_thread. shutdown() sets shutdown_ = true, notify_all()s,
    //      then blocks on cv_.wait until in_use == 0 (c1 still holds the only conn).
    //   3. Whichever thread reaches the mutex first, the waiter's checkout() will see
    //      shutdown_ == true — either via the early-return at the top of checkout()
    //      (if shutdown_thread ran first) or via the shutdown_ re-check inside
    //      wait_for_idle (if the waiter ran first). Either way: the shutdown error.
    //   4. waiter.join() returns once the waiter has set got_shutdown_error.
    //   5. Only then we reset c1, which lets shutdown_thread's cv_.wait observe
    //      in_use == 0 and complete.
    // Assert the *shutdown* error specifically, not just any error (#306): a
    // checkout_timeout_ms expiry would also return an error, silently turning
    // this test into a no-op if shutdown_thread is starved past the timeout.
    std::atomic<bool> got_shutdown_error{false};
    std::latch        waiter_started{1};
    std::thread       waiter([&pool, &got_shutdown_error, &waiter_started] {
        waiter_started.count_down();
        auto c             = pool->checkout();
        got_shutdown_error = !c.has_value() && c.error().message().contains("shut down");
    });

    waiter_started.wait();
    std::thread shutdown_thread([&pool] { pool->shutdown(); });

    waiter.join();
    c1->reset(); // Return c1 → shutdown_thread's cv_.wait predicate becomes true.
    shutdown_thread.join();
    EXPECT_TRUE(got_shutdown_error);
}

// Lines 232-234: find_idle() erases stale connection (is_open() == false)
TEST_F(MockPoolTest, Checkout_EvictsStaleConnection) {
    auto pool = ConnectionPool<MockPoolConnection>::create(
            "mock://db", {.min_connections = 1, .max_connections = 3, .validate_on_checkout = true}
    );
    ASSERT_TRUE(pool.has_value());

    // Checkout, mark as closed, then return
    MockPoolConnection* raw = nullptr;
    {
        auto c = pool->checkout();
        ASSERT_TRUE(c.has_value());
        raw           = c->get();
        raw->is_open_ = false; // Simulate closed connection
    }
    EXPECT_EQ(pool->size(), 1);

    // Next checkout: find_idle() sees is_open()==false, erases it, grows a new one
    auto c2 = pool->checkout();
    ASSERT_TRUE(c2.has_value());
    EXPECT_TRUE(c2->get()->is_open());
}

// Lines 83-84: shutdown detected after relocking in checkout grow path
TEST_F(MockPoolTest, Checkout_ShutdownDuringGrow) {
    // Two latches make the relock-then-see-shutdown branch deterministic without any
    // timing guess: open() signals `in_open` on entry (lock already released), then
    // blocks on `open_release` until the test has run shutdown().
    std::latch in_open{1};
    std::latch open_release{1};
    auto&      cfg   = mock_config();
    cfg.open_entered = &in_open;
    cfg.open_release = &open_release;
    auto pool = ConnectionPool<MockPoolConnection>::create("mock://db", {.min_connections = 0, .max_connections = 3});
    ASSERT_TRUE(pool.has_value());

    // grower: checkout → try_grow releases lock → open() signals in_open, then blocks
    std::atomic<bool> checkout_failed{false};
    std::thread       grower([&pool, &checkout_failed] {
        auto c          = pool->checkout();
        checkout_failed = !c.has_value();
    });

    // Deterministic ordering (no sleeps):
    //   1. in_open.wait() blocks until the grower is INSIDE open() — try_grow has
    //      already released the pool lock, so shutdown() can take it immediately.
    //   2. shutdown() sets shutdown_ = true and (min=0, nothing in use) returns.
    //   3. open_release lets open() return; the grower re-locks, sees shutdown_ ==
    //      true, and checkout fails (covers pool.cppm try_grow shutdown branch, #307).
    in_open.wait();
    pool->shutdown();
    open_release.count_down();
    grower.join();
    EXPECT_TRUE(checkout_failed);
}

// Lines 92-97: race condition — pool reached max after relock
TEST_F(MockPoolTest, Checkout_RaceConditionMaxReached) {
    auto pool = ConnectionPool<MockPoolConnection>::create(
            "mock://db", {.min_connections = 0, .max_connections = 2, .checkout_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value());

    // Use threads to trigger the race: both see pool < max, both try to grow
    std::atomic<int>         success_count{0};
    std::atomic<int>         failure_count{0};
    constexpr int            num_threads = 10;
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    // Hold connections to exhaust the pool
    std::vector<std::shared_ptr<MockPoolConnection>> held_conns;
    held_conns.reserve(2);

    for (int i = 0; i < 2; ++i) {
        auto c = pool->checkout();
        if (c.has_value()) {
            held_conns.push_back(std::move(*c));
        }
    }

    // Now pool is at max — further checkouts with timeout=0 should fail
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&pool, &success_count, &failure_count] {
            auto c = pool->checkout();
            if (c.has_value()) {
                ++success_count;
            } else {
                ++failure_count;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(success_count, 0);
    EXPECT_EQ(failure_count, num_threads);
    EXPECT_EQ(pool->size(), 2);
}

// try_grow re-check: pool hit max during the open() unlock window AND an idle entry
// exists → the grower adopts that idle entry instead of exceeding max (#544).
//
// This branch used to be covered only incidentally, by the 10-thread scramble in
// Checkout_RaceConditionMaxReached above, so whether it was hit depended on the
// scheduler — it made the CI 100%-line-coverage gate flake at 99.9% (see #544, first
// seen on PR #541, which touched nothing under src/db/). Forced deterministically here:
//
//   1. grower calls checkout() on an empty pool → try_grow sees 0 < max, releases the
//      lock and enters open(), which signals `in_open` and parks on `open_release`.
//      open_gate_call = 1 confines that handshake to THIS open() — step 2 must not park.
//   2. main (lock free) checks out c1 → grows the pool to max_connections = 1, opens
//      the second connection unimpeded, and releases it, leaving one IDLE entry.
//   3. open_release lets the grower's open() return. It relocks, sees size() >= max,
//      and find_idle() hands it the entry main just returned — the branch under test.
//
// The grower must therefore SUCCEED, and the pool must never exceed max. Asserting the
// success (not merely "no crash") is what keeps this test from silently degrading into
// a no-op if the branch is reordered away.
TEST_F(MockPoolTest, Checkout_RaceMaxReachedWithIdle) {
    std::latch in_open{1};
    std::latch open_release{1};
    auto&      cfg     = mock_config();
    cfg.open_entered   = &in_open;
    cfg.open_release   = &open_release;
    cfg.open_gate_call = 1; // only the grower's open() parks

    auto pool = ConnectionPool<MockPoolConnection>::create(
            "mock://db", {.min_connections = 0, .max_connections = 1, .checkout_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value());

    std::atomic<bool>                grower_ok{false};
    std::atomic<MockPoolConnection*> grower_raw{nullptr};
    std::thread                      grower([&pool, &grower_ok, &grower_raw] {
        auto c    = pool->checkout();
        grower_ok = c.has_value();
        if (c.has_value()) {
            grower_raw = c->get();
        }
    });

    in_open.wait(); // grower is inside open(), pool lock released

    // NO ASSERT_* between the spawn above and the count_down() below: ASSERT_* returns
    // from the test body, which would leave the grower parked on open_release forever
    // and destroy a joinable std::thread (→ std::terminate, and a dangling latch). Use
    // EXPECT_* here and let the flow reach the release + join.
    MockPoolConnection* main_raw = nullptr;
    {
        auto c1 = pool->checkout(); // grows to max (1) — its own open() is not gated
        EXPECT_TRUE(c1.has_value());
        main_raw = c1.has_value() ? c1->get() : nullptr;
    } // checkin → that entry is now idle, and entries_.size() == max_connections

    open_release.count_down(); // grower relocks, hits the re-check + find_idle() branch
    grower.join();

    EXPECT_TRUE(grower_ok);                             // adopted the idle entry, not an error
    EXPECT_EQ(grower_raw.load(), main_raw);             // ...specifically the one main returned
    EXPECT_EQ(pool->size(), 1);                         // never exceeded max_connections
    EXPECT_EQ(pool->in_use(), 0);                       // grower's shared_ptr died with its lambda
    EXPECT_EQ(mock_config().open_call_count.load(), 2); // one open() per thread, no more
}

// The sibling arm of the same race: pool hit max during the open() unlock window and
// every slot is IN USE, so there is nothing to adopt. try_grow returns nullopt and the
// caller falls through to the wait/timeout path (#544).
//
// Same handshake as Checkout_RaceMaxReachedWithIdle, except main HOLDS the connection it
// opens instead of returning it — so find_idle() finds nothing. With checkout_timeout_ms
// = 0 the grower then fails fast, which is the observable signal that it took this arm.
TEST_F(MockPoolTest, Checkout_RaceMaxReachedNoIdle) {
    std::latch in_open{1};
    std::latch open_release{1};
    auto&      cfg     = mock_config();
    cfg.open_entered   = &in_open;
    cfg.open_release   = &open_release;
    cfg.open_gate_call = 1; // only the grower's open() parks

    auto pool = ConnectionPool<MockPoolConnection>::create(
            "mock://db", {.min_connections = 0, .max_connections = 1, .checkout_timeout_ms = 0}
    );
    ASSERT_TRUE(pool.has_value());

    std::atomic<bool> grower_exhausted{false};
    std::thread       grower([&pool, &grower_exhausted] {
        auto c           = pool->checkout();
        grower_exhausted = !c.has_value() && c.error().message().contains("exhausted");
    });

    in_open.wait(); // grower is inside open(), pool lock released

    // EXPECT_*, not ASSERT_*: see Checkout_RaceMaxReachedWithIdle — an early return here
    // would strand the grower on open_release and destroy a joinable thread. Worse in
    // this test, since open_release would then go out of scope under the parked thread.
    auto c1 = pool->checkout(); // grows to max (1) and KEEPS it — no idle entry
    EXPECT_TRUE(c1.has_value());

    open_release.count_down(); // grower relocks → size() >= max, find_idle() == nullptr
    grower.join();

    // Assert the *exhausted* error specifically, not merely "an error": an open()
    // failure would also return one, silently turning this test into a no-op.
    EXPECT_TRUE(grower_exhausted);
    EXPECT_EQ(pool->size(), 1);   // never exceeded max_connections
    EXPECT_EQ(pool->in_use(), 1); // c1 still held
}

// Bad config: negative timeouts
TEST_F(MockPoolTest, Create_BadConfig_NegativeTimeouts) {
    auto p1 = ConnectionPool<MockPoolConnection>::create("mock://db", {.checkout_timeout_ms = -1});
    EXPECT_FALSE(p1.has_value());

    auto p2 = ConnectionPool<MockPoolConnection>::create("mock://db", {.max_lifetime_ms = -1});
    EXPECT_FALSE(p2.has_value());

    auto p3 = ConnectionPool<MockPoolConnection>::create("mock://db", {.idle_timeout_ms = -1});
    EXPECT_FALSE(p3.has_value());
}

// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes,readability-identifier-length,cppcoreguidelines-special-member-functions)
