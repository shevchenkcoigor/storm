#include <gtest/gtest.h>
#include "test_db_helpers.h"
#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

using storm::QuerySet;
using storm::db::DefaultConnection;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_join_data.h"
#include "test_seed_helpers.h"

// Test fixture for DISTINCT operations — templated on database backend
template <typename ConnType> class DistinctTest : public StormTestFixture<Person, ConnType, Message> {};

TYPED_TEST_SUITE(DistinctTest, DatabaseTypes);

namespace {

    using NameAge = std::tuple<std::string, int>;
    using AgeName = std::tuple<int, std::string>;

    // Batch-inserts the rows. Person's key is DB-generated, so any id given here is ignored;
    // each test's table is new, so the generated ids are 1..N.
    template <typename ConnType> auto seed(const std::vector<Person>& people) -> void {
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(people)));
    }

    // A DISTINCT result in the order the database returned it — for queries with ORDER BY,
    // or results of at most one row.
    template <typename Row> auto rows(const plf::hive<Row>& result) -> std::vector<Row> {
        return {result.begin(), result.end()};
    }

    // A DISTINCT result in sorted order — for queries without ORDER BY, whose row order is unspecified.
    template <typename Row> auto sorted_rows(const plf::hive<Row>& result) -> std::vector<Row> {
        auto sorted = rows(result);
        std::ranges::sort(sorted);
        return sorted;
    }

} // namespace

// ============================================================================
// Single-field DISTINCT
// ============================================================================

TYPED_TEST(DistinctTest, DistinctAgeFieldWithDuplicates) {
    seed<TypeParam>({{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 25}, {5, "Eve", 35}});

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<int>{25, 30, 35}));
}

TYPED_TEST(DistinctTest, DistinctNameFieldAllUnique) {
    seed<TypeParam>({{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 35}});

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::string>{"Alice", "Bob", "Charlie"}));
}

// Every row shares one age, so DISTINCT collapses the table to a single value.
TYPED_TEST(DistinctTest, DistinctSingleFieldAllDuplicates) {
    std::vector<Person> people;
    for (int i = 1; i <= 10; ++i) {
        people.emplace_back(i, std::format("Person{}", i), 42);
    }
    seed<TypeParam>(people);

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<int>{42}));
}

// distinct() with no selector projects the primary key.
TYPED_TEST(DistinctTest, DistinctDefaultsToPrimaryKey) {
    seed<TypeParam>({{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 35}});

    auto result = QuerySet<Person, TypeParam>().distinct().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<int>{1, 2, 3}));
}

TYPED_TEST(DistinctTest, DistinctWithSingleRow) {
    seed<TypeParam>({{.id = 0, .name = "Alice", .age = 30}});

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::string>{"Alice"}));
}

// 1000 rows with unique names but only 10 ages (100 duplicates of each).
TYPED_TEST(DistinctTest, DistinctWithLargeDataset) {
    std::vector<Person> people;
    for (int i = 1; i <= 1000; ++i) {
        people.emplace_back(i, std::format("Person{}", i), 20 + ((i - 1) % 10));
    }
    seed<TypeParam>(people);

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(result->size(), 10);
}

// ============================================================================
// Multi-field DISTINCT
// ============================================================================

// Names are unique, so every (name, age) pair survives even though ages repeat.
TYPED_TEST(DistinctTest, DistinctTwoFieldsNameAndAge) {
    seed<TypeParam>(
            {{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 25}, {5, "Eve", 35}, {6, "Frank", 30}}
    );

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.name, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(
            sorted_rows(*result),
            (std::vector<NameAge>{
                    {"Alice", 30}, {"Bob", 25}, {"Charlie", 30}, {"Dave", 25}, {"Eve", 35}, {"Frank", 30}
            })
    );
}

// The tuple follows the selector order, not the model's field order.
TYPED_TEST(DistinctTest, DistinctTwoFieldsDifferentOrder) {
    seed<TypeParam>({{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}, {4, "Dave", 25}});

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.age, fields::Person.name>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<AgeName>{{25, "Bob"}, {25, "Dave"}, {30, "Alice"}, {30, "Charlie"}}));
}

TYPED_TEST(DistinctTest, DistinctTwoFieldsWithSingleRow) {
    seed<TypeParam>({{.id = 0, .name = "Alice", .age = 30}});

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.name, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<NameAge>{{"Alice", 30}}));
}

// ============================================================================
// The same field named more than once — redundant, but valid SQL
// ============================================================================

TYPED_TEST(DistinctTest, DuplicateFieldSpecification) {
    seed<TypeParam>({{1, "Alice", 30}, {2, "Bob", 25}, {3, "Charlie", 30}});

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.age, fields::Person.age>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::tuple<int, int>>{{25, 25}, {30, 30}}));
}

TYPED_TEST(DistinctTest, TriplicateFieldSpecification) {
    seed<TypeParam>({{1, "Alice", 30}, {2, "Bob", 25}});

    auto result = QuerySet<Person, TypeParam>()
                          .template distinct<fields::Person.age, fields::Person.age, fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::tuple<int, int, int>>{{25, 25, 25}, {30, 30, 30}}));
}

TYPED_TEST(DistinctTest, MixedDuplicateFields) {
    seed<TypeParam>({{1, "Alice", 30}, {2, "Bob", 30}, {3, "Charlie", 25}});

    auto result = QuerySet<Person, TypeParam>()
                          .template distinct<fields::Person.age, fields::Person.name, fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(
            sorted_rows(*result),
            (std::vector<std::tuple<int, std::string, int>>{{25, "Charlie", 25}, {30, "Alice", 30}, {30, "Bob", 30}})
    );
}

// ============================================================================
// Return types — one element per selector, in selector order
// ============================================================================

TYPED_TEST(DistinctTest, ReturnTypes) {
    QuerySet<Person, TypeParam> queryset;

    auto names = queryset.template distinct<fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(names.value()), plf::hive<std::string>&>);

    auto ages = queryset.template distinct<fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(ages.value()), plf::hive<int>&>);

    auto ids = queryset.distinct().execute();
    static_assert(std::is_same_v<decltype(ids.value()), plf::hive<int>&>);

    auto name_age = queryset.template distinct<fields::Person.name, fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(name_age.value()), plf::hive<NameAge>&>);

    auto age_name = queryset.template distinct<fields::Person.age, fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(age_name.value()), plf::hive<AgeName>&>);

    auto name_twice = queryset.template distinct<fields::Person.name, fields::Person.name>().execute();
    static_assert(std::is_same_v<decltype(name_twice.value()), plf::hive<std::tuple<std::string, std::string>>&>);

    auto age_thrice =
            queryset.template distinct<fields::Person.age, fields::Person.age, fields::Person.age>().execute();
    static_assert(std::is_same_v<decltype(age_thrice.value()), plf::hive<std::tuple<int, int, int>>&>);
}

// ============================================================================
// DISTINCT + WHERE
// ============================================================================

TYPED_TEST(DistinctTest, DistinctWithWhereSingleField) {
    seed<TypeParam>({
            {0, "Alice", 25},
            {0, "Bob", 25},
            {0, "Charlie", 30},
            {0, "Dave", 30},
            {0, "Eve", 20}, // filtered out by WHERE
            {0, "Frank", 20},
            {0, "Grace", 35},
    });

    auto result = QuerySet<Person, TypeParam>()
                          .where(fields::Person.age > 22)
                          .template distinct<fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<int>{25, 30, 35}));
}

TYPED_TEST(DistinctTest, DistinctWithWhereMultipleFields) {
    seed<TypeParam>({
            {0, "Alice", 25},
            {0, "Bob", 25},
            {0, "Charlie", 30},
            {0, "Dave", 20}, // filtered out by WHERE
            {0, "Eve", 15},  // filtered out by WHERE
            {0, "Frank", 35},
    });

    auto result = QuerySet<Person, TypeParam>()
                          .where(fields::Person.age > 22)
                          .template distinct<fields::Person.name, fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<NameAge>{{"Alice", 25}, {"Bob", 25}, {"Charlie", 30}, {"Frank", 35}}));
}

TYPED_TEST(DistinctTest, DistinctWithWhereNoResults) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Bob", 30}});

    auto result = QuerySet<Person, TypeParam>()
                          .where(fields::Person.age > 100)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_TRUE(result->empty());
}

TYPED_TEST(DistinctTest, DistinctWithComplexWhere) {
    seed<TypeParam>({
            {0, "Alice", 25},
            {0, "Bob", 25},
            {0, "Charlie", 30},
            {0, "Dave", 30},
            {0, "Eve", 20}, // below the range
            {0, "Frank", 35},
            {0, "Grace", 40}, // above the range
    });

    auto result = QuerySet<Person, TypeParam>()
                          .where(fields::Person.age.between(25, 35))
                          .template distinct<fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<int>{25, 30, 35}));
}

TYPED_TEST(DistinctTest, DistinctDefaultPKWithWhere) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Bob", 30}, {0, "Charlie", 20}});

    auto result = QuerySet<Person, TypeParam>().where(fields::Person.age > 22).distinct().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(result->size(), 2); // Alice and Bob
}

// Chained where() calls combine with AND.
TYPED_TEST(DistinctTest, MultipleWhereClausesWithDistinct) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Bob", 30}, {0, "Charlie", 20}, {0, "David", 35}});

    auto result = QuerySet<Person, TypeParam>()
                          .where(fields::Person.age > 22)
                          .where(fields::Person.age < 32)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::string>{"Alice", "Bob"}));
}

// ============================================================================
// DISTINCT + JOIN (populate_join_test_data: 3 persons, 5 messages)
// ============================================================================

TYPED_TEST(DistinctTest, DistinctOnBaseTableWithoutJoin) {
    storm::test::populate_join_test_data<TypeParam>();

    auto result = QuerySet<Message, TypeParam>().template distinct<fields::Message.content>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::string>{"Goodbye", "Hello", "Hi there", "Test", "World"}));
}

TYPED_TEST(DistinctTest, DistinctWithJoinSingleField) {
    storm::test::populate_join_test_data<TypeParam>();

    auto result = QuerySet<Message, TypeParam>()
                          .template join<fields::Message.sender>()
                          .template distinct<fields::Message.content>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::string>{"Goodbye", "Hello", "Hi there", "Test", "World"}));
}

// where() before join() and join() before where() return the same rows.
TYPED_TEST(DistinctTest, WhereAndJoinChainInEitherOrder) {
    storm::test::populate_join_test_data<TypeParam>();
    const std::vector<std::string> contents_with_e = {"Goodbye", "Hello", "Hi there", "Test"};

    auto where_first = QuerySet<Message, TypeParam>()
                               .where(fields::Message.content.like("%e%"))
                               .template join<fields::Message.sender>()
                               .template distinct<fields::Message.content>()
                               .execute();
    ASSERT_TRUE(where_first.has_value()) << "WHERE -> JOIN -> DISTINCT failed: " << where_first.error().message();
    EXPECT_EQ(sorted_rows(*where_first), contents_with_e);

    auto join_first = QuerySet<Message, TypeParam>()
                              .template join<fields::Message.sender>()
                              .where(fields::Message.content.like("%e%"))
                              .template distinct<fields::Message.content>()
                              .execute();
    ASSERT_TRUE(join_first.has_value()) << "JOIN -> WHERE -> DISTINCT failed: " << join_first.error().message();
    EXPECT_EQ(sorted_rows(*join_first), contents_with_e);
}

// DISTINCT can project only base-table columns: a column of the JOINED table (here
// Person.name through Message.sender) has no ORM spelling yet, so the query needs raw SQL.
// Replace this with the ORM query, and delete the workaround, when #804 lands.
TYPED_TEST(DistinctTest, RawSQLWorkaround) {
    storm::test::populate_join_test_data<TypeParam>();

    const auto& conn        = DefaultConnection<TypeParam>::get();
    auto        stmt_result = conn->prepare(
            "SELECT DISTINCT Person.name FROM Message INNER JOIN Person ON Person.id = Message.sender_id"
    );
    ASSERT_TRUE(stmt_result.has_value());

    auto                     stmt = std::move(stmt_result.value());
    std::vector<std::string> sender_names;
    while (true) {
        int const step = stmt.step_raw();
        if (step == decltype(stmt)::NO_MORE_ROWS) {
            break;
        }
        ASSERT_EQ(step, decltype(stmt)::ROW_AVAILABLE) << "Query failed";
        sender_names.emplace_back(reinterpret_cast<const char*>(stmt.extract_text_ptr(0))); // NOSONAR
    }

    std::ranges::sort(sender_names);
    EXPECT_EQ(sender_names, (std::vector<std::string>{"Alice", "Bob", "Charlie"}));
}

// ============================================================================
// DISTINCT + ORDER BY / LIMIT / OFFSET
// ============================================================================

TYPED_TEST(DistinctTest, DistinctWithOrderByAsc) {
    seed<TypeParam>({{0, "Charlie", 30}, {0, "Alice", 25}, {0, "Bob", 35}, {0, "Dave", 25}});

    auto result = QuerySet<Person, TypeParam>()
                          .template order_by<fields::Person.age>()
                          .template distinct<fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<int>{25, 30, 35}));
}

TYPED_TEST(DistinctTest, DistinctWithOrderByDesc) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Charlie", 30}, {0, "Bob", 35}});

    auto result = QuerySet<Person, TypeParam>()
                          .template order_by<fields::Person.name, false>()
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::string>{"Charlie", "Bob", "Alice"}));
}

TYPED_TEST(DistinctTest, DistinctMultiFieldWithOrderBy) {
    seed<TypeParam>({{0, "Charlie", 30}, {0, "Alice", 25}, {0, "Bob", 25}, {0, "Dave", 30}});

    auto result = QuerySet<Person, TypeParam>()
                          .template order_by<fields::Person.name>()
                          .template distinct<fields::Person.name, fields::Person.age>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<NameAge>{{"Alice", 25}, {"Bob", 25}, {"Charlie", 30}, {"Dave", 30}}));
}

TYPED_TEST(DistinctTest, DistinctWithOrderByAndLimit) {
    seed<TypeParam>({{0, "Eve", 35}, {0, "Alice", 25}, {0, "Charlie", 30}, {0, "Bob", 28}, {0, "Dave", 40}});

    auto result = QuerySet<Person, TypeParam>()
                          .template order_by<fields::Person.name>()
                          .limit(3)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::string>{"Alice", "Bob", "Charlie"}));
}

TYPED_TEST(DistinctTest, DistinctWithOrderByDescAndLimit) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Bob", 28}, {0, "Charlie", 30}, {0, "Dave", 40}, {0, "Eve", 35}});

    auto result = QuerySet<Person, TypeParam>()
                          .template order_by<fields::Person.name, false>()
                          .limit(2)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::string>{"Eve", "Dave"}));
}

TYPED_TEST(DistinctTest, DistinctWithOrderByLimitOffset) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Bob", 28}, {0, "Charlie", 30}, {0, "Dave", 40}, {0, "Eve", 35}});

    auto result = QuerySet<Person, TypeParam>()
                          .template order_by<fields::Person.name>()
                          .limit(2)
                          .offset(2)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::string>{"Charlie", "Dave"})); // skipped Alice, Bob
}

TYPED_TEST(DistinctTest, DistinctWithWhereOrderByLimit) {
    seed<TypeParam>({
            {0, "Alice", 25}, // filtered out by WHERE
            {0, "Bob", 35},
            {0, "Charlie", 30},
            {0, "Dave", 40},
            {0, "Eve", 20}, // filtered out by WHERE
    });

    auto result = QuerySet<Person, TypeParam>()
                          .where(fields::Person.age > 25)
                          .template order_by<fields::Person.name>()
                          .limit(2)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::string>{"Bob", "Charlie"}));
}

// ============================================================================
// DISTINCT on std::optional fields — NULL is one distinct value
// ============================================================================

TYPED_TEST(DistinctTest, DistinctOptionalIntFieldWithNulls) {
    seed<TypeParam>({
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = 30, .nickname = std::nullopt},
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = std::nullopt},
            {.id = 0, .name = "Eve", .score = 25, .nickname = "Evie"},
            {.id = 0, .name = "Frank", .score = std::nullopt, .nickname = "Frankie"},
    });

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // The three NULL scores collapse to one.
    EXPECT_EQ(sorted_rows(*result), (std::vector<std::optional<int>>{std::nullopt, 25, 30}));
}

TYPED_TEST(DistinctTest, DistinctOptionalStringFieldWithNulls) {
    seed<TypeParam>({
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = 30, .nickname = std::nullopt},
            {.id = 0, .name = "Charlie", .score = 35, .nickname = "Ali"},
            {.id = 0, .name = "Dave", .score = 40, .nickname = std::nullopt},
            {.id = 0, .name = "Eve", .score = 45, .nickname = "Evie"},
            {.id = 0, .name = "Frank", .score = 50, .nickname = "Ali"},
    });

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.nickname>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::optional<std::string>>{std::nullopt, "Ali", "Evie"}));
}

TYPED_TEST(DistinctTest, DistinctOptionalFieldAllNulls) {
    seed<TypeParam>({
            {.id = 0, .name = "Alice", .score = std::nullopt, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = std::nullopt, .nickname = "Chuck"},
    });

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::optional<int>>{std::nullopt}));
}

TYPED_TEST(DistinctTest, DistinctOptionalFieldNoNulls) {
    seed<TypeParam>({
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = 30, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = 25, .nickname = "Chuck"},
    });

    auto result = QuerySet<Person, TypeParam>().template distinct<fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::optional<int>>{25, 30}));
}

TYPED_TEST(DistinctTest, DistinctMultiFieldWithOptional) {
    seed<TypeParam>({
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = 25, .nickname = "Chuck"},
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = "Davey"},
    });

    auto result =
            QuerySet<Person, TypeParam>().template distinct<fields::Person.name, fields::Person.score>().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(
            sorted_rows(*result),
            (std::vector<std::tuple<std::string, std::optional<int>>>{
                    {"Alice", 25}, {"Bob", std::nullopt}, {"Charlie", 25}, {"Dave", std::nullopt}
            })
    );
}

// A comparison against NULL is never true, so rows with a NULL score are filtered out.
TYPED_TEST(DistinctTest, DistinctOptionalWithWhereOnOptional) {
    seed<TypeParam>({
            {.id = 0, .name = "Alice", .score = 25, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = std::nullopt, .nickname = "Bobby"},
            {.id = 0, .name = "Charlie", .score = 30, .nickname = "Chuck"},
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = "Davey"},
    });

    auto result = QuerySet<Person, TypeParam>()
                          .where(fields::Person.score > 20)
                          .template distinct<fields::Person.name>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(sorted_rows(*result), (std::vector<std::string>{"Alice", "Charlie"}));
}

// NULL sorts first under ASC on both backends (PostgreSQL via the emitted NULLS FIRST).
TYPED_TEST(DistinctTest, DistinctOptionalWithOrderBy) {
    seed<TypeParam>({
            {.id = 0, .name = "Charlie", .score = 30, .nickname = "Chuck"},
            {.id = 0, .name = "Alice", .score = std::nullopt, .nickname = "Ali"},
            {.id = 0, .name = "Bob", .score = 25, .nickname = "Bobby"},
            {.id = 0, .name = "Dave", .score = std::nullopt, .nickname = "Davey"},
    });

    auto result = QuerySet<Person, TypeParam>()
                          .template order_by<fields::Person.score>()
                          .template distinct<fields::Person.score>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    EXPECT_EQ(rows(*result), (std::vector<std::optional<int>>{std::nullopt, 25, 30}));
}

// ============================================================================
// Statement caching
// ============================================================================

// The same DISTINCT query executed repeatedly reuses the cached statement.
TYPED_TEST(DistinctTest, StatementReuseStability) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Bob", 30}, {0, "Charlie", 35}});
    QuerySet<Person, TypeParam> queryset;

    for (int i = 0; i < 5; ++i) {
        auto result = queryset.template distinct<fields::Person.name>().execute();
        ASSERT_TRUE(result.has_value()) << "Iteration " << i << " failed: " << result.error().message();
        EXPECT_EQ(result->size(), 3) << "Iteration " << i << " returned wrong count";
    }
}

// Successive filters on one QuerySet (reset() between): new bind values for the same SQL,
// then a different comparison operator.
TYPED_TEST(DistinctTest, DifferentWhereExpressionsWithCaching) {
    seed<TypeParam>({{0, "Alice", 25}, {0, "Bob", 30}, {0, "Charlie", 35}, {0, "Dave", 40}});
    QuerySet<Person, TypeParam> queryset;

    auto result1 = queryset.where(fields::Person.age > 25).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value().size(), 3); // Bob, Charlie, Dave

    queryset.reset();

    auto result2 = queryset.where(fields::Person.age > 35).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 1); // Dave only

    queryset.reset();

    auto result3 = queryset.where(fields::Person.age < 30).template distinct<fields::Person.name>().execute();
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3.value().size(), 1); // Alice only
}

// ============================================================================
// SQL Verification Tests — prevent SQL generation regressions
// ============================================================================

// Helper: returns "ASC" or "ASC NULLS FIRST" depending on backend
template <typename ConnType> constexpr auto order_asc() -> std::string_view {
    if constexpr (storm::test::is_postgresql<ConnType>()) {
        return "ASC NULLS FIRST";
    } else {
        return "ASC";
    }
}

// Test: DISTINCT simple (no JOIN) generates correct SQL
TYPED_TEST(DistinctTest, SqlVerifyDistinctSimple) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name FROM Person");
}

// Test: DISTINCT on integer field
TYPED_TEST(DistinctTest, SqlVerifyDistinctIntField) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template distinct<fields::Person.age>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT age FROM Person");
}

// Test: DISTINCT default PK
TYPED_TEST(DistinctTest, SqlVerifyDistinctDefaultPK) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.distinct().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT id FROM Person");
}

// Test: DISTINCT multi-field
TYPED_TEST(DistinctTest, SqlVerifyDistinctMultiField) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template distinct<fields::Person.name, fields::Person.age>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name, age FROM Person");
}

// Test: DISTINCT + WHERE generates correct SQL
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithWhere) {
    QuerySet<Person, TypeParam> queryset;
    auto sql = queryset.where(fields::Person.age > 30).template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name FROM Person WHERE age > ?");
}

// Test: DISTINCT + JOIN generates projected fields with t1 prefix (the bug fix)
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithJoin) {
    QuerySet<Message, TypeParam> msg_qs;
    auto sql = msg_qs.template join<fields::Message.sender>().template distinct<fields::Message.content>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id");
}

// Test: DISTINCT + JOIN + WHERE
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithJoinAndWhere) {
    QuerySet<Message, TypeParam> msg_qs;
    auto                         sql = msg_qs.template join<fields::Message.sender>()
                       .where(fields::Message.content.like("%test%"))
                       .template distinct<fields::Message.content>()
                       .sql();
    EXPECT_EQ(
            sql,
            "SELECT DISTINCT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
            " WHERE t1.content LIKE ?"
    );
}

// Test: DISTINCT + JOIN multi-field with FK field
TYPED_TEST(DistinctTest, SqlVerifyDistinctJoinMultiFieldWithFK) {
    QuerySet<Message, TypeParam> msg_qs;
    auto                         sql = msg_qs.template join<fields::Message.sender>()
                       .template distinct<fields::Message.content, fields::Message.sender>()
                       .sql();
    EXPECT_EQ(
            sql,
            "SELECT DISTINCT t1.content, t1.sender_id"
            " FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
    );
}

// Test: VALUES (no DISTINCT keyword) + JOIN
TYPED_TEST(DistinctTest, SqlVerifyValuesWithJoin) {
    QuerySet<Message, TypeParam> msg_qs;
    auto sql = msg_qs.template join<fields::Message.sender>().template values<fields::Message.content>().sql();
    EXPECT_EQ(sql, "SELECT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id");
}

// Test: VALUES simple (no JOIN)
TYPED_TEST(DistinctTest, SqlVerifyValuesSimple) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.template values<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT name FROM Person");
}

// Test: DISTINCT + ORDER BY
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithOrderBy) {
    QuerySet<Person, TypeParam> queryset;
    auto sql = queryset.template order_by<fields::Person.name>().template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, std::format("SELECT DISTINCT name FROM Person ORDER BY name {}", order_asc<TypeParam>()));
}

// Test: DISTINCT + LIMIT + OFFSET
TYPED_TEST(DistinctTest, SqlVerifyDistinctWithLimitOffset) {
    QuerySet<Person, TypeParam> queryset;
    auto                        sql = queryset.limit(10).offset(5).template distinct<fields::Person.name>().sql();
    EXPECT_EQ(sql, "SELECT DISTINCT name FROM Person LIMIT 10 OFFSET 5");
}

// Test: DISTINCT + JOIN + WHERE + ORDER BY + LIMIT (full combo)
TYPED_TEST(DistinctTest, SqlVerifyDistinctFullCombo) {
    QuerySet<Message, TypeParam> msg_qs;
    auto                         sql = msg_qs.template join<fields::Message.sender>()
                       .where(fields::Message.value > 10)
                       .template order_by<fields::Message.content>()
                       .limit(5)
                       .template distinct<fields::Message.content>()
                       .sql();
    EXPECT_EQ(
            sql,
            std::format(
                    "SELECT DISTINCT t1.content FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id"
                    " WHERE t1.value > ? ORDER BY t1.content {} LIMIT 5",
                    order_asc<TypeParam>()
            )
    );
}

// NOLINTEND(misc-const-correctness)
