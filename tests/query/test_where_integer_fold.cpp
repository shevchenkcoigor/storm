#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness,performance-unnecessary-value-param)

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

#include "../../shared/models/extended_types.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// Verifies that WHERE filters on narrow / unsigned integer fields fold to the
// existing int / int64_t variant arms (the same fold enums already use). Before
// #407 these source types had no ComparisonExpr arm and failed to compile.
//
// Seeds three rows so range filters split the set:
//   r1: u_int=10,  ll_signed=100,  tiny_unsigned=1
//   r2: u_int=20,  ll_signed=200,  tiny_unsigned=2
//   r3: u_int=30,  ll_signed=300,  tiny_unsigned=3
template <typename ConnType> class WhereIntegerFoldTest : public StormTestFixture<ExtendedTypes, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<ExtendedTypes> batch = {
                {.u_int = 10U, .ll_signed = 100LL, .label = "r1", .tiny_unsigned = 1},
                {.u_int = 20U, .ll_signed = 200LL, .label = "r2", .tiny_unsigned = 2},
                {.u_int = 30U, .ll_signed = 300LL, .label = "r3", .tiny_unsigned = 3},
        };
        ASSERT_TRUE((InsertStatement<ExtendedTypes, ConnType>{}).query(batch).execute().has_value());
    }

    static auto count_where(auto expr) -> std::size_t {
        auto result = (SelectStatement<ExtendedTypes, ConnType>{}).where(expr).execute();
        EXPECT_TRUE(result.has_value()) << "WHERE failed: " << result.error().message();
        return result.has_value() ? result.value().size() : 0;
    }
};

TYPED_TEST_SUITE(WhereIntegerFoldTest, DatabaseTypes);

// ===== unsigned int → folds to int arm =====

TYPED_TEST(WhereIntegerFoldTest, UnsignedIntEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.u_int == 20U), 1U);
}
TYPED_TEST(WhereIntegerFoldTest, UnsignedIntGreater) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.u_int > 10U), 2U);
}
TYPED_TEST(WhereIntegerFoldTest, UnsignedIntBetween) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.u_int.between(15U, 25U)), 1U);
}
TYPED_TEST(WhereIntegerFoldTest, UnsignedIntIn) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.u_int.in(10U, 30U)), 2U);
}

// ===== long long → folds to int64_t arm =====

TYPED_TEST(WhereIntegerFoldTest, LongLongLessEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.ll_signed <= 200LL), 2U);
}
TYPED_TEST(WhereIntegerFoldTest, LongLongBetween) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.ll_signed.between(150LL, 350LL)), 2U);
}
TYPED_TEST(WhereIntegerFoldTest, LongLongIn) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.ll_signed.in(100LL, 300LL)), 2U);
}

// ===== unsigned char (narrow) → folds to int arm =====

TYPED_TEST(WhereIntegerFoldTest, TinyUnsignedGreaterEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.tiny_unsigned >= static_cast<unsigned char>(2)), 2U);
}

// ===== unsigned int above INT_MAX (#729) =====
// Values past INT_MAX must keep their magnitude on the SQL side. Bound as signed 32-bit they wrap
// negative, so comparison, ordering and aggregation silently disagree with the C++ values even
// though an insert/select round trip looks fine.
//
//   r1: u_int=10, r2: u_int=3'000'000'000, r3: u_int=4'000'000'000
template <typename ConnType> class WhereUnsignedAboveIntMaxTest : public StormTestFixture<ExtendedTypes, ConnType> {
  public:
    static constexpr unsigned int kThreeBillion = 3'000'000'000U;
    static constexpr unsigned int kFourBillion  = 4'000'000'000U;

    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<ExtendedTypes> batch = {
                {.u_int = 10U, .label = "r1"},
                {.u_int = kThreeBillion, .label = "r2"},
                {.u_int = kFourBillion, .label = "r3"},
        };
        ASSERT_TRUE((InsertStatement<ExtendedTypes, ConnType>{}).query(batch).execute().has_value());
    }

    static auto labels_where(auto expr) -> std::vector<std::string> {
        auto result = (SelectStatement<ExtendedTypes, ConnType>{}).where(expr).execute();
        EXPECT_TRUE(result.has_value()) << "WHERE failed: " << result.error().message();
        std::vector<std::string> labels;
        if (result.has_value()) {
            for (const auto& row : result.value()) {
                labels.push_back(row.label);
            }
        }
        std::ranges::sort(labels);
        return labels;
    }
};

TYPED_TEST_SUITE(WhereUnsignedAboveIntMaxTest, DatabaseTypes);

TYPED_TEST(WhereUnsignedAboveIntMaxTest, GreaterThanAboveIntMax) {
    using TestFixture = WhereUnsignedAboveIntMaxTest<TypeParam>;
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int > TestFixture::kThreeBillion),
            (std::vector<std::string>{"r3"})
    );
}
TYPED_TEST(WhereUnsignedAboveIntMaxTest, GreaterEqualAboveIntMax) {
    using TestFixture = WhereUnsignedAboveIntMaxTest<TypeParam>;
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int >= TestFixture::kThreeBillion),
            (std::vector<std::string>{"r2", "r3"})
    );
}
TYPED_TEST(WhereUnsignedAboveIntMaxTest, EqualAboveIntMax) {
    using TestFixture = WhereUnsignedAboveIntMaxTest<TypeParam>;
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int == TestFixture::kFourBillion),
            (std::vector<std::string>{"r3"})
    );
}
TYPED_TEST(WhereUnsignedAboveIntMaxTest, BetweenAboveIntMax) {
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int.between(2'500'000'000U, 3'500'000'000U)),
            (std::vector<std::string>{"r2"})
    );
}
TYPED_TEST(WhereUnsignedAboveIntMaxTest, InAboveIntMax) {
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int.in(10U, 4'000'000'000U)),
            (std::vector<std::string>{"r1", "r3"})
    );
}

TYPED_TEST(WhereUnsignedAboveIntMaxTest, NotEqualAboveIntMax) {
    using TestFixture = WhereUnsignedAboveIntMaxTest<TypeParam>;
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int != TestFixture::kThreeBillion),
            (std::vector<std::string>{"r1", "r3"})
    );
}
TYPED_TEST(WhereUnsignedAboveIntMaxTest, LessThanAboveIntMax) {
    using TestFixture = WhereUnsignedAboveIntMaxTest<TypeParam>;
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int < TestFixture::kFourBillion),
            (std::vector<std::string>{"r1", "r2"})
    );
}
TYPED_TEST(WhereUnsignedAboveIntMaxTest, LessEqualAboveIntMax) {
    using TestFixture = WhereUnsignedAboveIntMaxTest<TypeParam>;
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int <= TestFixture::kThreeBillion),
            (std::vector<std::string>{"r1", "r2"})
    );
}
TYPED_TEST(WhereUnsignedAboveIntMaxTest, OrderByAscendingKeepsMagnitudeOrder) {
    auto result = (SelectStatement<ExtendedTypes, TypeParam>{})
                          .all()
                          .template order_by<fields::ExtendedTypes.u_int>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    std::vector<std::string> labels;
    for (const auto& row : result.value()) {
        labels.push_back(row.label);
    }
    EXPECT_EQ(labels, (std::vector<std::string>{"r1", "r2", "r3"}));
}

// The documented fix for rows written before #729 (see FIELD_TYPES.md): a value above INT_MAX was
// stored wrapped negative, and adding 2^32 restores it. Simulate that legacy state with raw SQL,
// run the documented UPDATE, then read the rows back through the ORM.
TYPED_TEST(WhereUnsignedAboveIntMaxTest, DocumentedMigrationRestoresWrappedValues) {
    using TestFixture = WhereUnsignedAboveIntMaxTest<TypeParam>;
    const auto conn   = storm::db::BasicDefaultConnection<TypeParam>::get();

    const std::string legacy = "UPDATE ExtendedTypes SET u_int = u_int - 4294967296 WHERE label = 'r3'";
    ASSERT_TRUE(conn->execute(legacy).has_value()) << legacy;
    EXPECT_EQ(this->labels_where(fields::ExtendedTypes.u_int == TestFixture::kFourBillion).size(), 0U);

    const std::string migrate = "UPDATE ExtendedTypes SET u_int = u_int + 4294967296 WHERE u_int < 0";
    ASSERT_TRUE(conn->execute(migrate).has_value()) << migrate;

    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int == TestFixture::kFourBillion),
            (std::vector<std::string>{"r3"})
    );
    EXPECT_EQ(
            this->labels_where(fields::ExtendedTypes.u_int >= TestFixture::kThreeBillion),
            (std::vector<std::string>{"r2", "r3"})
    );
}

// NOLINTEND(misc-const-correctness,performance-unnecessary-value-param)
