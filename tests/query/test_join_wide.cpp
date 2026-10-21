#include <gtest/gtest.h>
#include <meta>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::field_specs_for;
using storm::primary;

// ============================================================================
// Regression #358 — table aliases are t<Is+2>, so a model with 9 FK fields
// spans t2..t10. The 9th FK (Is == 8) must emit the two-digit alias "t10",
// not a bare "t" (append_digit silently dropped values >= 10).
// ============================================================================

// Local model with 9 FK fields to Person — exercises the 8th+ joined FK (t10).
struct WideJoin {
    [[= primary]] int        id{};
    [[= storm::fk<>]] Person fk1;
    [[= storm::fk<>]] Person fk2;
    [[= storm::fk<>]] Person fk3;
    [[= storm::fk<>]] Person fk4;
    [[= storm::fk<>]] Person fk5;
    [[= storm::fk<>]] Person fk6;
    [[= storm::fk<>]] Person fk7;
    [[= storm::fk<>]] Person fk8;
    [[= storm::fk<>]] Person fk9;
};

// fields:: selector proxies (#518).
namespace fields {

    struct WideJoinT;
    consteval {
        std::meta::define_aggregate(^^WideJoinT, field_specs_for(^^WideJoin));
    }
    inline constexpr WideJoinT WideJoin{};

} // namespace fields

template <typename ConnType> class WideJoinTest : public StormTestFixture<Person, ConnType, WideJoin> {};

TYPED_TEST_SUITE(WideJoinTest, DatabaseTypes);

// Attaches all 9 FK joins to a WideJoin QuerySet (single source for the FK pack).
template <typename ConnType> auto join_all_nine(const QuerySet<WideJoin, ConnType>& qs) {
    return qs.template join<
            fields::WideJoin.fk1,
            fields::WideJoin.fk2,
            fields::WideJoin.fk3,
            fields::WideJoin.fk4,
            fields::WideJoin.fk5,
            fields::WideJoin.fk6,
            fields::WideJoin.fk7,
            fields::WideJoin.fk8,
            fields::WideJoin.fk9>();
}

// SQL structural check: the 9th FK alias must render as t10 (not bare "t").
TYPED_TEST(WideJoinTest, NineFKsEmitTwoDigitAlias) {
    QuerySet<WideJoin, TypeParam> qs;
    auto                          sql = join_all_nine(qs).select().sql();

    // 9th FK (Is == 8) → alias t10 in both the JOIN clause and the SELECT list.
    EXPECT_TRUE(sql.contains("INNER JOIN Person t10 ON t10.id = t1.fk9_id"))
            << "9th FK JOIN clause missing t10 alias:\n"
            << sql;
    EXPECT_TRUE(sql.contains("t10.id")) << "SELECT list missing t10 columns:\n" << sql;
    // A bare " Person t ON" would be the dropped-digit bug.
    EXPECT_FALSE(sql.contains(" Person t ON")) << "bare alias 't' leaked into JOIN:\n" << sql;
}

// Round-trip: insert one WideJoin row referencing 9 real Persons, JOIN them all
// back and confirm the 9th FK (t10) is populated — proves the SQL is executable.
TYPED_TEST(WideJoinTest, NineFKJoinRoundTrips) {
    QuerySet<Person, TypeParam> person_qs;
    std::array<int, 9>          person_ids{};
    for (int i = 0; i < 9; ++i) {
        Person const p{.name = std::format("WJ{}", i), .age = 20 + i};
        auto         res = person_qs.insert(p).execute();
        ASSERT_TRUE(res.has_value()) << res.error().message();
        person_ids[static_cast<std::size_t>(i)] = static_cast<int>(res.value());
    }

    QuerySet<WideJoin, TypeParam> wj_qs;
    WideJoin const                row{
                           .fk1 = {.id = person_ids[0]},
                           .fk2 = {.id = person_ids[1]},
                           .fk3 = {.id = person_ids[2]},
                           .fk4 = {.id = person_ids[3]},
                           .fk5 = {.id = person_ids[4]},
                           .fk6 = {.id = person_ids[5]},
                           .fk7 = {.id = person_ids[6]},
                           .fk8 = {.id = person_ids[7]},
                           .fk9 = {.id = person_ids[8]}
    };
    auto ins = wj_qs.insert(row).execute();
    ASSERT_TRUE(ins.has_value()) << ins.error().message();

    auto results = join_all_nine(wj_qs).select().execute();
    ASSERT_TRUE(results.has_value()) << results.error().message();
    ASSERT_EQ(results.value().size(), 1U);

    const auto& got = *results.value().begin();
    EXPECT_EQ(got.fk1.name, "WJ0");
    EXPECT_EQ(got.fk9.name, "WJ8") << "9th FK (alias t10) was not populated — broken SQL";
}

// NOLINTEND(misc-const-correctness)
