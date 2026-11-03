#pragma once

/**
 * Benchmark Models
 *
 * Person comes from shared/models/person.h (single source of truth).
 * User and FKMessage are benchmark-specific (JOIN benchmarks with 2 FKs).
 *
 * Include this BEFORE any `import` in the including TU: it does `import storm;` itself, after
 * the std headers person.h uses, to provide complete types.
 */

// Every std header person.h uses must be included textually BEFORE `import storm;`: with the
// storm BMI already loaded, a textual libc++ header re-declares entities the BMI holds and
// Linux fails with "cannot add 'abi_tag' attribute in a redeclaration" (copy.h:42). macOS does
// not show it. See COMPILER_ISSUES.md, Finding A (#756).
#include <cstdint>
#include <meta>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

import storm;

// Shared model structs — used by both tests and benchmarks. Include the
// per-model header rather than the shared/models.h umbrella (issue #634):
// Person is the only one of the twelve the benchmarks use.
#include "../shared/models/person.h"

namespace storm::benchmark {

    // Bring shared Person into benchmark namespace
    using ::Person;

    // Benchmark-specific models for JOIN tests (2 FK relationships)
    struct User {
        [[= storm::meta::primary]] int id;
        std::string                    name;
        int                            age;
    };

    struct FKMessage {
        [[= storm::meta::primary]] int id;
        [[= storm::meta::fk<>]] User   sender;
        [[= storm::meta::fk<>]] User   receiver;
        std::string                    text;
    };

    // INSERT-benchmark model — no UNIQUE, no indexes; matches the raw SQLite
    // anchor schema exactly so bulk inserts need no per-iteration DELETE.
    struct BenchPerson {
        [[= storm::meta::primary]] int id;
        std::string                    name;
        int                            age;
        double                         salary;
    };

} // namespace storm::benchmark
