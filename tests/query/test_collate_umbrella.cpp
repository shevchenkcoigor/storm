// test_collate_umbrella.cpp - `Collate` is reachable as `storm::Collate` through `import storm;` alone (#679)
//
// The user guide writes `using namespace storm;` and then a bare `Collate::NoCase`. That only
// resolves if the umbrella re-exports `Collate` into `storm::` itself; the canonical
// `storm::orm::utilities::Collate` is not found by a `using namespace storm;` directive.
// This TU deliberately imports ONLY `storm` and declares no `using ...::Collate;`.
#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"

// SQLite-only: COLLATE NOCASE is a SQLite collation sequence.
using CollateUmbrellaTypes = ::testing::Types<storm::db::sqlite::Connection>;

template <typename ConnType> class CollateUmbrellaTest : public StormTestFixture<Person, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<Person> const people = {
                {.id = 1, .name = "alice", .age = 25},
                {.id = 2, .name = "ALICE", .age = 30},
                {.id = 3, .name = "Bob", .age = 35},
        };
        auto result = storm::QuerySet<Person, ConnType>{}.insert(std::span<const Person>(people)).execute();
        ASSERT_TRUE(result.has_value()) << result.error().message();
    }
};

TYPED_TEST_SUITE(CollateUmbrellaTest, CollateUmbrellaTypes);

TYPED_TEST(CollateUmbrellaTest, BareCollateInWhereViaUsingNamespaceStorm) {
    using namespace storm;
    QuerySet<Person, TypeParam> qs;
    auto result = qs.where(fields::Person.name.collate(Collate::NoCase) == "Alice").select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 2U); // "alice" and "ALICE"
}

TYPED_TEST(CollateUmbrellaTest, BareCollateInOrderByViaUsingNamespaceStorm) {
    using namespace storm;
    QuerySet<Person, TypeParam> qs;
    auto result = qs.template order_by<fields::Person.name, Collate::NoCase, false>().select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 3U);
    EXPECT_EQ(result->begin()->name, "Bob"); // NOCASE DESC: "Bob" sorts above both alice spellings
}

// NOLINTEND(misc-const-correctness)
