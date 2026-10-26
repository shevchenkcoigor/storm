#include <gtest/gtest.h>
#include <meta>

import std;
import storm_model;
import storm_orm_field_attr;
import storm_orm_relation_meta;
import storm_orm_model_concepts;
import storm_orm_schema;
import storm_test_bmi_fk;

#include "test_bmi_fk_models.h" // NOSONAR cpp:S954 — models must follow the imports

// #694 / #262: questions the module's BMI never asked, put here to members taken from
// data_members_of_v<BmiMessage> — a list that BMI may already hold. These are the paths
// the #262 workaround protects (FK concept gate, ON DELETE policy, relation filter,
// max_length), so each reads annotations on a member reflection that crossed the BMI.
namespace {
    constexpr std::meta::info bmi_sender = storm::meta::data_members_of_v<BmiMessage>[1];
    constexpr std::meta::info bmi_text   = storm::meta::data_members_of_v<BmiMessage>[2];

    static_assert(storm::meta::is_fk_field(bmi_sender));
    static_assert(storm::meta::fk_on_delete_action_of(bmi_sender) == storm::RefAction::Cascade);
    static_assert(!storm::meta::is_relation_field(bmi_sender));
    static_assert(storm::orm::statements::FKFieldOf<BmiMessage, bmi_sender>);
    static_assert(!storm::meta::is_fk_field(bmi_text));
    static_assert(storm::meta::max_length_of(bmi_text) == 64U);
} // namespace

// #694: storm_test_bmi_fk instantiated SchemaStatement<BmiMessage> (and the FK target's
// model concepts) inside its BMI. Using the same models here reads whatever memoised
// reflection results that BMI holds; a memo stored as define_static_array storage was not
// a constant expression in an importer and broke the storm_bench build. This TU failing
// to COMPILE is the regression signal; the assertions check the DDL is the same on both
// sides of the boundary.
TEST(ReflectionCacheBmi, ModelInstantiatedInsideAModuleIsUsableFromAnImporter) {
    using storm::orm::schema::Dialect;
    using storm::orm::schema::SchemaStatement;

    EXPECT_EQ(SchemaStatement<BmiMessage>::create_table_sql<Dialect::SQLite>(), storm_test_bmi::message_sqlite_ddl());
    EXPECT_TRUE(SchemaStatement<BmiUser>::create_table_sql<Dialect::SQLite>().contains("BmiUser"));
    EXPECT_TRUE(SchemaStatement<BmiMessage>::create_table_sql<Dialect::SQLite>().contains("CHECK(length(text) <= 64)"));
    EXPECT_TRUE(SchemaStatement<BmiMessage>::create_table_sql<Dialect::SQLite>().contains("ON DELETE CASCADE"));
}
