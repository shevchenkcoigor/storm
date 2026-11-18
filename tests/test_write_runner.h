#pragma once

/**
 * @file test_write_runner.h
 * @brief InsertRunner, UpdateRunner, EraseRunner for UnifiedTestCase.
 *
 * Include AFTER `import storm;` — it pulls the helpers and models it needs itself.
 */

#include "test_record_helpers.h"
#include "test_seed_helpers.h"
#include "test_select_runner.h"

namespace storm::test {

// Row count of the table behind qs equals `expected` (the shared tail of the
// insert and erase runners).
inline auto expect_row_count(auto &qs, int expected) -> void {
    auto cnt = qs.count().execute();
    ASSERT_TRUE(cnt.has_value()) << cnt.error().message();
    EXPECT_EQ(static_cast<int>(cnt.value()), expected);
}

// ---------------------------------------------------------------------------
// InsertRunner -- insert_one / insert_batch
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class InsertRunner {
  public:
    template <const auto &Tc> static auto run() -> void {
        QuerySet<Model, ConnType, true> qs;
        if constexpr (Tc.query_type == "insert_one") {
            auto r = qs.insert(make_record<Model>(0)).execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            expect_row_count(qs, Tc.expected.count);

        } else if constexpr (Tc.query_type == "insert_batch") {
            std::vector<Model> batch;
            batch.reserve(static_cast<std::size_t>(Tc.insert_count));
            for (int i = 0; i < Tc.insert_count; ++i)
                batch.push_back(make_record<Model>(i));
            auto r = qs.insert(std::span<const Model>(batch)).execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            expect_row_count(qs, Tc.expected.count);
        }
    }
};

// ---------------------------------------------------------------------------
// UpdateRunner -- update_batch
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class UpdateRunner {
  public:
    template <const auto &Tc> static auto run() -> void {
        QuerySet<Model, ConnType, true> qs;
        seed_records<Tc, Model, ConnType>();

        // Re-fetch to get actual IDs
        auto sel = qs.select().execute();
        ASSERT_TRUE(sel.has_value()) << sel.error().message();

        // Update first update_count records
        std::vector<Model> to_update;
        to_update.reserve(static_cast<std::size_t>(Tc.update_count));
        std::size_t n = 0;
        for (const auto &r : sel.value()) {
            if (n++ >= static_cast<std::size_t>(Tc.update_count))
                break;
            to_update.push_back(make_updated_record<Model>(r));
        }
        if (!to_update.empty()) {
            auto upd = qs.update(std::span<const Model>(to_update)).execute();
            ASSERT_TRUE(upd.has_value()) << upd.error().message();
        }

        // Count records still with original prefix
        auto after = qs.select().execute();
        ASSERT_TRUE(after.has_value()) << after.error().message();
        int unchanged_count = 0;
        for (const auto &r : after.value())
            if (is_original_record<Model>(r))
                ++unchanged_count;
        EXPECT_EQ(unchanged_count, Tc.expected.unchanged);
    }
};

// ---------------------------------------------------------------------------
// EraseRunner -- erase_all / erase_where / erase_batch
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class EraseRunner {
  public:
    template <const auto &Tc> static auto run() -> void {
        QuerySet<Model, ConnType, true> qs;
        if constexpr (Tc.query_type == "erase_all") {
            seed_records<Tc, Model, ConnType>();

            auto rem = qs.erase_all().execute();
            ASSERT_TRUE(rem.has_value()) << rem.error().message();

            expect_row_count(qs, Tc.expected.remaining);

        } else if constexpr (Tc.query_type == "erase_batch") {
            seed_records<Tc, Model, ConnType>();

            // Re-fetch to get real IDs
            auto sel = qs.select().execute();
            ASSERT_TRUE(sel.has_value()) << sel.error().message();

            // Erase first erase_count records
            std::vector<Model> to_erase;
            to_erase.reserve(static_cast<std::size_t>(Tc.erase_count));
            std::size_t n = 0;
            for (const auto &r : sel.value()) {
                if (n++ >= static_cast<std::size_t>(Tc.erase_count))
                    break;
                to_erase.push_back(r);
            }
            auto rem = qs.erase(std::span<const Model>(to_erase)).execute();
            ASSERT_TRUE(rem.has_value()) << rem.error().message();

            expect_row_count(qs, Tc.expected.remaining);
        }
    }
};

} // namespace storm::test
