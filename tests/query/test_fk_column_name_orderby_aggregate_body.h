#pragma once

// Shared test body for test_fk_column_name_orderby_aggregate_sqlite.cpp / test_fk_column_name_orderby_aggregate_pg.cpp
// — the two single-backend TUs of a compile-time TU split (see test_db_helpers.h,
// DatabaseTypesSqliteHalf/DatabaseTypesPgHalf). Splitting a 2-backend TU into
// two lets ninja compile them in parallel instead of serially instantiating
// both backends in one TU; keeping the body here (instead of duplicating it
// into both .cpp files) removes the risk of the two halves silently drifting.
//
// The includer must #define STORM_SPLIT_TYPES / STORM_SPLIT_TYPE_NAMES to one
// backend's ::testing::Types<> alias / NameGenerator before #include-ing this
// file, and #undef both afterward. Never include this file directly.
#if !defined(STORM_SPLIT_TYPES) || !defined(STORM_SPLIT_TYPE_NAMES)
#error "test_fk_column_name_orderby_aggregate_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

// NOLINTBEGIN(misc-const-correctness)

#include <meta>

import storm;
import storm_orm_statements_orderby; // OrderBySelectorsAreSingleColumn — not re-exported by `storm`
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"

// Must follow the shared model headers: Shipment / LedgerEntryRef name storm:: annotations.
// Supplies the composite-PK FK targets the compile-time gate below is asserted on.
#include "crud/test_composite_pk_models.h" // NOSONAR cpp:S954
using storm::field_specs_for;
using storm::primary;
using storm::meta::selector_info;
using storm::orm::utilities::Collate;

using storm::QuerySet;

// A nullable single-column FK, local to this file: proves WHERE's is_null()/is_not_null()
// (#575) route through the same "<member>_id" rewrite as the comparison operators, on a
// member no other fixture here already covers with an optional FK.
struct NullableSenderMessage {
    [[= primary]] int id{};
    [[= storm::fk<>]] std::optional<Person> sender;
    std::string content;
};

namespace fields {
struct NullableSenderMessageT;
consteval { std::meta::define_aggregate(^^NullableSenderMessageT, field_specs_for(^^NullableSenderMessage)); }
inline constexpr NullableSenderMessageT NullableSenderMessage{};
} // namespace fields

namespace {

// Message::sender is an FK to Person → column `sender_id`.
// Person rows must exist first: the FK constraint is enforced on both backends.
template <typename ConnType> class FkColumnNameTest : public StormTestFixture<Person, ConnType, Message> {
  public:
    // Seeds 3 Persons and 5 Messages whose senders are, in order:
    // p1, p1, p2, p2, p3 — so COUNT(DISTINCT sender_id) == 3 while COUNT(*) == 5,
    // making the DISTINCT genuinely load-bearing (a plain COUNT would give 5).
    auto on_after_setup(const std::shared_ptr<ConnType> &conn) -> void override {
        QuerySet<Person, ConnType> person_qs;
        const std::vector<Person> people = {
            Person{.name = "Ann", .age = 30},
            Person{.name = "Bob", .age = 40},
            Person{.name = "Cid", .age = 50},
        };
        auto inserted = person_qs.insert(people).execute();
        ASSERT_TRUE(inserted.has_value()) << "Failed to seed Person rows";

        // Re-query: PostgreSQL assigns its own ids, so the seeded literals
        // above cannot be assumed (same pattern as MESSAGES_8's comment).
        auto stored = person_qs.template order_by<fields::Person.id>().select().execute();
        ASSERT_TRUE(stored.has_value()) << "Failed to read back Person rows";
        ASSERT_EQ(stored->size(), 3U);

        std::vector<int> ids;
        ids.reserve(3);
        for (const auto &p : *stored) {
            ids.push_back(p.id);
        }
        person_ids_ = {ids[0], ids[1], ids[2]};

        QuerySet<Message, ConnType> msg_qs;
        const std::vector<Message> messages = {
            Message{.content = "m1", .value = 10, .sender = {.id = ids[0]}},
            Message{.content = "m2", .value = 20, .sender = {.id = ids[0]}},
            Message{.content = "m3", .value = 30, .sender = {.id = ids[1]}},
            Message{.content = "m4", .value = 40, .sender = {.id = ids[1]}},
            Message{.content = "m5", .value = 50, .sender = {.id = ids[2]}},
        };
        ASSERT_TRUE(msg_qs.insert(messages).execute().has_value()) << "Failed to seed Message rows";
        (void)conn;
    }

    std::array<int, 3> person_ids_{};
};
TYPED_TEST_SUITE(FkColumnNameTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// ── WHERE on an FK member (#575) ─────────────────────────────────────────────

// The core reproduction: before the fix this emitted `WHERE sender = ?` and
// failed to prepare ("no such column: sender") on both backends.
TYPED_TEST(FkColumnNameTest, WhereFkMemberEqualsExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.where(fields::Message.sender == this->person_ids_[0]).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE == on an FK member failed: " << result.error().message();
    // m1 and m2 both have sender == person_ids_[0].
    ASSERT_EQ(result->size(), 2U);
    for (const auto &m : *result) {
        EXPECT_EQ(m.sender.id, this->person_ids_[0]);
    }
}

TYPED_TEST(FkColumnNameTest, WhereFkMemberNotEqualsExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.where(fields::Message.sender != this->person_ids_[0]).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE != on an FK member failed: " << result.error().message();
    // m3, m4, m5 have a different sender.
    ASSERT_EQ(result->size(), 3U);
    for (const auto &m : *result) {
        EXPECT_NE(m.sender.id, this->person_ids_[0]);
    }
}

TYPED_TEST(FkColumnNameTest, WhereFkMemberSqlNamesFkColumn) {
    QuerySet<Message, TypeParam> qs;
    const auto sql = qs.where(fields::Message.sender == 1).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id = ?")) << "Emitted SQL: " << sql;
    // Negative half: a regression that emitted BOTH the correct and the bare column would
    // still pass the positive assertion above.
    EXPECT_FALSE(sql.contains("WHERE sender ")) << "Emitted SQL: " << sql;
}

// The remaining comparison/pattern operators share field_name_sv with == above, so they are
// correct by construction — but that was exactly the argument that failed for ORDER BY in
// #570 and for WHERE's == in #575 itself. Pin each explicitly rather than trust the shared
// derivation silently.
TYPED_TEST(FkColumnNameTest, WhereFkMemberRemainingOperatorsSqlNameFkColumn) {
    QuerySet<Message, TypeParam> qs;
    auto sql = qs.where(fields::Message.sender > 1).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id > ?")) << "Emitted SQL: " << sql;
    sql = qs.where(fields::Message.sender >= 1).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id >= ?")) << "Emitted SQL: " << sql;
    sql = qs.where(fields::Message.sender < 1).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id < ?")) << "Emitted SQL: " << sql;
    sql = qs.where(fields::Message.sender <= 1).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id <= ?")) << "Emitted SQL: " << sql;
    sql = qs.where(fields::Message.sender.between(1, 2)).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id BETWEEN ? AND ?")) << "Emitted SQL: " << sql;
    // like() is SQL-text only here — LIKE against an integer column is nonsensical to
    // execute, but the writer must still name the right column.
    sql = qs.where(fields::Message.sender.like("1%")).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id LIKE ?")) << "Emitted SQL: " << sql;
}

// #610: in() on a single-column FK member routes the operand through the target's
// primary-key type and emits `sender_id IN (?, ?)`.
TYPED_TEST(FkColumnNameTest, WhereFkMemberInExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.where(fields::Message.sender.in(this->person_ids_[0], this->person_ids_[1])).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE IN on an FK member failed: " << result.error().message();
    // m1, m2 (sender p1) and m3, m4 (sender p2); m5 (p3) is excluded.
    ASSERT_EQ(result->size(), 4U);
    for (const auto &m : *result) {
        EXPECT_TRUE(m.sender.id == this->person_ids_[0] || m.sender.id == this->person_ids_[1]);
    }
}

TYPED_TEST(FkColumnNameTest, WhereFkMemberInSqlNamesFkColumn) {
    QuerySet<Message, TypeParam> qs;
    const auto sql = qs.where(fields::Message.sender.in(1, 2)).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id IN (?, ?)")) << "Emitted SQL: " << sql;
    // Negative half: a regression that emitted BOTH the correct and the bare column would
    // still pass the positive assertion above (same rationale as the == test's negative half).
    EXPECT_FALSE(sql.contains("WHERE sender ")) << "Emitted SQL: " << sql;
}

// #610: make_in_expr must accept an operand WIDER than the FK target's PK type — the natural
// shape of insert()'s own return type (int64_t) against Person::id (int). Regression test for
// a narrowing hard error: the requires-clause checks constructible_from (direct-init, permits
// narrowing) but the old body used list-init (TargetType{value}), which rejects it — a bug the
// existing tests couldn't see because they round-trip ids through std::array<int, 3>.
TYPED_TEST(FkColumnNameTest, WhereFkMemberInAcceptsWiderIntOperand) {
    QuerySet<Person, TypeParam> person_qs;
    auto inserted = person_qs.insert(Person{.name = "Dee", .age = 60}).execute();
    ASSERT_TRUE(inserted.has_value()) << "Failed to insert extra Person: " << inserted.error().message();
    const std::int64_t new_id = *inserted; // insert()'s natural return type, wider than Person::id

    QuerySet<Message, TypeParam> qs;
    auto result = qs.where(fields::Message.sender.in(new_id)).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE IN with an int64_t operand on an int-PK FK member failed: "
                                    << result.error().message();
    EXPECT_TRUE(result->empty()) << "The newly inserted Person has no Messages yet";
}

// The collate() path (where.cppm CollatedField::make_collated_name) shares the writer bug
// independently of the plain Field path above — must be rewritten too.
TYPED_TEST(FkColumnNameTest, WhereFkMemberCollateSqlNamesFkColumn) {
    QuerySet<Message, TypeParam> qs;
    const auto sql = qs.where(fields::Message.sender.collate(Collate::NoCase) == 1).select().sql();
    EXPECT_TRUE(sql.contains("sender_id COLLATE NOCASE = ?")) << "Emitted SQL: " << sql;
}

// #610: CollatedField::in() on an FK member — the path that drifted from Field::in in #578 —
// must independently route through InTargetType too, not just the plain Field path above.
TYPED_TEST(FkColumnNameTest, WhereFkMemberCollateInSqlNamesFkColumn) {
    QuerySet<Message, TypeParam> qs;
    const auto sql = qs.where(fields::Message.sender.collate(Collate::NoCase).in(1, 2)).select().sql();
    EXPECT_TRUE(sql.contains("sender_id COLLATE NOCASE IN (?, ?)")) << "Emitted SQL: " << sql;
}

// COLLATE NOCASE is SQLite-only syntax (PostgreSQL has no built-in NOCASE collation — the
// same reason CollateTest itself only runs on SqliteTypes); PG's rejection of the emitted
// SQL is expected, not a regression, so this only executes on SQLite.
TYPED_TEST(FkColumnNameTest, WhereFkMemberCollateExecutes) {
    if (storm::test::is_postgresql<TypeParam>()) {
        GTEST_SKIP() << "COLLATE NOCASE is SQLite-only syntax";
    }
    QuerySet<Message, TypeParam> qs;
    auto result = qs.where(fields::Message.sender.collate(Collate::NoCase) == this->person_ids_[0]).select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE == via collate() on an FK member failed: " << result.error().message();
    ASSERT_EQ(result->size(), 2U);
}

// Combined WHERE + ORDER BY, exercising two of the corrected writers on the same query.
TYPED_TEST(FkColumnNameTest, WhereFkMemberWithOrderByExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.where(fields::Message.value > 15).template order_by<fields::Message.sender>().select().execute();
    ASSERT_TRUE(result.has_value()) << "WHERE + ORDER BY on FK members failed: " << result.error().message();
    ASSERT_EQ(result->size(), 4U);
    std::vector<int> senders;
    senders.reserve(4);
    for (const auto &m : *result) {
        senders.push_back(m.sender.id);
    }
    EXPECT_TRUE(std::ranges::is_sorted(senders)) << "Rows are not ordered by sender_id";
}

// ── is_null()/is_not_null() on a nullable FK member ─────────────────────────

template <typename ConnType>
class NullableSenderMessageTest : public StormTestFixture<Person, ConnType, NullableSenderMessage> {};
TYPED_TEST_SUITE(NullableSenderMessageTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(NullableSenderMessageTest, WhereFkMemberIsNullExecutes) {
    QuerySet<Person, TypeParam> person_qs;
    Person const alice{.name = "Alice", .age = 30};
    auto alice_result = person_qs.insert(alice).execute();
    ASSERT_TRUE(alice_result.has_value());
    Person const alice_fk{.id = static_cast<int>(alice_result.value())};

    QuerySet<NullableSenderMessage, TypeParam> msg_qs;
    ASSERT_TRUE(
        msg_qs.insert(NullableSenderMessage{.sender = std::nullopt, .content = "no sender"}).execute().has_value());
    ASSERT_TRUE(
        msg_qs.insert(NullableSenderMessage{.sender = alice_fk, .content = "has sender"}).execute().has_value());

    auto null_result = msg_qs.where(fields::NullableSenderMessage.sender.is_null()).select().execute();
    ASSERT_TRUE(null_result.has_value()) << "WHERE IS NULL on an FK member failed: " << null_result.error().message();
    ASSERT_EQ(null_result->size(), 1U);
    EXPECT_EQ(null_result->begin()->content, "no sender");

    auto not_null_result = msg_qs.where(fields::NullableSenderMessage.sender.is_not_null()).select().execute();
    ASSERT_TRUE(not_null_result.has_value())
        << "WHERE IS NOT NULL on an FK member failed: " << not_null_result.error().message();
    ASSERT_EQ(not_null_result->size(), 1U);
    EXPECT_EQ(not_null_result->begin()->content, "has sender");
}

TYPED_TEST(NullableSenderMessageTest, WhereFkMemberIsNullSqlNamesFkColumn) {
    QuerySet<NullableSenderMessage, TypeParam> qs;
    const auto sql = qs.where(fields::NullableSenderMessage.sender.is_null()).select().sql();
    EXPECT_TRUE(sql.contains("WHERE sender_id IS NULL")) << "Emitted SQL: " << sql;
}

// ── ORDER BY on an FK member ────────────────────────────────────────────────

// The core reproduction: before the fix this emitted `ORDER BY sender` and
// failed to prepare ("no such column: sender") on both backends.
TYPED_TEST(FkColumnNameTest, OrderByFkMemberExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.template order_by<fields::Message.sender>().select().execute();
    ASSERT_TRUE(result.has_value()) << "ORDER BY on an FK member failed: " << result.error().message();
    ASSERT_EQ(result->size(), 5U);

    // Ordered by sender_id ASC — the FK column's value, not the member identifier.
    std::vector<int> senders;
    senders.reserve(5);
    for (const auto &m : *result) {
        senders.push_back(m.sender.id);
    }
    EXPECT_TRUE(std::ranges::is_sorted(senders)) << "Rows are not ordered by sender_id";
    EXPECT_EQ(senders.front(), this->person_ids_[0]);
    EXPECT_EQ(senders.back(), this->person_ids_[2]);
}

// DESC must order by the same column, reversed — proves the direction suffix
// still attaches to the rewritten column name rather than to a stale identifier.
TYPED_TEST(FkColumnNameTest, OrderByFkMemberDescExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.template order_by<fields::Message.sender, false>().select().execute();
    ASSERT_TRUE(result.has_value()) << "ORDER BY DESC on an FK member failed: " << result.error().message();
    ASSERT_EQ(result->size(), 5U);

    std::vector<int> senders;
    senders.reserve(5);
    for (const auto &m : *result) {
        senders.push_back(m.sender.id);
    }
    EXPECT_TRUE(std::ranges::is_sorted(senders, std::greater<>{})) << "Rows are not ordered by sender_id DESC";
    EXPECT_EQ(senders.front(), this->person_ids_[2]);
}

// Mixed pack: a non-FK member and an FK member in one ORDER BY. Guards the
// per-field dispatch — the writer must rewrite ONLY the FK entry.
TYPED_TEST(FkColumnNameTest, OrderByMixedFkAndPlainMemberExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.template order_by<fields::Message.sender, fields::Message.value>().select().execute();
    ASSERT_TRUE(result.has_value()) << "Mixed ORDER BY failed: " << result.error().message();
    ASSERT_EQ(result->size(), 5U);

    auto it = result->begin();
    EXPECT_EQ(it->value, 10);
    EXPECT_EQ(it->sender.id, this->person_ids_[0]);
}

// SQL text: the emitted ORDER BY names sender_id, and nothing spells the bare
// `sender` as a column. (Execution above is the real gate; this pins the text.)
TYPED_TEST(FkColumnNameTest, OrderByFkMemberSqlNamesFkColumn) {
    QuerySet<Message, TypeParam> qs;
    const auto sql = qs.template order_by<fields::Message.sender>().select().sql();
    EXPECT_TRUE(sql.contains("ORDER BY sender_id")) << "Emitted SQL: " << sql;
}

// ── COUNT(DISTINCT <fk>) ────────────────────────────────────────────────────

// Before the fix: "no such column: sender".
TYPED_TEST(FkColumnNameTest, CountDistinctFkMemberExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.template count_distinct<fields::Message.sender>().execute();
    ASSERT_TRUE(result.has_value()) << "COUNT(DISTINCT fk) failed: " << result.error().message();
    // 5 messages, 3 distinct senders — DISTINCT is load-bearing here.
    EXPECT_EQ(*result, 3);
}

TYPED_TEST(FkColumnNameTest, CountDistinctFkMemberWithWhereExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result = qs.where(fields::Message.value > 25).template count_distinct<fields::Message.sender>().execute();
    ASSERT_TRUE(result.has_value()) << "COUNT(DISTINCT fk) + WHERE failed: " << result.error().message();
    // Messages m3 (p2), m4 (p2), m5 (p3) survive → 2 distinct senders.
    EXPECT_EQ(*result, 2);
}

TYPED_TEST(FkColumnNameTest, CountDistinctFkMemberSqlNamesFkColumn) {
    QuerySet<Message, TypeParam> qs;
    const auto sql = qs.template count_distinct<fields::Message.sender>().sql();
    EXPECT_TRUE(sql.contains("COUNT(DISTINCT sender_id)")) << "Emitted SQL: " << sql;
}

// ── GROUP BY over an FK member (aggregate_grammar.hpp build_group_fields_sql — the third writer) ───

// GROUP BY's column list shares the same writer bug. It is reachable when the
// GROUP BY key is a plain column and the FK appears only as the COUNT(DISTINCT)
// target; grouping BY an FK member itself does not compile (the row extractor
// has no extract_column_value<Person>, extract.cppm), so this is the reachable
// half. Asserted so the corrected writer is exercised end to end.
TYPED_TEST(FkColumnNameTest, GroupByPlainWithCountDistinctFkExecutes) {
    QuerySet<Message, TypeParam> qs;
    auto result =
        qs.template group_by<fields::Message.value>().template count_distinct<fields::Message.sender>().execute();
    ASSERT_TRUE(result.has_value()) << "GROUP BY + COUNT(DISTINCT fk) failed: " << result.error().message();
    // Each of the 5 messages has a unique `value`, so every group holds 1 row
    // and therefore exactly 1 distinct sender.
    ASSERT_EQ(result->size(), 5U);
    for (const auto &row : *result) {
        EXPECT_EQ(std::get<1>(row), 1);
    }
}

// ── Composite-PK FK target is rejected at compile time ──────────────────────
//
// An FK whose target has an N-part composite PK spreads over "<member>_<part>"
// columns — there is NO "<member>_id" for it (#504). Routing such a member through
// the single-column writer would emit `ORDER BY line_id` / `COUNT(DISTINCT line_id)`,
// naming a column no table has: the #570 failure one shape further out.
//
// Rejected rather than expanded, because neither clause has a correct multi-column
// form. ORDER BY's direction/COLLATE suffix attaches once, after ONE column name, so
// a comma list would apply ASC/DESC to the LAST part only — valid SQL, wrong order,
// no diagnostic. And COUNT(DISTINCT a, b) is a SYNTAX ERROR in SQLite while
// PostgreSQL parses it as a row constructor, so expansion would be backend-divergent.
//
// Asserted on the gate predicates, not via the ill-formed calls: a TU cannot contain
// the ill-formed call it asserts about (same pattern as #502's ReturnIdSupported).

// Message::sender → single-PK Person: one column (`sender_id`), accepted.
static_assert(storm::meta::is_single_column_member(selector_info<fields::Message.sender>()),
              "an FK to a single-PK target is one column and stays usable in ORDER BY / COUNT(DISTINCT)");
// A plain non-FK column is trivially one column.
static_assert(storm::meta::is_single_column_member(selector_info<fields::Person.age>()),
              "an ordinary column is always single-column");
// Shipment::line → 2-part composite PK (order_id, product_id): rejected.
static_assert(!storm::meta::is_single_column_member(selector_info<fields::Shipment.line>()),
              "an FK to a composite-PK target spans several columns and has no <member>_id");
static_assert(storm::meta::column_span_of(selector_info<fields::Shipment.line>()) == 2,
              "Shipment::line spans exactly the target's two PK parts");

// The gate is not arity-limited — it counts the target's parts rather than testing
// for exactly two. Asserted on the reflection directly, since LedgerEntryRef (whose
// `ledger` FK targets the 3-part Ledger) has no fields:: selector declared.
static_assert(storm::meta::column_span_of(^^LedgerEntryRef::ledger) == 3,
              "a 3-part composite FK target spans three columns");
static_assert(!storm::meta::is_single_column_member(^^LedgerEntryRef::ledger), "and is rejected the same way");

// The gates themselves, not just the predicate under them — a correct predicate that
// was never wired into a constraint would still ship the bug. Both are asserted, so
// deleting either requires-clause fails the build rather than silently reopening #570.
static_assert(storm::orm::statements::OrderBySelectorsAreSingleColumn<fields::Message.sender>,
              "ORDER BY <single-PK fk> stays available");
static_assert(!storm::orm::statements::OrderBySelectorsAreSingleColumn<fields::Shipment.line>,
              "ORDER BY <composite fk> is refused at the call site");
// The modifier args (bool / Collate) must not trip the per-arg concept. Load-bearing:
// selector_info is constrained by AnySelector and does not normalise for a bool, so
// this pins that is_field_arg short-circuits before it is ever reached.
static_assert(storm::orm::statements::OrderBySelectorsAreSingleColumn<fields::Person.age, false>,
              "a DESC modifier is not a field arg and must pass the gate");

// The COUNT(DISTINCT) gate — storm::meta::SingleColumnSelector (fields.cppm), consolidated
// (#613) with the ORDER BY gate above, both now built on the same predicate.
static_assert(storm::meta::SingleColumnSelector<fields::Message.sender>,
              "COUNT(DISTINCT <single-PK fk>) stays available");
static_assert(!storm::meta::SingleColumnSelector<fields::Shipment.line>,
              "COUNT(DISTINCT <composite fk>) is refused at the call site");
static_assert(!storm::meta::SingleColumnSelector<fields::OptionalShipment.line>,
              "and the nullable composite form is refused too");

// WHERE's Field<M>/CollatedField<M> proxies (where.cppm, #575) gate the same policy, but
// via a per-operator requires-clause rather than a class-level one on Field<M> itself:
// field_specs_for defines a FieldRef<M> (which derives from Field<M>) for EVERY member
// unconditionally, so Field<M> must still instantiate for a composite-PK FK member — only
// the individual comparison/like/between/in/collate/is_null methods reject it.
//
// Routed through template concepts rather than a bare `requires { ... }` directly inside
// static_assert: `fields::Shipment.line == 1` at namespace scope is a fully non-dependent
// expression, and this compiler hard-errors on its failed overload resolution instead of
// treating it as a SFINAE failure inside the requires-expression (the same reason the
// "gates are WIRED" concepts below thread the call through a template parameter).
template <auto S, typename V = int>
concept CanWhereEqual = requires(V v) { S == v; };
template <auto S, typename V = int>
concept CanWhereNotEqual = requires(V v) { S != v; };
template <auto S, typename V = int>
concept CanWhereIn = requires(V v) { S.in(v, v); };
template <auto S, typename Str = std::string_view>
concept CanWhereLike = requires(Str s) { S.like(s); };
template <auto S, typename V = int>
concept CanWhereBetween = requires(V v) { S.between(v, v); };
template <auto S, typename Col = Collate>
concept CanWhereCollate = requires(Col c) { S.collate(c); };
template <auto S>
concept CanWhereIsNull = requires { S.is_null(); };
template <auto S>
concept CanWhereIsNotNull = requires { S.is_not_null(); };

static_assert(CanWhereEqual<fields::Message.sender>, "WHERE == on a single-PK fk must still compile");
static_assert(!CanWhereEqual<fields::Shipment.line>, "WHERE == on a composite-PK fk must be rejected at compile time");
// A non-bindable operand (a model struct instead of its key) is a clean compile-time
// rejection, not a hard error inside make_comparison_expr (#610).
static_assert(!CanWhereEqual<fields::Message.sender, Person>,
              "WHERE == against a non-bindable operand (a model struct) is rejected at compile time");
static_assert(!CanWhereNotEqual<fields::Shipment.line>, "!= is rejected the same way");
// in() now routes its operand through the FK TARGET's primary-key type instead of FieldType
// (#610), so it works on a single-PK FK member; a composite-PK target still has no single key
// type to construct against and stays rejected.
static_assert(!CanWhereIn<fields::Shipment.line>, "in() rejects a composite-PK fk");
static_assert(CanWhereIn<fields::Message.sender>, "in() now works on a single-PK fk (#610)");
static_assert(CanWhereIn<fields::NullableSenderMessage.sender>,
              "in() also works on a NULLABLE single-PK fk (the optional_inner_type_t unwrap)");
static_assert(!CanWhereLike<fields::Shipment.line>, "like() is rejected the same way");
static_assert(!CanWhereBetween<fields::Shipment.line>, "between() is rejected the same way");
static_assert(!CanWhereBetween<fields::Message.sender, Person>,
              "between() against a non-bindable operand (a model struct) is rejected at compile time (#610)");
static_assert(!CanWhereCollate<fields::Shipment.line>, "collate() is rejected the same way");
static_assert(!CanWhereIsNull<fields::OptionalShipment.line>, "is_null() rejects a nullable composite-PK fk too");
static_assert(!CanWhereIsNotNull<fields::OptionalShipment.line>, "and is_not_null() likewise");
static_assert(CanWhereIsNull<fields::NullableSenderMessage.sender>,
              "is_null() on a nullable single-PK fk must still compile");

// ── The gates are WIRED, not merely defined ─────────────────────────────────
//
// The assertions above test the concepts; they all still hold if someone deletes the
// requires-clause that USES one, which would silently reopen #570 (mutation-verified:
// removing both clauses reintroduces `COUNT(DISTINCT line_id)` with the whole suite
// still green). These assert the CALL EXPRESSION instead, which is what actually
// fails when a gate is unwired. A requires-expression can name an ill-formed call
// without the TU containing it, so no negative-compile file is needed.
template <typename Qs, auto S>
concept CanOrderBy = requires(Qs qs) { qs.template order_by<S>(); };
template <typename Qs, auto S>
concept CanCountDistinct = requires(Qs qs) { qs.template count_distinct<S>(); };
// #611: count()/distinct()/values()/group_by() carry the same gate now too.
template <typename Qs, auto S>
concept CanCount = requires(Qs qs) { qs.template count<S>(); };
template <typename Qs, auto S>
concept CanDistinct = requires(Qs qs) { qs.template distinct<S>(); };
template <typename Qs, auto S>
concept CanValues = requires(Qs qs) { qs.template values<S>(); };
// G/S below each isolate one of group_by()/count()'s two independent gates.
template <typename Qs, auto G, auto S>
concept CanGroupedCount = requires(Qs qs) { qs.template group_by<G>().template count<S>(); };
template <typename Qs, auto S1, auto S2>
concept CanChainedCount = requires(Qs qs) { qs.template count<S1>().template count<S2>(); };

using MessageQs = QuerySet<Message, storm::db::sqlite::Connection>;
using ShipmentQs = QuerySet<Shipment, storm::db::sqlite::Connection>;

static_assert(CanOrderBy<MessageQs, fields::Message.sender>, "ORDER BY on a single-PK FK must still compile");
static_assert(!CanOrderBy<ShipmentQs, fields::Shipment.line>,
              "QuerySet::order_by must REJECT a composite-PK FK — fails if the requires-clause is dropped");
static_assert(CanCountDistinct<MessageQs, fields::Message.sender>,
              "COUNT(DISTINCT) on a single-PK FK must still compile");
static_assert(!CanCountDistinct<ShipmentQs, fields::Shipment.line>,
              "QuerySet::count_distinct must REJECT a composite-PK FK — fails if the requires-clause is dropped");
static_assert(CanCount<MessageQs, fields::Message.sender>, "count() on a single-PK FK must still compile");
static_assert(!CanCount<ShipmentQs, fields::Shipment.line>, "count() must REJECT a composite-PK FK (#611)");
static_assert(CanDistinct<MessageQs, fields::Message.sender>, "distinct() on a single-PK FK must still compile");
static_assert(!CanDistinct<ShipmentQs, fields::Shipment.line>, "distinct() must REJECT a composite-PK FK (#611)");
static_assert(CanValues<MessageQs, fields::Message.sender>, "values() on a single-PK FK must still compile");
static_assert(!CanValues<ShipmentQs, fields::Shipment.line>, "values() must REJECT a composite-PK FK (#611)");
static_assert(CanGroupedCount<MessageQs, fields::Message.value, fields::Message.sender>, "single-PK FK ok");
static_assert(!CanGroupedCount<ShipmentQs, fields::Shipment.line, fields::Shipment.carrier>,
              "QuerySet::group_by must REJECT a composite-PK FK (#611)");
static_assert(!CanGroupedCount<ShipmentQs, fields::Shipment.carrier, fields::Shipment.line>,
              "GroupByBuilder::count must REJECT a composite-PK FK (#611)");
static_assert(CanChainedCount<MessageQs, fields::Message.sender, fields::Message.sender>, "single-PK FK ok");
static_assert(!CanChainedCount<ShipmentQs, fields::Shipment.carrier, fields::Shipment.line>,
              "chained AggregateStatement::count must REJECT a composite-PK FK (#611)");

// ── Non-FK regression: SQL must stay byte-identical ─────────────────────────

// The ~50 in-tree models are non-FK; the rewrite must not touch a single byte
// of their emitted SQL. These pin the exact strings for a plain column at each
// of the corrected writers.
template <typename ConnType> class NonFkSqlParityTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(NonFkSqlParityTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(NonFkSqlParityTest, OrderByPlainMemberSqlUnchanged) {
    QuerySet<Person, TypeParam> qs;
    const auto asc = storm::test::is_postgresql<TypeParam>() ? "ASC NULLS FIRST" : "ASC";
    const auto desc = storm::test::is_postgresql<TypeParam>() ? "DESC NULLS LAST" : "DESC";

    EXPECT_EQ(qs.template order_by<fields::Person.name>().select().sql(),
              std::format("SELECT id, name, age, salary, is_active, years_experience, department, score, nickname,"
                          " avatar FROM Person ORDER BY name {}",
                          asc));
    // Bound to a local first: the comma inside order_by<..., false> would be
    // parsed as a macro-argument separator by EXPECT_EQ.
    const auto desc_sql = qs.template order_by<fields::Person.age, false>().select().sql();
    EXPECT_EQ(desc_sql,
              std::format("SELECT id, name, age, salary, is_active, years_experience, department, score, nickname,"
                          " avatar FROM Person ORDER BY age {}",
                          desc));
}

TYPED_TEST(NonFkSqlParityTest, WherePlainMemberSqlUnchanged) {
    QuerySet<Person, TypeParam> qs;
    EXPECT_EQ(qs.where(fields::Person.age == 30).select().sql(),
              "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
              " FROM Person WHERE age = ?");
    EXPECT_EQ(qs.where(fields::Person.age.collate(Collate::NoCase) == 30).select().sql(),
              "SELECT id, name, age, salary, is_active, years_experience, department, score, nickname, avatar"
              " FROM Person WHERE age COLLATE NOCASE = ?");
}

TYPED_TEST(NonFkSqlParityTest, AggregateOnPlainMemberSqlUnchanged) {
    QuerySet<Person, TypeParam> qs;
    EXPECT_EQ(qs.template count_distinct<fields::Person.department>().sql(),
              "SELECT COUNT(DISTINCT department) FROM Person");
    EXPECT_EQ(qs.template sum<fields::Person.age>().sql(), "SELECT SUM(age) FROM Person");
    // Two-field SUM: bound to a local so the comma is not a macro separator.
    const auto sum_two = qs.template sum<fields::Person.age, fields::Person.years_experience>().sql();
    EXPECT_EQ(sum_two, "SELECT SUM(age + years_experience) FROM Person");
    EXPECT_EQ(qs.template avg<fields::Person.salary>().sql(), "SELECT AVG(salary) FROM Person");
    EXPECT_EQ(qs.template min<fields::Person.age>().sql(), "SELECT MIN(age) FROM Person");
    EXPECT_EQ(qs.template max<fields::Person.age>().sql(), "SELECT MAX(age) FROM Person");
}

TYPED_TEST(NonFkSqlParityTest, GroupByPlainMemberSqlUnchanged) {
    QuerySet<Person, TypeParam> qs;
    EXPECT_EQ(qs.template group_by<fields::Person.department>().count().sql(),
              "SELECT department, COUNT(*) FROM Person GROUP BY department");
    // Bound to a local first: the comma inside group_by<..., ...> would be
    // parsed as a macro-argument separator by EXPECT_EQ.
    const auto two_key_sql = qs.template group_by<fields::Person.department, fields::Person.age>().count().sql();
    EXPECT_EQ(two_key_sql, "SELECT department, age, COUNT(*) FROM Person GROUP BY department, age");
}

} // namespace

// NOLINTEND(misc-const-correctness)
