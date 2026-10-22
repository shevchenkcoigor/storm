#include <gtest/gtest.h>

import std;
import storm_orm_value_bind;
import storm_orm_stitch_key;
// Deliberately NOT importing storm_orm_utilities or storm: this TU proves the value-binding
// helpers and StitchKey live in leaf modules that stand on their own (#671).

namespace {
    using namespace storm::orm::utilities;

    struct NotBindable {
        int payload{};
    };

    // ── storm_orm_value_bind ─────────────────────────────────────────────────────
    static_assert(is_optional_v<std::optional<float>>);
    static_assert(!is_optional_v<float>);
    static_assert(std::is_same_v<optional_inner_type_t<std::optional<std::string>>, std::string>);
    static_assert(is_chrono_duration_v<std::chrono::minutes>);
    static_assert(BindableType<std::optional<std::chrono::year_month_day>>);
    static_assert(!BindableType<NotBindable>);

    // ── storm_orm_stitch_key ─────────────────────────────────────────────────────
    static_assert(StitchKey::MAX_PARTS == 4, "StitchKey holds at most four 8-byte parts");
} // namespace

TEST(UtilitiesModules, ValueBindRoundTripsDateThroughChronoConv) {
    constexpr std::chrono::year_month_day ymd{std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{22}};
    const auto                            text = chrono_conv::ymd_to_string(ymd);
    EXPECT_EQ(text, "2026-10-22");
    EXPECT_TRUE(chrono_conv::string_to_ymd(text) == ymd);
}

TEST(UtilitiesModules, ValueBindRoundTripsTimePointThroughChronoConv) {
    const auto tp = std::chrono::sys_days{std::chrono::year{2026} / 10 / 22} + std::chrono::hours{13} +
                    std::chrono::minutes{4} + std::chrono::seconds{5};
    const auto text = chrono_conv::tp_to_string(tp);
    EXPECT_EQ(text, "2026-10-22 13:04:05");
    EXPECT_TRUE(chrono_conv::string_to_tp(text) == tp);
}

TEST(UtilitiesModules, ValueBindExposesUuidAndItsHash) {
    const auto generated = UUID::generate();
    EXPECT_TRUE(UUID::is_valid(generated.value));
    EXPECT_FALSE(UUID::is_valid("not-a-uuid"));

    std::unordered_map<UUID, int> by_uuid;
    by_uuid.try_emplace(generated, 7);
    EXPECT_EQ(by_uuid.at(UUID{generated.value}), 7);
}

TEST(UtilitiesModules, StitchKeyModuleExposesEqualityHashAndFirstWord) {
    StitchKey lhs;
    lhs.append_int64(99);
    StitchKey rhs;
    rhs.append_int64(99);
    EXPECT_TRUE(lhs == rhs);
    EXPECT_EQ(std::hash<StitchKey>{}(lhs), std::hash<StitchKey>{}(rhs));
    EXPECT_EQ(lhs.first_word(), std::uint64_t{99});
}
