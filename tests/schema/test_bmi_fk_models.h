#pragma once

// Models for the #694 cross-BMI regression test. Included both in the global module
// fragment of test_bmi_fk_module.cppm — so the model concepts and statement statics are
// instantiated INSIDE that module's BMI — and, after the imports, in the plain TU that
// uses the same models. That is the shape of benchmarks/models.hpp + storm_benchmark_*,
// which no tests/ TU had before: a memoised reflection result built in one BMI and read
// from an importer. The `import` lives here, like benchmarks/models.hpp, because a
// global module fragment cannot import directly.

import storm_model;

#include <string>

struct BmiUser {
    [[= storm::primary]] int id{};
    std::string name;
};

struct BmiMessage {
    [[= storm::primary]] int id{};
    [[= storm::fk<storm::RefAction::Cascade>]] BmiUser sender;
    [[= storm::max_length<64>]] std::string text;
};
