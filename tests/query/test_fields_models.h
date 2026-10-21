#ifndef TESTS_QUERY_TEST_FIELDS_MODELS_H
#define TESTS_QUERY_TEST_FIELDS_MODELS_H

// Models local to the #518 fields-selector tests. Include AFTER `import storm;`
// — the [[= storm::*]] annotations need the module, and the fields:: blocks below
// call field_specs_for, a function, which is a harder dependency still.

#include <meta>
#include <string>
#include <vector>
using storm::field_specs_for;
using storm::many_to_many;
using storm::primary;

// A plain model: three scalar columns, no relations.
struct FSPerson {
    [[= primary]] int id{};
    std::string name;
    int age{};
};

struct FSTag {
    [[= primary]] int id{};
    std::string label;
};

// Carries a relation member — the generated struct MUST omit `tags`.
struct FSArticle {
    [[= primary]] int id{};
    std::string title;
    [[= many_to_many<>]] std::vector<FSTag> tags;
};

// Same as FSPerson plus one field — proves generation reads the model rather
// than a frozen list, with a byte-identical declaration below.
struct FSPersonExtra {
    [[= primary]] int id{};
    std::string name;
    int age{};
    double salary{};
};

namespace fields {

// The 2-line user-side declaration this issue standardises. No field names, so
// it cannot drift when a model gains or loses a member.
struct FSPersonT;
consteval { std::meta::define_aggregate(^^FSPersonT, field_specs_for(^^FSPerson)); }
inline constexpr FSPersonT FSPerson{};

struct FSArticleT;
consteval { std::meta::define_aggregate(^^FSArticleT, field_specs_for(^^FSArticle)); }
inline constexpr FSArticleT FSArticle{};

struct FSPersonExtraT;
consteval { std::meta::define_aggregate(^^FSPersonExtraT, field_specs_for(^^FSPersonExtra)); }
inline constexpr FSPersonExtraT FSPersonExtra{};

} // namespace fields

#endif // TESTS_QUERY_TEST_FIELDS_MODELS_H
