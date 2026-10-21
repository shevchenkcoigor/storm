/**
 * @file fuzz_batch_insert.cpp
 * @brief libFuzzer harness — fuzz batch sizes near the SQLite 999-param limit.
 *
 * Uses the fuzz input size (modulo 1100) as the batch count, exercising the
 * boundary at and beyond the 999-parameter SQLite limit. Field values are
 * derived from fuzz bytes to produce varied inputs.
 */

// NOLINTBEGIN(modernize-use-trailing-return-type,bugprone-unused-return-value,bugprone-empty-catch,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

#include <cstddef>
#include <cstdint>

import storm;
import std;

#include "fuzz_models.h" // AFTER import storm;

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    return fuzz_init_db(/*with_seed=*/false);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Use size modulo 1100 to get a batch count that exercises the 999 boundary
    const std::size_t batch_count = (size % 1100) + 1;

    // Build a batch; derive name/value from fuzz bytes for variety
    std::vector<FuzzModel> batch;
    batch.reserve(batch_count);
    for (std::size_t i = 0; i < batch_count; ++i) {
        const int value = (size > 0) ? static_cast<int>(data[i % size]) : static_cast<int>(i);
        batch.push_back(FuzzModel{.name = std::to_string(i), .value = value});
    }

    try {
        (void)QuerySet<FuzzModel>().insert(std::span<const FuzzModel>(batch)).execute();
    } catch (...) { // NOSONAR cpp:S2486 — SQL errors expected; only ASAN/crash = bug
    }
    return 0;
}

// NOLINTEND(modernize-use-trailing-return-type,bugprone-unused-return-value,bugprone-empty-catch,cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
