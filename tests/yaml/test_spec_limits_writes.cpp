// Issue #871: the write spec of the YAML corpus (`set:`, `on_conflict:`, `expected.remaining` /
// `matching`) through the test entry point (tests/test_parser.hpp). Each key is read by the
// query types that use it and rejected anywhere else, and every shape a runner would silently
// ignore (a `where` on update_all, an `order_by` on a write, an upsert with no `update`) is a
// compile error: TestCase::write_shape_error() names the first broken rule, and the corpus
// static_asserts it empty with that text as the message.

#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_queryset;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_utilities;
import storm_orm_where;
import std;

#include "shared/models/person.h" // NOSONAR cpp:S954
#include "test_select_runner.h"
#include "test_write_runner.h"
#include "test_parser.hpp"

namespace {

    constexpr auto parse_test_case(std::string_view json) -> storm::test::TestCase {
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc;
    }

    constexpr auto error_of(std::string_view json) -> std::string_view {
        return parse_test_case(json).write_shape_error();
    }

    // --- The positive controls: one of each write query type ---
    constexpr auto kUpdateWhere = parse_test_case(
            R"({"query_type":"update_where","where":{"conditions":[{"field":"age","op":">","values":[30]}]},"set":{"age":77,"name":"x"},"expected":{"remaining":25,"matching":3}})"
    );
    constexpr auto kUpdateAll = parse_test_case(
            R"({"query_type":"update_all","set":{"age":77},"expected":{"remaining":25,"matching":25}})"
    );
    constexpr auto kEraseWhere = parse_test_case(
            R"({"query_type":"erase_where","where":{"conditions":[{"field":"age","op":">","values":[30]}]},"expected":{"remaining":5}})"
    );
    constexpr auto kUpsertUpdate = parse_test_case(
            R"({"query_type":"upsert_update","set":{"name":"Alice","age":99},"on_conflict":{"target":["name"],"update":["age"]},"expected":{"remaining":25,"matching":1}})"
    );
    constexpr auto kUpsertNothing = parse_test_case(
            R"({"query_type":"upsert_nothing","set":{"name":"Alice","department":"Engineering"},"on_conflict":{"target":["name","department"]},"expected":{"remaining":25,"matching":0}})"
    );
    static_assert(kUpdateWhere.write_shape_error().empty());
    static_assert(kUpdateAll.write_shape_error().empty());
    static_assert(kEraseWhere.write_shape_error().empty());
    static_assert(kUpsertUpdate.write_shape_error().empty());
    static_assert(kUpsertNothing.write_shape_error().empty());
    static_assert(kUpdateWhere.within_limits() && kUpsertUpdate.within_limits() && kUpsertNothing.within_limits());

    // The parsed keys.
    static_assert(kUpdateWhere.set.count == 2);
    static_assert(kUpdateWhere.set.fields[0].name == "age" && kUpdateWhere.set.fields[0].value.as_int == 77);
    static_assert(kUpdateWhere.set.fields[1].name == "name" && kUpdateWhere.set.fields[1].value.as_string == "x");
    static_assert(kUpdateWhere.expected.remaining == 25 && kUpdateWhere.expected.matching == 3);
    static_assert(kUpsertUpdate.on_conflict.enabled && kUpsertUpdate.on_conflict.target.declared_count == 1);
    static_assert(kUpsertUpdate.on_conflict.target.fields[0] == "name");
    static_assert(kUpsertUpdate.on_conflict.update.declared_count == 1);
    static_assert(kUpsertUpdate.on_conflict.update.fields[0] == "age");
    static_assert(kUpsertNothing.on_conflict.target.declared_count == 2);
    static_assert(kUpsertNothing.on_conflict.update.declared_count == 0);
    // A `where_expr:` is a where too.
    static_assert(
            error_of(
                    R"({"query_type":"erase_where","where_expr":{"field":"age","op":">","value_int":1},"expected":{"remaining":5}})"
            )
                    .empty()
    );
    // A repeated `set:` replaces the first, as every repeated key does (#843).
    constexpr auto kSetTwice = parse_test_case(
            R"({"query_type":"update_all","set":{"age":1,"name":"a"},"set":{"salary":2.5},"expected":{"remaining":1,"matching":1}})"
    );
    static_assert(kSetTwice.set.count == 1 && kSetTwice.set.fields[0].name == "salary");

    // --- Caps: counted past, then rejected (#843) ---
    constexpr auto kSetOverCap = parse_test_case(
            R"({"query_type":"update_all","set":{"a":1,"b":2,"c":3,"d":4,"e":5},"expected":{"remaining":1,"matching":1}})"
    );
    static_assert(kSetOverCap.set.count == 5 && !kSetOverCap.within_limits());
    constexpr auto kTargetOverCap = parse_test_case(
            R"({"query_type":"upsert_nothing","set":{"a":1},"on_conflict":{"target":["a","b","c","d","e"]},"expected":{"remaining":1,"matching":1}})"
    );
    static_assert(!kTargetOverCap.within_limits());
    constexpr auto kUpdateOverCap = parse_test_case(
            R"({"query_type":"upsert_update","set":{"a":1},"on_conflict":{"target":["a"],"update":["a","b","c","d","e"]},"expected":{"remaining":1,"matching":1}})"
    );
    static_assert(!kUpdateOverCap.within_limits());
    // A member name longer than its buffer is a truncated string, rejected like an over-long array (#845).
    constexpr auto kSetLongName = parse_test_case(
            R"({"query_type":"update_all","set":{"a_member_name_longer_than_the_buffer_holds":1},"expected":{"remaining":1,"matching":1}})"
    );
    static_assert(!kSetLongName.within_limits());

    // --- The filter: where on update_where / erase_where only, nothing a write ignores ---
    static_assert(
            error_of(R"({"query_type":"update_where","set":{"age":1},"expected":{"remaining":1,"matching":1}})") ==
            "update_where / erase_where need a where: (or where_expr:)"
    );
    static_assert(
            error_of(R"({"query_type":"erase_where","expected":{"remaining":1}})") ==
            "update_where / erase_where need a where: (or where_expr:)"
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"set":{"age":1},"expected":{"remaining":1,"matching":1}})"
            ) == "update_all / upsert_update / upsert_nothing take no where: the write would ignore it"
    );
    static_assert(
            error_of(
                    R"({"query_type":"upsert_nothing","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"set":{"name":"a"},"on_conflict":{"target":["name"]},"expected":{"remaining":1,"matching":1}})"
            ) == "update_all / upsert_update / upsert_nothing take no where: the write would ignore it"
    );
    constexpr std::string_view kIgnoredModifier =
            "a write ignores join: / order_by: / limit:, so a case may not write them";
    static_assert(
            error_of(
                    R"({"query_type":"erase_where","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"order_by":[{"field":"age"}],"expected":{"remaining":1}})"
            ) == kIgnoredModifier
    );
    static_assert(
            error_of(
                    R"({"query_type":"erase_where","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"limit":{"value":2},"expected":{"remaining":1}})"
            ) == kIgnoredModifier
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_where","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"join":{"type":"inner","fk":"sender"},"set":{"age":1},"expected":{"remaining":1,"matching":1}})"
            ) == kIgnoredModifier
    );
    constexpr std::string_view kResultShape =
            "a write runs no values / distinct / group_by / aggregate / setop / first / get";
    static_assert(
            error_of(
                    R"({"query_type":"update_all","values":{"fields":["age"]},"set":{"age":1},"expected":{"remaining":1,"matching":1}})"
            ) == kResultShape
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","setop":{"operands":[{},{"op":"union"}]},"set":{"age":1},"expected":{"remaining":1,"matching":1}})"
            ) == kResultShape
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","aggregate":{"func":"count"},"set":{"age":1},"expected":{"remaining":1,"matching":1}})"
            ) == kResultShape
    );

    // --- What a write case checks ---
    static_assert(
            error_of(R"({"query_type":"update_all","set":{"age":1},"expected":{"matching":1}})") ==
            "a write case needs expected.remaining: the rows in the table after the write"
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"expected":{"count":1,"remaining":1,"matching":1}})"
            ) == "a write case checks expected.remaining / matching; expected.count would not be read"
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"expected":{"remaining":1,"matching":1,"first":{"age":1}}})"
            ) == "a write case checks expected.remaining / matching; expected.first / rows would not be read"
    );
    static_assert(
            error_of(R"({"query_type":"update_all","set":{"age":1},"expected":{"remaining":1}})") ==
            "update_where / update_all / upsert_* need expected.matching: the rows equal to set: after the write"
    );
    static_assert(
            error_of(
                    R"({"query_type":"erase_where","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"expected":{"remaining":1,"matching":0}})"
            ) == "expected.matching is read by update_where / update_all / upsert_* only"
    );
    static_assert(
            error_of(R"({"query_type":"update_all","expected":{"remaining":1,"matching":1}})") ==
            "update_where / update_all / upsert_* need a set: of at least one field"
    );
    static_assert(
            error_of(R"({"query_type":"update_all","set":{},"expected":{"remaining":1,"matching":1}})") ==
            "update_where / update_all / upsert_* need a set: of at least one field"
    );
    static_assert(
            error_of(
                    R"({"query_type":"erase_where","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"set":{"age":1},"expected":{"remaining":1}})"
            ) == "set: is read by update_where / update_all / upsert_* only"
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"erase_count":3,"expected":{"remaining":1,"matching":1}})"
            ) == "insert_count / update_count / erase_count belong to the batch query types"
    );

    // --- on_conflict ---
    static_assert(
            error_of(R"({"query_type":"upsert_update","set":{"name":"a"},"expected":{"remaining":1,"matching":1}})") ==
            "upsert_update / upsert_nothing need on_conflict: {target: [...]}"
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"on_conflict":{"target":["name"]},"expected":{"remaining":1,"matching":1}})"
            ) == "on_conflict: is read by upsert_update / upsert_nothing only"
    );
    static_assert(
            error_of(
                    R"({"query_type":"upsert_nothing","set":{"name":"a"},"on_conflict":{"update":["age"]},"expected":{"remaining":1,"matching":1}})"
            ) == "on_conflict.target names the conflict columns: at least one"
    );
    // #871: a DO UPDATE column with no set: value would be overwritten with the member's default.
    static_assert(
            error_of(
                    R"({"query_type":"upsert_update","set":{"name":"a","age":1},"on_conflict":{"target":["name"],"update":["salary"]},"expected":{"remaining":1,"matching":1}})"
            ) == "every on_conflict.update column needs its value in set:"
    );
    static_assert(
            error_of(
                    R"({"query_type":"upsert_update","set":{"name":"a","age":1},"on_conflict":{"target":["name"],"update":["age","salary"]},"expected":{"remaining":1,"matching":1}})"
            ) == "every on_conflict.update column needs its value in set:"
    );

    // #871: `set:` must be a mapping; a list or a scalar is never read as one, on any query type.
    constexpr std::string_view kSetNotMapping = "set: is a {member: value} mapping";
    static_assert(
            error_of(R"({"query_type":"update_all","set":["age",1],"expected":{"remaining":1,"matching":1}})") ==
            kSetNotMapping
    );
    static_assert(
            error_of(R"({"query_type":"update_all","set":"age","expected":{"remaining":1,"matching":1}})") ==
            kSetNotMapping
    );
    static_assert(error_of(R"({"query_type":"select","set":[1]})") == kSetNotMapping);

    // #871: an expected.* key no write runner reads is rejected, not skipped.
    constexpr std::string_view kUnreadExpected =
            "a write case checks expected.remaining / matching only; another expected.* key would not be read";
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"expected":{"remaining":1,"matching":1,"int_val":3}})"
            ) == kUnreadExpected
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"expected":{"remaining":1,"matching":1,"dbl_val":-1.0}})"
            ) == kUnreadExpected
    );
    static_assert(
            error_of(
                    R"({"query_type":"erase_where","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"expected":{"remaining":1,"unchanged":2}})"
            ) == kUnreadExpected
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"expected":{"remaining":1,"matching":1,"group_keys":[1],"groups_count":1}})"
            ) == kUnreadExpected
    );
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"expected":{"remaining":1,"matching":1,"bogus":1}})"
            ) == kUnreadExpected
    );
    // A per-step aggregation result needs an aggregation, which no write runs.
    static_assert(
            error_of(
                    R"({"query_type":"update_all","set":{"age":1},"aggregations":[{"func":"count","result":{"value":1}}],"expected":{"remaining":1,"matching":1}})"
            ) == "a write runs no values / distinct / group_by / aggregate / setop / first / get"
    );
    // Read-side cases keep their expected keys.
    static_assert(error_of(R"({"query_type":"count","expected":{"int_val":3}})").empty());

    // #871: a conflict column with no set: value would conflict on the member's default.
    static_assert(
            error_of(
                    R"({"query_type":"upsert_nothing","set":{"age":1},"on_conflict":{"target":["name"]},"expected":{"remaining":1,"matching":1}})"
            ) == "every on_conflict.target column needs its value in set:"
    );
    static_assert(
            error_of(
                    R"({"query_type":"upsert_nothing","set":{"age":1,"name":"a"},"on_conflict":{"target":["name","department"]},"expected":{"remaining":1,"matching":1}})"
            ) == "every on_conflict.target column needs its value in set:"
    );
    static_assert(
            error_of(
                    R"({"query_type":"upsert_update","set":{"name":"a"},"on_conflict":{"target":["name"]},"expected":{"remaining":1,"matching":1}})"
            ) == "upsert_update needs on_conflict.update, upsert_nothing takes none"
    );
    static_assert(
            error_of(
                    R"({"query_type":"upsert_nothing","set":{"name":"a"},"on_conflict":{"target":["name"],"update":["age"]},"expected":{"remaining":1,"matching":1}})"
            ) == "upsert_update needs on_conflict.update, upsert_nothing takes none"
    );
    // A misspelt key (`fields` for `target`) or a non-object is rejected, never skipped.
    static_assert(
            error_of(
                    R"({"query_type":"upsert_nothing","set":{"name":"a"},"on_conflict":{"target":["name"],"fields":["age"]},"expected":{"remaining":1,"matching":1}})"
            ) == "on_conflict: takes target and update only"
    );
    static_assert(
            error_of(
                    R"({"query_type":"upsert_nothing","set":{"name":"a"},"on_conflict":["name"],"expected":{"remaining":1,"matching":1}})"
            ) == "on_conflict: takes target and update only"
    );

    // --- The write keys on a query type that would not read them ---
    static_assert(
            error_of(R"({"query_type":"select","set":{"age":1},"expected":{"count":1}})") ==
            "set: is read by update_where / update_all / upsert_* only"
    );
    static_assert(
            error_of(R"({"query_type":"insert_one","on_conflict":{"target":["name"]},"expected":{"count":1}})") ==
            "on_conflict: is read by upsert_update / upsert_nothing only"
    );
    static_assert(
            error_of(R"({"query_type":"erase_all","expected":{"remaining":0,"matching":0}})") ==
            "expected.matching is read by update_where / update_all / upsert_* only"
    );
    // Untouched: the existing write and read cases.
    static_assert(error_of(R"({"query_type":"erase_all","expected":{"remaining":0}})").empty());
    static_assert(
            error_of(
                    R"({"query_type":"select","where":{"conditions":[{"field":"age","op":">","values":[1]}]},"expected":{"count":1}})"
            )
                    .empty()
    );

    // --- Values: `set:` is read in each member's type, like a WHERE operand (#844) ---
    template <const auto& Tc>
    concept SetFits = storm::test::SetSpecFits<Person, Tc>;

    static_assert(SetFits<kUpdateWhere> && SetFits<kUpsertUpdate> && SetFits<kUpsertNothing>);
    // The rows equal to set: compare every set: entry; the upserted row is found by the target's alone.
    static_assert(storm::test::set_match_spec<kUpsertUpdate>.where.value_count == 2);
    static_assert(storm::test::set_match_spec<kUpsertUpdate, true>.where.value_count == 1);
    static_assert(storm::test::set_match_spec<kUpsertUpdate, true>.where.values[0].as_string == "Alice");
    static_assert(storm::test::set_match_spec<kUpsertNothing, true>.where.value_count == 2);
    constexpr auto kSetNull = parse_test_case(
            R"({"query_type":"update_all","set":{"score":null,"nickname":"n"},"expected":{"remaining":1,"matching":1}})"
    );
    static_assert(SetFits<kSetNull>);
    constexpr auto kSetMisfit =
            parse_test_case(R"({"query_type":"update_all","set":{"age":"x"},"expected":{"remaining":1,"matching":1}})");
    constexpr auto kSetNullNotNull = parse_test_case(
            R"({"query_type":"update_all","set":{"age":null},"expected":{"remaining":1,"matching":1}})"
    );
    constexpr auto kSetFraction =
            parse_test_case(R"({"query_type":"update_all","set":{"age":1.5},"expected":{"remaining":1,"matching":1}})");
    constexpr auto kSetUnknownMember = parse_test_case(
            R"({"query_type":"update_all","set":{"height":180},"expected":{"remaining":1,"matching":1}})"
    );
    // A blob has no WHERE comparison, so the rows a write produced could not be counted (#853, #863).
    constexpr auto kSetBlob = parse_test_case(
            R"({"query_type":"update_all","set":{"avatar":"DEADBEEF"},"expected":{"remaining":1,"matching":1}})"
    );
    static_assert(!SetFits<kSetMisfit> && !SetFits<kSetNullNotNull> && !SetFits<kSetFraction>);
    static_assert(!SetFits<kSetUnknownMember> && !SetFits<kSetBlob>);

    // The prototype holds the written values, in each member's type.
    TEST(SpecLimitsWrites, PrototypeHoldsTheSetValues) {
        const Person p = storm::test::set_prototype<kUpdateWhere, Person>();
        EXPECT_EQ(p.age, 77);
        EXPECT_EQ(p.name, "x");
        EXPECT_EQ(p.salary, 0.0); // an unlisted member keeps its default
        const Person n = storm::test::set_prototype<kSetNull, Person>();
        EXPECT_FALSE(n.score.has_value());
        ASSERT_TRUE(n.nickname.has_value());
        EXPECT_EQ(*n.nickname, "n");
    }

} // namespace
