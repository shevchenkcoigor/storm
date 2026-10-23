#include <gtest/gtest.h>
#include <meta>

import storm_model;
import std;

// Compile-time-only verification of the ValidFieldInfo concept (#478). It gates a
// std::meta::info NTTP as a valid non-static data-member reference: true iff the
// info is a non-static data member AND has an identifier. Every static_assert is
// checked at TU compile time; the runtime TEST body exists only so the file
// registers with GoogleTest and the assertions are compiled.

using storm::meta::ValidFieldInfo;

// A plain reflectable model with a mix of member kinds.
struct Sample {
    int         id{};
    std::string name;
    double      salary{};

    static int shared_counter; // static data member — NOT a field
    void       method() {}     // member function — NOT a data member

    static constexpr int constant = 42; // static — NOT a field
};
int Sample::shared_counter = 0;

// ── Positive: real non-static data-member selectors satisfy ValidFieldInfo ────
static_assert(ValidFieldInfo<^^Sample::id>);
static_assert(ValidFieldInfo<^^Sample::name>);
static_assert(ValidFieldInfo<^^Sample::salary>);

// ── Negative: static members, member functions, and whole types are rejected ─
static_assert(!ValidFieldInfo<^^Sample::shared_counter>);
static_assert(!ValidFieldInfo<^^Sample::constant>);
static_assert(!ValidFieldInfo<^^Sample::method>);
static_assert(!ValidFieldInfo<^^Sample>); // the class type itself, not a member
static_assert(!ValidFieldInfo<^^int>);    // a scalar type
static_assert(!ValidFieldInfo<^^std::string>);

// ── Load-bearing: usable as a template constraint the way f<> uses it ─────────
// Naming an info NTTP concept in a requires-clause must SFINAE-soft-fail for a
// non-field info, not hard-error. Wrap the probe in a variable template so the
// failed-constraint case is a dependent soft-fail.
template <std::meta::info M>
    requires ValidFieldInfo<M>
consteval auto field_identifier() {
    return std::meta::identifier_of(M);
}

static_assert(field_identifier<^^Sample::name>() == "name");

template <std::meta::info M> constexpr bool field_selectable = requires { field_identifier<M>(); };
static_assert(field_selectable<^^Sample::id>);
static_assert(!field_selectable<^^Sample::shared_counter>);
static_assert(!field_selectable<^^Sample>);

TEST(ValidFieldInfoConcept, CompileTimeOnly) {
    // The static_asserts above are the real test; this keeps the TU a runnable
    // GTest target so the suite reports it.
    SUCCEED();
}
