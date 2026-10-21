#pragma once

/**
 * @file test_seed_helpers.h
 * @brief Batch insert/update/erase helpers for test fixture setup.
 *
 * Centralises the ORM batch API calls so that if the API changes,
 * only this file needs updating. All test fixtures should use these
 * instead of per-row loops or inline batch calls.
 *
 * Model-agnostic — the PEOPLE_25-seeding fixture that used to live here is
 * now test_person_seed_fixture.h (issue #634).
 *
 * Must be included AFTER `import storm;` (uses ORM types).
 * Usage: ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(people)));
 */

#include <gtest/gtest.h>
#include <span>
#include <vector>
import storm_defaults;
import storm_orm_statements_update;
import storm_orm_statements_erase;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::UpdateStatement;

namespace storm::test {

/// Batch-insert a vector of Model objects.  Returns true on success.
/// On failure, records the error via ADD_FAILURE() and returns false.
/// An empty vector is a no-op that always succeeds.
template <typename Model, typename ConnType> auto batch_insert(const std::vector<Model> &objects) -> bool {
    if (objects.empty())
        return true;

    auto result = (InsertStatement<Model, ConnType>{}).query(std::span<const Model>(objects)).execute();
    if (!result.has_value()) {
        ADD_FAILURE() << "batch_insert failed: " << result.error().message();
        return false;
    }
    return true;
}

/// Batch-update a vector of Model objects.  Returns true on success.
/// On failure, records the error via ADD_FAILURE() and returns false.
/// An empty vector is a no-op that always succeeds.
template <typename Model, typename ConnType> auto batch_update(const std::vector<Model> &objects) -> bool {
    if (objects.empty())
        return true;

    auto result = (UpdateStatement<Model, ConnType>{}).query(std::span<const Model>(objects)).execute();
    if (!result.has_value()) {
        ADD_FAILURE() << "batch_update failed: " << result.error().message();
        return false;
    }
    return true;
}

/// Batch-erase a vector of Model objects.  Returns true on success.
/// On failure, records the error via ADD_FAILURE() and returns false.
/// An empty vector is a no-op that always succeeds.
template <typename Model, typename ConnType> auto batch_remove(const std::vector<Model> &objects) -> bool {
    if (objects.empty())
        return true;

    auto result = (EraseStatement<Model, ConnType>{}).query(std::span<const Model>(objects)).execute();
    if (!result.has_value()) {
        ADD_FAILURE() << "batch_remove failed: " << result.error().message();
        return false;
    }
    return true;
}

} // namespace storm::test
