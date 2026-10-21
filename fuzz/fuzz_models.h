#pragma once

// IMPORTANT: Include this file AFTER `import storm;` and `import std;`.
// The [[= storm::primary]] attribute requires the storm module
// to be imported before this struct is compiled. std::string is supplied by the
// preceding `import std;` — a textual <string> here would be re-pulled after the
// module and trip a redeclaration error (COMPILER_ISSUES §9, Finding B).

struct FuzzModel {
    [[= storm::primary]] int id{};
    std::string name;
    int value{};
};

// NOLINTBEGIN(modernize-use-trailing-return-type,bugprone-unused-return-value,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

using storm::QuerySet;
using storm::db::DefaultConnection;
using storm::orm::schema::SchemaStatement;

/// Common DB initialisation — call from LLVMFuzzerInitialize().
/// @param with_seed  Insert one seed row so WHERE/LIKE queries can match.
/// @return 0 (libFuzzer convention; failures are non-fatal here).
inline int fuzz_init_db(bool with_seed = true) {
    auto result = DefaultConnection<>::set(":memory:");
    if (!result.has_value()) {
        return 0;
    }
    const auto &conn = DefaultConnection<>::get();
    (void)SchemaStatement<FuzzModel>::create_table_if_not_exists(conn);
    if (with_seed) {
        (void)QuerySet<FuzzModel>().insert(FuzzModel{.name = "seed", .value = 42}).execute();
    }
    return 0;
}

// NOLINTEND(modernize-use-trailing-return-type,bugprone-unused-return-value,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
