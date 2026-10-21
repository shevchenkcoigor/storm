/**
 * @file fuzz_where_int.cpp
 * @brief libFuzzer harness — fuzz int/double WHERE filter values.
 *
 * Interprets raw fuzz bytes as int/double values and exercises
 * the runtime SQL binding layer for numeric comparisons:
 * ==, !=, >, <, IN, BETWEEN.
 */

// NOLINTBEGIN(modernize-use-trailing-return-type,bugprone-unused-return-value,bugprone-empty-catch,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <meta> // BEFORE import std; — import std; does not export std::meta::

import storm;
import std;

#include "fuzz_models.h" // AFTER import storm;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    return fuzz_init_db();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Extract two int values from the fuzz input (pad with zeros if short)
    int val1 = 0;
    int val2 = 0;

    if (size >= sizeof(int)) {
        std::memcpy(&val1, data, sizeof(int));
    }
    if (size >= 2 * sizeof(int)) {
        std::memcpy(&val2, data + sizeof(int), sizeof(int));
    }

    try {
        // Equal int
        (void)QuerySet<FuzzModel>().where(storm::orm::where::field<^^FuzzModel::value>() == val1).select().execute();
        // Not equal
        (void)QuerySet<FuzzModel>().where(storm::orm::where::field<^^FuzzModel::value>() != val1).select().execute();
        // Greater than
        (void)QuerySet<FuzzModel>().where(storm::orm::where::field<^^FuzzModel::value>() > val1).select().execute();
        // Less than
        (void)QuerySet<FuzzModel>().where(storm::orm::where::field<^^FuzzModel::value>() < val2).select().execute();
        // IN with two values
        (void)QuerySet<FuzzModel>()
                .where(storm::orm::where::field<^^FuzzModel::value>().in(val1, val2))
                .select()
                .execute();
        // BETWEEN (ensure lower <= upper to form a valid range)
        const int lower = val1 < val2 ? val1 : val2;
        const int upper = val1 < val2 ? val2 : val1;
        (void)QuerySet<FuzzModel>()
                .where(storm::orm::where::field<^^FuzzModel::value>().between(lower, upper))
                .select()
                .execute();
    } catch (...) { // NOSONAR cpp:S2486 — SQL errors expected; only ASAN/crash = bug
    }
    return 0;
}

// NOLINTEND(modernize-use-trailing-return-type,bugprone-unused-return-value,bugprone-empty-catch,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
