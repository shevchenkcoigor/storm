#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_db_default_connection;
import storm_db_pool;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_update;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_person_seed_fixture.h"
#include "test_seed_helpers.h"

template <typename ConnType> class ComplexWhereTest : public PersonSeedFixture<ConnType> {};

TYPED_TEST_SUITE(ComplexWhereTest, DatabaseTypes);

TYPED_TEST(ComplexWhereTest, OrWithAnd) {
    auto young    = fields::Person.age < 26;
    auto old      = fields::Person.age > 35;
    auto mkt      = fields::Person.department == "Marketing";
    auto combined = young || (old && mkt);

    auto result = this->qs->where(combined).select().execute();
    ASSERT_TRUE(result.has_value()) << "Complex OR/AND should work";
    EXPECT_GE(result.value().size(), 1);
}

TYPED_TEST(ComplexWhereTest, NestedAndOr) {
    auto eng_young = fields::Person.department == "Engineering" && fields::Person.age < 30;
    auto sales_old = fields::Person.department == "Sales" && fields::Person.age > 29;

    auto result = this->qs->where(eng_young || sales_old).select().execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_GE(result.value().size(), 1);
}
