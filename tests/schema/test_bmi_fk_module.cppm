module;

#include <meta>

#include "test_bmi_fk_models.h"

export module storm_test_bmi_fk;

import std;
import storm_orm_schema;

// #694 regression fixture: instantiates SchemaStatement<BmiMessage> — and through its FK,
// the model concepts for BmiUser — inside this module's BMI, the way storm_benchmark_*
// does for storm::benchmark::User.
export namespace storm_test_bmi {

    auto message_sqlite_ddl() -> std::string {
        return storm::orm::schema::SchemaStatement<BmiMessage>::create_table_sql<storm::orm::schema::Dialect::SQLite>();
    }

} // namespace storm_test_bmi
