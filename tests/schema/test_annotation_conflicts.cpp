#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import std;
import storm_orm_statements_base;
using storm::full_unsigned;
using storm::primary;
using storm::primary_autoincrement;
using storm::signed_storage;
using storm::orm::statements::BaseStatement;

// ── #492: ModelAnnotationsValid<T> conflict rejection ────────────────────────
// The free-standing flag annotation objects (primary, storm::unique, …)
// replaced enum class FieldAttr. An enum member was mutually exclusive for free;
// stackable annotation objects are not. ModelAnnotationsValid<T> restores the
// exclusivity guarantee at compile time, rejecting per member:
//   * both primary and primary_autoincrement (two primary-key modes),
//   * both signed_storage and full_unsigned (two 64-bit storage modes).
// It is ANDed into the BaseStatement<T> constraint list, so a conflicting model
// fails to instantiate a statement with a clear constraint violation. The first
// block asserts the gate predicate directly (not a public call); the second block
// asserts the constraint actually blocks BaseStatement<T> instantiation — together
// they pin the contract without a compile-fail harness.

namespace {
    using storm::orm::statements::ModelAnnotationsValid;

    // --- Conflict: primary + primary_autoincrement on the same member ---
    struct DoublePrimaryModel {
        [[= primary]][[= primary_autoincrement]] int id{};
        std::string                                  name;
    };
    static_assert(
            !ModelAnnotationsValid<DoublePrimaryModel>,
            "primary + primary_autoincrement on one member must NOT satisfy ModelAnnotationsValid"
    );

    // --- Conflict: signed_storage + full_unsigned on the same member ---
    struct DoubleStorageModel {
        [[= primary]] int                                     id{};
        [[= signed_storage]][[= full_unsigned]] std::uint64_t value{};
    };
    static_assert(
            !ModelAnnotationsValid<DoubleStorageModel>,
            "signed_storage + full_unsigned on one member must NOT satisfy ModelAnnotationsValid"
    );

    // --- Valid: each flag used at most once per member ---
    struct PlainPrimaryModel {
        [[= primary]] int               id{};
        [[= storm::unique]] std::string name;
    };
    static_assert(
            ModelAnnotationsValid<PlainPrimaryModel>,
            "single primary + single unique must satisfy ModelAnnotationsValid"
    );

    struct AutoincrementModel {
        [[= primary_autoincrement]] int id{};
        std::string                     name;
    };
    static_assert(
            ModelAnnotationsValid<AutoincrementModel>, "single primary_autoincrement must satisfy ModelAnnotationsValid"
    );

    struct SingleStorageModel {
        [[= primary]] int                 id{};
        [[= full_unsigned]] std::uint64_t value{};
    };
    static_assert(
            ModelAnnotationsValid<SingleStorageModel>,
            "single full_unsigned storage annotation must satisfy ModelAnnotationsValid"
    );
} // namespace

// The gate is load-bearing at the statement layer: BaseStatement<T> (and thus every
// statement it backs) is instantiable for a conflict-free model and NOT for a model
// carrying a conflicting pair — the ModelAnnotationsValid<T> requires-clause rejects
// it, not deep reflection. The `typename BaseStatement<T>` probe is wrapped in a
// variable template so the failed-constraint case is a dependent SFINAE soft-fail;
// naming the constrained specialization directly in a namespace-scope `requires` is
// eagerly instantiated by clang-p2996 and hard-errors instead of yielding false
// (same pattern as the #472 Entity test).
template <class T> constexpr bool base_statement_instantiable = requires { typename BaseStatement<T>; };
static_assert(base_statement_instantiable<PlainPrimaryModel>, "conflict-free model must back a statement");
static_assert(
        !base_statement_instantiable<DoublePrimaryModel>,
        "primary + primary_autoincrement conflict must block statement instantiation"
);
static_assert(
        !base_statement_instantiable<DoubleStorageModel>,
        "signed_storage + full_unsigned conflict must block statement instantiation"
);
