#include <gtest/gtest.h>
#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness,performance-unnecessary-value-param)

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

#include "../../shared/models/extended_types.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

using std::chrono::day;
using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::month;
using std::chrono::seconds;
using std::chrono::sys_days;
using std::chrono::system_clock;
using std::chrono::year;
using std::chrono::year_month_day;
using storm::UUID;

namespace {

    auto make_tp(int yer, unsigned mon, unsigned dom, int hrs = 0, int min = 0, int sec = 0)
            -> system_clock::time_point {
        return sys_days{year_month_day{year{yer}, month{mon}, day{dom}}} + hours{hrs} + minutes{min} + seconds{sec};
    }

} // namespace

// Fixture: seeds three ExtendedTypes rows with distinct date / datetime / UUID values.
//   r1: 2024-01-01, datetime 2024-01-01 08:00:00, uuid ...0001
//   r2: 2024-06-15, datetime 2024-06-15 12:30:00, uuid ...0002
//   r3: 2024-12-31, datetime 2024-12-31 23:59:59, uuid ...0003
template <typename ConnType> class WhereTemporalTest : public StormTestFixture<ExtendedTypes, ConnType> {
  public:
    static constexpr const char* kUuid1 = "00000000-0000-4000-8000-000000000001";
    static constexpr const char* kUuid2 = "00000000-0000-4000-8000-000000000002";
    static constexpr const char* kUuid3 = "00000000-0000-4000-8000-000000000003";

  protected:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        std::vector<ExtendedTypes> batch = {
                {.label          = "r1",
                 .date_field     = year_month_day{year{2024}, month{1}, day{1}},
                 .datetime_field = make_tp(2024, 1, 1, 8, 0, 0),
                 .uuid_field     = UUID{kUuid1}},
                {.label          = "r2",
                 .date_field     = year_month_day{year{2024}, month{6}, day{15}},
                 .datetime_field = make_tp(2024, 6, 15, 12, 30, 0),
                 .uuid_field     = UUID{kUuid2}},
                {.label          = "r3",
                 .date_field     = year_month_day{year{2024}, month{12}, day{31}},
                 .datetime_field = make_tp(2024, 12, 31, 23, 59, 59),
                 .uuid_field     = UUID{kUuid3}},
        };
        ASSERT_TRUE((InsertStatement<ExtendedTypes, ConnType>{}).query(batch).execute().has_value());
    }

    static auto count_where(auto expr) -> std::size_t {
        auto result = (SelectStatement<ExtendedTypes, ConnType>{}).where(expr).execute();
        EXPECT_TRUE(result.has_value()) << "WHERE failed: " << result.error().message();
        return result.has_value() ? result.value().size() : 0;
    }
};

TYPED_TEST_SUITE(WhereTemporalTest, DatabaseTypes);

// ===== year_month_day: all 6 comparison operators =====

TYPED_TEST(WhereTemporalTest, DateEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.date_field == year_month_day{year{2024}, month{6}, day{15}}), 1U);
}
TYPED_TEST(WhereTemporalTest, DateNotEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.date_field != year_month_day{year{2024}, month{6}, day{15}}), 2U);
}
TYPED_TEST(WhereTemporalTest, DateGreater) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.date_field > year_month_day{year{2024}, month{6}, day{15}}), 1U);
}
TYPED_TEST(WhereTemporalTest, DateGreaterEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.date_field >= year_month_day{year{2024}, month{6}, day{15}}), 2U);
}
TYPED_TEST(WhereTemporalTest, DateLess) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.date_field < year_month_day{year{2024}, month{6}, day{15}}), 1U);
}
TYPED_TEST(WhereTemporalTest, DateLessEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.date_field <= year_month_day{year{2024}, month{6}, day{15}}), 2U);
}

// ===== year_month_day: BETWEEN + IN =====

TYPED_TEST(WhereTemporalTest, DateBetween) {
    EXPECT_EQ(
            this->count_where(
                    fields::ExtendedTypes.date_field.between(
                            year_month_day{year{2024}, month{3}, day{1}}, year_month_day{year{2024}, month{9}, day{1}}
                    )
            ),
            1U
    );
}
TYPED_TEST(WhereTemporalTest, DateIn) {
    EXPECT_EQ(
            this->count_where(
                    fields::ExtendedTypes.date_field.in(
                            year_month_day{year{2024}, month{1}, day{1}}, year_month_day{year{2024}, month{12}, day{31}}
                    )
            ),
            2U
    );
}

// ===== system_clock::time_point: all 6 comparison operators =====

TYPED_TEST(WhereTemporalTest, DatetimeEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.datetime_field == make_tp(2024, 6, 15, 12, 30, 0)), 1U);
}
TYPED_TEST(WhereTemporalTest, DatetimeNotEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.datetime_field != make_tp(2024, 6, 15, 12, 30, 0)), 2U);
}
TYPED_TEST(WhereTemporalTest, DatetimeGreater) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.datetime_field > make_tp(2024, 6, 15, 12, 30, 0)), 1U);
}
TYPED_TEST(WhereTemporalTest, DatetimeGreaterEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.datetime_field >= make_tp(2024, 6, 15, 12, 30, 0)), 2U);
}
TYPED_TEST(WhereTemporalTest, DatetimeLess) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.datetime_field < make_tp(2024, 6, 15, 12, 30, 0)), 1U);
}
TYPED_TEST(WhereTemporalTest, DatetimeLessEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.datetime_field <= make_tp(2024, 6, 15, 12, 30, 0)), 2U);
}

// ===== system_clock::time_point: BETWEEN + IN =====

TYPED_TEST(WhereTemporalTest, DatetimeBetween) {
    EXPECT_EQ(
            this->count_where(
                    fields::ExtendedTypes.datetime_field
                            .between(make_tp(2024, 3, 1, 0, 0, 0), make_tp(2024, 9, 1, 0, 0, 0))
            ),
            1U
    );
}
TYPED_TEST(WhereTemporalTest, DatetimeIn) {
    EXPECT_EQ(
            this->count_where(
                    fields::ExtendedTypes.datetime_field
                            .in(make_tp(2024, 1, 1, 8, 0, 0), make_tp(2024, 12, 31, 23, 59, 59))
            ),
            2U
    );
}

// ===== UUID: equality + IN (no ordering / BETWEEN) =====

TYPED_TEST(WhereTemporalTest, UuidEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.uuid_field == UUID{TestFixture::kUuid2}), 1U);
}
TYPED_TEST(WhereTemporalTest, UuidNotEqual) {
    EXPECT_EQ(this->count_where(fields::ExtendedTypes.uuid_field != UUID{TestFixture::kUuid2}), 2U);
}
TYPED_TEST(WhereTemporalTest, UuidIn) {
    EXPECT_EQ(
            this->count_where(
                    fields::ExtendedTypes.uuid_field.in(UUID{TestFixture::kUuid1}, UUID{TestFixture::kUuid3})
            ),
            2U
    );
}

// ===== Combined temporal + logical =====

TYPED_TEST(WhereTemporalTest, DateRangeAndLogical) {
    EXPECT_EQ(
            this->count_where(
                    fields::ExtendedTypes.date_field >= year_month_day{year{2024}, month{1}, day{1}} &&
                    fields::ExtendedTypes.date_field < year_month_day{year{2024}, month{12}, day{31}}
            ),
            2U
    );
}

// NOLINTEND(misc-const-correctness,performance-unnecessary-value-param)
