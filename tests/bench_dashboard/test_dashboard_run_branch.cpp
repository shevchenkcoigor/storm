// Regression test for Issue #267.
//
// The daemon used to capture git_hash/branch ONCE at startup
// (ensure_host_context) and reuse the cached values for every BenchRun row.
// If the developer switched branches or committed while the daemon ran, every
// later run was mislabeled with the daemon's startup branch — which silently
// corrupted `--baseline auto` (it filters runs by branch+host).
//
// The fix: re-read the git working-tree state on EACH insert_run, via an
// injectable git-capture callable. This test injects a stub that returns a
// different branch per call (A, B, A) and asserts the three BenchRun rows carry
// distinct, per-call branch values — proving the value is re-read, not cached.

#include <meta> // must precede the imports (Finding A) — db.hpp uses `^^`

#include <gtest/gtest.h>

import storm_db_default_connection;
import storm_db_sqlite;
import storm_meta;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import std;
import storm.bench_dashboard.wire;

#include "../../benchmarks/dashboard/models.hpp" // NOSONAR cpp:S954
#include "../../benchmarks/dashboard/args.hpp"
#include "../../benchmarks/dashboard/db.hpp"
using storm::orm::schema::SchemaStatement;
using storm::orm::statements::SelectStatement;

namespace {
    using storm::db::DefaultConnection;

    // Materialise every BenchRun row ordered by id, ascending.
    auto all_runs() -> std::vector<bench_dashboard::BenchRun> {
        auto rows = (SelectStatement<bench_dashboard::BenchRun>{})
                            .all()
                            .template order_by<bench_dashboard::fields::BenchRun.id>()
                            .execute()
                            .transform([](auto&& hive) {
                                return std::forward<decltype(hive)>(hive) | std::views::as_rvalue |
                                       std::ranges::to<std::vector<bench_dashboard::BenchRun>>();
                            });
        return rows ? std::move(*rows) : std::vector<bench_dashboard::BenchRun>{};
    }

} // namespace

class DashboardRunBranch : public ::testing::Test {
  public:
    void SetUp() override {
        ASSERT_TRUE(DefaultConnection<>::set(":memory:").has_value());
        ASSERT_TRUE((SchemaStatement<bench_dashboard::BenchRun>::create_table_if_not_exists()));
    }

    void TearDown() override {
        DefaultConnection<>::clear();
    }
};

// Start the "daemon" on branch A, then bench on B, then bench on A again. Each
// insert_run must re-read git, so the three rows carry the actual per-call
// branch (A, B, A) — not the startup value (A, A, A).
TEST_F(DashboardRunBranch, BranchReReadPerRun) {
    std::vector<DashboardDB::GitContext> const script = {
            {.git_hash = "aaaaaaa", .branch = "feature/A"},
            {.git_hash = "bbbbbbb", .branch = "feature/B"},
            {.git_hash = "ccccccc", .branch = "feature/A"},
    };
    std::size_t call = 0;
    DashboardDB db{[&]() -> DashboardDB::GitContext { return script.at(call++); }};

    for (std::size_t i = 0; i < script.size(); ++i) {
        ASSERT_TRUE(db.insert_run("", /*is_full_run=*/true, /*is_raw=*/false).has_value());
    }

    const auto runs = all_runs();
    ASSERT_EQ(runs.size(), 3U);
    EXPECT_EQ(runs[0].branch, "feature/A");
    EXPECT_EQ(runs[0].git_hash, "aaaaaaa");
    EXPECT_EQ(runs[1].branch, "feature/B");
    EXPECT_EQ(runs[1].git_hash, "bbbbbbb");
    EXPECT_EQ(runs[2].branch, "feature/A");
    EXPECT_EQ(runs[2].git_hash, "ccccccc");
}
