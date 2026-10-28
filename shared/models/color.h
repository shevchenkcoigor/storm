#ifndef STORM_SHARED_MODELS_COLOR_H
#define STORM_SHARED_MODELS_COLOR_H

/**
 * @file color.h
 * @brief Color enum — the enum-column type used by ExtendedTypes.
 *
 * The one header here with no storm dependency, so unlike its siblings it parses
 * standalone — which is why it keeps an include guard and stays off
 * scripts/clang_tidy_skiplist.py. Still included after `import storm;` in
 * practice, since its consumers pull extended_types.h alongside it.
 */

// Enum type for testing enum field support.
//
// The `: int` is load-bearing, not an oversight: schema.cppm's integer_width_of<T>
// resolves an enum column's PG width from its underlying type (#603), so this is
// what makes the column INTEGER. Narrowing it to std::uint8_t as
// performance-enum-size suggests would silently emit SMALLINT instead and change
// the DDL that tests/schema/test_types.cpp asserts.
// NOLINTNEXTLINE(performance-enum-size)
enum class Color : int { Red = 0, Green = 1, Blue = 2 };

#endif // STORM_SHARED_MODELS_COLOR_H
