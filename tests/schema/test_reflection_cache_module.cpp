#include <gtest/gtest.h>
#include <meta>

import std;
import storm_orm_reflection_cache;
import storm_orm_field_attr;
import storm_orm_relation_meta;

// #694: memoised<R>, the annotation predicates that route through it, and the per-model
// member list data_members_of_v<T>. Each must give the same answers as the uncached
// query it replaced, on the first call and on every later one (which reads the memo).
namespace {
    template <int N> inline constexpr int square_v = N * N;

    template <std::meta::info Member> inline constexpr bool is_int_member_v = std::meta::type_of(Member) == ^^int;

    template <typename A, typename B> inline constexpr bool same_v = std::is_same_v<A, B>;

    struct Probe {
        int    count{};
        double ratio{};
    };

    consteval auto probe_member(std::size_t index) -> std::meta::info {
        return std::meta::nonstatic_data_members_of(^^Probe, std::meta::access_context::unchecked())[index];
    }

    static_assert(storm::meta::memoised<int>(^^square_v, {std::meta::reflect_constant(7)}) == 49);
    static_assert(storm::meta::memoised<bool>(^^is_int_member_v, {std::meta::reflect_constant(probe_member(0))}));
    static_assert(!storm::meta::memoised<bool>(^^is_int_member_v, {std::meta::reflect_constant(probe_member(1))}));
    static_assert(storm::meta::memoised<bool>(^^same_v, {^^int, ^^int}));
    static_assert(!storm::meta::memoised<bool>(^^same_v, {^^int, ^^long}));

    // Identical arguments name one specialisation — the property memoisation rests on
    // (that a specialisation's initializer runs once is the language's guarantee).
    consteval auto same_specialisation() -> bool {
        const auto arg = std::meta::reflect_constant(probe_member(0));
        return std::meta::substitute(^^is_int_member_v, {arg}) == std::meta::substitute(^^is_int_member_v, {arg});
    }
    static_assert(same_specialisation());

    // data_members_of_v<T>: the member list, once per T, held by value.
    struct Empty {};

    template <typename T> consteval auto matches_raw_members() -> bool {
        const auto raw = std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked());
        return std::ranges::equal(storm::meta::data_members_of_v<T>, raw);
    }

    static_assert(matches_raw_members<Probe>());
    static_assert(matches_raw_members<Empty>());
    static_assert(storm::meta::data_members_of_v<Empty>.empty());
    static_assert(std::is_same_v<
                  std::remove_cv_t<decltype(storm::meta::data_members_of_v<Probe>)>,
                  std::array<std::meta::info, 2>>);

    struct Target {
        [[= storm::meta::primary]] int id{};
    };

    struct Model {
        [[= storm::meta::primary]] int                                         id{};
        std::string                                                            plain;
        [[= storm::meta::unique]][[= storm::meta::max_length<32>]] std::string code;
        [[= storm::meta::fk<storm::meta::RefAction::Cascade>]] Target          owner;
        [[= storm::meta::many_to_many<>]] std::vector<Target>                  tags;
    };

    consteval auto member(std::size_t index) -> std::meta::info {
        return std::meta::nonstatic_data_members_of(^^Model, std::meta::access_context::unchecked())[index];
    }

    // One annotation, none, several — each asked twice so the second call reads the memo.
    static_assert(storm::meta::is_primary_member(member(0)) && storm::meta::is_primary_member(member(0)));
    static_assert(!storm::meta::is_primary_member(member(1)) && !storm::meta::is_primary_member(member(1)));
    static_assert(storm::meta::is_unique(member(2)) && !storm::meta::is_unique(member(1)));
    static_assert(!storm::meta::has_annotation_type<storm::meta::Primary>(member(2)));

    static_assert(storm::meta::max_length_of(member(2)) == 32U);
    static_assert(!storm::meta::max_length_of(member(1)).has_value());
    static_assert(!storm::meta::max_length_of(member(3)).has_value());

    static_assert(storm::meta::is_fk_field(member(3)) && storm::meta::is_fk_field(member(3)));
    static_assert(!storm::meta::is_fk_field(member(2)));
    static_assert(storm::meta::fk_annotation_type_of(member(3)) == ^^storm::meta::Fk<storm::meta::RefAction::Cascade>);

    static_assert(storm::meta::is_m2m_field(member(4)) && storm::meta::is_relation_field(member(4)));
    static_assert(!storm::meta::is_m2m_field(member(3)) && !storm::meta::is_relation_field(member(1)));
    static_assert(matches_raw_members<Model>());
} // namespace

TEST(ReflectionCacheModule, MemoisedReadsTheVariableTemplateValue) {
    EXPECT_EQ(storm::meta::memoised<int>(^^square_v, {std::meta::reflect_constant(7)}), 49);
    EXPECT_TRUE(storm::meta::memoised<bool>(^^same_v, {^^int, ^^int}));
    EXPECT_TRUE(same_specialisation());
}

TEST(ReflectionCacheModule, DataMembersOfMatchesTheRawMemberList) {
    EXPECT_TRUE(matches_raw_members<Probe>());
    EXPECT_TRUE(matches_raw_members<Model>());
    constexpr std::size_t model_members = storm::meta::data_members_of_v<Model>.size();
    constexpr bool        empty_members = storm::meta::data_members_of_v<Empty>.empty();
    EXPECT_EQ(model_members, 5U);
    EXPECT_TRUE(empty_members);
}

TEST(ReflectionCacheModule, MemoisedAnnotationPredicatesKeepTheirAnswers) {
    EXPECT_TRUE(storm::meta::is_primary_member(member(0)));
    EXPECT_FALSE(storm::meta::is_primary_member(member(1)));
    EXPECT_EQ(storm::meta::max_length_of(member(2)), 32U);
    EXPECT_TRUE(storm::meta::is_fk_field(member(3)));
    EXPECT_TRUE(storm::meta::is_m2m_field(member(4)));
}
