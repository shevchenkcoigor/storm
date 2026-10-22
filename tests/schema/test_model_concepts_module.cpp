#include <gtest/gtest.h>
#include <meta>

import std;
import storm_orm_field_attr;
import storm_orm_utilities;
import storm_orm_model_concepts;
// Deliberately NOT importing storm_orm_statements_base or storm: this TU proves the model-validation
// concepts live in a leaf that does not depend on the statement hub.

namespace {
    using namespace storm::orm::statements;
    using storm::meta::RefAction;

    // ── Accepted shapes ──────────────────────────────────────────────────────────
    struct SinglePk {
        [[= storm::meta::primary]] int id{};
        std::string                    name;
    };

    struct CompositePk {
        [[= storm::meta::primary_part]] int          tenant_id{};
        [[= storm::meta::primary_part]] std::int64_t entry_id{};
        int                                          payload{};
    };

    struct UuidPk {
        [[= storm::meta::primary]] storm::orm::utilities::UUID id{};
        std::string                                            title;
    };

    template <typename T>
    concept AllModelConcepts = ModelWithPrimaryKey<T> && PrimaryKeyType<T> && ModelStorageAnnotated<T> &&
                               ModelFkPoliciesValid<T> && ModelAnnotationsValid<T> && ModelMaxLengthValid<T> &&
                               ModelPrimaryKeyValid<T> && ModelTimestampPkValid<T> && ModelPrimaryKeyPartLimit<T>;

    static_assert(AllModelConcepts<SinglePk>, "single integer PK must satisfy every model concept");
    static_assert(AllModelConcepts<CompositePk>, "composite PK must satisfy every model concept");
    static_assert(AllModelConcepts<UuidPk>, "UUID PK must satisfy every model concept");

    // ── One rejection per concept ────────────────────────────────────────────────
    struct NoPk {
        int         id{};
        std::string name;
    };
    static_assert(!ModelWithPrimaryKey<NoPk>);

    struct FloatingPointPk {
        [[= storm::meta::primary]] double id{};
    };
    static_assert(!PrimaryKeyType<FloatingPointPk>);

    struct NullablePk {
        [[= storm::meta::primary]] std::optional<int> id;
    };
    static_assert(!ModelPrimaryKeyValid<NullablePk>);

    struct DoublePrimary {
        [[= storm::meta::primary]][[= storm::meta::primary_autoincrement]] int id{};
    };
    static_assert(!ModelAnnotationsValid<DoublePrimary>);

    struct BareUint64 {
        [[= storm::meta::primary]] int id{};
        std::uint64_t                  counter{};
    };
    static_assert(!ModelStorageAnnotated<BareUint64>);

    struct FkTarget {
        [[= storm::meta::primary]] int id{};
    };
    struct SetNullNonNullable {
        [[= storm::meta::primary]] int                     id{};
        [[= storm::meta::fk<RefAction::SetNull>]] FkTarget owner;
    };
    static_assert(!ModelFkPoliciesValid<SetNullNonNullable>);

    struct MaxLengthOnInt {
        [[= storm::meta::primary]] int       id{};
        [[= storm::meta::max_length<8>]] int age{};
    };
    static_assert(!ModelMaxLengthValid<MaxLengthOnInt>);

    struct AutoStampedPk {
        [[= storm::meta::primary]][[= storm::meta::auto_update]] std::chrono::system_clock::time_point id{};
    };
    static_assert(!ModelTimestampPkValid<AutoStampedPk>);

    struct FivePartKey {
        [[= storm::meta::primary_part]] int a{};
        [[= storm::meta::primary_part]] int b{};
        [[= storm::meta::primary_part]] int c{};
        [[= storm::meta::primary_part]] int d{};
        [[= storm::meta::primary_part]] int e{};
    };
    static_assert(!ModelPrimaryKeyPartLimit<FivePartKey>);
} // namespace

TEST(ModelConceptsModule, LeafModuleExposesModelConcepts) {
    EXPECT_TRUE(AllModelConcepts<SinglePk>);
}
