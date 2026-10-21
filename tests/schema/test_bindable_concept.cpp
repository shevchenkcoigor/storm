#include <gtest/gtest.h>

import storm_orm_utilities;
import std;

// Compile-time-only verification of the BindableType concept (#473). Every
// static_assert below is checked at TU compile time; the runtime TEST body only
// exists so the file registers with GoogleTest and the assertions are compiled.

using storm::orm::utilities::BindableType;
using storm::orm::utilities::UUID;

// ---- Positive: every field type the binder dispatch actually supports --------

// Integer storage classes (is_int_source_v)
static_assert(BindableType<int>);
static_assert(BindableType<short>);
static_assert(BindableType<unsigned short>);
static_assert(BindableType<unsigned int>);
static_assert(BindableType<signed char>);
static_assert(BindableType<unsigned char>);
static_assert(BindableType<char>);

// 64-bit integer storage classes (is_int64_source_v)
static_assert(BindableType<std::int64_t>);
static_assert(BindableType<long>);
static_assert(BindableType<long long>);
static_assert(BindableType<std::uint64_t>);
static_assert(BindableType<unsigned long>);
static_assert(BindableType<unsigned long long>);

// Floating point
static_assert(BindableType<double>);
static_assert(BindableType<float>);

// Boolean
static_assert(BindableType<bool>);

// Text-shaped
static_assert(BindableType<std::string>);
static_assert(BindableType<std::string_view>);
static_assert(BindableType<std::filesystem::path>);
static_assert(BindableType<UUID>);

// Chrono
static_assert(BindableType<std::chrono::year_month_day>);
static_assert(BindableType<std::chrono::system_clock::time_point>);
static_assert(BindableType<std::chrono::seconds>);
static_assert(BindableType<std::chrono::milliseconds>);

// BLOB
static_assert(BindableType<std::vector<std::uint8_t>>);
static_assert(BindableType<std::vector<unsigned char>>);
static_assert(BindableType<std::vector<std::byte>>);

// Enum (bound via underlying integer). WideEnum intentionally uses a 64-bit
// underlying type to cover the wide-enum arm of the concept / bind_enum, so the
// oversized base type is deliberate here.
enum class Color : std::uint8_t { Red, Green, Blue };
enum class WideEnum : std::int64_t { A, B }; // NOLINT(performance-enum-size)
static_assert(BindableType<Color>);
static_assert(BindableType<WideEnum>);

// std::optional<T> of any bindable T is itself bindable
static_assert(BindableType<std::optional<int>>);
static_assert(BindableType<std::optional<std::string>>);
static_assert(BindableType<std::optional<double>>);
static_assert(BindableType<std::optional<UUID>>);
static_assert(BindableType<std::optional<std::chrono::year_month_day>>);
static_assert(BindableType<std::optional<std::vector<std::uint8_t>>>);

// ---- Negative: types the binder cannot dispatch must be rejected -------------

struct NotBindable {
    int a;
    int b;
};

static_assert(!BindableType<NotBindable>);
static_assert(!BindableType<void*>);
static_assert(!BindableType<int*>);
static_assert(!BindableType<std::vector<int>>);           // non-byte vector is not a BLOB
static_assert(!BindableType<std::optional<NotBindable>>); // optional of unsupported inner

TEST(BindableConceptTest, CompileTimeOnly) {
    // The verification is entirely in the static_asserts above; this body just
    // gives GoogleTest something to run.
    SUCCEED();
}
