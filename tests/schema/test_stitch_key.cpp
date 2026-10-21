#include <gtest/gtest.h>

import storm_orm_utilities;
import std;
using storm::orm::utilities::StitchKey;

TEST(StitchKeyTest, SingleInt64PartEqualsItself) {
    StitchKey key_a;
    key_a.append_int64(42);
    StitchKey key_b;
    key_b.append_int64(42);
    EXPECT_EQ(key_a, key_b);
}

TEST(StitchKeyTest, DifferentInt64PartsAreNotEqual) {
    StitchKey key_a;
    key_a.append_int64(42);
    StitchKey key_b;
    key_b.append_int64(43);
    EXPECT_NE(key_a, key_b);
}

TEST(StitchKeyTest, TwoPartOrderMatters) {
    StitchKey key_a;
    key_a.append_int64(1);
    key_a.append_int64(2);
    StitchKey key_b;
    key_b.append_int64(2);
    key_b.append_int64(1);
    EXPECT_NE(key_a, key_b); // (1,2) is a different key from (2,1)
}

TEST(StitchKeyTest, ThreePartCompositeKeyRoundTrips) {
    StitchKey key_a;
    key_a.append_int64(7);
    key_a.append_string("sku-123");
    key_a.append_int64(2026);
    StitchKey key_b;
    key_b.append_int64(7);
    key_b.append_string("sku-123");
    key_b.append_int64(2026);
    EXPECT_EQ(key_a, key_b);
}

TEST(StitchKeyTest, HashableInUnorderedMap) {
    std::unordered_map<StitchKey, int> map;
    StitchKey                          key;
    key.append_int64(99);
    map[key] = 7;
    StitchKey lookup;
    lookup.append_int64(99);
    ASSERT_TRUE(map.contains(lookup));
    EXPECT_EQ(map.at(lookup), 7);
}

TEST(StitchKeyTest, StringPartDifferentValuesHashDifferently) {
    StitchKey key_a;
    key_a.append_int64(1);
    key_a.append_string("alice-warehouse");
    StitchKey key_b;
    key_b.append_int64(1);
    key_b.append_string("bob-warehouse");
    EXPECT_NE(key_a, key_b);
}

// The length gate in operator== is load-bearing, not defensive: without it the
// word loop would return true as soon as it exhausted the SHORTER key, so a
// 1-part key would compare equal to a 2-part key sharing its first part — and a
// composite owner would stitch onto a single-PK one. No other test compares keys
// of different arity, so this branch is only covered here.
TEST(StitchKeyTest, ShorterKeyIsNotEqualToLongerKeySharingItsPrefix) {
    StitchKey one_part;
    one_part.append_int64(42);

    StitchKey two_parts;
    two_parts.append_int64(42); // identical first word
    two_parts.append_int64(7);

    EXPECT_NE(one_part, two_parts);
    EXPECT_NE(two_parts, one_part); // both argument orders exercise the gate
}
