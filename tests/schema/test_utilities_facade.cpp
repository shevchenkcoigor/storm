#include <gtest/gtest.h>

import std;
import storm_orm_utilities;
// Deliberately imports ONLY the storm_orm_utilities facade: it must keep re-exporting
// storm_orm_value_bind and storm_orm_stitch_key so existing importers stay unchanged (#671).

namespace {
    using storm::orm::utilities::BindableType;
    using storm::orm::utilities::StitchKey;

    static_assert(BindableType<storm::UUID>, "facade re-exports the value-binding concept and the storm::UUID alias");
} // namespace

TEST(UtilitiesFacade, ReExportsStitchKeyAndUuidHash) {
    std::unordered_map<StitchKey, int> by_key;
    StitchKey                          key;
    key.append_string("facade");
    by_key.try_emplace(key, 1);
    EXPECT_EQ(by_key.size(), 1U);

    std::unordered_set<storm::UUID> uuids;
    uuids.insert(storm::UUID::generate());
    EXPECT_EQ(uuids.size(), 1U);
}
