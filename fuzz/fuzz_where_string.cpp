/**
 * @file fuzz_where_string.cpp
 * @brief libFuzzer harness — fuzz string WHERE filter values.
 *
 * Exercises the runtime SQL binding layer for string comparisons:
 * ==, !=, LIKE. SQL errors are expected and caught;
 * only crashes or ASAN/UBSAN reports indicate real bugs.
 */

// NOLINTBEGIN(modernize-use-trailing-return-type,bugprone-unused-return-value,bugprone-empty-catch,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include <cstddef>
#include <cstdint>
#include <meta> // BEFORE import std; — import std; does not export std::meta::

import storm;
import std;

#include "fuzz_models.h" // AFTER import storm;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    return fuzz_init_db();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    try {
        // Equal
        (void)QuerySet<FuzzModel>().where(storm::orm::where::field<^^FuzzModel::name>() == input).select().execute();
        // Not equal
        (void)QuerySet<FuzzModel>().where(storm::orm::where::field<^^FuzzModel::name>() != input).select().execute();
        // LIKE
        (void)QuerySet<FuzzModel>().where(storm::orm::where::field<^^FuzzModel::name>().like(input)).select().execute();
    } catch (...) { // NOSONAR cpp:S2486 — SQL errors expected; only ASAN/crash = bug
    }
    return 0;
}

// NOLINTEND(modernize-use-trailing-return-type,bugprone-unused-return-value,bugprone-empty-catch,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
