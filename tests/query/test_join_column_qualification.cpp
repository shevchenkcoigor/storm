#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

// #785: over a JOIN the base table is aliased t1, so a base-table column in WHERE /
// ORDER BY / an aggregate operand / GROUP BY / HAVING must be emitted as t1.<col>.
// Message and Person both have an `id` column, so a bare `id` there is rejected as
// ambiguous ("ambiguous column name: id" on SQLite, "column reference is ambiguous" on
// PostgreSQL). Every test below asserts the generated SQL AND executes the statement
// against the real database, which is what catches the ambiguity. A column of the
// JOINED model keeps its bare spelling (it is not a t1 column).

import storm;
import std;

using storm::QuerySet;
using storm::orm::utilities::Collate;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"

// Two FKs (both to Person) so one join<> emits two JOINs: Letter t1, Person t2, Person t3 —
// all three tables have `id`. Local to this TU, like the fk-fields test models.
struct Letter {
    [[= storm::primary]] int id{};
    std::string              body;
    int                      value{};
    [[= storm::fk<>]] Person sender;
    [[= storm::fk<>]] Person receiver;
};

namespace fields {
    struct LetterT;
    consteval {
        std::meta::define_aggregate(^^LetterT, storm::field_specs_for(^^Letter));
    }
    inline constexpr LetterT Letter{};
} // namespace fields

namespace {

    constexpr std::string_view kJoinSelect =
            "SELECT t1.id, t1.content, t1.value, t2.id, t2.name, t2.age, t2.salary,"
            " t2.is_active, t2.years_experience, t2.department, t2.score, t2.nickname, t2.avatar"
            " FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id";

    constexpr std::string_view kJoinFrom = " FROM Message t1 INNER JOIN Person t2 ON t2.id = t1.sender_id";

    template <typename ConnType> constexpr auto asc_suffix() -> std::string_view {
        if constexpr (storm::test::is_postgresql<ConnType>()) {
            return "ASC NULLS FIRST";
        } else {
            return "ASC";
        }
    }

    template <typename ConnType> constexpr auto desc_suffix() -> std::string_view {
        if constexpr (storm::test::is_postgresql<ConnType>()) {
            return "DESC NULLS LAST";
        } else {
            return "DESC";
        }
    }

} // namespace

// Seeds two senders (alice age 10, bob age 5) and three messages: values 10 / 20 / 30,
// sent by alice / bob / alice, inserted in that order (so id order == value order).
template <typename ConnType> class JoinQualifyTest : public StormTestFixture<Message, ConnType, Person> {
  public:
    std::vector<std::int64_t> message_ids;

  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
        QuerySet<Person, ConnType> person_qs;
        auto                       alice = person_qs.insert(Person{.id = 0, .name = "alice", .age = 10}).execute();
        ASSERT_TRUE(alice.has_value()) << alice.error().message();
        auto bob = person_qs.insert(Person{.id = 0, .name = "bob", .age = 5}).execute();
        ASSERT_TRUE(bob.has_value()) << bob.error().message();

        const std::array<std::pair<std::int64_t, int>, 3> rows{
                {{alice.value(), 10}, {bob.value(), 20}, {alice.value(), 30}}
        };
        QuerySet<Message, ConnType> msg_qs;
        for (const auto& [sender_id, value] : rows) {
            auto id = msg_qs.insert(
                                    Message{.id      = 0,
                                            .content = std::format("msg{}", value),
                                            .value   = value,
                                            .sender  = Person{.id = static_cast<int>(sender_id)}}
            ).execute();
            ASSERT_TRUE(id.has_value()) << id.error().message();
            message_ids.emplace_back(id.value());
        }
    }
};

TYPED_TEST_SUITE(JoinQualifyTest, DatabaseTypes);

// ============================================================================
// SELECT … JOIN … WHERE
// ============================================================================

TYPED_TEST(JoinQualifyTest, SelectJoinWhereOnSharedColumnIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         query = qs.where(fields::Message.id > 0).template join<fields::Message.sender>();

    EXPECT_EQ(query.select().sql(), std::format("{} WHERE t1.id > ?", kJoinSelect));
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3U);
}

TYPED_TEST(JoinQualifyTest, SelectJoinWhereOnJoinedColumnStaysBare) {
    QuerySet<Message, TypeParam> qs;
    auto                         query = qs.template join<fields::Message.sender>().where(fields::Person.age >= 10);

    EXPECT_EQ(query.select().sql(), std::format("{} WHERE age >= ?", kJoinSelect));
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 2U);
}

TYPED_TEST(JoinQualifyTest, SelectJoinWhereEveryExpressionKindIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         expr = (fields::Message.id.in(this->message_ids[0], this->message_ids[2]) &&
                 fields::Message.value.between(5, 35)) &&
                (fields::Message.content.like("msg1%") || fields::Message.content == "msg30") && fields::Person.age > 0;
    auto query = qs.template join<fields::Message.sender>().where(expr);

    EXPECT_EQ(
            query.select().sql(),
            std::format(
                    "{} WHERE (((t1.id IN (?, ?) AND t1.value BETWEEN ? AND ?) AND (t1.content LIKE ? OR "
                    "t1.content = ?)) AND age > ?)",
                    kJoinSelect
            )
    );
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 2U);
}

// COLLATE NOCASE is SQLite-only, so the statement is executed there only; the SQL text
// is asserted on both backends.
TYPED_TEST(JoinQualifyTest, SelectJoinWhereAndOrderByCollatedAreQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         query = qs.template join<fields::Message.sender>()
                         .where(fields::Message.content.collate(Collate::NoCase) == "MSG20")
                         .template order_by<fields::Person.age, fields::Message.content, Collate::NoCase>();

    EXPECT_EQ(
            query.select().sql(),
            std::format(
                    "{} WHERE t1.content COLLATE NOCASE = ? ORDER BY age {}, t1.content COLLATE NOCASE {}",
                    kJoinSelect,
                    asc_suffix<TypeParam>(),
                    asc_suffix<TypeParam>()
            )
    );
    if constexpr (!storm::test::is_postgresql<TypeParam>()) {
        auto result = query.select().execute();
        ASSERT_TRUE(result.has_value()) << result.error().message();
        ASSERT_EQ(result->size(), 1U);
        EXPECT_EQ(result->begin()->value, 20);
    }
}

TYPED_TEST(JoinQualifyTest, SelectJoinWhereEmptyInStaysAlwaysFalse) {
    QuerySet<Message, TypeParam> qs;
    auto                         query = qs.template join<fields::Message.sender>().where(fields::Message.id.in());

    EXPECT_EQ(query.select().sql(), std::format("{} WHERE 1 = 0", kJoinSelect));
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_TRUE(result->empty());
}

TYPED_TEST(JoinQualifyTest, SelectJoinWhereNullCheckOnJoinedColumnStaysBare) {
    QuerySet<Message, TypeParam> qs;
    auto query = qs.template join<fields::Message.sender>().where(fields::Person.score.is_null());

    EXPECT_EQ(query.select().sql(), std::format("{} WHERE score IS NULL", kJoinSelect));
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3U);
}

TYPED_TEST(JoinQualifyTest, SelectWithoutJoinStaysBare) {
    QuerySet<Message, TypeParam> qs;
    auto                         query = qs.where(fields::Message.id > 0).template order_by<fields::Message.id>();

    EXPECT_EQ(
            query.select().sql(),
            std::format(
                    "SELECT id, content, value, sender_id FROM Message WHERE id > ? ORDER BY id {}",
                    asc_suffix<TypeParam>()
            )
    );
}

// ============================================================================
// SELECT … JOIN … ORDER BY
// ============================================================================

TYPED_TEST(JoinQualifyTest, SelectJoinOrderByOnSharedColumnIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto query = qs.template join<fields::Message.sender>().template order_by<fields::Message.id, false>();

    EXPECT_EQ(query.select().sql(), std::format("{} ORDER BY t1.id {}", kJoinSelect, desc_suffix<TypeParam>()));
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 3U);
    EXPECT_EQ(result->begin()->value, 30);
}

TYPED_TEST(JoinQualifyTest, SelectJoinOrderByMixedOwnersQualifiesOnlyBase) {
    QuerySet<Message, TypeParam> qs;
    auto                         query = qs.template join<fields::Message.sender>()
                         .template order_by<fields::Person.age, fields::Message.value, false>();

    EXPECT_EQ(
            query.select().sql(),
            std::format(
                    "{} ORDER BY age {}, t1.value {}", kJoinSelect, asc_suffix<TypeParam>(), desc_suffix<TypeParam>()
            )
    );
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 3U);
    auto it = result->begin();
    EXPECT_EQ(it->value, 20); // bob (age 5) first
    ++it;
    EXPECT_EQ(it->value, 30); // then alice's, value DESC
}

TYPED_TEST(JoinQualifyTest, SelectJoinFirstAndRowsAreQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         query = qs.template join<fields::Message.sender>()
                         .where(fields::Message.id > 0)
                         .template order_by<fields::Message.id, false>();

    auto first = query.first().execute();
    ASSERT_TRUE(first.has_value()) << first.error().message();
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ((*first)->value, 30);

    std::size_t rows = 0;
    for (auto&& row : query.rows()) {
        ASSERT_TRUE(row.has_value()) << row.error().message();
        ++rows;
    }
    EXPECT_EQ(rows, 3U);
}

// ============================================================================
// Aggregates over a JOIN
// ============================================================================

TYPED_TEST(JoinQualifyTest, AggregateJoinSumOnSharedColumnIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         agg =
            qs.where(fields::Message.id > 0).template join<fields::Message.sender>().template sum<fields::Message.id>();

    EXPECT_EQ(agg.sql(), std::format("SELECT SUM(t1.id){} WHERE t1.id > ?", kJoinFrom));
    auto result = agg.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value(), std::ranges::fold_left(this->message_ids, std::int64_t{0}, std::plus{}));
}

TYPED_TEST(JoinQualifyTest, AggregateJoinMultiOpMixedOwners) {
    QuerySet<Message, TypeParam> qs;
    auto                         agg = qs.template join<fields::Message.sender>()
                       .template max<fields::Message.id>()
                       .template sum<fields::Message.value, fields::Person.age>()
                       .template count<fields::Message.id>();

    EXPECT_EQ(agg.sql(), std::format("SELECT MAX(t1.id), SUM(t1.value + age), COUNT(t1.id){}", kJoinFrom));
    auto result = agg.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(std::get<1>(result.value()), 60 + 10 + 5 + 10);
    EXPECT_EQ(std::get<2>(result.value()), 3);
}

TYPED_TEST(JoinQualifyTest, AggregateJoinCountDistinctIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto agg = qs.template join<fields::Message.sender>().template count_distinct<fields::Message.id>();

    EXPECT_EQ(agg.sql(), std::format("SELECT COUNT(DISTINCT t1.id){}", kJoinFrom));
    auto result = agg.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result.value(), 3);
}

TYPED_TEST(JoinQualifyTest, AggregateJoinGroupByHavingIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         agg = qs.where(fields::Message.id > 0)
                       .template join<fields::Message.sender>()
                       .template order_by<fields::Message.id>()
                       .template group_by<fields::Message.id>()
                       .having(fields::Message.id > 0)
                       .count();

    EXPECT_EQ(
            agg.sql(),
            std::format(
                    "SELECT t1.id, COUNT(*){} WHERE t1.id > ? GROUP BY t1.id HAVING t1.id > ? ORDER BY t1.id {}",
                    kJoinFrom,
                    asc_suffix<TypeParam>()
            )
    );
    auto result = agg.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3U);
}

TYPED_TEST(JoinQualifyTest, AggregateJoinGroupByWithoutWhereIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto agg = qs.template join<fields::Message.sender>().template group_by<fields::Message.id>().count().having(
            fields::Message.id > 0
    );

    EXPECT_EQ(agg.sql(), std::format("SELECT t1.id, COUNT(*){} GROUP BY t1.id HAVING t1.id > ?", kJoinFrom));
    auto result = agg.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3U);
}

TYPED_TEST(JoinQualifyTest, AggregateWithoutJoinStaysBare) {
    QuerySet<Message, TypeParam> qs;
    auto                         agg = qs.where(fields::Message.id > 0)
                       .template group_by<fields::Message.id>()
                       .count()
                       .having(fields::Message.id > 0);

    EXPECT_EQ(agg.sql(), "SELECT id, COUNT(*) FROM Message WHERE id > ? GROUP BY id HAVING id > ?");
}

// ============================================================================
// DISTINCT / values() over a JOIN
// ============================================================================

TYPED_TEST(JoinQualifyTest, DistinctJoinWhereOrderByIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         stmt = qs.template join<fields::Message.sender>()
                        .where(fields::Message.id > 0)
                        .template order_by<fields::Message.id>()
                        .template distinct<fields::Message.id>();

    EXPECT_EQ(
            stmt.sql(),
            std::format("SELECT DISTINCT t1.id{} WHERE t1.id > ? ORDER BY t1.id {}", kJoinFrom, asc_suffix<TypeParam>())
    );
    auto result = stmt.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3U);
}

TYPED_TEST(JoinQualifyTest, ValuesJoinWhereOrderByIsQualified) {
    QuerySet<Message, TypeParam> qs;
    auto                         stmt = qs.template join<fields::Message.sender>()
                        .where(fields::Message.id > 0)
                        .template order_by<fields::Message.id, false>()
                        .template values<fields::Message.id, fields::Message.value>();

    EXPECT_EQ(
            stmt.sql(),
            std::format(
                    "SELECT t1.id, t1.value{} WHERE t1.id > ? ORDER BY t1.id {}", kJoinFrom, desc_suffix<TypeParam>()
            )
    );
    auto result = stmt.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 3U);
    EXPECT_EQ(std::get<1>(*result->begin()), 30);
}

// ============================================================================
// Set-operation operand carrying a JOIN
// ============================================================================

TYPED_TEST(JoinQualifyTest, SetOpJoinOperandWhereIsQualified) {
    QuerySet<Message, TypeParam> lhs;
    QuerySet<Message, TypeParam> rhs;
    auto                         setop = lhs.template join<fields::Message.sender>()
                         .where(fields::Message.id > 0)
                         .union_(rhs.template join<fields::Message.sender>().where(fields::Message.value > 15));

    EXPECT_EQ(setop.sql(), std::format("{} WHERE t1.id > ? UNION {} WHERE t1.value > ?", kJoinSelect, kJoinSelect));
    auto result = setop.execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->size(), 3U);
}

// ============================================================================
// Multi-relation FK join: join<sender, receiver>() — Letter t1, Person t2, Person t3
// ============================================================================

namespace {

    auto person_cols(std::string_view alias) -> std::string {
        constexpr std::array<std::string_view, 10> cols{
                "id",
                "name",
                "age",
                "salary",
                "is_active",
                "years_experience",
                "department",
                "score",
                "nickname",
                "avatar"
        };
        std::string out;
        for (const auto col : cols) {
            out += std::format("{}{}.{}", out.empty() ? "" : ", ", alias, col);
        }
        return out;
    }

    auto letter_from(std::string_view kw) -> std::string {
        return std::format(
                " FROM Letter t1 {0} Person t2 ON t2.id = t1.sender_id {0} Person t3 ON t3.id = t1.receiver_id", kw
        );
    }

    auto letter_select(std::string_view kw) -> std::string {
        return std::format(
                "SELECT t1.id, t1.body, t1.value, {}, {}{}", person_cols("t2"), person_cols("t3"), letter_from(kw)
        );
    }

} // namespace

// Seeds alice and bob, then three letters (values 10 / 20 / 30, ids ascending):
// alice→bob, bob→alice, alice→alice.
template <typename ConnType> class MultiFkJoinQualifyTest : public StormTestFixture<Person, ConnType, Letter> {
  public:
    std::vector<std::int64_t> letter_ids;

  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {
        QuerySet<Person, ConnType> person_qs;
        auto                       alice = person_qs.insert(Person{.id = 0, .name = "alice", .age = 10}).execute();
        ASSERT_TRUE(alice.has_value()) << alice.error().message();
        auto bob = person_qs.insert(Person{.id = 0, .name = "bob", .age = 5}).execute();
        ASSERT_TRUE(bob.has_value()) << bob.error().message();
        const auto a = static_cast<int>(alice.value());
        const auto b = static_cast<int>(bob.value());

        const std::array<std::tuple<int, int, int>, 3> rows{{{a, b, 10}, {b, a, 20}, {a, a, 30}}};
        QuerySet<Letter, ConnType>                     letter_qs;
        for (const auto& [sender, receiver, value] : rows) {
            auto id = letter_qs
                              .insert(
                                      Letter{.id       = 0,
                                             .body     = std::format("letter{}", value),
                                             .value    = value,
                                             .sender   = Person{.id = sender},
                                             .receiver = Person{.id = receiver}}
                              )
                              .execute();
            ASSERT_TRUE(id.has_value()) << id.error().message();
            letter_ids.emplace_back(id.value());
        }
    }
};

TYPED_TEST_SUITE(MultiFkJoinQualifyTest, DatabaseTypes);

TYPED_TEST(MultiFkJoinQualifyTest, SelectWhereAndOrderByAreQualified) {
    QuerySet<Letter, TypeParam> qs;
    auto                        query = qs.template join<fields::Letter.sender, fields::Letter.receiver>()
                         .where(fields::Letter.id > 0)
                         .template order_by<fields::Letter.id, false>();

    EXPECT_EQ(
            query.select().sql(),
            std::format("{} WHERE t1.id > ? ORDER BY t1.id {}", letter_select("INNER JOIN"), desc_suffix<TypeParam>())
    );
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 3U);
    EXPECT_EQ(result->begin()->value, 30);
    EXPECT_EQ(result->begin()->receiver.name, "alice");
}

TYPED_TEST(MultiFkJoinQualifyTest, LeftJoinWhereAndOrderByAreQualified) {
    QuerySet<Letter, TypeParam> qs;
    auto                        query = qs.template left_join<fields::Letter.sender, fields::Letter.receiver>()
                         .where(fields::Letter.id > this->letter_ids[0])
                         .template order_by<fields::Letter.id>();

    EXPECT_EQ(
            query.select().sql(),
            std::format("{} WHERE t1.id > ? ORDER BY t1.id {}", letter_select("LEFT JOIN"), asc_suffix<TypeParam>())
    );
    auto result = query.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(result->begin()->value, 20);
    EXPECT_EQ(result->begin()->sender.name, "bob");
}

TYPED_TEST(MultiFkJoinQualifyTest, SumAndCountAreQualified) {
    QuerySet<Letter, TypeParam> qs;
    auto joined = qs.where(fields::Letter.id > 0).template join<fields::Letter.sender, fields::Letter.receiver>();

    auto sum = joined.template sum<fields::Letter.id>();
    EXPECT_EQ(sum.sql(), std::format("SELECT SUM(t1.id){} WHERE t1.id > ?", letter_from("INNER JOIN")));
    auto total = sum.execute();
    ASSERT_TRUE(total.has_value()) << total.error().message();
    EXPECT_EQ(total.value(), std::ranges::fold_left(this->letter_ids, std::int64_t{0}, std::plus{}));

    auto count = joined.count();
    EXPECT_EQ(count.sql(), std::format("SELECT COUNT(*){} WHERE t1.id > ?", letter_from("INNER JOIN")));
    auto rows = count.execute();
    ASSERT_TRUE(rows.has_value()) << rows.error().message();
    EXPECT_EQ(rows.value(), 3);
}

TYPED_TEST(MultiFkJoinQualifyTest, LeftJoinGroupByHavingIsQualified) {
    QuerySet<Letter, TypeParam> qs;
    auto                        agg = qs.where(fields::Letter.id > 0)
                       .template left_join<fields::Letter.sender, fields::Letter.receiver>()
                       .template order_by<fields::Letter.id>()
                       .template group_by<fields::Letter.id>()
                       .having(fields::Letter.id > 0)
                       .count();

    EXPECT_EQ(
            agg.sql(),
            std::format(
                    "SELECT t1.id, COUNT(*){} WHERE t1.id > ? GROUP BY t1.id HAVING t1.id > ? ORDER BY t1.id {}",
                    letter_from("LEFT JOIN"),
                    asc_suffix<TypeParam>()
            )
    );
    auto groups = agg.execute();
    ASSERT_TRUE(groups.has_value()) << groups.error().message();
    EXPECT_EQ(groups->size(), 3U);
}

// NOLINTEND(misc-const-correctness)
