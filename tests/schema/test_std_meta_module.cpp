#include <gtest/gtest.h>

import std;
import storm_std_meta;
// Deliberately NO `#include <meta>`: `import std;` does not export std::meta::, so this TU
// reaches the reflection API only through storm_std_meta, the one module that owns the
// textual <meta> include (#756). It reflects a plain struct the way the library does.

namespace {
    struct MetaProbe {
        int         id{};
        std::string name;
        double      score{};
    };

    consteval auto member_count() -> std::size_t {
        return std::meta::nonstatic_data_members_of(^^MetaProbe, std::meta::access_context::unchecked()).size();
    }

    consteval auto member_name(std::size_t index) -> std::string_view {
        return std::meta::identifier_of(
                std::meta::nonstatic_data_members_of(^^MetaProbe, std::meta::access_context::unchecked())[index]
        );
    }

    consteval auto member_is_int(std::size_t index) -> bool {
        auto member = std::meta::nonstatic_data_members_of(^^MetaProbe, std::meta::access_context::unchecked())[index];
        return std::meta::dealias(std::meta::type_of(member)) == ^^int;
    }
} // namespace

TEST(StdMetaModuleTest, ReflectsMemberCount) {
    constexpr std::size_t count = member_count();
    EXPECT_EQ(count, 3U);
}

TEST(StdMetaModuleTest, ReflectsMemberNames) {
    constexpr std::string_view first = member_name(0);
    constexpr std::string_view last  = member_name(2);
    EXPECT_EQ(first, "id");
    EXPECT_EQ(last, "score");
}

TEST(StdMetaModuleTest, ReflectsMemberTypes) {
    constexpr bool id_is_int   = member_is_int(0);
    constexpr bool name_is_int = member_is_int(1);
    EXPECT_TRUE(id_is_int);
    EXPECT_FALSE(name_is_int);
}

TEST(StdMetaModuleTest, InfoIsAUsableValueType) {
    constexpr std::meta::info a         = ^^MetaProbe;
    constexpr std::meta::info b         = ^^MetaProbe;
    constexpr std::meta::info c         = ^^int;
    constexpr bool            same_type = (a == b);
    constexpr bool            diff_type = (a == c);
    EXPECT_TRUE(same_type);
    EXPECT_FALSE(diff_type);
}
