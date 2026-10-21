#pragma once

/**
 * @file test_person_seed_fixture.h
 * @brief StormTestFixture specialisation that seeds PEOPLE_25.
 *
 * Split out of test_seed_helpers.h (issue #634): it is the only part of that
 * header that needs Person and the 25-row seed array, and only two TUs use it,
 * while the batch_* helpers it lived next to are model-agnostic.
 *
 * IMPORTANT: Include AFTER `import storm;` — the model header below requires it.
 */

#include "../shared/models/people_25.h"
#include "../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"

#include <gtest/gtest.h>
#include <memory>
#include <vector>
using storm::QuerySet;

/// Fixture that creates a Person QuerySet and seeds PEOPLE_25.
/// Reuse to avoid duplicating the same on_after_setup / TearDown / qs boilerplate.
template <typename ConnType> class PersonSeedFixture : public StormTestFixture<Person, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        qs = std::make_unique<QuerySet<Person, ConnType>>();

        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
            std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end()))));
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<Person, ConnType>::TearDown();
    }

    std::unique_ptr<QuerySet<Person, ConnType>> qs;
};
