#include <gtest/gtest.h>
#include <meta>
#include "test_db_helpers.h"
#include <sqlite3.h>

import storm;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::auto_update;
using storm::field_specs_for;
using storm::primary;
using storm::QuerySet;
using storm::UUID;
using storm::orm::statements::ConflictTargetUnique;
using storm::orm::statements::ReturnId;
using storm::orm::statements::UpsertSettable;

struct CompositeUpsertRecord {
    [[= primary]] int id{};
    std::string       name;
    std::string       department;
    int               age{};
};

// fields:: selector proxies (#518).
namespace fields {

    struct CompositeUpsertRecordT;
    consteval {
        std::meta::define_aggregate(^^CompositeUpsertRecordT, field_specs_for(^^CompositeUpsertRecord));
    }
    inline constexpr CompositeUpsertRecordT CompositeUpsertRecord{};

} // namespace fields

template <> struct storm::Indexes<CompositeUpsertRecord> {
    using type = std::tuple<storm::UniqueIndex<^^CompositeUpsertRecord::name, ^^CompositeUpsertRecord::department>>;
};

struct TimestampedUpsertRecord {
    [[= primary]] int                                              id{};
    [[= storm::unique]] std::string                                name;
    [[= storm::auto_create]] std::chrono::system_clock::time_point created_at{};
    [[= auto_update]] std::chrono::system_clock::time_point        updated_at{};
};

// fields:: selector proxies (#518).
namespace fields {
    struct TimestampedUpsertRecordT;
    consteval {
        std::meta::define_aggregate(^^TimestampedUpsertRecordT, field_specs_for(^^TimestampedUpsertRecord));
    }
    inline constexpr TimestampedUpsertRecordT TimestampedUpsertRecord{};
} // namespace fields

// The UUID-PK mirror of TimestampedUpsertRecord. A UUID key is caller data, so it
// occupies a VALUES placeholder like any other column (#565) — which moves where the
// DO UPDATE auto_update tail starts binding. Any count that re-derives the placeholder
// total arithmetically instead of sharing the iterator that WRITES the placeholders
// goes stale here, binding now() over the last VALUES parameter and leaving the tail
// one unbound: PG rejects it, SQLite silently writes updated_at = NULL.
// No auto_create field: only the auto_update TAIL drives the offset under test, and
// the extra column would merely pad the VALUES list.
struct UuidTimestampedUpsertRecord {
    [[= primary]] UUID                                      id{};
    [[= storm::unique]] std::string                         label;
    [[= auto_update]] std::chrono::system_clock::time_point updated_at{};
};

// fields:: selector proxies (#518).
namespace fields {
    struct UuidTimestampedUpsertRecordT;
    consteval {
        std::meta::define_aggregate(^^UuidTimestampedUpsertRecordT, field_specs_for(^^UuidTimestampedUpsertRecord));
    }
    inline constexpr UuidTimestampedUpsertRecordT UuidTimestampedUpsertRecord{};
} // namespace fields

struct UniqueOwnerRecord {
    [[= primary]] int                           id{};
    [[= storm::unique]][[= storm::fk<>]] Person owner;
    std::string                                 label;
};

namespace {
    using storm::orm::statements::UpsertGrammar;
}

// Grammar emits a single-column conflict target via the FK-aware writer.
TEST(UpsertGrammarTest, ConflictTargetSingleColumn) {
    constexpr auto    target = UpsertGrammar<Person>::build_conflict_target<^^Person::name>();
    const std::string s(target);
    EXPECT_EQ(s, "(name)");
}

// Grammar emits col=excluded.col for each listed SET column.
TEST(UpsertGrammarTest, ExcludedSetClauseMultipleColumns) {
    constexpr auto    set = UpsertGrammar<Person>::build_excluded_set_clause<^^Person::age, ^^Person::salary>();
    const std::string s(set);
    EXPECT_EQ(s, "age=excluded.age, salary=excluded.salary");
}

TEST(UpsertGrammarTest, ForeignKeyColumnsUseIdSuffix) {
    constexpr auto target = UpsertGrammar<UniqueOwnerRecord>::build_conflict_target<^^UniqueOwnerRecord::owner>();
    constexpr auto set    = UpsertGrammar<UniqueOwnerRecord>::build_excluded_set_clause<^^UniqueOwnerRecord::owner>();
    EXPECT_EQ(std::string(target), "(owner_id)");
    EXPECT_EQ(std::string(set), "owner_id=excluded.owner_id");
}

// Full DO NOTHING statement for Person conflicting on name.
TEST(UpsertGrammarTest, FullSqlDoNothing) {
    const std::string& sql = UpsertGrammar<Person>::nothing_sql<^^Person::name>();
    EXPECT_TRUE(sql.starts_with("INSERT INTO Person ")) << sql;
    EXPECT_NE(sql.find("ON CONFLICT (name) DO NOTHING"), std::string::npos) << sql;
    EXPECT_TRUE(sql.ends_with("RETURNING id")) << sql;
}

// Full DO UPDATE statement, conflict on name, set age.
TEST(UpsertGrammarTest, FullSqlDoUpdate) {
    const std::string sql = UpsertGrammar<Person>::update_sql<^^Person::name>(
            UpsertGrammar<Person>::build_excluded_set_clause<^^Person::age>()
    );
    EXPECT_NE(sql.find("ON CONFLICT (name) DO UPDATE SET age=excluded.age"), std::string::npos) << sql;
    EXPECT_TRUE(sql.ends_with("RETURNING id")) << sql;
}

// Positive: a single unique field IS a valid conflict target.
static_assert(ConflictTargetUnique<Person, ^^Person::name>);
// Positive: the UniqueIndex<name, department> column set IS valid.
static_assert(ConflictTargetUnique<Person, ^^Person::name, ^^Person::department>);
// Negative: a non-unique column is NOT a valid conflict target.
static_assert(!ConflictTargetUnique<Person, ^^Person::age>);
// Negative: inserts omit the PK, so it cannot be a conflict target.
static_assert(!ConflictTargetUnique<Person, ^^Person::id>);
// Negative: the PK is NOT settable.
static_assert(!UpsertSettable<Person, ^^Person::id>);
// Positive: a normal column IS settable.
static_assert(UpsertSettable<Person, ^^Person::age>);

TEST(UpsertGrammarTest, ConstraintsCompile) {
    SUCCEED();
}

template <typename ConnType> class UpsertTest : public StormTestFixture<Person, ConnType> {};
TYPED_TEST_SUITE(UpsertTest, DatabaseTypes);

template <typename ConnType> class CompositeUpsertTest : public StormTestFixture<CompositeUpsertRecord, ConnType> {};
TYPED_TEST_SUITE(CompositeUpsertTest, DatabaseTypes);

// Seeds the initial "Zed" row via the DO NOTHING proxy and returns its id.
// Shared by both conflict tests below so each only adds its own conflicting insert.
template <typename ConnType> auto seed_zed(QuerySet<Person, ConnType>& qs) -> std::int64_t {
    Person const first{.name = "Zed", .age = 1, .department = "X"};
    auto         first_id = qs.insert(first).template on_conflict<fields::Person.name>().nothing().execute();
    EXPECT_TRUE(first_id.has_value());
    EXPECT_TRUE(first_id.value().has_value());
    return first_id.value().value();
}

template <typename ConnType> constexpr bool on_conflict_is_rvalue_only() {
    using Proxy = decltype(std::declval<storm::QuerySet<Person, ConnType>&>().insert(std::declval<const Person&>()));
    return !requires(Proxy& p) { p.template on_conflict<fields::Person.name>(); }&&
        requires(Proxy && p)
    {
        std::move(p).template on_conflict<fields::Person.name>();
    };
}

static_assert(on_conflict_is_rvalue_only<storm::db::sqlite::Connection>());

// DO NOTHING via the fluent proxy: first insert lands, the conflicting second
// insert is skipped (no row touched), leaving the original row untouched.
TYPED_TEST(UpsertTest, DoNothingSkipsOnConflict) {
    QuerySet<Person, TypeParam> qs;
    seed_zed(qs);

    Person const conflicting{.name = "Zed", .age = 99, .department = "Y"};
    auto         second = qs.insert(conflicting).template on_conflict<fields::Person.name>().nothing().execute();
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second.value().has_value()); // skipped — no row touched

    auto rows = qs.where(fields::Person.name == "Zed").select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_EQ(rows.value().begin()->age, 1); // untouched by the conflicting insert
}

// DO UPDATE via the fluent proxy: the conflicting insert overwrites the listed
// column (age) on the existing row.
TYPED_TEST(UpsertTest, DoUpdateOverwritesListedColumn) {
    QuerySet<Person, TypeParam> qs;
    const std::int64_t          first_id = seed_zed(qs);

    Person const conflicting{.name = "Zed", .age = 99, .department = "Y"};
    auto         updated_id = qs.insert(conflicting)
                              .template on_conflict<fields::Person.name>()
                              .template update<fields::Person.age>()
                              .execute();
    ASSERT_TRUE(updated_id.has_value());
    EXPECT_EQ(updated_id.value(), first_id);

    auto rows = qs.where(fields::Person.name == "Zed").select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_EQ(rows.value().begin()->age, 99); // overwritten by the conflicting insert
}

// .sql() golden — DO NOTHING shape via the fluent proxy.
TYPED_TEST(UpsertTest, SqlGoldenNothing) {
    QuerySet<Person, TypeParam> qs;
    Person const                row{.name = "Q", .age = 1, .department = "X"};
    const std::string           sql = qs.insert(row).template on_conflict<fields::Person.name>().nothing().sql();
    EXPECT_NE(sql.find("ON CONFLICT (name) DO NOTHING"), std::string::npos) << sql;
}

// Composite conflict target via UniqueIndex<name, department>.
TYPED_TEST(CompositeUpsertTest, CompositeConflictTarget) {
    QuerySet<CompositeUpsertRecord, TypeParam> qs;
    CompositeUpsertRecord const                rec{.name = "Cara", .department = "Eng", .age = 20};
    auto                                       first = qs.insert(rec)
                         .template on_conflict<
                                 fields::CompositeUpsertRecord.name,
                                 fields::CompositeUpsertRecord.department>()
                         .template update<fields::CompositeUpsertRecord.age>()
                         .execute();
    ASSERT_TRUE(first.has_value()) << first.error().message();
    CompositeUpsertRecord const upd{.name = "Cara", .department = "Eng", .age = 21};
    auto                        second = qs.insert(upd)
                          .template on_conflict<
                                  fields::CompositeUpsertRecord.name,
                                  fields::CompositeUpsertRecord.department>()
                          .template update<fields::CompositeUpsertRecord.age>()
                          .execute();
    ASSERT_TRUE(second.has_value()) << second.error().message();
    EXPECT_EQ(second.value(), first.value());
}

// DO UPDATE with multiple SET columns.
TYPED_TEST(UpsertTest, DoUpdateMultipleColumns) {
    QuerySet<Person, TypeParam> qs;
    Person const                rec{.name = "Dan", .age = 10, .salary = 100.0, .department = "Eng"};
    auto                        first = qs.insert(rec)
                         .template on_conflict<fields::Person.name>()
                         .template update<fields::Person.age, fields::Person.salary>()
                         .execute();
    ASSERT_TRUE(first.has_value()) << first.error().message();
    Person const upd{.name = "Dan", .age = 11, .salary = 200.0, .department = "Eng"};
    auto         r = qs.insert(upd)
                     .template on_conflict<fields::Person.name>()
                     .template update<fields::Person.age, fields::Person.salary>()
                     .execute();
    ASSERT_TRUE(r.has_value()) << r.error().message();
    EXPECT_EQ(r.value(), first.value());
    auto row = qs.where(fields::Person.name == std::string("Dan")).select().execute();
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row.value().begin()->age, 11);
    EXPECT_DOUBLE_EQ(row.value().begin()->salary, 200.0);
}

// to_sql() inlines the bound params (debug helper).
TYPED_TEST(UpsertTest, ToSqlInlinesParams) {
    QuerySet<Person, TypeParam> qs;
    Person const                rec{.name = "Eve", .age = 5, .department = "X"};
    auto                        r = qs.insert(rec).template on_conflict<fields::Person.name>().nothing().to_sql();
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(r.value().find("ON CONFLICT (name) DO NOTHING"), std::string::npos) << r.value();
}

// auto_update (#209) is re-stamped on a DO UPDATE upsert. The unique name
// forces the second insert to hit the existing row; the auto_update updated_at
// column is bound now() afresh, so the stored value advances. Timestamps are
// bind-time-only (no write-back), so re-SELECT to read.
template <typename ConnType> class UpsertTimestampTest : public StormTestFixture<TimestampedUpsertRecord, ConnType> {};
TYPED_TEST_SUITE(UpsertTimestampTest, DatabaseTypes);

TYPED_TEST(UpsertTimestampTest, DoUpdateRefreshesAutoUpdateTimestamp) {
    QuerySet<TimestampedUpsertRecord, TypeParam> qs;

    TimestampedUpsertRecord const seed{.name = "record"};
    auto                          inserted = qs.insert(seed)
                            .template on_conflict<fields::TimestampedUpsertRecord.name>()
                            .template update<fields::TimestampedUpsertRecord.name>()
                            .execute();
    ASSERT_TRUE(inserted.has_value());

    auto before = qs.where(fields::TimestampedUpsertRecord.name == "record").select().execute();
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before.value().size(), 1U);
    const auto first_stamp = before.value().begin()->updated_at;

    // Sleep one second: the stored format has 1s resolution (tp_to_string), so a
    // sub-second gap would round to the same string and falsely fail the advance check.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    TimestampedUpsertRecord const conflicting{.name = "record"};
    auto                          updated = qs.insert(conflicting)
                           .template on_conflict<fields::TimestampedUpsertRecord.name>()
                           .template update<fields::TimestampedUpsertRecord.name>()
                           .execute();
    ASSERT_TRUE(updated.has_value());

    auto after = qs.where(fields::TimestampedUpsertRecord.name == "record").select().execute();
    ASSERT_TRUE(after.has_value());
    ASSERT_EQ(after.value().size(), 1U);
    EXPECT_GT(after.value().begin()->updated_at, first_stamp); // updated_at advanced
}

// .sql() golden — the auto_update tail inside the FULL assembled statement (#542).
// The clause is asserted in isolation by AutoUpdateSetTail in test_composite_pk_sql.cpp;
// this pins that it lands correctly in the complete INSERT ... ON CONFLICT text.
TYPED_TEST(UpsertTimestampTest, AutoUpdateTailInFullUpsertSql) {
    QuerySet<TimestampedUpsertRecord, TypeParam> qs;
    TimestampedUpsertRecord const                row{.name = "record"};
    const std::string                            sql = qs.insert(row)
                                    .template on_conflict<fields::TimestampedUpsertRecord.name>()
                                    .template update<fields::TimestampedUpsertRecord.name>()
                                    .sql();
    EXPECT_NE(sql.find("DO UPDATE SET name=excluded.name, updated_at=?"), std::string::npos) << sql;
}

// A UUID PK occupies a VALUES placeholder (#565), so the DO UPDATE auto_update tail
// starts one slot later than on an int-PK model. The failure is asymmetric and neither
// half is a thrown error: PG rejects the unbound trailing parameter, while SQLite binds
// it NULL and silently writes updated_at = NULL — so this asserts the stamp ADVANCED
// rather than merely that the statement succeeded.
template <typename ConnType>
class UuidUpsertTimestampTest : public StormTestFixture<UuidTimestampedUpsertRecord, ConnType> {};
TYPED_TEST_SUITE(UuidUpsertTimestampTest, DatabaseTypes);

TYPED_TEST(UuidUpsertTimestampTest, DoUpdateRefreshesAutoUpdateTimestampWithUuidPk) {
    QuerySet<UuidTimestampedUpsertRecord, TypeParam> qs;
    // A UUID key is caller-supplied, so nothing is RETURNed on any of these paths (#572) —
    // ReturnId::No is now what plain insert() resolves to, spelled out here to keep this
    // test's subject (the auto_update bind offset) independent of that default.
    using storm::orm::statements::ReturnId;
    const UUID key{"44444444-4444-4444-8444-444444444444"};

    auto inserted = qs.template insert<ReturnId::No>(UuidTimestampedUpsertRecord{.id = key, .label = "rec"})
                            .template on_conflict<fields::UuidTimestampedUpsertRecord.label>()
                            .template update<fields::UuidTimestampedUpsertRecord.label>()
                            .execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    auto before = qs.where(fields::UuidTimestampedUpsertRecord.label == "rec").select().execute();
    ASSERT_TRUE(before.has_value()) << before.error().message();
    ASSERT_EQ(before.value().size(), 1U);
    const auto first_stamp = before.value().begin()->updated_at;
    // The key itself must have survived the INSERT — a stale placeholder count also
    // shifts which column each VALUES parameter lands in.
    EXPECT_EQ(before.value().begin()->id.value, key.value);

    // 1s resolution in the stored format (tp_to_string): a sub-second gap would round
    // to the same string and falsely fail the advance check.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    auto updated = qs.template insert<ReturnId::No>(UuidTimestampedUpsertRecord{.id = key, .label = "rec"})
                           .template on_conflict<fields::UuidTimestampedUpsertRecord.label>()
                           .template update<fields::UuidTimestampedUpsertRecord.label>()
                           .execute();
    ASSERT_TRUE(updated.has_value()) << updated.error().message();

    auto after = qs.where(fields::UuidTimestampedUpsertRecord.label == "rec").select().execute();
    ASSERT_TRUE(after.has_value()) << after.error().message();
    ASSERT_EQ(after.value().size(), 1U);
    EXPECT_GT(after.value().begin()->updated_at, first_stamp); // updated_at advanced, not NULLed
}
