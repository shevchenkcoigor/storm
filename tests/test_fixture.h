#pragma once

/**
 * @file test_fixture.h
 * @brief Model-agnostic TYPED_TEST fixture + table-creation helpers.
 *
 * (header comment unchanged; StormTestFixture now references
 *  DefaultConnection instead of QuerySet for connection management)
 */

#ifndef STORM_TESTS_TEST_DB_HELPERS_H
#error                                                                                                                 \
    "test_fixture.h: #include \"test_db_helpers.h\" BEFORE `import storm;` (it forward-declares Connection in the global module)"
#endif

#include <gtest/gtest.h>
using storm::db::DefaultConnection;
using storm::orm::schema::SchemaStatement;

namespace storm::test {

template <typename T, typename ConnType> inline auto ensure_table() {
    return SchemaStatement<T>::template create_table_if_not_exists<ConnType>();
}

template <typename T, typename ConnType> inline auto ensure_table(auto &conn) {
    return SchemaStatement<T>::create_table_if_not_exists(conn);
}

template <typename ConnType = db::sqlite::Connection, typename... Models> inline auto ensure_tables() -> bool {
    return (ensure_table<Models, ConnType>().has_value() && ...);
}

template <typename ConnType, typename... Models>
inline auto ensure_tables(const std::shared_ptr<ConnType> &conn) -> bool {
    return (ensure_table<Models, ConnType>(conn).has_value() && ...);
}

} // namespace storm::test

/**
 * @brief Base fixture for typed ORM tests — template method pattern.
 *
 *   SetUp():    setup_connection() → pg_schema_init (once) → on_setup(conn) [virtual hook]
 *   TearDown(): rollback PG schema → clear connection
 *
 * (usage examples unchanged)
 *
 * Note: the default connection is stored per ConnType (and per thread) in
 * DefaultConnection<ConnType>. Model only determines which tables
 * on_setup() creates; any QuerySet<*, ConnType> in the test uses the same database.
 */
template <typename Model, typename ConnType, typename... ExtraModels> class StormTestFixture : public ::testing::Test {
  public:
    using connection_type = std::shared_ptr<ConnType>;

  protected:
    using Default = storm::db::DefaultConnection<ConnType>;

    auto SetUp() -> void override {
        if (!setup_connection()) {
            GTEST_SKIP() << "Backend unavailable";
            return;
        }
        const auto conn = Default::get(); // copy: stays valid even if the slot is cleared
        storm::test::pg_schema_init<ConnType>(conn);
        on_setup(conn);
        if (StormTestFixture::HasFatalFailure())
            return;
        on_after_setup(conn);
    }

    virtual auto on_setup(const std::shared_ptr<ConnType> &conn) -> void {
        ASSERT_TRUE((storm::test::ensure_tables<ConnType, Model, ExtraModels...>(conn))) << "Failed to create table(s)";
    }

    virtual auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void {
        // Default: no additional setup.
    }

    auto TearDown() -> void override {
        if constexpr (storm::test::is_postgresql<ConnType>()) {
            // try_get(): never aborts and never touches a registered pool.
            if (auto conn = Default::try_get()) {
                storm::test::rollback_test_txn<ConnType>(conn);
            }
        }
        Default::clear();
    }

    // Sets the default connection for this ConnType on the calling thread.
    // Returns false if the backend is unavailable (caller should GTEST_SKIP()).
    auto setup_connection() -> bool {
        if (!storm::test::backend_available<ConnType>())
            return false;
        return Default::set(storm::test::get_connection_string<ConnType>()).has_value();
    }
};
