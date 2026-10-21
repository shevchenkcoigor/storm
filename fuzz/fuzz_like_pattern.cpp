/**
 * @file fuzz_like_pattern.cpp
 * @brief libFuzzer harness — fuzz LIKE pattern strings with adversarial input.
 *
 * Exercises the LIKE binding path with inputs that contain null bytes,
 * SQLite wildcards (%,_), quote characters, and unicode. SQL errors are
 * expected and caught; only crashes/ASAN hits are bugs.
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
    const std::string pattern(reinterpret_cast<const char*>(data), size);

    try {
        (void)QuerySet<FuzzModel>()
                .where(storm::orm::where::field<^^FuzzModel::name>().like(pattern))
                .select()
                .execute();
    } catch (...) { // NOSONAR cpp:S2486 — SQL errors expected; only ASAN/crash = bug
    }
    return 0;
}

// NOLINTEND(modernize-use-trailing-return-type,bugprone-unused-return-value,bugprone-empty-catch,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
