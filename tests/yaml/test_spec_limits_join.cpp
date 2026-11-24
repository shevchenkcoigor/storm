// Issue #869: the JOIN spec through the test entry point (tests/test_parser.hpp) and the shared
// QueryBuilder. A join is built as written or not at all: an unknown join type, `fk` and `fks`
// together, or neither, is a compile error (SpecWellFormed); a one-entry `fks` is that one member.
// And the YAML member resolver (tests/test_yaml_models.h) that the corpus runners hand the builder.

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

#include "shared/models/message.h" // NOSONAR cpp:S954
#include "test_select_runner.h"
#include "test_parser.hpp"

namespace {

    using storm::orm::query_builder::QueryBuilder;

    constexpr auto parse_case(std::string_view json) -> storm::benchmark::BenchmarkTest {
        storm::test::TestCase tc;
        std::size_t           pos = 0;
        storm::test::parse_test_case_into(json, pos, tc);
        return tc.bench;
    }

    template <auto Spec>
    concept Buildable = requires { typename QueryBuilder<Message, Spec>; };

    // The join type is "inner" or "left"; anything else used to build an inner join.
    constexpr auto kInner  = parse_case(R"({"join":{"type":"inner","fk":"sender"}})");
    constexpr auto kLeft   = parse_case(R"({"join":{"type":"left","fk":"sender"}})");
    constexpr auto kOuter  = parse_case(R"({"join":{"type":"outer","fk":"sender"}})");
    constexpr auto kNoType = parse_case(R"({"join":{"fk":"sender"}})");
    static_assert(Buildable<kInner> && Buildable<kLeft>);
    static_assert(!Buildable<kOuter> && !Buildable<kNoType>);

    // Exactly one of `fk` / `fks`: with both, one used to be dropped; with neither, an empty name.
    constexpr auto kBoth    = parse_case(R"({"join":{"type":"inner","fk":"sender","fks":["receiver"]}})");
    constexpr auto kNeither = parse_case(R"({"join":{"type":"inner"}})");
    constexpr auto kOneFks  = parse_case(R"({"join":{"type":"inner","fks":["receiver"]}})");
    constexpr auto kTwoFks  = parse_case(R"({"join":{"type":"left","fks":["sender","receiver"]}})");
    static_assert(!Buildable<kBoth> && !Buildable<kNeither>);
    static_assert(Buildable<kOneFks> && Buildable<kTwoFks>);

    // A one-entry `fks` is that member, not the (empty) `fk`.
    static_assert(kOneFks.join.member_count() == 1 && kOneFks.join.member(0) == "receiver");
    static_assert(kTwoFks.join.member_count() == 2 && kTwoFks.join.member(1) == "receiver");
    static_assert(kInner.join.member_count() == 1 && kInner.join.member(0) == "sender");

    // A case without a join is untouched by the join rules.
    constexpr auto kNoJoin = parse_case(R"({"dataset_size":5})");
    static_assert(Buildable<kNoJoin> && kNoJoin.join.is_well_formed());

    // The YAML member resolver: a member of the case's model, or `<yaml model>.<member>` for a
    // cross-model FK selector; a null info for anything else, which the builder rejects.
    namespace m2m = storm::test::relation_models;
    using storm::test::yaml_join_member;
    static_assert(yaml_join_member<Message>("sender") == ^^Message::sender);
    static_assert(yaml_join_member<m2m::BMember>("clubs") == ^^m2m::BMember::clubs);
    static_assert(yaml_join_member<m2m::BAuthor>("posts") == ^^m2m::BAuthor::posts);
    static_assert(yaml_join_member<m2m::BReporter>("bug.author") == ^^m2m::BBug::author);
    static_assert(yaml_join_member<m2m::BReporter>("bug.reviewer") == ^^m2m::BBug::reviewer);
    static_assert(yaml_join_member<Message>("nope") == std::meta::info{});
    static_assert(yaml_join_member<m2m::BReporter>("nobody.author") == std::meta::info{});
    static_assert(yaml_join_member<m2m::BReporter>("bug.nope") == std::meta::info{});

    // Every relation model is tied to dataset `relations`, the one that creates its tables.
    static_assert(storm::test::YAML_MODELS[storm::test::yaml_model_index("bug")].dataset == "relations");
    static_assert(storm::test::YAML_MODELS[storm::test::yaml_model_index("person")].dataset.empty());
    // ...and a case on such a model without that dataset does not compile (YamlModelDatasetMatches);
    // an unknown model is never a match, so it reaches the KnownYamlModel message instead.
    using storm::test::yaml_model_dataset_matches;
    static_assert(yaml_model_dataset_matches("bug", "relations") && !yaml_model_dataset_matches("bug", ""));
    static_assert(!yaml_model_dataset_matches("bug", "main") && yaml_model_dataset_matches("person", "main"));
    static_assert(yaml_model_dataset_matches("person", "") && !yaml_model_dataset_matches("bugs", "relations"));

    TEST(SpecLimitsJoinTestParser, JoinMembersAreReadFromFkOrFks) {
        EXPECT_EQ(kTwoFks.join.fk_count, 2U);
        EXPECT_EQ(kTwoFks.join.type.view(), "left");
        EXPECT_TRUE(kOneFks.join.fk.empty());
        EXPECT_EQ(kOneFks.join.fks[0].view(), "receiver");
    }

} // namespace
