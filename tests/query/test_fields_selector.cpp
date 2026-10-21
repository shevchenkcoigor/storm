#include <gtest/gtest.h>
#include <meta>

import storm_meta;
import std;
import storm_orm_fields;

#include "test_fields_models.h" // NOSONAR cpp:S954
using storm::field_specs_for;

using storm::meta::FieldSelector;
using storm::meta::RelationSelector;
using storm::meta::selector_info;
using storm::meta::ValidSelector;

// ── The proxy carries the right member ───────────────────────────────────────
static_assert(std::meta::identifier_of(selector_info<fields::FSPerson.age>()) == "age");
static_assert(std::meta::identifier_of(selector_info<fields::FSPerson.name>()) == "name");
static_assert(std::meta::identifier_of(selector_info<fields::FSPerson.id>()) == "id");
static_assert(std::meta::identifier_of(selector_info<fields::FSPerson.age>()) != "name");

// ── The inherited Field<M> static reaches through the base ───────────────────
// Load-bearing: removing the base class breaks exactly this (mutation-verified).
static_assert(decltype(fields::FSPerson.age)::field_name_sv == "age");

// ── The proxy resolves to exactly the member's reflection ───────────────────
// A RAW ^^Model::member is no longer accepted anywhere: selector_info is
// proxy-only, so there is one public spelling rather than two.
static_assert(selector_info<fields::FSPerson.age>() == ^^FSPerson::age);
// A raw info is rejected. Routed through a template parameter: `requires` naming
// a concrete unsatisfied call at namespace scope is a hard error, not a soft false.
template <auto S> constexpr bool convertible = requires { selector_info<S>(); };
static_assert(convertible<fields::FSPerson.age>);
static_assert(!convertible<^^FSPerson::age>);

// ── The concept distinguishes the two spellings ──────────────────────────────
static_assert(FieldSelector<std::remove_cvref_t<decltype(fields::FSPerson.age)>>);
static_assert(!FieldSelector<std::meta::info>);

// ── ValidSelector accepts ONLY the proxy spelling ───────────────────────────
// The widened QuerySet/aggregate/order_by methods constrain on this concept, so
// a typo inside it surfaces here rather than buried in template noise later.
static_assert(ValidSelector<fields::FSPerson.age>); // the proxy
static_assert(!ValidSelector<^^FSPerson::age>);     // a raw info: REJECTED now
static_assert(!ValidSelector<42>);                  // not a selector at all

// ── Relation members ARE present, but as a DIFFERENT proxy type ──────────────
// A relation member (m2m, reverse_fk) is not a column, but it IS a legal join
// target — `join<fields::Article.tags>()` has to work. So field_specs_for emits
// RelationRef for it instead of FieldRef. RelationRef deliberately has NO
// where::Field<M> base, which is what keeps a relation out of a WHERE clause
// (the #408 guarantee) while still naming it for a join.
template <typename FieldsT> constexpr bool has_tags  = requires(const FieldsT& obj) { obj.tags; };
template <typename FieldsT> constexpr bool has_title = requires(const FieldsT& obj) { obj.title; };

static_assert(has_title<decltype(fields::FSArticle)>); // a real column
static_assert(has_tags<decltype(fields::FSArticle)>);  // the m2m member, as RelationRef

// The two proxies are distinct types, and only the column one is a FieldSelector.
static_assert(FieldSelector<std::remove_cvref_t<decltype(fields::FSArticle.title)>>);
static_assert(!FieldSelector<std::remove_cvref_t<decltype(fields::FSArticle.tags)>>);
static_assert(RelationSelector<std::remove_cvref_t<decltype(fields::FSArticle.tags)>>);
static_assert(!RelationSelector<std::remove_cvref_t<decltype(fields::FSArticle.title)>>);

// Both resolve to their member for the NTTP positions.
static_assert(std::meta::identifier_of(selector_info<fields::FSArticle.tags>()) == "tags");

// ── A relation proxy is NOT usable in a WHERE clause (#408 preserved) ────────
// This is the load-bearing difference from FieldRef, and the reason RelationRef
// does not derive from where::Field<M>. The comparison operators exist but are
// `= delete("...")`, so the rejection carries an actionable message naming the
// fix instead of the bare "invalid operands to binary expression" a missing
// operator produces. Deleted still means not-viable, so these stay false.
//
// SCOPE of these asserts: they guard that a relation stays REJECTED. They cannot
// distinguish "operator absent" from "operator deleted with a message" — both
// yield false — so they are not a regression guard on the diagnostic TEXT. That
// text was verified by compiling a probe TU; if the `= delete("...")` reasons are
// ever dropped these keep passing, so re-check the message by hand when touching
// RelationRef's operators.
template <auto S> constexpr bool where_capable     = requires { S == 1; };
template <auto S> constexpr bool orderable         = requires { S < 1; };
template <auto S> constexpr bool inequality_usable = requires { S != 1; };
static_assert(where_capable<fields::FSPerson.age>);        // a column: comparable
static_assert(!where_capable<fields::FSArticle.tags>);     // a relation: NOT comparable
static_assert(!orderable<fields::FSArticle.tags>);         // ...and not orderable
static_assert(!inequality_usable<fields::FSArticle.tags>); // ...on any comparison
// The relation proxy also offers none of Field<M>'s named predicates. These go
// through a template parameter: `requires` on a concrete type whose member does
// not exist is a hard error at namespace scope, not a soft false.
template <typename P> constexpr bool has_like    = requires(const P& p) { p.like("x"); };
template <typename P> constexpr bool has_in      = requires(const P& p) { p.in(1, 2); };
template <typename P> constexpr bool has_is_null = requires(const P& p) { p.is_null(); };
static_assert(has_like<decltype(fields::FSPerson.name)>);   // a column HAS them
static_assert(!has_like<decltype(fields::FSArticle.tags)>); // a relation does NOT
static_assert(!has_in<decltype(fields::FSArticle.tags)>);
static_assert(!has_is_null<decltype(fields::FSArticle.tags)>);

// NullableField narrowing (#613): a plain, non-optional column must NOT expose
// is_null()/is_not_null(). Routes through utilities::is_optional_v now, not
// duck-typing on `::value_type` — std::string has one too, and used to satisfy
// the old check, silently compiling an IS NULL that could never match. Pins
// the behavior so a future loosening of NullableField fails here, not silently.
static_assert(!has_is_null<decltype(fields::FSPerson.name)>); // std::string: NOT nullable
static_assert(!has_is_null<decltype(fields::FSPerson.age)>);  // int: NOT nullable

// ValidSelector gates the column positions, so it must REJECT a relation proxy.
static_assert(ValidSelector<fields::FSArticle.title>);
static_assert(!ValidSelector<fields::FSArticle.tags>);

// ── Field counts ─────────────────────────────────────────────────────────────
static_assert(field_specs_for(^^FSPerson).size() == 3);
static_assert(field_specs_for(^^FSArticle).size() == 3); // id, title, tags

// ── Adding a field auto-propagates: no edit to the fields:: declaration ─────
static_assert(field_specs_for(^^FSPersonExtra).size() == 4);
static_assert(std::meta::identifier_of(selector_info<fields::FSPersonExtra.salary>()) == "salary");

TEST(FieldsSelector, CompileTimeOnly) {
    SUCCEED(); // the static_asserts above are the test
}

// ── Destructuring works, and its arity is compiler-checked ───────────────────
// Caveats (documented in Task 5): positional, and arity-fragile. A structured
// binding cannot be constexpr; a plain `const auto&` to the constexpr object is
// the supported form.
TEST(FieldsSelector, DestructuringBindsByPosition) {
    const auto& [fs_id, fs_name, fs_age] = fields::FSPerson;
    static_assert(std::meta::identifier_of(selector_info<fs_id>()) == "id");
    static_assert(std::meta::identifier_of(selector_info<fs_age>()) == "age");
    SUCCEED();
}
