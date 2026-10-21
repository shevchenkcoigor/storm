#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm;
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "../../shared/models/timestamped_record.h"
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::QuerySet;

// clang-tidy's readability-implicit-bool-conversion mis-fires on the GTest
// streamed-message idiom `EXPECT_*(...) << "literal"`. Band the whole file.
// NOLINTBEGIN(readability-implicit-bool-conversion)

// Conditional bulk UPDATE: qs.where(cond).update<members...>(proto)  (#403)
// Message (FK column) and TimestampedRecord (auto_update column) tables are created
// alongside Person so the FK / auto_update to_sql() shape tests can prepare statements.
template <typename ConnType>
class ConditionalUpdateTest : public StormTestFixture<Person, ConnType, Message, TimestampedRecord> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<Person> seeded = {
                {.id = 1, .name = "Alice", .age = 30, .salary = 50000.0, .is_active = true, .department = "Eng"},
                {.id = 2, .name = "Bob", .age = 25, .salary = 40000.0, .is_active = false, .department = "Eng"},
                {.id = 3, .name = "Charlie", .age = 35, .salary = 60000.0, .is_active = true, .department = "Sales"},
                {.id = 4, .name = "Dave", .age = 40, .salary = 70000.0, .is_active = false, .department = "Sales"},
                {.id = 5, .name = "Eve", .age = 45, .salary = 80000.0, .is_active = true, .department = "Legacy"},
        };
        seeded[0].score = 10;
        seeded[2].score = 30;
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(seeded)));
    }

    static auto countPersons() -> int {
        QuerySet<Person, ConnType> qs;
        auto                       result = qs.count().execute();
        return result.has_value() ? static_cast<int>(result.value()) : -1;
    }

    // Fetch a single person by id (asserts exactly one row).
    static auto getPerson(int person_id) -> Person {
        const QuerySet<Person, ConnType> qs;
        auto                             rows = qs.where(fields::Person.id == person_id).select().execute();
        EXPECT_TRUE(rows.has_value());
        EXPECT_EQ(rows.value().size(), 1U);
        return *rows.value().begin();
    }
};

TYPED_TEST_SUITE(ConditionalUpdateTest, DatabaseTypes);

TYPED_TEST(ConditionalUpdateTest, Setup) {
    EXPECT_EQ(this->countPersons(), 5);
}

// --- SET coverage: single column, multiple columns, each type --------------
TYPED_TEST(ConditionalUpdateTest, SingleSetColumn) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age == 30)
                        .template update<fields::Person.salary>(Person{.salary = 99000.0})
                        .execute()
                        .has_value());
    EXPECT_DOUBLE_EQ(this->getPerson(1).salary, 99000.0);
    EXPECT_DOUBLE_EQ(this->getPerson(2).salary, 40000.0) << "unmatched row unchanged";
}

TYPED_TEST(ConditionalUpdateTest, MultipleSetColumns) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.where(fields::Person.department == "Eng")
                          .template update<fields::Person.salary, fields::Person.is_active>(
                                  Person{.salary = 12345.0, .is_active = true}
                          )
                          .execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(this->getPerson(1).salary, 12345.0);
    EXPECT_DOUBLE_EQ(this->getPerson(2).salary, 12345.0);
    EXPECT_TRUE(this->getPerson(2).is_active) << "Bob flipped to active";
    EXPECT_DOUBLE_EQ(this->getPerson(3).salary, 60000.0) << "Sales row unchanged";
}

TYPED_TEST(ConditionalUpdateTest, SetIntColumn) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.id == 1)
                        .template update<fields::Person.age>(Person{.age = 77})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(1).age, 77);
}

TYPED_TEST(ConditionalUpdateTest, SetStringColumn) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.id == 1)
                        .template update<fields::Person.department>(Person{.department = "Relocated"})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(1).department, "Relocated");
}

TYPED_TEST(ConditionalUpdateTest, SetBoolColumn) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.id == 1)
                        .template update<fields::Person.is_active>(Person{.is_active = false})
                        .execute()
                        .has_value());
    EXPECT_FALSE(this->getPerson(1).is_active);
}

// --- WHERE coverage: 6 operators -------------------------------------------
TYPED_TEST(ConditionalUpdateTest, WhereEqual) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age == 30)
                        .template update<fields::Person.age>(Person{.age = 31})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(1).age, 31);
}

TYPED_TEST(ConditionalUpdateTest, WhereNotEqual) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age != 30)
                        .template update<fields::Person.department>(Person{.department = "X"})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(1).department, "Eng") << "age==30 unchanged";
    EXPECT_EQ(this->getPerson(2).department, "X");
}

TYPED_TEST(ConditionalUpdateTest, WhereGreater) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age > 35)
                        .template update<fields::Person.is_active>(Person{.is_active = false})
                        .execute()
                        .has_value());
    EXPECT_FALSE(this->getPerson(4).is_active);
    EXPECT_FALSE(this->getPerson(5).is_active);
    EXPECT_TRUE(this->getPerson(3).is_active) << "age 35 not > 35";
}

TYPED_TEST(ConditionalUpdateTest, WhereGreaterEqual) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age >= 35)
                        .template update<fields::Person.age>(Person{.age = 0})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(3).age, 0);
    EXPECT_EQ(this->getPerson(1).age, 30) << "age 30 untouched";
}

TYPED_TEST(ConditionalUpdateTest, WhereLess) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age < 30)
                        .template update<fields::Person.age>(Person{.age = 1})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(2).age, 1) << "Bob 25 updated";
    EXPECT_EQ(this->getPerson(1).age, 30) << "age 30 untouched";
}

TYPED_TEST(ConditionalUpdateTest, WhereLessEqual) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age <= 30)
                        .template update<fields::Person.age>(Person{.age = 2})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(1).age, 2);
    EXPECT_EQ(this->getPerson(2).age, 2);
    EXPECT_EQ(this->getPerson(3).age, 35) << "age 35 untouched";
}

// --- WHERE coverage: IN / BETWEEN / LIKE / NULL ----------------------------
TYPED_TEST(ConditionalUpdateTest, WhereIn) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age.in(25, 35))
                        .template update<fields::Person.is_active>(Person{.is_active = false})
                        .execute()
                        .has_value());
    EXPECT_FALSE(this->getPerson(2).is_active);
    EXPECT_FALSE(this->getPerson(3).is_active);
}

TYPED_TEST(ConditionalUpdateTest, WhereBetween) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age.between(30, 40))
                        .template update<fields::Person.salary>(Person{.salary = 1.0})
                        .execute()
                        .has_value());
    EXPECT_DOUBLE_EQ(this->getPerson(1).salary, 1.0);
    EXPECT_DOUBLE_EQ(this->getPerson(4).salary, 1.0);
    EXPECT_DOUBLE_EQ(this->getPerson(5).salary, 80000.0) << "age 45 untouched";
}

TYPED_TEST(ConditionalUpdateTest, WhereLike) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.department.like("Eng%"))
                        .template update<fields::Person.salary>(Person{.salary = 5.0})
                        .execute()
                        .has_value());
    EXPECT_DOUBLE_EQ(this->getPerson(1).salary, 5.0);
    EXPECT_DOUBLE_EQ(this->getPerson(2).salary, 5.0);
}

TYPED_TEST(ConditionalUpdateTest, WhereIsNull) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.score.is_null())
                        .template update<fields::Person.salary>(Person{.salary = 7.0})
                        .execute()
                        .has_value());
    EXPECT_DOUBLE_EQ(this->getPerson(2).salary, 7.0) << "NULL-score row updated";
    EXPECT_DOUBLE_EQ(this->getPerson(1).salary, 50000.0) << "non-NULL score untouched";
}

TYPED_TEST(ConditionalUpdateTest, WhereIsNotNull) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.score.is_not_null())
                        .template update<fields::Person.salary>(Person{.salary = 8.0})
                        .execute()
                        .has_value());
    EXPECT_DOUBLE_EQ(this->getPerson(1).salary, 8.0);
    EXPECT_DOUBLE_EQ(this->getPerson(3).salary, 8.0);
    EXPECT_DOUBLE_EQ(this->getPerson(2).salary, 40000.0) << "NULL score untouched";
}

// --- WHERE coverage: AND / OR / nested -------------------------------------
TYPED_TEST(ConditionalUpdateTest, WhereAnd) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.department == "Sales" && fields::Person.is_active == false)
                        .template update<fields::Person.age>(Person{.age = 100})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(4).age, 100) << "Dave (Sales, inactive)";
    EXPECT_EQ(this->getPerson(3).age, 35) << "Charlie (Sales, active) untouched";
}

TYPED_TEST(ConditionalUpdateTest, WhereOr) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.age < 26 || fields::Person.age > 44)
                        .template update<fields::Person.salary>(Person{.salary = 3.0})
                        .execute()
                        .has_value());
    EXPECT_DOUBLE_EQ(this->getPerson(2).salary, 3.0);
    EXPECT_DOUBLE_EQ(this->getPerson(5).salary, 3.0);
    EXPECT_DOUBLE_EQ(this->getPerson(1).salary, 50000.0);
}

TYPED_TEST(ConditionalUpdateTest, WhereNested) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where((fields::Person.department == "Eng" && fields::Person.age < 28) ||
                         fields::Person.salary > 75000.0)
                        .template update<fields::Person.age>(Person{.age = 9})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(2).age, 9) << "Bob (Eng, 25)";
    EXPECT_EQ(this->getPerson(5).age, 9) << "Eve (salary 80k)";
    EXPECT_EQ(this->getPerson(1).age, 30) << "Alice untouched";
}

// --- Chained where() (AND-combined) ----------------------------------------
TYPED_TEST(ConditionalUpdateTest, ChainedWhere) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(qs.where(fields::Person.department == "Eng")
                        .where(fields::Person.is_active == true)
                        .template update<fields::Person.age>(Person{.age = 88})
                        .execute()
                        .has_value());
    EXPECT_EQ(this->getPerson(1).age, 88) << "Alice (Eng, active)";
    EXPECT_EQ(this->getPerson(2).age, 25) << "Bob (Eng, inactive) untouched";
}

// --- Result-set sizes ------------------------------------------------------
TYPED_TEST(ConditionalUpdateTest, EmptyResultNoOp) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(
            qs.where(fields::Person.age > 1000)
                    .template update<fields::Person.age>(Person{.age = 1})
                    .execute()
                    .has_value()
    ) << "Matching nothing is a successful no-op";
    EXPECT_EQ(this->getPerson(1).age, 30) << "no rows changed";
}

TYPED_TEST(ConditionalUpdateTest, LargeResultSet) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         batch;
    batch.reserve(150);
    for (int i = 100; i < 250; ++i) {
        batch.emplace_back(Person{.id = i, .name = std::format("Bulk{}", i), .age = 50, .department = "Bulk"});
    }
    ASSERT_TRUE(qs.insert(std::span<const Person>(batch)).execute().has_value());
    ASSERT_TRUE(qs.where(fields::Person.department == "Bulk")
                        .template update<fields::Person.age>(Person{.age = 51})
                        .execute()
                        .has_value());
    // INSERT auto-assigns PKs (explicit ids ignored), so verify by re-SELECTing the
    // "Bulk" rows: all 150 must now have age 51, and none should retain the seed age 50.
    auto bulk = qs.where(fields::Person.department == "Bulk").select().execute();
    ASSERT_TRUE(bulk.has_value());
    EXPECT_EQ(bulk.value().size(), 150U);
    for (const auto& p : bulk.value()) {
        EXPECT_EQ(p.age, 51);
    }
}

// --- Safety: empty WHERE refuses the full-table write ----------------------
TYPED_TEST(ConditionalUpdateTest, EmptyWhereIsRefused) {
    QuerySet<Person, TypeParam> qs;
    auto                        result = qs.template update<fields::Person.age>(Person{.age = 0}).execute();
    ASSERT_FALSE(result.has_value()) << "update<...>() with no where() must refuse";
    // The refusal message must point at update_all() — the method that actually exists (#409).
    EXPECT_TRUE(result.error().message().contains("update_all()")) << result.error().message();
    EXPECT_EQ(this->getPerson(1).age, 30) << "Table must be untouched";
}

TYPED_TEST(ConditionalUpdateTest, ToSqlEmptyWhereRefused) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template update<fields::Person.age>(Person{.age = 0}).to_sql();
    EXPECT_FALSE(sql.has_value()) << "to_sql() on an unfiltered update<...>() must also refuse";
}

// --- to_sql() shape --------------------------------------------------------
TYPED_TEST(ConditionalUpdateTest, ToSqlShape) {
    QuerySet<Person, TypeParam> qs;
    auto sql = qs.where(fields::Person.age > 30).template update<fields::Person.salary>(Person{.salary = 1.0}).to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("UPDATE")) << sql.value();
    EXPECT_TRUE(sql.value().contains("SET")) << sql.value();
    EXPECT_TRUE(sql.value().contains("salary")) << sql.value();
    EXPECT_TRUE(sql.value().contains("WHERE")) << sql.value();
}

// --- FK SET column: emits <name>_id ----------------------------------------
TYPED_TEST(ConditionalUpdateTest, FkSetColumnToSql) {
    QuerySet<Message, TypeParam> mqs;
    auto                         sql = mqs.where(fields::Message.id == 1)
                       .template update<fields::Message.sender>(Message{.sender = Person{.id = 7}})
                       .to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("sender_id")) << sql.value();
}

// --- auto_update field auto-included ---------------------------------------
TYPED_TEST(ConditionalUpdateTest, AutoUpdateFieldIncludedInSql) {
    QuerySet<TimestampedRecord, TypeParam> tqs;
    auto                                   sql = tqs.where(fields::TimestampedRecord.id == 1)
                       .template update<fields::TimestampedRecord.name>(TimestampedRecord{.name = "renamed"})
                       .to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("name")) << sql.value();
    EXPECT_TRUE(sql.value().contains("updated_at")) << sql.value() << "auto_update field must be auto-appended to SET";
}

// --- auto_update field actually advances on execute() ----------------------
TYPED_TEST(ConditionalUpdateTest, AutoUpdateFieldAdvancesOnExecute) {
    using std::chrono::system_clock;
    QuerySet<TimestampedRecord, TypeParam> tqs;
    // Seed one row with a stale updated_at far in the past.
    const auto stale = system_clock::time_point{};
    ASSERT_TRUE(tqs.insert(TimestampedRecord{.id = 0, .name = "before", .created_at = stale, .updated_at = stale})
                        .execute()
                        .has_value());
    // Conditional UPDATE of an unrelated column must still refresh updated_at (auto-appended).
    ASSERT_TRUE(tqs.where(fields::TimestampedRecord.name == "before")
                        .template update<fields::TimestampedRecord.name>(TimestampedRecord{.name = "after"})
                        .execute()
                        .has_value());
    auto rows = tqs.where(fields::TimestampedRecord.name == "after").select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    const auto& row = *rows.value().begin();
    EXPECT_GT(row.updated_at.time_since_epoch().count(), stale.time_since_epoch().count())
            << "auto_update column must advance past the stale value";
}

// --- model without timestamp: no extra column ------------------------------
TYPED_TEST(ConditionalUpdateTest, NoTimestampNoExtraColumn) {
    QuerySet<Person, TypeParam> qs;
    auto sql = qs.where(fields::Person.id == 1).template update<fields::Person.age>(Person{.age = 5}).to_sql();
    ASSERT_TRUE(sql.has_value());
    // The SET segment (between "SET " and " WHERE") has exactly one assignment and no
    // comma — Person has no auto_update field, so nothing extra is appended.
    const auto& s         = sql.value();
    const auto  set_pos   = s.find("SET ");
    const auto  where_pos = s.find(" WHERE");
    ASSERT_NE(set_pos, std::string::npos) << s;
    ASSERT_NE(where_pos, std::string::npos) << s;
    const auto set_clause = s.substr(set_pos + 4, where_pos - (set_pos + 4));
    EXPECT_FALSE(set_clause.contains(',')) << "no extra column appended: " << set_clause;
    EXPECT_TRUE(set_clause.contains("age")) << set_clause;
}

// --- update_all(): explicit full-table write (#409) ------------------------
// Symmetry with erase_all(): the documented escape hatch for an unfiltered update.
TYPED_TEST(ConditionalUpdateTest, UpdateAllWritesEveryRow) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE(
            qs.template update_all<fields::Person.department>(Person{.department = "Global"}).execute().has_value()
    );
    // Every seeded row must now carry the new department.
    for (int id = 1; id <= 5; ++id) {
        EXPECT_EQ(this->getPerson(id).department, "Global") << "row " << id << " not updated";
    }
}

TYPED_TEST(ConditionalUpdateTest, UpdateAllMultipleColumns) {
    QuerySet<Person, TypeParam> qs;
    ASSERT_TRUE((qs.template update_all<fields::Person.age, fields::Person.is_active>(Person{.age       = 99,
                                                                                             .is_active = true})
                         .execute()
                         .has_value()));
    for (int id = 1; id <= 5; ++id) {
        EXPECT_EQ(this->getPerson(id).age, 99) << "row " << id;
        EXPECT_TRUE(this->getPerson(id).is_active) << "row " << id;
    }
}

TYPED_TEST(ConditionalUpdateTest, UpdateAllToSqlHasNoWhere) {
    QuerySet<Person, TypeParam> qs;
    auto                        sql = qs.template update_all<fields::Person.salary>(Person{.salary = 1.0}).to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("UPDATE")) << sql.value();
    EXPECT_TRUE(sql.value().contains("SET")) << sql.value();
    EXPECT_TRUE(sql.value().contains("salary")) << sql.value();
    EXPECT_FALSE(sql.value().contains("WHERE")) << "update_all() must emit no WHERE: " << sql.value();
}

// auto_update column is auto-appended and stamped on the full-table write too.
TYPED_TEST(ConditionalUpdateTest, UpdateAllStampsAutoUpdate) {
    using std::chrono::system_clock;
    QuerySet<TimestampedRecord, TypeParam> tqs;
    const auto                             stale = system_clock::time_point{};
    ASSERT_TRUE(tqs.insert(TimestampedRecord{.id = 0, .name = "seed", .created_at = stale, .updated_at = stale})
                        .execute()
                        .has_value());
    auto sql = tqs.template update_all<fields::TimestampedRecord.name>(TimestampedRecord{.name = "renamed"}).to_sql();
    ASSERT_TRUE(sql.has_value());
    EXPECT_TRUE(sql.value().contains("updated_at")) << sql.value() << "auto_update must be auto-appended";

    ASSERT_TRUE(tqs.template update_all<fields::TimestampedRecord.name>(TimestampedRecord{.name = "renamed"})
                        .execute()
                        .has_value());
    auto rows = tqs.where(fields::TimestampedRecord.name == "renamed").select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    EXPECT_GT(rows.value().begin()->updated_at.time_since_epoch().count(), stale.time_since_epoch().count())
            << "auto_update column must advance past the stale value";
}

// NOLINTEND(readability-implicit-bool-conversion)
