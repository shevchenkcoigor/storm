#pragma once

// AggregateTest fixture shared across test_aggregate*.cpp split files.
// Include AFTER `import storm;` — it pulls the models and helpers it needs itself.

#include "../../shared/models/message.h"
#include "../../shared/models/people_25.h"
#include "../../shared/models/person.h"
#include "../test_fixture.h"
#include "../test_seed_helpers.h"
using storm::QuerySet;

template <typename ConnType> class AggregateTest : public StormTestFixture<Person, ConnType, Message> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        qs = std::make_unique<QuerySet<Person, ConnType>>();
        msg_qs = std::make_unique<QuerySet<Message, ConnType>>();
    }

    auto TearDown() -> void override {
        qs = nullptr;
        msg_qs = nullptr;
        StormTestFixture<Person, ConnType, Message>::TearDown();
    }

    auto insert_test_data() -> void {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
            std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end()))));
    }

    auto insert_full_chain_data() -> void {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(std::vector<Person>{
            {.name = "Alice", .age = 25},
            {.name = "Bob", .age = 35},
            {.name = "Charlie", .age = 45},
            {.name = "Dave", .age = 30},
        })));
        ASSERT_TRUE((storm::test::batch_insert<Message, ConnType>(std::vector<Message>{
            {.content = "A1", .value = 10, .sender = {.id = 1}},
            {.content = "A2", .value = 20, .sender = {.id = 1}},
            {.content = "A3", .value = 30, .sender = {.id = 1}},
            {.content = "B1", .value = 50, .sender = {.id = 2}},
            {.content = "B2", .value = 70, .sender = {.id = 2}},
            {.content = "C1", .value = 5, .sender = {.id = 3}},
            {.content = "C2", .value = 15, .sender = {.id = 3}},
            {.content = "C3", .value = 25, .sender = {.id = 3}},
            {.content = "C4", .value = 35, .sender = {.id = 3}},
            {.content = "D1", .value = 100, .sender = {.id = 4}},
        })));
    }

    auto insert_join_test_data() -> void {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
            std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end()))));

        QuerySet<Person, ConnType> pqs;
        auto people_result = pqs.template order_by<fields::Person.name>().select().execute();
        ASSERT_TRUE(people_result.has_value()) << people_result.error().message();
        ASSERT_EQ(people_result.value().size(), 25u);
        std::array<int, 3> sender_ids{};
        for (const auto &p : people_result.value()) {
            if (p.name == "Alice")
                sender_ids[0] = p.id;
            else if (p.name == "Bob")
                sender_ids[1] = p.id;
            else if (p.name == "Charlie")
                sender_ids[2] = p.id;
        }
        std::vector<Message> const messages = {
            {.content = "Hello", .value = 10, .sender = {.id = sender_ids[0]}},
            {.content = "World", .value = 20, .sender = {.id = sender_ids[0]}},
            {.content = "Hi", .value = 30, .sender = {.id = sender_ids[1]}},
            {.content = "There", .value = 40, .sender = {.id = sender_ids[1]}},
            {.content = "Foo", .value = 50, .sender = {.id = sender_ids[1]}},
            {.content = "Bar", .value = 60, .sender = {.id = sender_ids[2]}},
        };
        ASSERT_TRUE((storm::test::batch_insert<Message, ConnType>(messages)));
    }

    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes)
    std::unique_ptr<QuerySet<Person, ConnType>> qs;      // NOSONAR cpp:S3656
    std::unique_ptr<QuerySet<Message, ConnType>> msg_qs; // NOSONAR cpp:S3656
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes)
};
