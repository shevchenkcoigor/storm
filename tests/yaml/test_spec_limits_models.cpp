// Issue #872: the key-shape models of dataset `keys` (tests/test_key_models.h) through the YAML model table and
// the shared checks the corpus runners apply. What the corpus cannot spell must not compile: a key-shape model
// without its dataset, an `expected.first` value on an FK member (a whole related row), a write case on a model
// whose records need an FK target, or a value that is not the key's type.

#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_select;
import storm_orm_utilities;
import storm_orm_where;
import std;

#include "test_select_runner.h" // NOSONAR cpp:S954
#include "test_keys_dataset.h"
#include "test_parser.hpp"

namespace {

    namespace km = storm::test::key_models;
    using storm::test::yaml_model_dataset_matches;
    using storm::test::yaml_model_index;
    using storm::test::YAML_MODELS;

    using storm::orm::query_builder::QueryBuilder;

    constexpr auto parse_case(std::string_view json) -> storm::benchmark::BenchmarkTest {
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc.bench;
    }

    // One WHERE condition `field op value`; `value` is the JSON text of the operand.
    constexpr auto where_case(std::string_view field, std::string_view op, std::string_view value)
            -> storm::benchmark::BenchmarkTest {
        std::string json = R"({"where":{"conditions":[{"field":")";
        json += field;
        json += R"(","op":")";
        json += op;
        json += R"(","values":[)";
        json += value;
        json += R"(]}]},"dataset_size":5})";
        return parse_case(json);
    }

    template <typename Model, auto Spec>
    concept BuildableOn = requires { typename QueryBuilder<Model, Spec>; };

    // Every key-shape model resolves to its type and is tied to dataset `keys`, the one that creates its tables.
    static_assert(YAML_MODELS[yaml_model_index("uuid_pk")].type == ^^UuidPkModel);
    static_assert(YAML_MODELS[yaml_model_index("timestamped")].type == ^^TimestampedRecord);
    static_assert(YAML_MODELS[yaml_model_index("stock")].type == ^^km::KStock);
    static_assert(YAML_MODELS[yaml_model_index("course")].type == ^^storm::benchmark::m2m::BCourse);
    static_assert(yaml_model_dataset_matches("stock", "keys") && !yaml_model_dataset_matches("stock", ""));
    static_assert(!yaml_model_dataset_matches("uuid_pk", "main") && !yaml_model_dataset_matches("ticket", "relations"));
    static_assert(!yaml_model_dataset_matches("course", "keys") && yaml_model_dataset_matches("course", "relations"));

    // A WHERE on a key is read in the key's type: a UUID text for the UUID key, an int for an int part, a
    // date-time for an auto-timestamp (a date alone is not one).
    constexpr auto kUuidEq    = where_case("id", "==", R"("00000000-0000-4000-8000-000000000001")");
    constexpr auto kUuidInt   = where_case("id", "==", "1");
    constexpr auto kUuidUpper = where_case("id", "==", R"("00000000-0000-4000-8000-00000000000A")");
    constexpr auto kPartInt   = where_case("warehouse", "==", "1");
    constexpr auto kPartText  = where_case("warehouse", "==", R"("1")");
    constexpr auto kStampTime = where_case("created_at", ">", R"("2000-01-01T00:00:00")");
    constexpr auto kStampDate = where_case("created_at", ">", R"("2000-01-01")");
    static_assert(BuildableOn<UuidPkModel, kUuidEq> && !BuildableOn<UuidPkModel, kUuidInt>);
    static_assert(!BuildableOn<UuidPkModel, kUuidUpper>);
    static_assert(BuildableOn<km::KStock, kPartInt> && !BuildableOn<km::KStock, kPartText>);
    static_assert(BuildableOn<TimestampedRecord, kStampTime> && !BuildableOn<TimestampedRecord, kStampDate>);

    // expected.first on an FK member would compare a whole related row: no YAML value fits it.
    using storm::test::ExpectedValueFits;
    constexpr auto kText = kUuidEq.where.values[0];
    constexpr auto kInt  = kPartInt.where.values[0];
    static_assert(!ExpectedValueFits<UuidPkModel, kText> && !ExpectedValueFits<km::KStock, kInt>);
    static_assert(ExpectedValueFits<storm::UUID, kText> && ExpectedValueFits<int, kInt>);

    // A write case needs make_record for its model; an FK model has none, so such a case is a compile error.
    template <typename Model>
    concept HasWriteRecord = requires { storm::test::make_record<Model>(0); };
    static_assert(HasWriteRecord<km::KStock> && HasWriteRecord<km::KTicket> && HasWriteRecord<UuidPkModel>);
    static_assert(HasWriteRecord<TimestampedRecord>);
    static_assert(!HasWriteRecord<km::KShipment> && !HasWriteRecord<km::KUuidRef>);

    TEST(SpecLimitsModels, WriteRecordsKeepTheSeededKeysFree) {
        EXPECT_EQ(storm::test::make_record<km::KStock>(0).warehouse, 100);
        EXPECT_EQ(storm::test::make_record<km::KTicket>(2).id, 1002);
        const km::KStock original{.warehouse = 1, .sku = "apple", .quantity = 10};
        const auto       updated = storm::test::make_updated_record<km::KStock>(original);
        EXPECT_EQ(updated.warehouse, original.warehouse);
        EXPECT_EQ(updated.sku, original.sku);
        EXPECT_TRUE(storm::test::is_original_record<km::KStock>(original));
        EXPECT_FALSE(storm::test::is_original_record<km::KStock>(updated));
    }

} // namespace
