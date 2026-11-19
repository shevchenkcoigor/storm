#pragma once

// Compile-time `BENCHMARK_TESTS` array — materialized locally per-TU.
//
// Why is this not in the storm_benchmark_parser module?
// ------------------------------------------------------
// Putting a `consteval` function whose body contains a 50KB `#embed` literal
// (and the resulting `inline constexpr` array initializer) into the module
// purview triggers a clang PCM deserialization bug ("declaration ID
// out-of-range for AST file") when any TU imports the module and instantiates
// the call. The parser primitives (parse_int, parse_str, parse_tests<N>,
// count_tests, ...) serialize into the PCM fine — only the `consteval`
// `#embed` driver is affected.
//
// Keeping `load_benchmark_tests()` + `BENCHMARK_TESTS` in this textual header
// (included only by register.cpp) sidesteps the bug while preserving the rest
// of the parser's module conversion.

#include <array>
#include <cstddef>
#include <string_view>

import storm_benchmark_schema; // BenchmarkTest
import storm_benchmark_parser; // parse_tests<>, count_tests

namespace storm::benchmark {

    // Every case within the spec caps and its string buffers (#845). The select cases are also held
    // to it by QueryBuilder (SpecWithinLimits); a write case is checked only here.
    template <std::size_t N> consteval auto all_within_limits(const std::array<BenchmarkTest, N>& tests) -> bool {
        for (const BenchmarkTest& t : tests)
            if (!t.within_limits())
                return false;
        return true;
    }

    consteval auto load_benchmark_tests() {
        static constexpr const char json_data[] = {
// #embed is a C23 feature Clang offers as an extension in C++26 mode; it is the
// only portable way to embed the JSON test corpus at compile time. Suppress
// -Wc23-extensions here so the -Werror policy (issue #317) stays clean.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#embed "tests/benchmark_tests.json"
#pragma clang diagnostic pop
                , '\0'
        };
        constexpr std::string_view json_str(json_data);
        constexpr std::size_t      test_count = count_tests(json_str);
        constexpr auto             tests      = parse_tests<test_count>(json_str);
        static_assert(all_within_limits(tests), "a benchmark case exceeds a spec cap or a string buffer (#843/#845)");
        return tests;
    }

    inline constexpr auto BENCHMARK_TESTS = load_benchmark_tests();

} // namespace storm::benchmark
