// Issue #853: value rules left over from the #844 reviews. A YAML value is read in its field's
// type; one that cannot be held exactly is a compile error, never a coercion. Every rule here is a
// constraint on QueryBuilder, so the tests are static_asserts on whether it can be named.

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

#include "shared/models/person.h" // NOSONAR cpp:S954
#include "shared/models/extended_types.h"
#include "test_select_runner.h"
#include "test_parser.hpp"

namespace {

    using storm::orm::query_builder::QueryBuilder;
    using storm::orm::query_builder::ValueFits;
    using Ext = ExtendedTypes;

    constexpr auto parse_case(std::string_view json) -> storm::benchmark::BenchmarkTest {
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc.bench;
    }

    template <typename Model, auto Spec>
    concept BuildableOn = requires { typename QueryBuilder<Model, Spec>; };

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

    // The same condition written as a `where_expr:` leaf with the typed `value:` key.
    constexpr auto where_expr_case(std::string_view field, std::string_view op, std::string_view value)
            -> storm::benchmark::BenchmarkTest {
        std::string json = R"({"where_expr":{"field":")";
        json += field;
        json += R"(","op":")";
        json += op;
        json += R"(","value":)";
        json += value;
        json += R"(},"dataset_size":5})";
        return parse_case(json);
    }

    constexpr auto operand(std::string_view field, std::string_view value) -> storm::benchmark::TypedValue {
        return where_case(field, "==", value).where.values[0];
    }

    // bool: an integer literal is 0 or 1, nothing else (`is_active == 7` is not a truth value).
    constexpr auto kBoolZero  = where_case("is_active", "==", "0");
    constexpr auto kBoolOne   = where_case("is_active", "==", "1");
    constexpr auto kBoolSeven = where_case("is_active", "==", "7");
    constexpr auto kBoolMinus = where_case("is_active", "==", "-1");
    static_assert(BuildableOn<Person, kBoolZero>);
    static_assert(BuildableOn<Person, kBoolOne>);
    static_assert(!BuildableOn<Person, kBoolSeven>, "an integer other than 0 / 1 is not a bool");
    static_assert(!BuildableOn<Person, kBoolMinus>);

    // float: an integer literal must be exact in a float, as an int64 literal already must be.
    constexpr auto kFloatExact   = where_case("approx", "==", "16777216");
    constexpr auto kFloatInexact = where_case("approx", "==", "16777217");
    constexpr auto kFloatSmall   = where_case("approx", ">", "5");
    static_assert(BuildableOn<Ext, kFloatExact>);
    static_assert(BuildableOn<Ext, kFloatSmall>);
    static_assert(!BuildableOn<Ext, kFloatInexact>, "16777217 rounds to 16777216 in a float");

    // duration: ExpressionVariant has no ComparisonExpr<duration> arm, so a WHERE on a duration column cannot
    // be built; the constraint says so at the use site (#853). The value rule below still gates
    // expected.first, which reads the member itself.
    constexpr auto kDurSmall = where_case("duration_field", "==", "5");
    constexpr auto kDurBig   = where_case("duration_field", "==", "5000000000");
    constexpr auto kDurIn    = where_case("duration_field", "IN", "1,5000000000");
    static_assert(!BuildableOn<Ext, kDurSmall>, "no duration comparison exists in ExpressionVariant");
    static_assert(!BuildableOn<Ext, kDurBig>);
    static_assert(!BuildableOn<Ext, kDurIn>);

    // The rule itself: an Int or an Int64 literal is range-checked against the duration's rep.
    constexpr auto kInt100      = operand("duration_field", "100");
    constexpr auto kInt300      = operand("duration_field", "300");
    constexpr auto kInt100k     = operand("duration_field", "100000");
    constexpr auto kInt64Big    = operand("duration_field", "5000000000");
    constexpr auto kIntNegative = operand("duration_field", "-129");
    using Millis                = std::chrono::milliseconds;
    using Short                 = std::chrono::duration<short>;
    using Tiny                  = std::chrono::duration<signed char>;
    using Int32                 = std::chrono::duration<int>;
    static_assert(ValueFits<Millis, kInt100> && ValueFits<Millis, kInt64Big>);
    static_assert(ValueFits<Short, kInt100> && ValueFits<Short, kInt300>);
    static_assert(!ValueFits<Short, kInt100k>, "100000 does not fit a 16-bit rep");
    static_assert(!ValueFits<Short, kInt64Big>);
    static_assert(ValueFits<Tiny, kInt100> && !ValueFits<Tiny, kInt300>);
    static_assert(!ValueFits<Tiny, kIntNegative>, "-129 does not fit an 8-bit rep");
    static_assert(ValueFits<Int32, kInt100k> && !ValueFits<Int32, kInt64Big>);
    static_assert(ValueFits<std::optional<Millis>, kInt64Big>);

    // UUID: lower-case hex only. storm::UUID keeps its text verbatim, so an upper-case literal would
    // not match a stored lower-case UUID on SQLite (TEXT compare) and would not round-trip on PG.
    constexpr auto kUuidLower = where_case("uuid_field", "==", R"("550e8400-e29b-41d4-a716-446655440000")");
    constexpr auto kUuidUpper = where_case("uuid_field", "==", R"("550E8400-E29B-41D4-A716-446655440000")");
    constexpr auto kUuidMixed = where_case("uuid_field", "==", R"("550e8400-E29b-41d4-a716-446655440000")");
    constexpr auto kUuidInUp  = where_case(
            "uuid_field", "IN", R"("550e8400-e29b-41d4-a716-446655440000","550E8400-E29B-41D4-A716-446655440001")"
    );
    static_assert(BuildableOn<Ext, kUuidLower>);
    static_assert(!BuildableOn<Ext, kUuidUpper>, "upper-case hex is not the stored spelling");
    static_assert(!BuildableOn<Ext, kUuidMixed>);
    static_assert(!BuildableOn<Ext, kUuidInUp>);

    // blob: ExpressionVariant has no ComparisonExpr<blob> arm, so a WHERE on a blob column cannot be built.
    // The constraint says so at the use site instead of failing deep inside make_shared (#853).
    constexpr auto kBlobEq   = where_case("raw_data", "==", R"("DEADBEEF")");
    constexpr auto kBlobIn   = where_case("raw_data", "IN", R"("DEADBEEF","00FF")");
    constexpr auto kBlobNull = where_case("raw_data", "==", "null");
    static_assert(!BuildableOn<Ext, kBlobEq>, "no blob comparison exists in ExpressionVariant");
    static_assert(!BuildableOn<Ext, kBlobIn>);
    static_assert(!BuildableOn<Ext, kBlobNull>);

    // expected.first reads the same rule.
    constexpr auto expected_value(std::string_view value) -> storm::benchmark::TypedValue {
        std::string json = R"({"expected":{"count":1,"first":{"m":)";
        json += value;
        json += R"(}},"dataset_size":5})";
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc.expected.first.fields[0].value;
    }
    constexpr auto kExpSec     = expected_value("90");
    constexpr auto kExpSecBig  = expected_value("5000000000");
    constexpr auto kExpSecText = expected_value(R"("90s")");
    static_assert(storm::test::ExpectedValueFits<std::chrono::seconds, kExpSec>);
    static_assert(storm::test::ExpectedValueFits<std::chrono::seconds, kExpSecBig>, "64-bit rep");
    static_assert(!storm::test::ExpectedValueFits<std::chrono::duration<int>, kExpSecBig>);
    static_assert(!storm::test::ExpectedValueFits<std::chrono::seconds, kExpSecText>);
    constexpr auto kExpUuidUpper = expected_value(R"("550E8400-E29B-41D4-A716-446655440000")");
    constexpr auto kExpUuidLower = expected_value(R"("550e8400-e29b-41d4-a716-446655440000")");
    static_assert(storm::test::ExpectedValueFits<decltype(Ext::uuid_field), kExpUuidLower>);
    static_assert(!storm::test::ExpectedValueFits<decltype(Ext::uuid_field), kExpUuidUpper>);

    // A joined WHERE may target the related model, so QueryBuilder's class constraint does not check its values
    // against the base model (whose field of the same name has another type). This pins only that the base-model
    // check is skipped; apply_where's per-WhereModel static_assert is the one that checks the related model.
    struct JoinOwner {
        [[= storm::primary]] int id{};
        int                      level{};
    };
    struct JoinChild {
        [[= storm::primary]] int    id{};
        [[= storm::fk<>]] JoinOwner owner;
        std::string                 level;
    };
    constexpr auto kLevelNoJoin =
            parse_case(R"({"where":{"conditions":[{"field":"level","op":">","values":[5]}]},"dataset_size":5})");
    constexpr auto kLevelJoined = parse_case(
            R"({"join":{"type":"inner","fk":"owner"},"where":{"conditions":[{"field":"level","op":">","values":[5]}]},"dataset_size":5})"
    );
    static_assert(!BuildableOn<JoinChild, kLevelNoJoin>, "level is a string on the base model");
    static_assert(
            BuildableOn<JoinChild, kLevelJoined>, "the base model's string `level` must not reject a joined spec"
    );

    // where_expr leaves take the same typed `value:` as `where:` (null, int64, string).
    constexpr auto kExprNull   = where_expr_case("opt_double", "==", "null");
    constexpr auto kExprInt64  = where_expr_case("big_num", "==", "5000000000");
    constexpr auto kExprUuid   = where_expr_case("uuid_field", "==", R"("550e8400-e29b-41d4-a716-446655440000")");
    constexpr auto kExprDate   = where_expr_case("date_field", "==", R"("2024-01-15")");
    constexpr auto kExprNullOn = where_expr_case("label", "==", "null");
    constexpr auto kExprBigInt = where_expr_case("id", "==", "5000000000");
    static_assert(kExprNull.where.is_well_formed() && kExprNull.where.value_count == 1);
    static_assert(BuildableOn<Ext, kExprNull>);
    static_assert(BuildableOn<Ext, kExprInt64>);
    static_assert(BuildableOn<Ext, kExprUuid>);
    static_assert(BuildableOn<Ext, kExprDate>);
    static_assert(!BuildableOn<Ext, kExprNullOn>, "null on a NOT NULL column");
    static_assert(!BuildableOn<Ext, kExprBigInt>, "5000000000 does not fit an int field");

    TEST(TypedValueRules, WhereExprLeafKeepsNullAndInt64Apart) {
        using Kind = storm::benchmark::TypedValue::Kind;
        EXPECT_TRUE(kExprNull.where.values[0].kind == Kind::Null);
        EXPECT_TRUE(kExprInt64.where.values[0].kind == Kind::Int64);
        EXPECT_EQ(kExprInt64.where.values[0].as_int64, 5000000000LL);
        EXPECT_TRUE(kExprUuid.where.values[0].kind == Kind::String);
    }

} // namespace
