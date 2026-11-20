#pragma once

/**
 * @file test_yaml_register.h
 * @brief GTest dynamic registration for YAML-driven test cases.
 *
 * Provides:
 * - YamlTestInstance<I, Fixture, ConnType>: primary template; each test file
 *   provides partial specializations with the actual TestBody().
 * - register_for_backend(): registers N test instances for one DB backend.
 * - register_both_backends(): convenience wrapper for SQLite + PostgreSQL.
 *
 * Include AFTER `import storm;` (needs Connection types for template args).
 */

#include <gtest/gtest.h>
#include <string>
#include <utility>

namespace storm::test {

// Primary template -- TestBody is provided by partial specializations in each
// test file.  The fallback body fires a failure if no specialization exists.
template <std::size_t I, typename Fixture, typename ConnType> class YamlTestInstance : public Fixture {
    void TestBody() override { ADD_FAILURE() << "YamlTestInstance: no TestBody specialization for index " << I; }
};

// Registration helper: registers all N cases as separate named GTests.
// The factory returns Fixture* (not YamlTestInstance*) so GTest sees a single
// fixture type for the whole suite -- avoids "different fixture class" errors.
template <const auto &Tests, typename Fixture, typename ConnType>
auto register_for_backend(const char *suite, const char *backend) -> void {
    static const std::string suite_name = std::string(suite) + "/" + backend;
    [suite_ptr = suite_name.c_str()]<std::size_t... I>(std::index_sequence<I...>) {
        (..., [&] {
            static_assert(Tests[I].backends_well_formed(),
                          "`backends:` is [sqlite] or absent, and a WHERE collate needs `backends: [sqlite]`");
            // A SQLite-only case is not registered for PG at all: it shows up as absent from that
            // suite, not as a pass.
            if constexpr (storm::test::is_postgresql<ConnType>() && Tests[I].sqlite_only) {
                return; // discarded, so the PG body of such a case is not even compiled
            } else {
                ::testing::RegisterTest(
                    suite_ptr,
                    Tests[I].bench.test_name.c_str(),     // ConstexprString::c_str() -> static storage
                    nullptr, nullptr, __FILE__, __LINE__, // NOSONAR - GTest API requires raw file/line
                    []() -> Fixture * {
                        return new YamlTestInstance<I, Fixture, ConnType>(); // NOSONAR - GTest API owns the pointer
                    });
            }
        }());
    }(std::make_index_sequence<Tests.size()>{});
}

// Convenience: register for both SQLite and PostgreSQL.
// Returns true so callers can assign to [[maybe_unused]] const bool for static init.
template <const auto &Tests, template <typename> class FixtureTpl>
auto register_both_backends(const char *suite) -> bool {
    using sqlite_t = storm::db::sqlite::Connection;
    using pg_t = storm::db::postgresql::Connection;
    register_for_backend<Tests, FixtureTpl<sqlite_t>, sqlite_t>(suite, "SQLite");
    register_for_backend<Tests, FixtureTpl<pg_t>, pg_t>(suite, "PG");
    return true;
}

} // namespace storm::test
