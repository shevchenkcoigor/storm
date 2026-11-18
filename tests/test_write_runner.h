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
template <typename Model, typename ConnType> class InsertRunner : public QueryRunnerBase<Model, ConnType> {
  public:
    template <const auto &Tc> auto run() -> void {
        if constexpr (Tc.query_type == "insert_one") {
            auto r = this->qs_.insert(make_record<Model>(0)).execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            expect_row_count(this->qs_, Tc.expected.count);

        } else if constexpr (Tc.query_type == "insert_batch") {
            std::vector<Model> batch;
            batch.reserve(static_cast<std::size_t>(Tc.insert_count));
            for (int i = 0; i < Tc.insert_count; ++i)
                batch.push_back(make_record<Model>(i));
            auto r = this->qs_.insert(std::span<const Model>(batch)).execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            expect_row_count(this->qs_, Tc.expected.count);
        }
    }
};

// ---------------------------------------------------------------------------
// UpdateRunner -- update_batch
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class UpdateRunner : public QueryRunnerBase<Model, ConnType> {
  public:
    template <const auto &Tc> auto run() -> void {
        this->template seed_and_insert<Tc>();

        // Re-fetch to get actual IDs
        auto sel = this->qs_.select().execute();
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
            auto upd = this->qs_.update(std::span<const Model>(to_update)).execute();
            ASSERT_TRUE(upd.has_value()) << upd.error().message();
        }

        // Count records still with original prefix
        auto after = this->qs_.select().execute();
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
template <typename Model, typename ConnType> class EraseRunner : public QueryRunnerBase<Model, ConnType> {
  public:
    template <const auto &Tc> auto run() -> void {
        if constexpr (Tc.query_type == "erase_all") {
            this->template seed_and_insert<Tc>();

            auto rem = this->qs_.erase_all().execute();
            ASSERT_TRUE(rem.has_value()) << rem.error().message();

            expect_row_count(this->qs_, Tc.expected.remaining);

        } else if constexpr (Tc.query_type == "erase_batch") {
            this->template seed_and_insert<Tc>();

            // Re-fetch to get real IDs
            auto sel = this->qs_.select().execute();
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
            auto rem = this->qs_.erase(std::span<const Model>(to_erase)).execute();
            ASSERT_TRUE(rem.has_value()) << rem.error().message();

            expect_row_count(this->qs_, Tc.expected.remaining);
        }
    }
};

} // namespace storm::test
