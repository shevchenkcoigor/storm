#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import std;

#include "../../shared/models/simple_record.h"
#include "../../shared/models/timestamped_record.h"
#include "test_fixture.h"
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateStatement;

// readability-implicit-bool-conversion: false positive from GTest's EXPECT_TRUE(cond) << "msg"
// macro expansion (the streamed message literal is misattributed); same pattern is used
// throughout tests/ — see tests/mock_libpq/mock_libpq.cpp for the same file-level suppression.
// NOLINTBEGIN(misc-const-correctness,readability-implicit-bool-conversion)

using std::chrono::system_clock;

namespace {

    // Seconds-granularity epoch is what the TEXT/TIMESTAMP round-trip preserves
    // (tp_to_string emits "YYYY-MM-DD HH:MM:SS"). Compare at that resolution.
    auto to_seconds(system_clock::time_point tp) -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    }

    // A timestamp is "stamped" if it lands in a sane window around the test's now()
    // (not the zero-epoch default the object was constructed with).
    auto is_recent(system_clock::time_point tp, system_clock::time_point ref) -> bool {
        auto delta = std::chrono::abs(tp - ref);
        return delta < std::chrono::seconds{120};
    }

    // Insert a single record and SELECT the (only) row back. Returns the row by value.
    // Fails the calling test via fatal assertions if any step errors.
    template <typename ConnType> auto insert_and_read_one(TimestampedRecord const& obj) -> TimestampedRecord {
        auto inserted = InsertStatement<TimestampedRecord, ConnType>{}.query(obj).execute();
        [&] { ASSERT_TRUE(inserted.has_value()); }();
        auto selected = SelectStatement<TimestampedRecord, ConnType>{}.all().execute();
        [&] { ASSERT_TRUE(selected.has_value()); }();
        [&] { ASSERT_FALSE(selected.value().empty()); }();
        return *selected.value().begin();
    }

} // namespace

template <typename ConnType> class AutoTimestampTest : public StormTestFixture<TimestampedRecord, ConnType> {};

TYPED_TEST_SUITE(AutoTimestampTest, DatabaseTypes);

// ===== INSERT =====

TYPED_TEST(AutoTimestampTest, InsertSingleStampsBothFields) {
    auto before = system_clock::now();
    // Both timestamp fields left at zero-epoch default — must be overwritten with now().
    auto row = insert_and_read_one<TypeParam>(TimestampedRecord{.id = 0, .name = "alpha"});

    EXPECT_EQ(row.name, "alpha");
    EXPECT_TRUE(is_recent(row.created_at, before)) << "created_at not stamped on INSERT";
    EXPECT_TRUE(is_recent(row.updated_at, before)) << "updated_at not stamped on INSERT";
    // On INSERT both are stamped from the same batch now().
    EXPECT_EQ(to_seconds(row.created_at), to_seconds(row.updated_at));
}

TYPED_TEST(AutoTimestampTest, InsertIgnoresManualTimestampValues) {
    auto before = system_clock::now();
    // Manually set a clearly-stale 2001 value — design says auto fields always override.
    auto stale = system_clock::from_time_t(978'307'200); // 2001-01-01 00:00:00 UTC
    auto row   = insert_and_read_one<TypeParam>(
            TimestampedRecord{.id = 0, .name = "manual", .created_at = stale, .updated_at = stale}
    );

    EXPECT_TRUE(is_recent(row.created_at, before)) << "manual created_at not overridden";
    EXPECT_TRUE(is_recent(row.updated_at, before)) << "manual updated_at not overridden";
}

TYPED_TEST(AutoTimestampTest, InsertBatchSharesOneTimestampPerBatch) {
    auto                           before = system_clock::now();
    std::vector<TimestampedRecord> batch  = {
            {.id = 0, .name = "one"},
            {.id = 0, .name = "two"},
            {.id = 0, .name = "three"},
    };

    auto result = InsertStatement<TimestampedRecord, TypeParam>{}.query(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = SelectStatement<TimestampedRecord, TypeParam>{}.all().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 3);

    std::int64_t shared = -1;
    for (auto const& row : selected.value()) {
        EXPECT_TRUE(is_recent(row.created_at, before)) << "batch created_at not stamped";
        EXPECT_TRUE(is_recent(row.updated_at, before)) << "batch updated_at not stamped";
        // All rows in one batch share the same created_at second.
        if (shared == -1) {
            shared = to_seconds(row.created_at);
        }
        EXPECT_EQ(to_seconds(row.created_at), shared) << "batch rows do not share one now()";
    }
}

// ===== UPDATE =====

TYPED_TEST(AutoTimestampTest, UpdateStampsUpdatedAtPreservesCreatedAt) {
    auto inserted                = insert_and_read_one<TypeParam>(TimestampedRecord{.id = 0, .name = "before"});
    auto created_at_after_insert = inserted.created_at;

    // Wait so the UPDATE's now() is observably later than the INSERT's.
    std::this_thread::sleep_for(std::chrono::seconds{1});

    // The caller carries the original created_at in the object (no write-back needed):
    // auto_create on UPDATE binds the object's stored value, not now().
    TimestampedRecord const updated{
            .id = inserted.id, .name = "after", .created_at = created_at_after_insert, .updated_at = {}
    };
    auto update_result = UpdateStatement<TimestampedRecord, TypeParam>{}.query(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = SelectStatement<TimestampedRecord, TypeParam>{}.all().execute();
    ASSERT_TRUE(selected.has_value());
    auto const& row = *selected.value().begin();

    EXPECT_EQ(row.name, "after");
    EXPECT_EQ(to_seconds(row.created_at), to_seconds(created_at_after_insert))
            << "created_at must be preserved on UPDATE";
    EXPECT_GT(to_seconds(row.updated_at), to_seconds(created_at_after_insert)) << "updated_at must advance on UPDATE";
}

// ===== No-op: a model without timestamp fields is unaffected =====

TYPED_TEST(AutoTimestampTest, ModelWithoutTimestampFieldsUnaffected) {
    ASSERT_TRUE((storm::test::ensure_tables<TypeParam, SimpleRecord>()));
    SimpleRecord const obj{.id = 0, .name = "plain", .value = 42};

    auto result = InsertStatement<SimpleRecord, TypeParam>{}.query(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = SelectStatement<SimpleRecord, TypeParam>{}.all().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->value, 42);
}

// NOLINTEND(misc-const-correctness,readability-implicit-bool-conversion)
