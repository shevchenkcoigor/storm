#pragma once

/**
 * @file test_unified_yaml_body.h
 * @brief Shared fixture body for the YAML-driven unified test corpus.
 *
 * Issue #561: the corpus is split across one TU per category so its ~190 ms/case
 * of template instantiation compiles in parallel (53 s -> 24 s wall). Everything
 * the category TUs share lives here; each TU supplies only its corpus slice.
 *
 * A TU uses it in four lines:
 *
 *     #define STORM_UNIFIED_CASES_FILE "test_cases/unified_cases_select.json"
 *     #include "test_parser.hpp"
 *     #include "test_unified_yaml_body.h"
 *     STORM_REGISTER_UNIFIED_YAML_SUITE(Select, "UnifiedYamlSelectTest")
 *
 * The tag argument is load-bearing, not cosmetic. YamlTestInstance is partial-
 * specialized on the fixture type, and each TU's specialization reads its own
 * corpus; without a distinct fixture type per TU those specializations would be
 * the same entity with different definitions -- an ODR violation the linker may
 * resolve either way, silently running one slice's cases against another's
 * expectations. The tag makes each TU's fixture a distinct type.
 *
 * Include AFTER `import storm;`, the runner headers and
 * test_parser.hpp, all of which it uses.
 */

#include <gtest/gtest.h>

#include "../shared/models/message.h"
#include "../shared/models/people_25.h"
#include "../shared/models/person.h"
#include "../shared/models/simple_record.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::db::DefaultConnection;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::SelectStatement;

namespace storm::test {

// YAML `model:` name -> model type (#834). Names are explicit rather than
// derived from identifier_of: the corpus spells SimpleRecord as "simple".
struct YamlModel {
    std::string_view name;
    std::meta::info type;
};
inline constexpr std::array YAML_MODELS{
    YamlModel{.name = "person", .type = ^^Person},
    YamlModel{.name = "message", .type = ^^Message},
    YamlModel{.name = "simple", .type = ^^SimpleRecord},
};

consteval auto yaml_model_index(std::string_view name) -> std::size_t {
    for (std::size_t i = 0; i < YAML_MODELS.size(); ++i)
        if (YAML_MODELS[i].name == name)
            return i;
    return YAML_MODELS.size();
}

// A case naming a model missing from YAML_MODELS fails here, at compile time,
// instead of silently running nothing.
template <const auto &Tc>
concept KnownYamlModel = yaml_model_index(Tc.bench.model.view()) < YAML_MODELS.size();

template <const auto &Tc>
    requires KnownYamlModel<Tc>
using YamlModelOf = typename[:YAML_MODELS[yaml_model_index(Tc.bench.model.view())].type:];

// YAML `query_type` -> runner (#837). An entry matches the exact name, or any name
// starting with it when is_prefix (group_<func>). A query_type missing from the
// table fails KnownYamlQueryType at compile time instead of running nothing.
struct YamlRunner {
    std::string_view query_type;
    bool is_prefix;
    std::meta::info runner;
};
inline constexpr std::array YAML_RUNNERS{
    YamlRunner{.query_type = "select", .is_prefix = false, .runner = ^^SelectRunner},
    YamlRunner{.query_type = "first", .is_prefix = false, .runner = ^^SelectRunner},
    YamlRunner{.query_type = "one", .is_prefix = false, .runner = ^^SelectRunner},
    YamlRunner{.query_type = "count", .is_prefix = false, .runner = ^^AggregateRunner},
    YamlRunner{.query_type = "count_field", .is_prefix = false, .runner = ^^AggregateRunner},
    YamlRunner{.query_type = "count_distinct", .is_prefix = false, .runner = ^^AggregateRunner},
    YamlRunner{.query_type = "sum", .is_prefix = false, .runner = ^^AggregateRunner},
    YamlRunner{.query_type = "avg", .is_prefix = false, .runner = ^^AggregateRunner},
    YamlRunner{.query_type = "min", .is_prefix = false, .runner = ^^AggregateRunner},
    YamlRunner{.query_type = "max", .is_prefix = false, .runner = ^^AggregateRunner},
    YamlRunner{.query_type = "chain", .is_prefix = false, .runner = ^^ChainAggRunner},
    YamlRunner{.query_type = "distinct", .is_prefix = false, .runner = ^^DistinctRunner},
    YamlRunner{.query_type = "group_", .is_prefix = true, .runner = ^^GroupByRunner},
    YamlRunner{.query_type = "insert_one", .is_prefix = false, .runner = ^^InsertRunner},
    YamlRunner{.query_type = "insert_batch", .is_prefix = false, .runner = ^^InsertRunner},
    YamlRunner{.query_type = "update_batch", .is_prefix = false, .runner = ^^UpdateRunner},
    YamlRunner{.query_type = "erase_all", .is_prefix = false, .runner = ^^EraseRunner},
    YamlRunner{.query_type = "erase_batch", .is_prefix = false, .runner = ^^EraseRunner},
};

consteval auto yaml_runner_index(std::string_view query_type) -> std::size_t {
    for (std::size_t i = 0; i < YAML_RUNNERS.size(); ++i)
        if (YAML_RUNNERS[i].is_prefix ? query_type.starts_with(YAML_RUNNERS[i].query_type)
                                      : query_type == YAML_RUNNERS[i].query_type)
            return i;
    return YAML_RUNNERS.size();
}

template <const auto &Tc>
concept KnownYamlQueryType = yaml_runner_index(Tc.query_type.view()) < YAML_RUNNERS.size();

// Per-category fixture; Tag distinguishes the TUs -- see the ODR note above.
template <typename Tag, typename ConnType>
class UnifiedYamlFixture : public StormTestFixture<Person, ConnType, SimpleRecord, Message> {};

/**
 * One test case's body. @p Tc is the case (an NTTP reference into the TU's
 * corpus), so every query below is built at compile time -- which is the
 * property these tests exist to verify.
 */
template <const auto &Tc, typename Tag, typename ConnType>
class UnifiedYamlBody : public UnifiedYamlFixture<Tag, ConnType> {
  protected:
    static constexpr auto &tc = Tc;

    auto reset_tables() -> void {
        std::shared_ptr<ConnType> conn = // NOSONAR(S1659)
            DefaultConnection<ConnType>::get();
        ASSERT_NE(conn, nullptr);
        storm::test::rollback_test_txn<ConnType>(conn);
        ASSERT_TRUE((storm::test::ensure_tables<ConnType, Person, SimpleRecord, Message>(conn)));
        ASSERT_TRUE((EraseStatement<Person, ConnType>{}).query_all().execute().has_value());
        ASSERT_TRUE((EraseStatement<SimpleRecord, ConnType>{}).query_all().execute().has_value());
        ASSERT_TRUE((EraseStatement<Message, ConnType>{}).query_all().execute().has_value());
    }

    auto seed_main_msg() -> void {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
            std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end()))));
        auto people_result =
            (SelectStatement<Person, ConnType>{}).all().template order_by<fields::Person.name>().execute();
        ASSERT_TRUE(people_result.has_value()) << people_result.error().message();
        ASSERT_EQ(people_result.value().size(), 25U);
        std::array<int, 4> sender_ids{};
        for (const auto &p : people_result.value()) {
            if (p.name == "Alice") {
                sender_ids[0] = p.id;
            } else if (p.name == "Bob") {
                sender_ids[1] = p.id;
            } else if (p.name == "Charlie") {
                sender_ids[2] = p.id;
            } else if (p.name == "Diana") {
                sender_ids[3] = p.id;
            }
        }
        const std::vector<Message> msgs = {
            {.content = "Hello", .value = 10, .sender = {.id = sender_ids[0]}},
            {.content = "World", .value = 20, .sender = {.id = sender_ids[0]}},
            {.content = "Hi there", .value = 30, .sender = {.id = sender_ids[0]}},
            {.content = "Goodbye", .value = 40, .sender = {.id = sender_ids[1]}},
            {.content = "Testing", .value = 50, .sender = {.id = sender_ids[1]}},
            {.content = "Greetings", .value = 60, .sender = {.id = sender_ids[2]}},
            {.content = "Reply", .value = 70, .sender = {.id = sender_ids[2]}},
            {.content = "Quick note", .value = 80, .sender = {.id = sender_ids[3]}},
        };
        ASSERT_TRUE((storm::test::batch_insert<Message, ConnType>(msgs)));
    }

    auto seed_dataset() -> void {
        if constexpr (tc.dataset == "main") {
            ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
                std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end()))));
        } else if constexpr (tc.dataset == "main_msg") {
            seed_main_msg();
        } else if constexpr (tc.bench.dataset_size > 0 && tc.dataset.empty()) {
            std::vector<Person> seed;
            seed.reserve(static_cast<std::size_t>(tc.bench.dataset_size));
            for (int i = 1; i <= tc.bench.dataset_size; ++i) {
                seed.emplace_back(Person{.id = 0, .name = storm::test::numbered("P", i), .age = 20 + i});
            }
            ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(seed)));
        }
    }

    // The runner for tc.query_type, spliced from YAML_RUNNERS; an unknown
    // query_type fails KnownYamlQueryType at the call.
    auto run_case() -> void
        requires KnownYamlQueryType<tc>
    {
        constexpr auto runner = YAML_RUNNERS[yaml_runner_index(tc.query_type.view())].runner;
        // Parenthesised so `template` is not read as an explicit-instantiation declaration.
        (template[:runner:]<YamlModelOf<tc>, ConnType>::template run<tc>());
    }

  public:
    void TestBody() override {
        reset_tables();
        seed_dataset();
        run_case();
    }
};

} // namespace storm::test

/**
 * Defines one category's corpus, fixture alias, YamlTestInstance specialization
 * and static registration. @p Tag names the category (a C++ identifier, used
 * only to make the fixture type distinct per TU -- see the ODR note above);
 * @p SuiteName is the GTest suite name. The corpus is whichever slice
 * STORM_UNIFIED_CASES_FILE selected before test_parser.hpp was included.
 */
#define STORM_REGISTER_UNIFIED_YAML_SUITE(Tag, SuiteName)                                                              \
    /* Anonymous namespace, so the tag is a DISTINCT type per TU no matter what                                        \
       identifier is passed. At namespace storm::test scope it had external linkage,                                   \
       and two TUs passing the same Tag would have merged back into one type --                                        \
       silently restoring the very bug the tag exists to prevent. This makes a                                         \
       duplicate Tag harmless instead of catastrophic. */                                                              \
    namespace {                                                                                                        \
    struct Tag##CategoryTag {};                                                                                        \
    }                                                                                                                  \
    /* Internal linkage, NOT inline: each TU parses a different corpus into this                                       \
       name. `inline constexpr` would make the four definitions one entity and                                         \
       the linker would pick a single slice for every suite -- the failure this                                        \
       replaced, which registered all 4 suites under the last one's cases. */                                          \
    constexpr auto UNIFIED_TESTS = storm::test::load_unified_tests();                                                  \
                                                                                                                       \
    template <typename ConnType> using UnifiedYamlTest = storm::test::UnifiedYamlFixture<Tag##CategoryTag, ConnType>;  \
                                                                                                                       \
    template <std::size_t I, typename ConnType>                                                                        \
    class storm::test::YamlTestInstance<I, ::UnifiedYamlTest<ConnType>, ConnType>                                      \
        : public storm::test::UnifiedYamlBody<UNIFIED_TESTS[I], Tag##CategoryTag, ConnType> {};                        \
                                                                                                                       \
    namespace {                                                                                                        \
    [[maybe_unused]] const bool unified_yaml_registered_ =                                                             \
        storm::test::register_both_backends<UNIFIED_TESTS, ::UnifiedYamlTest>(SuiteName);                              \
    }
