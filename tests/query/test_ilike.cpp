#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

// #814: Field::ilike() is the portable case-insensitive LIKE. like() stays backend-native — SQLite
// folds ASCII case, PostgreSQL does not — so the two are pinned side by side. ilike() emits
// `lower(col) LIKE lower(?)` on both backends (the expression layer has no dialect to branch on).

import storm;
import std;

using storm::QuerySet;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"

namespace {

    // Same letters, four casings: the data the SQLAlchemy test_ilike scenario uses (one/TwO/ONE/OnE).
    // `nickname` is NULL for ids 1 and 2 so NULL handling is exercised on the same table.
    template <typename ConnType> class IlikeTest : public StormTestFixture<Person, ConnType> {
      public:
        auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
            ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(std::vector<Person>{
                    {.id = 1, .name = "one", .age = 10},
                    {.id = 2, .name = "TwO", .age = 20},
                    {.id = 3, .name = "ONE", .age = 30, .nickname = "n3"},
                    {.id = 4, .name = "OnE", .age = 40, .nickname = "n4"},
            })));
        }

        // Ids of the rows a WHERE returns, sorted, so a test states the expected set directly.
        template <typename Filter> static auto ids_where(Filter&& filter) -> std::vector<int> {
            QuerySet<Person, ConnType> qs;
            auto                       result = qs.where(std::forward<Filter>(filter)).select().execute();
            EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message());
            std::vector<int> ids;
            if (result.has_value()) {
                for (const auto& person : result.value()) {
                    ids.push_back(person.id);
                }
            }
            std::ranges::sort(ids);
            return ids;
        }
    };

} // namespace

TYPED_TEST_SUITE(IlikeTest, DatabaseTypes);

TYPED_TEST(IlikeTest, MatchesEveryCasingOfThePattern) {
    EXPECT_EQ(TestFixture::ids_where(fields::Person.name.ilike("one")), (std::vector<int>{1, 3, 4}));
}

TYPED_TEST(IlikeTest, PatternCasingDoesNotMatter) {
    EXPECT_EQ(TestFixture::ids_where(fields::Person.name.ilike("TWO")), (std::vector<int>{2}));
    EXPECT_EQ(TestFixture::ids_where(fields::Person.name.ilike("tWo")), (std::vector<int>{2}));
}

TYPED_TEST(IlikeTest, NoMatchReturnsNothing) {
    EXPECT_TRUE(TestFixture::ids_where(fields::Person.name.ilike("three")).empty());
}

TYPED_TEST(IlikeTest, PercentWildcardMatchesAcrossCases) {
    EXPECT_EQ(TestFixture::ids_where(fields::Person.name.ilike("%O%")), (std::vector<int>{1, 2, 3, 4}));
    EXPECT_EQ(TestFixture::ids_where(fields::Person.name.ilike("o%")), (std::vector<int>{1, 3, 4}));
}

TYPED_TEST(IlikeTest, UnderscoreWildcardMatchesOneCharacter) {
    EXPECT_EQ(TestFixture::ids_where(fields::Person.name.ilike("O_E")), (std::vector<int>{1, 3, 4}));
    EXPECT_TRUE(TestFixture::ids_where(fields::Person.name.ilike("O_")).empty());
}

// `like` is the backend-native operator and is deliberately left alone: only SQLite folds case.
TYPED_TEST(IlikeTest, LikeKeepsTheBackendDefinedCaseSensitivity) {
    const auto matched = TestFixture::ids_where(fields::Person.name.like("one"));
    if constexpr (std::same_as<TypeParam, storm::db::sqlite::Connection>) {
        EXPECT_EQ(matched, (std::vector<int>{1, 3, 4})) << "SQLite LIKE is case-insensitive for ASCII";
    } else {
        EXPECT_EQ(matched, (std::vector<int>{1})) << "PostgreSQL LIKE is case-sensitive";
    }
}

// SQL three-valued logic: NULL LIKE ... is NULL, so the row is omitted — same as with like().
TYPED_TEST(IlikeTest, NullColumnIsOmitted) {
    EXPECT_EQ(TestFixture::ids_where(fields::Person.nickname.ilike("N%")), (std::vector<int>{3, 4}));
}

TYPED_TEST(IlikeTest, ComposesWithAndOr) {
    EXPECT_EQ(
            TestFixture::ids_where(fields::Person.name.ilike("one") && fields::Person.age > 30), (std::vector<int>{4})
    );
    EXPECT_EQ(
            TestFixture::ids_where(fields::Person.name.ilike("two") || fields::Person.age == 10),
            (std::vector<int>{1, 2})
    );
}

// A std::string operand (not a literal): the pattern is copied into the expression, so a temporary
// cannot dangle by the time the statement binds it.
TYPED_TEST(IlikeTest, AcceptsAStdStringTemporary) {
    EXPECT_EQ(TestFixture::ids_where(fields::Person.name.ilike(std::string{"ONE"})), (std::vector<int>{1, 3, 4}));
}

// An empty pattern matches only the empty string, never every row. The id is read back from the
// insert: Person.id is DB-generated, so a literal id in the inserted object is ignored.
TYPED_TEST(IlikeTest, EmptyPatternMatchesOnlyEmptyStrings) {
    QuerySet<Person, TypeParam> qs;
    auto                        inserted = qs.insert(Person{.name = "", .age = 50}).execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();
    EXPECT_EQ(
            TestFixture::ids_where(fields::Person.name.ilike("")),
            (std::vector<int>{static_cast<int>(inserted.value())})
    );
}

// SQLite's lower() folds ASCII only, which is the documented limit. PostgreSQL's lower() follows the
// database locale, so that backend is deliberately left unpinned.
TYPED_TEST(IlikeTest, NonAsciiCaseFoldingIsBackendDefined) {
    QuerySet<Person, TypeParam> qs;
    auto                        inserted = qs.insert(Person{.name = "\u00C9COLE", .age = 60}).execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();
    if constexpr (std::same_as<TypeParam, storm::db::sqlite::Connection>) {
        EXPECT_TRUE(TestFixture::ids_where(fields::Person.name.ilike("\u00E9cole")).empty());
    }
    EXPECT_EQ(
            TestFixture::ids_where(fields::Person.name.ilike("\u00C9COLE")),
            (std::vector<int>{static_cast<int>(inserted.value())})
    );
}

TYPED_TEST(IlikeTest, EmitsLowerOnBothSidesOfLike) {
    QuerySet<Person, TypeParam> qs;
    const auto                  sql = qs.where(fields::Person.name.ilike("one")).select().sql();
    EXPECT_TRUE(sql.contains("lower(name) LIKE lower(?)")) << sql;
}

// The pattern is bound, never spliced into the text.
TYPED_TEST(IlikeTest, PatternIsBoundNotInlined) {
    QuerySet<Person, TypeParam> qs;
    const auto                  sql = qs.where(fields::Person.name.ilike("O'NE")).select().sql();
    EXPECT_FALSE(sql.contains("O'NE")) << sql;
}

namespace {

    // Message and Person both have `id`; over a JOIN the base table is aliased t1 (#785), so the
    // lower() argument must be the qualified column, not a bare one.
    template <typename ConnType> class IlikeJoinTest : public StormTestFixture<Message, ConnType, Person> {};

} // namespace

TYPED_TEST_SUITE(IlikeJoinTest, DatabaseTypes);

TYPED_TEST(IlikeJoinTest, BaseColumnIsQualifiedUnderJoin) {
    QuerySet<Person, TypeParam>  people;
    QuerySet<Message, TypeParam> qs;
    auto                         sender = people.insert(Person{.id = 0, .name = "Ann", .age = 30}).execute();
    ASSERT_TRUE(sender.has_value()) << sender.error().message();
    ASSERT_TRUE(qs.insert(Message{.id      = 0,
                                  .content = "Say HELLO",
                                  .value   = 1,
                                  .sender  = Person{.id = static_cast<int>(sender.value())}})
                        .execute()
                        .has_value());

    auto query = qs.where(fields::Message.content.ilike("%hello%")).template join<fields::Message.sender>().select();

    EXPECT_TRUE(query.sql().contains("lower(t1.content) LIKE lower(?)")) << query.sql();
    auto result = query.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1U) << "the qualified lower() must match through the JOIN";
    EXPECT_EQ(result.value().begin()->content, "Say HELLO");
}

// NOLINTEND(misc-const-correctness)
