#include <gtest/gtest.h>
#include "test_db_helpers.h"

#include <numbers>

// NOLINTBEGIN(misc-const-correctness)

import storm;
import std;

using storm::db::DefaultConnection;

#include "../../shared/models/color.h"
#include "../../shared/models/extended_types.h"
#include "../../shared/models/person.h"
#include "../../shared/models/set_null_child.h"
#include "../../shared/models/simple_record.h"
#include "test_fixture.h"
using storm::create_table_sql;
using storm::full_unsigned;
using storm::primary;
using storm::RefAction;
using storm::signed_storage;
using storm::UUID;
using storm::orm::statements::ModelStorageAnnotated;

using std::chrono::day;
using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::month;
using std::chrono::seconds;
using std::chrono::sys_days;
using std::chrono::system_clock;
using std::chrono::year;
using std::chrono::year_month_day;
using storm::QuerySet;

// ===== INTEGER TYPES TESTS =====

template <typename ConnType> class IntTypesInsertUpdateTest : public StormTestFixture<ExtendedTypes, ConnType> {};

TYPED_TEST_SUITE(IntTypesInsertUpdateTest, DatabaseTypes);

TYPED_TEST(IntTypesInsertUpdateTest, InsertSingleIntTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .big_num = 9223372036854775807LL, .ll_signed = 32767LL};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 1);
    EXPECT_EQ(selected.value().begin()->big_num, 9223372036854775807LL);
    EXPECT_EQ(selected.value().begin()->ll_signed, 32767LL);
}

TYPED_TEST(IntTypesInsertUpdateTest, InsertBatchIntTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.id = 0, .big_num = 100LL, .ll_signed = 10LL},
            {.id = 0, .big_num = 200LL, .ll_signed = 20LL},
            {.id = 0, .big_num = 300LL, .ll_signed = 30LL},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    // Batch insert returns void, verify via SELECT
    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 3);
    EXPECT_EQ(selected.value().begin()->big_num, 100LL);
}

TYPED_TEST(IntTypesInsertUpdateTest, UpdateSingleIntTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .big_num = 100LL, .ll_signed = 10LL};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    // Verify INSERT worked
    auto check1 = qs.select().execute();
    ASSERT_TRUE(check1.has_value());
    auto it1 = check1.value().begin();
    EXPECT_EQ(it1->big_num, 100LL);
    EXPECT_EQ(it1->ll_signed, 10LL);

    // Update with the returned ID
    ExtendedTypes const updated{.id = static_cast<int>(id), .big_num = 999LL, .ll_signed = 99LL};
    auto                update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto it2 = selected.value().begin();
    EXPECT_EQ(it2->big_num, 999LL);
    EXPECT_EQ(it2->ll_signed, 99LL);
}

TYPED_TEST(IntTypesInsertUpdateTest, UpdateBatchIntTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.id = 1, .big_num = 100LL, .ll_signed = 10LL},
            {.id = 2, .big_num = 200LL, .ll_signed = 20LL},
            {.id = 3, .big_num = 300LL, .ll_signed = 30LL},
    };

    auto insert_result = qs.insert(batch).execute();
    ASSERT_TRUE(insert_result.has_value());

    // Update all
    std::vector<ExtendedTypes> updates = {
            {.id = 1, .big_num = 111LL, .ll_signed = 11LL},
            {.id = 2, .big_num = 222LL, .ll_signed = 22LL},
            {.id = 3, .big_num = 333LL, .ll_signed = 33LL},
    };
    auto update_result = qs.update(std::span<const ExtendedTypes>(updates)).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto it = selected.value().begin();
    EXPECT_EQ(it->big_num, 111LL);
    ++it;
    EXPECT_EQ(it->big_num, 222LL);
    ++it;
    EXPECT_EQ(it->big_num, 333LL);
}

// ===== FLOATING POINT TYPES TESTS =====

template <typename ConnType> class FloatTypesInsertUpdateTest : public StormTestFixture<ExtendedTypes, ConnType> {};

TYPED_TEST_SUITE(FloatTypesInsertUpdateTest, DatabaseTypes);

TYPED_TEST(FloatTypesInsertUpdateTest, InsertSingleFloatTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .precise = std::numbers::pi, .approx = std::numbers::e_v<float>};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_NEAR(selected.value().begin()->precise, std::numbers::pi, 1e-10);
    EXPECT_NEAR(selected.value().begin()->approx, std::numbers::e_v<float>, 1e-6);
}

TYPED_TEST(FloatTypesInsertUpdateTest, UpdateSingleFloatTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .precise = 1.0, .approx = 1.0F};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    ExtendedTypes const updated{
            .id = static_cast<int>(id), .precise = std::numbers::e, .approx = std::numbers::pi_v<float>
    };
    auto update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_NEAR(selected.value().begin()->precise, std::numbers::e, 1e-10);
    EXPECT_NEAR(selected.value().begin()->approx, std::numbers::pi_v<float>, 1e-6);
}

// ===== BOOLEAN AND STRING TYPES TESTS =====

template <typename ConnType> class MixedTypesInsertUpdateTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(MixedTypesInsertUpdateTest, DatabaseTypes);

TYPED_TEST(MixedTypesInsertUpdateTest, InsertBooleanTrue) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "active_user", .is_active = true};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_TRUE(selected.value().begin()->is_active);
    EXPECT_EQ(selected.value().begin()->name, "active_user");
}

TYPED_TEST(MixedTypesInsertUpdateTest, InsertBooleanFalse) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "inactive_user", .is_active = false};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->is_active);
    EXPECT_EQ(selected.value().begin()->name, "inactive_user");
}

TYPED_TEST(MixedTypesInsertUpdateTest, UpdateBooleanAndString) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "old_name", .is_active = false};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    Person const updated{.id = static_cast<int>(id), .name = "new_name", .is_active = true};
    auto         update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_TRUE(selected.value().begin()->is_active);
    EXPECT_EQ(selected.value().begin()->name, "new_name");
}

TYPED_TEST(MixedTypesInsertUpdateTest, InsertBatchMixedTypes) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         batch = {
            {.id = 1, .name = "user1", .is_active = true},
            {.id = 2, .name = "user2", .is_active = false},
            {.id = 3, .name = "user3", .is_active = true},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 3);
    auto it = selected.value().begin();
    EXPECT_TRUE(it->is_active);
    ++it;
    EXPECT_FALSE(it->is_active);
    ++it;
    EXPECT_EQ(it->name, "user3");
}

// ===== NULL TEXT EXTRACTION =====

template <typename ConnType> class NullTextExtractionTest : public StormTestFixture<SimpleRecord, ConnType> {};

TYPED_TEST_SUITE(NullTextExtractionTest, DatabaseTypes);

// A SQL NULL in a TEXT column extracts into a non-optional std::string as an
// empty string (read_text_view nullptr branch). The generated schema declares
// name TEXT NOT NULL, so rebuild the table without the constraint to let a
// NULL reach the extraction path.
TYPED_TEST(NullTextExtractionTest, NullTextColumnExtractsAsEmptyString) {
    auto conn = DefaultConnection<TypeParam>::get();
    ASSERT_TRUE(conn->execute("DROP TABLE SimpleRecord").has_value());
    ASSERT_TRUE(
            conn->execute("CREATE TABLE SimpleRecord (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)").has_value()
    );
    ASSERT_TRUE(conn->execute("INSERT INTO SimpleRecord (id, name, value) VALUES (1, NULL, 42)").has_value());

    QuerySet<SimpleRecord, TypeParam> qs;
    auto                              result = qs.select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1);
    const auto it = result.value().begin();
    EXPECT_EQ(it->name, "");
    EXPECT_EQ(it->value, 42);
}

// ===== OPTIONAL TYPES TESTS =====

template <typename ConnType> class OptTypesInsertUpdateTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(OptTypesInsertUpdateTest, DatabaseTypes);

TYPED_TEST(OptTypesInsertUpdateTest, InsertWithValues) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "with_value", .score = std::optional<int>(42)};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->score.has_value());
    EXPECT_EQ(selected.value().begin()->score.value(), 42);
    EXPECT_EQ(selected.value().begin()->name, "with_value");
}

TYPED_TEST(OptTypesInsertUpdateTest, InsertWithNull) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "null_value", .score = std::nullopt};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->score.has_value());
    EXPECT_EQ(selected.value().begin()->name, "null_value");
}

TYPED_TEST(OptTypesInsertUpdateTest, UpdateFromValueToNull) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "original", .score = std::optional<int>(100)};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    // Update to NULL
    Person const updated{.id = static_cast<int>(id), .name = "updated_to_null", .score = std::nullopt};
    auto         update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->score.has_value());
    EXPECT_EQ(selected.value().begin()->name, "updated_to_null");
}

TYPED_TEST(OptTypesInsertUpdateTest, UpdateFromNullToValue) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "null_start", .score = std::nullopt};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    // Update from NULL to value
    Person const updated{.id = static_cast<int>(id), .name = "updated_to_value", .score = std::optional<int>(999)};
    auto         update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->score.has_value());
    EXPECT_EQ(selected.value().begin()->score.value(), 999);
    EXPECT_EQ(selected.value().begin()->name, "updated_to_value");
}

TYPED_TEST(OptTypesInsertUpdateTest, InsertBatchMixedNulls) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         batch = {
            {.id = 1, .name = "has_value", .score = std::optional<int>(1)},
            {.id = 2, .name = "is_null", .score = std::nullopt},
            {.id = 3, .name = "has_value2", .score = std::optional<int>(3)},
            {.id = 4, .name = "is_null2", .score = std::nullopt},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 4);

    // Verify mixed NULL/value pattern
    auto it = selected.value().begin();
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 1);
    ++it;
    EXPECT_FALSE(it->score.has_value());
    ++it;
    EXPECT_TRUE(it->score.has_value());
    EXPECT_EQ(it->score.value(), 3);
    ++it;
    EXPECT_FALSE(it->score.has_value());
}

// ===== BLOB TYPES TESTS =====

template <typename ConnType> class DataTypesInsertUpdateTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(DataTypesInsertUpdateTest, DatabaseTypes);

TYPED_TEST(DataTypesInsertUpdateTest, InsertSmallBlob) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "test_blob", .avatar = {0xDE, 0xAD, 0xBE, 0xEF}};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->avatar, (std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
    EXPECT_EQ(selected.value().begin()->name, "test_blob");
}

TYPED_TEST(DataTypesInsertUpdateTest, InsertLargeBlob) {
    QuerySet<Person, TypeParam> qs;

    std::vector<std::uint8_t> large_data(1024);
    for (std::size_t i = 0; i < large_data.size(); ++i) {
        large_data[i] = static_cast<std::uint8_t>(i % 256);
    }

    Person const obj{.id = 0, .name = "large", .avatar = large_data};
    auto         result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto it = selected.value().begin();
    ASSERT_EQ(it->avatar.size(), 1024);

    for (std::size_t i = 0; i < 1024; ++i) {
        EXPECT_EQ(it->avatar[i], static_cast<std::uint8_t>(i % 256));
    }
}

TYPED_TEST(DataTypesInsertUpdateTest, InsertEmptyBlob) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "empty", .avatar = {}};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_TRUE(selected.value().begin()->avatar.empty());
}

TYPED_TEST(DataTypesInsertUpdateTest, UpdateBlob) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "original", .avatar = {0x01, 0x02}};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    // Update with different blob
    Person const updated{.id = static_cast<int>(id), .name = "updated", .avatar = {0xFF, 0xEE, 0xDD}};
    auto         update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->avatar, (std::vector<std::uint8_t>{0xFF, 0xEE, 0xDD}));
    EXPECT_EQ(selected.value().begin()->name, "updated");
}

TYPED_TEST(DataTypesInsertUpdateTest, InsertBatchBlobs) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         batch = {
            {.id = 1, .name = "blob1", .avatar = {0x01}},
            {.id = 2, .name = "blob2", .avatar = {0x02, 0x03}},
            {.id = 3, .name = "blob3", .avatar = {0x04, 0x05, 0x06}},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 3);
    auto it = selected.value().begin();
    EXPECT_EQ(it->avatar.size(), 1);
    ++it;
    EXPECT_EQ(it->avatar.size(), 2);
    ++it;
    EXPECT_EQ(it->avatar.size(), 3);
}

// ===== EXTREME VALUE TESTS =====

TYPED_TEST(IntTypesInsertUpdateTest, ExtremeIntegerValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;

    // Min values
    ExtendedTypes const min_obj{.id = 0, .big_num = -9223372036854775807LL - 1, .ll_signed = -32768LL};
    auto                result = qs.insert(min_obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto it = selected.value().begin();
    EXPECT_EQ(it->big_num, -9223372036854775807LL - 1);
    EXPECT_EQ(it->ll_signed, -32768LL);
}

TYPED_TEST(FloatTypesInsertUpdateTest, SpecialFloatValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;

    // Zero, negative, very small
    ExtendedTypes const obj{.id = 0, .precise = 0.0, .approx = -0.0F};
    auto                result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_DOUBLE_EQ(selected.value().begin()->precise, 0.0);
}

// ===== INSERT OPTIONS TESTS =====

template <typename ConnType> class InsertOptionsTest : public StormTestFixture<ExtendedTypes, ConnType> {};

TYPED_TEST_SUITE(InsertOptionsTest, DatabaseTypes);

TYPED_TEST(InsertOptionsTest, BatchInsertReturnsVoid) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.id = 0, .big_num = 100LL, .ll_signed = 10LL},
            {.id = 0, .big_num = 200LL, .ll_signed = 20LL},
            {.id = 0, .big_num = 300LL, .ll_signed = 30LL},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    // Verify data was inserted by selecting
    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 3);
}

TYPED_TEST(InsertOptionsTest, InsertWithCustomBatchSize) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch;

    // Create 100 objects
    batch.reserve(100);
    for (int i = 0; i < 100; ++i) {
        batch.emplace_back(
                ExtendedTypes{
                        .id = 0, .big_num = static_cast<std::int64_t>(i) * 10, .ll_signed = static_cast<long long>(i)
                }
        );
    }

    // Use batch_size of 10
    auto result = qs.insert(batch, {{.batch_size = 10}}).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 100);
}

TYPED_TEST(InsertOptionsTest, InsertBatchSizeCappedToMax) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.id = 0, .big_num = 100LL, .ll_signed = 10LL},
            {.id = 0, .big_num = 200LL, .ll_signed = 20LL},
    };

    // ExtendedTypes has 8 non-PK fields, so max = 999/8 = 124
    // Request batch_size of 1000, should be capped to 124
    auto result = qs.insert(batch, {{.batch_size = 1000}}).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 2);
}

TYPED_TEST(InsertOptionsTest, SingleInsertReturnsId) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .big_num = 999LL, .ll_signed = 99LL};

    // Single insert still returns the auto-generated ID
    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 1);
    EXPECT_EQ(selected.value().begin()->big_num, 999LL);
}

TYPED_TEST(InsertOptionsTest, LargeBatchWithCustomChunkSize) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch;

    // Create 1000 objects
    batch.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
        batch.emplace_back(
                ExtendedTypes{
                        .id = 0, .big_num = static_cast<std::int64_t>(i), .ll_signed = static_cast<long long>(i % 100)
                }
        );
    }

    // Use small batch_size to force multiple chunks
    auto result = qs.insert(batch, {{.batch_size = 50}}).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 1000);
}

TYPED_TEST(InsertOptionsTest, OptionsWithOnlyBatchSize) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.id = 0, .big_num = 100LL, .ll_signed = 10LL},
            {.id = 0, .big_num = 200LL, .ll_signed = 20LL},
    };

    // Only specify batch_size
    auto result = qs.insert(batch, {{.batch_size = 1}}).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 2);
}

// ===== UNSIGNED INTEGER TYPES TESTS =====

template <typename ConnType> class UnsignedTypesInsertUpdateTest : public StormTestFixture<ExtendedTypes, ConnType> {};

TYPED_TEST_SUITE(UnsignedTypesInsertUpdateTest, DatabaseTypes);

TYPED_TEST(UnsignedTypesInsertUpdateTest, InsertUnsignedValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .u_int = 4294967295U};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 1);
    EXPECT_EQ(selected.value().begin()->u_int, 4294967295U);
}

TYPED_TEST(UnsignedTypesInsertUpdateTest, InsertBatchUnsigned) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.id = 0, .u_int = 100U},
            {.id = 0, .u_int = 200U},
            {.id = 0, .u_int = 300U},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 3);
}

TYPED_TEST(UnsignedTypesInsertUpdateTest, UpdateUnsignedValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .u_int = 100U};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    ExtendedTypes const updated{.id = static_cast<int>(id), .u_int = 999U};
    auto                update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->u_int, 999U);
}

// ===== LONG LONG TYPES TESTS =====

template <typename ConnType> class LongLongTypesInsertUpdateTest : public StormTestFixture<ExtendedTypes, ConnType> {};

TYPED_TEST_SUITE(LongLongTypesInsertUpdateTest, DatabaseTypes);

TYPED_TEST(LongLongTypesInsertUpdateTest, InsertLongLongValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .ll_signed = 9223372036854775807LL};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 1);
    EXPECT_EQ(selected.value().begin()->ll_signed, 9223372036854775807LL);
}

TYPED_TEST(LongLongTypesInsertUpdateTest, InsertNegativeLongLong) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .ll_signed = -9223372036854775807LL};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->ll_signed, -9223372036854775807LL);
}

TYPED_TEST(LongLongTypesInsertUpdateTest, UpdateLongLongValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .ll_signed = 100LL};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    ExtendedTypes const updated{.id = static_cast<int>(id), .ll_signed = -999LL};
    auto                update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->ll_signed, -999LL);
}

// Note: WHERE clause tests with unsigned types are not included because the
// ExpressionVariant doesn't support unsigned types directly. INSERT/UPDATE work
// with unsigned types through bind_parameter_value which casts them appropriately.

// ===== ERROR HANDLING TESTS =====

// Test Error struct accessors for code coverage
TEST(ErrorHandlingTest, ErrorAccessors) {
    // Try to open a database in a non-existent directory (should fail)
    auto result = DefaultConnection<>::set("/nonexistent/path/database.db");

    // Verify operation failed
    ASSERT_FALSE(result.has_value()) << "Should fail to open database in non-existent path";

    // Test Error accessors (coverage for Error::code() and Error::message())
    const auto& error = result.error();
    EXPECT_NE(error.code(), 0) << "Error code should be non-zero for failed operation";
    EXPECT_FALSE(error.message().empty()) << "Error message should not be empty";

    // Verify message is meaningful
    std::string_view msg = error.message();
    EXPECT_GT(msg.size(), 5) << "Error message should be descriptive";
}

// Test select on non-existent table
TEST(ErrorHandlingTest, SelectFromNonExistentTable) {
    // Set up in-memory database (no table created)
    auto conn_result = DefaultConnection<>::set(storm::test::get_connection_string());
    ASSERT_TRUE(conn_result.has_value());

    // Try to select from non-existent table
    QuerySet<ExtendedTypes> qs;
    auto                    select_result = qs.select().execute();

    // Should fail because table doesn't exist
    ASSERT_FALSE(select_result.has_value()) << "Should fail to select from non-existent table";

    // Test error accessors
    const auto& error = select_result.error();
    EXPECT_NE(error.code(), 0);
    EXPECT_FALSE(error.message().empty());

    DefaultConnection<>::clear();
}

// =============================================================================
// Additional Optional Type Coverage (from test_coverage_gaps.cpp)
// =============================================================================

// NOLINTBEGIN(readability-identifier-length,readability-uppercase-literal-suffix,modernize-use-std-numbers)

template <typename ConnType> class OptionalTypesTest : public StormTestFixture<ExtendedTypes, ConnType> {};

TYPED_TEST_SUITE(OptionalTypesTest, DatabaseTypes);

TYPED_TEST(OptionalTypesTest, OptionalDoubleWithValue) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .opt_double = std::numbers::pi, .label = "pi"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->opt_double.has_value());
    EXPECT_NEAR(selected.value().begin()->opt_double.value(), 3.14159, 0.0001);
}

TYPED_TEST(OptionalTypesTest, OptionalDoubleNull) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .opt_double = std::nullopt, .label = "null_double"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->opt_double.has_value());
}

TYPED_TEST(OptionalTypesTest, OptionalDoubleBatch) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.id = 0, .opt_double = 1.1, .label = "first"},
            {.id = 0, .opt_double = std::nullopt, .label = "second"},
            {.id = 0, .opt_double = 2.2, .label = "third"},
            {.id = 0, .opt_double = std::nullopt, .label = "fourth"},
    };

    auto result = qs.insert(std::span<const ExtendedTypes>(batch)).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 4);

    auto it = selected.value().begin();
    EXPECT_TRUE(it->opt_double.has_value());
    ++it;
    EXPECT_FALSE(it->opt_double.has_value());
    ++it;
    EXPECT_TRUE(it->opt_double.has_value());
    ++it;
    EXPECT_FALSE(it->opt_double.has_value());
}

TYPED_TEST(OptionalTypesTest, OptionalInt64WithValue) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .opt_int64 = 9223372036854775807LL, .label = "max_int64"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->opt_int64.has_value());
    EXPECT_EQ(selected.value().begin()->opt_int64.value(), 9223372036854775807LL);
}

TYPED_TEST(OptionalTypesTest, OptionalInt64Null) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .opt_int64 = std::nullopt, .label = "null_int64"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->opt_int64.has_value());
}

TYPED_TEST(OptionalTypesTest, UpdateOptionalDoubleToNull) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .opt_double = 100.5, .label = "to_null"};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    ExtendedTypes const updated{.id = static_cast<int>(id), .opt_double = std::nullopt, .label = "now_null"};
    auto                update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->opt_double.has_value());
}

TYPED_TEST(OptionalTypesTest, UpdateOptionalInt64FromNull) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes const                obj{.id = 0, .opt_int64 = std::nullopt, .label = "from_null"};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    ExtendedTypes const updated{.id = static_cast<int>(id), .opt_int64 = 42LL, .label = "now_has_value"};
    auto                update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->opt_int64.has_value());
    EXPECT_EQ(selected.value().begin()->opt_int64.value(), 42LL);
}

// =============================================================================
// Optional String (nickname) Type Coverage
// =============================================================================

template <typename ConnType> class OptionalStringTest : public StormTestFixture<Person, ConnType> {};

TYPED_TEST_SUITE(OptionalStringTest, DatabaseTypes);

TYPED_TEST(OptionalStringTest, InsertWithValue) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "alice", .nickname = "Ali"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->nickname.has_value());
    EXPECT_EQ(selected.value().begin()->nickname.value(), "Ali");
}

TYPED_TEST(OptionalStringTest, InsertWithNull) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "bob", .nickname = std::nullopt};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->nickname.has_value());
}

TYPED_TEST(OptionalStringTest, UpdateFromValueToNull) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "charlie", .nickname = "Chuck"};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    Person const updated{.id = static_cast<int>(id), .name = "charlie", .nickname = std::nullopt};
    auto         update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->nickname.has_value());
}

TYPED_TEST(OptionalStringTest, UpdateFromNullToValue) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "diana", .nickname = std::nullopt};

    auto insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    Person const updated{.id = static_cast<int>(id), .name = "diana", .nickname = "Di"};
    auto         update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->nickname.has_value());
    EXPECT_EQ(selected.value().begin()->nickname.value(), "Di");
}

TYPED_TEST(OptionalStringTest, InsertBatchMixedNulls) {
    QuerySet<Person, TypeParam> qs;
    std::vector<Person>         batch = {
            {.id = 1, .name = "p1", .nickname = "Nick1"},
            {.id = 2, .name = "p2", .nickname = std::nullopt},
            {.id = 3, .name = "p3", .nickname = "Nick3"},
            {.id = 4, .name = "p4", .nickname = std::nullopt},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 4);

    auto it = selected.value().begin();
    ASSERT_TRUE(it->nickname.has_value());
    EXPECT_EQ(it->nickname.value(), "Nick1");
    ++it;
    EXPECT_FALSE(it->nickname.has_value());
    ++it;
    ASSERT_TRUE(it->nickname.has_value());
    EXPECT_EQ(it->nickname.value(), "Nick3");
    ++it;
    EXPECT_FALSE(it->nickname.has_value());
}

TYPED_TEST(OptionalStringTest, InsertEmptyString) {
    QuerySet<Person, TypeParam> qs;
    Person const                obj{.id = 0, .name = "eve", .nickname = std::string("")};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->nickname.has_value());
    EXPECT_EQ(selected.value().begin()->nickname.value(), "");
}

// =============================================================================
// Unsigned Integer Type Coverage (from test_coverage_gaps.cpp)
// =============================================================================

template <typename ConnType> class UnsignedTypesTest : public StormTestFixture<ExtendedTypes, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        qs = std::make_unique<QuerySet<ExtendedTypes, ConnType>>();
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<ExtendedTypes, ConnType>::TearDown();
    }

    std::unique_ptr<QuerySet<ExtendedTypes, ConnType>> qs;
};

TYPED_TEST_SUITE(UnsignedTypesTest, DatabaseTypes);

TYPED_TEST(UnsignedTypesTest, InsertMaxValues) {
    ExtendedTypes const obj{
            .id    = 0,
            .u_int = std::numeric_limits<unsigned int>::max(),
    };

    auto result = this->qs->insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = this->qs->select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->u_int, std::numeric_limits<unsigned int>::max());
}

TYPED_TEST(UnsignedTypesTest, InsertZeroValues) {
    ExtendedTypes const obj{.id = 0, .u_int = 0};

    auto result = this->qs->insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = this->qs->select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->u_int, 0u);
}

TYPED_TEST(UnsignedTypesTest, BatchUnsignedTypes) {
    std::vector<ExtendedTypes> batch = {
            {.id = 0, .u_int = 100},
            {.id = 0, .u_int = 200},
            {.id = 0, .u_int = 300},
    };

    auto result = this->qs->insert(std::span<const ExtendedTypes>(batch)).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = this->qs->select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 3);
}

TYPED_TEST(UnsignedTypesTest, UpdateUnsignedTypes) {
    ExtendedTypes const obj{.id = 0, .u_int = 100};

    auto insert_result = this->qs->insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t const id = insert_result.value();

    ExtendedTypes const updated{.id = static_cast<int>(id), .u_int = 999};
    auto                update_result = this->qs->update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = this->qs->select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->u_int, 999u);
}

// =============================================================================
// Float Type Coverage (from test_coverage_gaps.cpp)
// =============================================================================

template <typename ConnType> class FloatTypeTest : public StormTestFixture<ExtendedTypes, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>&) -> void override {
        qs = std::make_unique<QuerySet<ExtendedTypes, ConnType>>();
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<ExtendedTypes, ConnType>::TearDown();
    }

    std::unique_ptr<QuerySet<ExtendedTypes, ConnType>> qs;
};

TYPED_TEST_SUITE(FloatTypeTest, DatabaseTypes);

TYPED_TEST(FloatTypeTest, InsertFloatValue) {
    ExtendedTypes const obj{.id = 0, .approx = std::numbers::pi_v<float>, .label = "pi"};

    auto result = this->qs->insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = this->qs->select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_NEAR(selected.value().begin()->approx, 3.14159f, 0.0001f);
}

TYPED_TEST(FloatTypeTest, InsertNegativeFloat) {
    ExtendedTypes const obj{.id = 0, .approx = -123.456f, .label = "negative"};

    auto result = this->qs->insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = this->qs->select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_NEAR(selected.value().begin()->approx, -123.456f, 0.001f);
}

TYPED_TEST(FloatTypeTest, BatchFloatValues) {
    std::vector<ExtendedTypes> batch = {
            {.id = 0, .approx = 1.1f, .label = "first"},
            {.id = 0, .approx = 2.2f, .label = "second"},
            {.id = 0, .approx = 3.3f, .label = "third"},
    };

    auto result = this->qs->insert(std::span<const ExtendedTypes>(batch)).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = this->qs->select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 3);
}

// ===== CHAR TYPES TESTS =====

template <typename ConnType> class CharTypesTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(CharTypesTest, DatabaseTypes);

TYPED_TEST(CharTypesTest, InsertAndSelectCharTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes obj{.label = "char_test", .tiny_signed = -42, .tiny_unsigned = 200, .single_char = 'Z'};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 1);
    auto it = selected.value().begin();
    EXPECT_EQ(it->tiny_signed, -42);
    EXPECT_EQ(it->tiny_unsigned, 200);
    EXPECT_EQ(it->single_char, 'Z');
}

TYPED_TEST(CharTypesTest, MinMaxCharValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{
                                 .label         = "minmax",
                                 .tiny_signed   = -128, // SCHAR_MIN
                                 .tiny_unsigned = 255,  // UCHAR_MAX
                                 .single_char   = '\0'
    };

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto it = selected.value().begin();
    EXPECT_EQ(it->tiny_signed, -128);
    EXPECT_EQ(it->tiny_unsigned, 255);
    EXPECT_EQ(it->single_char, '\0');
}

TYPED_TEST(CharTypesTest, UpdateCharTypes) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "upd", .tiny_signed = 10, .tiny_unsigned = 20, .single_char = 'A'};
    auto                               insert_result = qs.insert(obj).execute();
    ASSERT_TRUE(insert_result.has_value());
    std::int64_t id = insert_result.value();

    ExtendedTypes updated{
            .id = static_cast<int>(id), .label = "upd", .tiny_signed = -10, .tiny_unsigned = 30, .single_char = 'B'
    };
    auto update_result = qs.update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto it = selected.value().begin();
    EXPECT_EQ(it->tiny_signed, -10);
    EXPECT_EQ(it->tiny_unsigned, 30);
    EXPECT_EQ(it->single_char, 'B');
}

// ===== ENUM TYPES TESTS =====

template <typename ConnType> class EnumTypesTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(EnumTypesTest, DatabaseTypes);

TYPED_TEST(EnumTypesTest, InsertAndSelectEnum) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "enum_test", .color = Color::Blue};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->color, Color::Blue);
}

TYPED_TEST(EnumTypesTest, InsertAllEnumValues) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.label = "red", .color = Color::Red},
            {.label = "green", .color = Color::Green},
            {.label = "blue", .color = Color::Blue},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 3);
    auto it = selected.value().begin();
    EXPECT_EQ(it->color, Color::Red);
    ++it;
    EXPECT_EQ(it->color, Color::Green);
    ++it;
    EXPECT_EQ(it->color, Color::Blue);
}

TYPED_TEST(EnumTypesTest, OptionalEnumNull) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "opt_null"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->opt_color.has_value());
}

TYPED_TEST(EnumTypesTest, OptionalEnumWithValue) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "opt_val", .opt_color = Color::Green};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->opt_color.has_value());
    EXPECT_EQ(selected.value().begin()->opt_color.value(), Color::Green);
}

TYPED_TEST(EnumTypesTest, WhereEnumEqual) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.label = "r", .color = Color::Red},
            {.label = "g", .color = Color::Green},
            {.label = "b", .color = Color::Blue},
    };
    auto insert_result = qs.insert(batch).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto selected = qs.where(fields::ExtendedTypes.color == Color::Green).select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 1);
    EXPECT_EQ(selected.value().begin()->label, "g");
}

TYPED_TEST(EnumTypesTest, WhereEnumNotEqual) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.label = "r", .color = Color::Red},
            {.label = "g", .color = Color::Green},
            {.label = "b", .color = Color::Blue},
    };
    auto insert_result = qs.insert(batch).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto selected = qs.where(fields::ExtendedTypes.color != Color::Red).select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 2);
}

TYPED_TEST(EnumTypesTest, WhereEnumBetween) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.label = "r", .color = Color::Red},
            {.label = "g", .color = Color::Green},
            {.label = "b", .color = Color::Blue},
    };
    auto insert_result = qs.insert(batch).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto selected = qs.where(fields::ExtendedTypes.color.between(Color::Green, Color::Blue)).select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().size(), 2);
}

// ===== CHRONO DATE TESTS =====

template <typename ConnType> class ChronoDateTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(ChronoDateTest, DatabaseTypes);

TYPED_TEST(ChronoDateTest, InsertAndSelectDate) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    auto                               ymd = year_month_day{year{2026}, month{3}, day{21}};
    ExtendedTypes                      obj{.label = "date_test", .date_field = ymd};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto stored = selected.value().begin()->date_field;
    EXPECT_EQ(stored.year(), year{2026});
    EXPECT_EQ(stored.month(), month{3});
    EXPECT_EQ(stored.day(), day{21});
}

TYPED_TEST(ChronoDateTest, InsertAndSelectDatetime) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    auto          tp = sys_days{year_month_day{year{2024}, month{12}, day{25}}} + hours{14} + minutes{30} + seconds{45};
    ExtendedTypes obj{.label = "dt_test", .datetime_field = tp};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    auto stored_tp = selected.value().begin()->datetime_field;
    EXPECT_EQ(stored_tp, tp);
}

TYPED_TEST(ChronoDateTest, InsertAndSelectDuration) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "dur_test", .duration_field = seconds{3600}};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->duration_field, seconds{3600});
}

TYPED_TEST(ChronoDateTest, ZeroDuration) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "zero_dur", .duration_field = seconds{0}};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->duration_field, seconds{0});
}

TYPED_TEST(ChronoDateTest, EpochDatetime) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    auto                               epoch = system_clock::time_point{};
    ExtendedTypes                      obj{.label = "epoch", .datetime_field = epoch};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->datetime_field, epoch);
}

TYPED_TEST(ChronoDateTest, OptionalTimestampNull) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "opt_ts_null"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->opt_timestamp.has_value());
}

TYPED_TEST(ChronoDateTest, OptionalTimestampWithValue) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    auto                               tp = sys_days{year_month_day{year{2025}, month{6}, day{15}}} + hours{10};
    ExtendedTypes                      obj{.label = "opt_ts_val", .opt_timestamp = tp};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->opt_timestamp.has_value());
    EXPECT_EQ(selected.value().begin()->opt_timestamp.value(), tp);
}

TYPED_TEST(ChronoDateTest, BatchChronoInsert) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.label = "d1", .date_field = year_month_day{year{2024}, month{1}, day{1}}, .duration_field = seconds{60}},
            {.label          = "d2",
                     .date_field     = year_month_day{year{2024}, month{6}, day{15}},
                     .duration_field = seconds{120}},
            {.label          = "d3",
                     .date_field     = year_month_day{year{2024}, month{12}, day{31}},
                     .duration_field = seconds{180}},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 3);
}

// ===== FILESYSTEM PATH TESTS =====

template <typename ConnType> class PathTypesTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(PathTypesTest, DatabaseTypes);

TYPED_TEST(PathTypesTest, InsertAndSelectPath) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes obj{.label = "path_test", .file_path = std::filesystem::path("/home/user/file.txt")};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->file_path, std::filesystem::path("/home/user/file.txt"));
}

TYPED_TEST(PathTypesTest, EmptyPath) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "empty_path", .file_path = std::filesystem::path{}};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_TRUE(selected.value().begin()->file_path.empty());
}

TYPED_TEST(PathTypesTest, OptionalPathNull) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "opt_path_null"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_FALSE(selected.value().begin()->opt_path.has_value());
}

TYPED_TEST(PathTypesTest, OptionalPathWithValue) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "opt_path_val", .opt_path = std::filesystem::path("/tmp/test")};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_TRUE(selected.value().begin()->opt_path.has_value());
    EXPECT_EQ(selected.value().begin()->opt_path.value(), std::filesystem::path("/tmp/test"));
}

// ===== VECTOR<BYTE> TESTS =====

template <typename ConnType> class ByteVectorTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(ByteVectorTest, DatabaseTypes);

TYPED_TEST(ByteVectorTest, InsertAndSelectByteVector) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<std::byte>             data = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    ExtendedTypes                      obj{.label = "byte_test", .raw_data = data};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->raw_data, data);
}

TYPED_TEST(ByteVectorTest, EmptyByteVector) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "empty_bytes"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_TRUE(selected.value().begin()->raw_data.empty());
}

// ===== UUID TESTS =====

template <typename ConnType> class UUIDTypesTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(UUIDTypesTest, DatabaseTypes);

TYPED_TEST(UUIDTypesTest, InsertAndSelectUUID) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes obj{.label = "uuid_test", .uuid_field = UUID{"550e8400-e29b-41d4-a716-446655440000"}};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->uuid_field.value, "550e8400-e29b-41d4-a716-446655440000");
}

TYPED_TEST(UUIDTypesTest, EmptyUUIDAutoGeneratesOnInsert) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "empty_uuid"};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());

    const auto& stored_uuid = selected.value().begin()->uuid_field.value;
    EXPECT_FALSE(stored_uuid.empty());
    EXPECT_EQ(stored_uuid.size(), 36);
}

TYPED_TEST(UUIDTypesTest, BatchUUIDInsert) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.label = "u1", .uuid_field = UUID{"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"}},
            {.label = "u2", .uuid_field = UUID{"11111111-2222-3333-4444-555555555555"}},
            {.label = "u3", .uuid_field = UUID{"99999999-8888-7777-6666-555544443333"}},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 3);
    auto it = selected.value().begin();
    EXPECT_EQ(it->uuid_field.value, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
}

TYPED_TEST(UUIDTypesTest, StringViewConstructorAndConversion) {
    std::string_view sv = "abcdefab-1234-5678-9abc-def012345678";
    UUID             uuid{sv};
    EXPECT_EQ(uuid.value, sv);

    std::string_view converted = uuid;
    EXPECT_EQ(converted, sv);

    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "sv_test", .uuid_field = UUID{sv}};
    auto                               result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(std::string_view(selected.value().begin()->uuid_field), sv);
}

TYPED_TEST(UUIDTypesTest, GeneratedUUIDRoundTrip) {
    auto generated = UUID::generate();

    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "generated", .uuid_field = generated};
    auto                               result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->uuid_field.value, generated.value);
}

TYPED_TEST(UUIDTypesTest, BatchEmptyUUIDsAutoGenerate) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::vector<ExtendedTypes>         batch = {
            {.label = "auto1"},
            {.label = "auto2"},
            {.label = "auto3"},
    };

    auto result = qs.insert(batch).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 3);

    std::set<std::string> generated_uuids;
    for (const auto& row : selected.value()) {
        EXPECT_FALSE(row.uuid_field.value.empty());
        EXPECT_EQ(row.uuid_field.value.size(), 36);
        generated_uuids.insert(row.uuid_field.value);
    }
    EXPECT_EQ(generated_uuids.size(), 3);
}

TYPED_TEST(UUIDTypesTest, UserProvidedUUIDPreserved) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    std::string                        custom_uuid = "a0b1c2d3-e4f5-1678-9abc-def012345678";
    ExtendedTypes                      obj{.label = "custom", .uuid_field = UUID{custom_uuid}};

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected.value().begin()->uuid_field.value, custom_uuid);
}

TYPED_TEST(UUIDTypesTest, InvalidUUIDFormatRejected) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "bad", .uuid_field = UUID{"not-a-valid-uuid"}};

    auto result = qs.insert(obj).execute();
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message().find("Invalid UUID format"), std::string::npos);
}

TYPED_TEST(UUIDTypesTest, InvalidUUIDWrongLength) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes                      obj{.label = "short", .uuid_field = UUID{"1234"}};

    auto result = qs.insert(obj).execute();
    ASSERT_FALSE(result.has_value());
}

TYPED_TEST(UUIDTypesTest, InvalidUUIDNonHexChars) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    ExtendedTypes obj{.label = "nonhex", .uuid_field = UUID{"zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz"}};

    auto result = qs.insert(obj).execute();
    ASSERT_FALSE(result.has_value());
}

// TODO(#507): UUID PK DDL tests — requires UuidPkModel and UuidPkRef test models + SchemaBuilder API
// TYPED_TEST(UUIDTypesTest, UuidPkColumnDDL) {
//     const auto ddl = SchemaBuilder<UuidPkModel>::build();
//     if (std::is_same_v<TypeParam, postgresql::Connection>) {
//         EXPECT_THAT(ddl, HasSubstr("id UUID PRIMARY KEY"));
//     } else {
//         EXPECT_THAT(ddl, HasSubstr("id TEXT PRIMARY KEY"));
//     }
//     // Verify no AUTOINCREMENT/IDENTITY for UUID PK
//     EXPECT_THAT(ddl, Not(HasSubstr("AUTOINCREMENT")));
//     EXPECT_THAT(ddl, Not(HasSubstr("IDENTITY")));
// }
//
// TYPED_TEST(UUIDTypesTest, FkToUuidPkColumnDDL) {
//     const auto ddl = SchemaBuilder<UuidPkRef>::build();
//     if (std::is_same_v<TypeParam, postgresql::Connection>) {
//         EXPECT_THAT(ddl, HasSubstr("owner_id UUID NOT NULL"));
//     } else {
//         EXPECT_THAT(ddl, HasSubstr("owner_id TEXT NOT NULL"));
//     }
//     EXPECT_THAT(ddl, HasSubstr("REFERENCES UuidPkModel(id)"));
// }

// ===== SCHEMA GENERATION TESTS =====

template <typename ConnType> class NewTypesSchemaTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(NewTypesSchemaTest, DatabaseTypes);

TYPED_TEST(NewTypesSchemaTest, SchemaContainsNewColumns) {
    const auto& sql = create_table_sql<ExtendedTypes>();
    EXPECT_NE(sql.find("tiny_signed INTEGER NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("tiny_unsigned INTEGER NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("single_char INTEGER NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("color INTEGER NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("date_field TEXT NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("datetime_field TEXT NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("duration_field INTEGER NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("file_path TEXT NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("raw_data BLOB"), std::string::npos);
    EXPECT_NE(sql.find("uuid_field TEXT NOT NULL"), std::string::npos);
    EXPECT_NE(sql.find("opt_color INTEGER"), std::string::npos);
    EXPECT_NE(sql.find("opt_timestamp TEXT"), std::string::npos);
    EXPECT_NE(sql.find("opt_path TEXT"), std::string::npos);
}

// ===== COMBINED ROUNDTRIP TEST =====

template <typename ConnType> class AllNewTypesRoundtripTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(AllNewTypesRoundtripTest, DatabaseTypes);

TYPED_TEST(AllNewTypesRoundtripTest, FullRoundtrip) {
    QuerySet<ExtendedTypes, TypeParam> qs;

    auto date     = year_month_day{year{2025}, month{1}, day{15}};
    auto datetime = sys_days{year_month_day{year{2025}, month{6}, day{20}}} + hours{8} + minutes{30} + seconds{0};
    std::vector<std::byte> blob = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

    ExtendedTypes obj{
            .big_num        = 42LL,
            .precise        = 3.14,
            .approx         = 2.71f,
            .u_int          = 100,
            .ll_signed      = 999LL,
            .opt_double     = 1.5,
            .opt_int64      = 123LL,
            .label          = "roundtrip",
            .tiny_signed    = -50,
            .tiny_unsigned  = 200,
            .single_char    = 'X',
            .color          = Color::Blue,
            .date_field     = date,
            .datetime_field = datetime,
            .duration_field = seconds{7200},
            .file_path      = std::filesystem::path("/data/file.csv"),
            .raw_data       = blob,
            .uuid_field     = UUID{"12345678-1234-1234-1234-123456789abc"},
            .opt_color      = Color::Green,
            .opt_timestamp  = datetime,
            .opt_path       = std::filesystem::path("/opt/backup"),
    };

    auto result = qs.insert(obj).execute();
    ASSERT_TRUE(result.has_value());

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value());
    ASSERT_EQ(selected.value().size(), 1);

    auto it = selected.value().begin();
    EXPECT_EQ(it->big_num, 42LL);
    EXPECT_NEAR(it->precise, 3.14, 1e-10);
    EXPECT_NEAR(it->approx, 2.71f, 1e-6);
    EXPECT_EQ(it->u_int, 100u);
    EXPECT_EQ(it->ll_signed, 999LL);
    ASSERT_TRUE(it->opt_double.has_value());
    EXPECT_NEAR(it->opt_double.value(), 1.5, 1e-10);
    ASSERT_TRUE(it->opt_int64.has_value());
    EXPECT_EQ(it->opt_int64.value(), 123LL);
    EXPECT_EQ(it->label, "roundtrip");
    EXPECT_EQ(it->tiny_signed, -50);
    EXPECT_EQ(it->tiny_unsigned, 200);
    EXPECT_EQ(it->single_char, 'X');
    EXPECT_EQ(it->color, Color::Blue);
    EXPECT_EQ(it->date_field, date);
    EXPECT_EQ(it->datetime_field, datetime);
    EXPECT_EQ(it->duration_field, seconds{7200});
    EXPECT_EQ(it->file_path, std::filesystem::path("/data/file.csv"));
    EXPECT_EQ(it->raw_data, blob);
    EXPECT_EQ(it->uuid_field.value, "12345678-1234-1234-1234-123456789abc");
    ASSERT_TRUE(it->opt_color.has_value());
    EXPECT_EQ(it->opt_color.value(), Color::Green);
    ASSERT_TRUE(it->opt_timestamp.has_value());
    EXPECT_EQ(it->opt_timestamp.value(), datetime);
    ASSERT_TRUE(it->opt_path.has_value());
    EXPECT_EQ(it->opt_path.value(), std::filesystem::path("/opt/backup"));
}

// ============================================================================
// PostgreSQL Dialect Schema Tests for ExtendedTypes (no DB connection needed)
// ============================================================================

using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;

TEST(PgDialectTypesSchemaTest, DateFieldIsDate) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("date_field DATE NOT NULL"), std::string::npos)
            << "Expected 'date_field DATE NOT NULL' in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, DatetimeFieldIsTimestamp) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("datetime_field TIMESTAMP NOT NULL"), std::string::npos)
            << "Expected 'datetime_field TIMESTAMP NOT NULL' in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, UuidFieldIsUuid) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("uuid_field UUID NOT NULL"), std::string::npos)
            << "Expected 'uuid_field UUID NOT NULL' in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, DurationFieldIsBigint) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("duration_field BIGINT NOT NULL"), std::string::npos)
            << "Expected 'duration_field BIGINT NOT NULL' in PG SQL: " << sql;
}

// ── PG integer column width (#603) — every IntWidth branch, on real fields ────
// Prior to these tests, IntWidth::Small (SMALLINT), the enum branch, and the
// unsigned-promoted-one-class rule were reachable only by manual tracing — no
// in-tree model exercised them on PostgreSQL. ExtendedTypes already carries
// every needed C++ shape (signed char/unsigned char/char/enum/unsigned int/
// long long), so no new model fixture is needed.

TEST(PgDialectTypesSchemaTest, SignedCharFieldIsSmallint) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("tiny_signed SMALLINT NOT NULL"), std::string::npos)
            << "signed char (1-byte signed) must map to SMALLINT in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, UnsignedCharFieldIsSmallint) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("tiny_unsigned SMALLINT NOT NULL"), std::string::npos)
            << "unsigned char (1-byte unsigned, 0..255 fits SMALLINT's 0..32767) must map to "
               "SMALLINT in PG SQL: "
            << sql;
}

TEST(PgDialectTypesSchemaTest, CharFieldIsSmallint) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("single_char SMALLINT NOT NULL"), std::string::npos)
            << "char (1-byte, signed or not) must map to SMALLINT in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, EnumFieldFollowsUnderlyingType) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    // Color's underlying type is `int` (4-byte signed) -> INTEGER, exercising the
    // std::is_enum_v branch of integer_width_of (recurses to std::underlying_type_t).
    EXPECT_NE(sql.find("color INTEGER NOT NULL"), std::string::npos)
            << "enum : int must map to INTEGER (underlying-type recursion) in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, OptionalEnumFieldFollowsUnderlyingTypeNullable) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    const std::size_t  pos = sql.find("opt_color INTEGER");
    ASSERT_NE(pos, std::string::npos) << "Expected 'opt_color INTEGER' (nullable) in PG SQL: " << sql;
    const std::string after = sql.substr(pos, 30);
    EXPECT_EQ(after.find("NOT NULL"), std::string::npos) << "opt_color should be nullable, got: " << after;
}

TEST(PgDialectTypesSchemaTest, UnsignedIntFieldIsBigint) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    // unsigned int (4-byte unsigned, 0..4294967295) does not fit INTEGER's
    // 4-byte signed range -> promoted one class to BIGINT.
    EXPECT_NE(sql.find("u_int BIGINT NOT NULL"), std::string::npos)
            << "unsigned int must stay on BIGINT (promoted one class) in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, LongLongFieldIsBigint) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("ll_signed BIGINT NOT NULL"), std::string::npos)
            << "long long (8-byte signed) must map to BIGINT in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, Int64FieldIsBigint) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("big_num BIGINT NOT NULL"), std::string::npos)
            << "int64_t must stay on BIGINT in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, NarrowFieldsSqliteUnchanged) {
    // SQLite has one dynamic INTEGER storage class for every width (#603 is PG-only).
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql();
    EXPECT_NE(sql.find("tiny_signed INTEGER NOT NULL"), std::string::npos) << sql;
    EXPECT_NE(sql.find("tiny_unsigned INTEGER NOT NULL"), std::string::npos) << sql;
    EXPECT_NE(sql.find("single_char INTEGER NOT NULL"), std::string::npos) << sql;
    EXPECT_NE(sql.find("color INTEGER NOT NULL"), std::string::npos) << sql;
    EXPECT_NE(sql.find("u_int INTEGER NOT NULL"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("SMALLINT"), std::string::npos) << "SQLite must never emit SMALLINT: " << sql;
}

TEST(PgDialectTypesSchemaTest, OptionalTimestampIsTimestamp) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("opt_timestamp TIMESTAMP"), std::string::npos)
            << "Expected 'opt_timestamp TIMESTAMP' (nullable) in PG SQL: " << sql;
    const std::size_t pos = sql.find("opt_timestamp TIMESTAMP");
    ASSERT_NE(pos, std::string::npos);
    const std::string after = sql.substr(pos, 30);
    EXPECT_EQ(after.find("NOT NULL"), std::string::npos) << "opt_timestamp should be nullable, got: " << after;
}

TEST(PgDialectTypesSchemaTest, FilePathIsText) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("file_path TEXT NOT NULL"), std::string::npos)
            << "Expected 'file_path TEXT NOT NULL' (unchanged) in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, BlobIsBytea) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("raw_data BYTEA"), std::string::npos) << "Expected 'raw_data BYTEA' in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, OptPathIsText) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("opt_path TEXT"), std::string::npos)
            << "Expected 'opt_path TEXT' (unchanged) in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, SqliteDialectUnchanged) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql();
    EXPECT_NE(sql.find("date_field TEXT NOT NULL"), std::string::npos)
            << "SQLite dialect should use TEXT for date: " << sql;
    EXPECT_NE(sql.find("uuid_field TEXT NOT NULL"), std::string::npos)
            << "SQLite dialect should use TEXT for UUID: " << sql;
}

TEST(PgDialectTypesSchemaTest, BoolFieldIsBoolean) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_EQ(sql.find("BOOLEAN"), std::string::npos)
            << "ExtendedTypes has no bool field, should not contain BOOLEAN: " << sql;
    const std::string& person_sql = SchemaStatement<Person>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(person_sql.find("is_active BOOLEAN NOT NULL DEFAULT FALSE"), std::string::npos)
            << "Expected 'is_active BOOLEAN NOT NULL DEFAULT FALSE' in PG SQL: " << person_sql;
}

TEST(PgDialectTypesSchemaTest, FloatFieldIsReal) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("approx REAL NOT NULL"), std::string::npos)
            << "Expected 'approx REAL NOT NULL' for float in PG SQL: " << sql;
}

TEST(PgDialectTypesSchemaTest, DoubleFieldIsDoublePrecision) {
    const std::string& sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("precise DOUBLE PRECISION NOT NULL"), std::string::npos)
            << "Expected 'precise DOUBLE PRECISION NOT NULL' for double in PG SQL: " << sql;
}

// Test PG-dialect optional types that have no dedicated fields in ExtendedTypes
struct PgOptionalSpecialTypes {
    [[= primary]] int                          id{};
    std::optional<std::chrono::year_month_day> opt_date;
    std::optional<UUID>                        opt_uuid;
    std::optional<bool>                        opt_bool;
};

TEST(PgDialectTypesSchemaTest, OptionalDateIsPgDate) {
    const std::string& sql = SchemaStatement<PgOptionalSpecialTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("opt_date DATE"), std::string::npos) << "Expected 'opt_date DATE' in PG SQL: " << sql;
    const std::size_t pos = sql.find("opt_date DATE");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(sql.substr(pos, 20).find("NOT NULL"), std::string::npos) << "opt_date should be nullable";
}

TEST(PgDialectTypesSchemaTest, OptionalUuidIsPgUuid) {
    const std::string& sql = SchemaStatement<PgOptionalSpecialTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("opt_uuid UUID"), std::string::npos) << "Expected 'opt_uuid UUID' in PG SQL: " << sql;
    const std::size_t pos = sql.find("opt_uuid UUID");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(sql.substr(pos, 20).find("NOT NULL"), std::string::npos) << "opt_uuid should be nullable";
}

TEST(PgDialectTypesSchemaTest, OptionalBoolIsBoolean) {
    const std::string& sql = SchemaStatement<PgOptionalSpecialTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("opt_bool BOOLEAN"), std::string::npos) << "Expected 'opt_bool BOOLEAN' in PG SQL: " << sql;
    const std::size_t pos = sql.find("opt_bool BOOLEAN");
    ASSERT_NE(pos, std::string::npos);
    EXPECT_EQ(sql.substr(pos, 23).find("NOT NULL"), std::string::npos) << "opt_bool should be nullable";
}

TEST(PgDialectTypesSchemaTest, OptionalSpecialTypesSqliteUnchanged) {
    const std::string& sql = SchemaStatement<PgOptionalSpecialTypes>::create_table_sql();
    EXPECT_NE(sql.find("opt_date TEXT"), std::string::npos) << "SQLite should use TEXT for opt date: " << sql;
    EXPECT_NE(sql.find("opt_uuid TEXT"), std::string::npos) << "SQLite should use TEXT for opt UUID: " << sql;
    EXPECT_NE(sql.find("opt_bool INTEGER"), std::string::npos) << "SQLite should use INTEGER for opt bool: " << sql;
}

// ===== 64-BIT UNSIGNED SIGNED-STORAGE CAVEAT TESTS (#419) =====
//
// uint64_t / unsigned long / unsigned long long are bound and stored as a signed
// int64 (SQLite has no unsigned integer; PostgreSQL maps them to BIGINT, also
// signed). For values <= INT64_MAX everything is exact. For values > INT64_MAX the
// value wraps to a negative int64 on write. It still round-trips for EQUALITY
// (the read casts the signed bits back to unsigned), but:
//   1. ORDER BY / range comparisons sort by the signed reinterpretation, so a huge
//      uint64 (>= 2^63) sorts *before* a small one.
//   2. Any external reader (raw SQL, BI tool) sees the negative signed value.
// These tests PIN that behavior so it is intentional, not accidental. See
// docs/reference/FIELD_TYPES.md "Signed storage caveat for 64-bit unsigned types".

template <typename ConnType> class Uint64SignedStorageTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(Uint64SignedStorageTest, DatabaseTypes);

// A uint64 > INT64_MAX round-trips for EQUALITY: the read casts the stored signed
// bits back to unsigned, recovering the exact value.
TYPED_TEST(Uint64SignedStorageTest, ValueAboveInt64MaxRoundTripsForEquality) {
    constexpr std::uint64_t            above_max = (std::uint64_t{1} << 63) + 1; // 2^63 + 1 > INT64_MAX
    QuerySet<ExtendedTypes, TypeParam> qs;

    auto result = qs.insert(ExtendedTypes{.big_unsigned = above_max, .label = "above_max"}).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    ASSERT_EQ(selected.value().size(), 1);
    EXPECT_EQ(selected.value().begin()->big_unsigned, above_max);
}

// The full unsigned 64-bit max also round-trips for equality.
TYPED_TEST(Uint64SignedStorageTest, Uint64MaxRoundTripsForEquality) {
    constexpr std::uint64_t            u64_max = std::numeric_limits<std::uint64_t>::max();
    QuerySet<ExtendedTypes, TypeParam> qs;

    auto result = qs.insert(ExtendedTypes{.big_unsigned = u64_max, .label = "u64_max"}).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    EXPECT_EQ(selected.value().begin()->big_unsigned, u64_max);
}

// DOCUMENTED TRAP: ORDER BY sorts by the SIGNED interpretation. A uint64 >= 2^63 is
// stored as a negative int64, so it sorts BEFORE a small value — the opposite of
// unsigned order. This is intentional given signed storage (#419).
TYPED_TEST(Uint64SignedStorageTest, OrderBySortsBySignedInterpretation) {
    constexpr std::uint64_t            above_max = (std::uint64_t{1} << 63) + 1; // huge: stored negative
    constexpr std::uint64_t            small     = 1;                            // stored as +1
    QuerySet<ExtendedTypes, TypeParam> qs;

    auto result = qs.insert(std::vector<ExtendedTypes>{
                                    {.big_unsigned = above_max, .label = "huge"},
                                    {.big_unsigned = small, .label = "small"},
                            })
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    // ORDER BY big_unsigned ASC. By unsigned value, "small" (1) < "huge" (2^63+1),
    // so "small" should come first. But signed storage flips it: the huge value is
    // a negative int64, so "huge" sorts FIRST. We pin the (wrong-for-unsigned) order.
    auto selected = qs.template order_by<fields::ExtendedTypes.big_unsigned>().select().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    ASSERT_EQ(selected.value().size(), 2);
    auto it = selected.value().begin();
    // uint64 >= 2^63 sorts first (signed storage), not last.
    EXPECT_EQ(it->label, "huge");
    ++it;
    EXPECT_EQ(it->label, "small");
}

// ===== 64-BIT UNSIGNED FULL-RANGE STORAGE TESTS (#436) =====
//
// full_unsigned stores an unsigned-64 field order-preservingly:
// PostgreSQL NUMERIC(20,0), SQLite zero-padded 20-char TEXT (lexicographic ==
// numeric order). Unlike signed_storage (#419), values > INT64_MAX both round-trip
// AND sort correctly, and external readers see the true unsigned value.
// signed_storage keeps today's signed BIGINT/INTEGER storage.

// --- Compile-time refusal: a bare unsigned-64 field is rejected (#436) ---
// ModelStorageAnnotated<T> is false for a model with an un-annotated uint64 field, so
// BaseStatement<T> (and thus QuerySet<T>) fails to instantiate with a clear constraint
// violation. The annotated variants satisfy it. These static_asserts pin that contract
// without needing a compile-fail harness.
namespace {
    struct BareUint64Model {
        [[= primary]] int id{};
        std::uint64_t     value{}; // bare — no storage annotation
    };
    struct SignedStorageModel {
        [[= primary]] int                  id{};
        [[= signed_storage]] std::uint64_t value{};
    };
    struct FullUnsignedModel {
        [[= primary]] int                 id{};
        [[= full_unsigned]] std::uint64_t value{};
    };
    static_assert(
            !ModelStorageAnnotated<BareUint64Model>,
            "bare unsigned-64 field must NOT satisfy storm::orm::statements::ModelStorageAnnotated"
    );
    static_assert(
            ModelStorageAnnotated<SignedStorageModel>,
            "signed_storage uint64 field must satisfy storm::orm::statements::ModelStorageAnnotated"
    );
    static_assert(
            ModelStorageAnnotated<FullUnsignedModel>,
            "full_unsigned uint64 field must satisfy storm::orm::statements::ModelStorageAnnotated"
    );
} // namespace

// --- Compile-time refusal: SET NULL requires a nullable FK (#431) ---
// ModelFkPoliciesValid<T> is false for a model whose fk<RefAction::SetNull>
// sits on a NOT NULL (non-optional) FK, so BaseStatement<T> fails to instantiate with
// a clear constraint violation. CASCADE/RESTRICT on a non-nullable FK are fine, and
// SET NULL on a std::optional FK is fine. static_asserts pin the contract without a
// compile-fail harness.
namespace {
    struct SetNullNonNullableFk {
        [[= primary]] int                          id{};
        [[= storm::fk<RefAction::SetNull>]] Person owner; // non-optional → invalid
    };
    struct CascadeNonNullableFk {
        [[= primary]] int                          id{};
        [[= storm::fk<RefAction::Cascade>]] Person owner;
    };
    static_assert(
            !storm::orm::statements::ModelFkPoliciesValid<SetNullNonNullableFk>,
            "SET NULL on a non-nullable FK must NOT satisfy ModelFkPoliciesValid"
    );
    static_assert(
            storm::orm::statements::ModelFkPoliciesValid<CascadeNonNullableFk>,
            "CASCADE on a non-nullable FK must satisfy ModelFkPoliciesValid"
    );
    static_assert(
            storm::orm::statements::ModelFkPoliciesValid<SetNullChild>,
            "SET NULL on an optional FK must satisfy ModelFkPoliciesValid"
    );
} // namespace

// --- Schema: the storage class drives the column type per dialect ---

TYPED_TEST(NewTypesSchemaTest, SignedStorageKeepsIntegerColumn) {
    // SQLite: signed_storage uint64 stays INTEGER (byte-identical to today).
    const auto& sqlite_sql = create_table_sql<ExtendedTypes>();
    EXPECT_NE(sqlite_sql.find("big_unsigned INTEGER NOT NULL"), std::string::npos) << sqlite_sql;
    // PostgreSQL: signed_storage uint64 stays BIGINT.
    const std::string& pg_sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(pg_sql.find("big_unsigned BIGINT NOT NULL"), std::string::npos) << pg_sql;
}

TYPED_TEST(NewTypesSchemaTest, FullUnsignedUsesOrderPreservingColumn) {
    // SQLite: full_unsigned uint64 maps to TEXT (zero-padded decimal on write).
    const auto& sqlite_sql = create_table_sql<ExtendedTypes>();
    EXPECT_NE(sqlite_sql.find("big_unsigned_full TEXT NOT NULL"), std::string::npos) << sqlite_sql;
    // PostgreSQL: full_unsigned uint64 maps to NUMERIC(20,0).
    const std::string& pg_sql = SchemaStatement<ExtendedTypes>::create_table_sql<Dialect::PostgreSQL>();
    EXPECT_NE(pg_sql.find("big_unsigned_full NUMERIC(20,0) NOT NULL"), std::string::npos) << pg_sql;
}

// --- Round-trip: full_unsigned recovers the exact value across the full range ---

template <typename ConnType> class Uint64FullUnsignedTest : public StormTestFixture<ExtendedTypes, ConnType> {};
TYPED_TEST_SUITE(Uint64FullUnsignedTest, DatabaseTypes);

TYPED_TEST(Uint64FullUnsignedTest, ValueAboveInt64MaxRoundTrips) {
    constexpr std::uint64_t            above_max = (std::uint64_t{1} << 63) + 1; // 2^63 + 1 > INT64_MAX
    QuerySet<ExtendedTypes, TypeParam> qs;

    auto result = qs.insert(ExtendedTypes{.big_unsigned_full = above_max, .label = "above_max"}).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    ASSERT_EQ(selected.value().size(), 1);
    EXPECT_EQ(selected.value().begin()->big_unsigned_full, above_max);
}

TYPED_TEST(Uint64FullUnsignedTest, Uint64MaxRoundTrips) {
    constexpr std::uint64_t            u64_max = std::numeric_limits<std::uint64_t>::max();
    QuerySet<ExtendedTypes, TypeParam> qs;

    auto result = qs.insert(ExtendedTypes{.big_unsigned_full = u64_max, .label = "u64_max"}).execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = qs.select().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    EXPECT_EQ(selected.value().begin()->big_unsigned_full, u64_max);
}

TYPED_TEST(Uint64FullUnsignedTest, ZeroAndSmallValuesRoundTrip) {
    QuerySet<ExtendedTypes, TypeParam> qs;
    auto                               result = qs.insert(std::vector<ExtendedTypes>{
                                    {.big_unsigned_full = 0, .label = "zero"},
                                    {.big_unsigned_full = 42, .label = "small"},
                            })
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = qs.template order_by<fields::ExtendedTypes.id>().select().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    ASSERT_EQ(selected.value().size(), 2);
    auto it = selected.value().begin();
    EXPECT_EQ(it->big_unsigned_full, 0U);
    ++it;
    EXPECT_EQ(it->big_unsigned_full, 42U);
}

// THE FIX: ORDER BY sorts by the TRUE unsigned value. A uint64 >= 2^63 sorts
// AFTER a small value — the opposite of the signed_storage trap (#419).
TYPED_TEST(Uint64FullUnsignedTest, OrderBySortsByUnsignedValue) {
    constexpr std::uint64_t            above_max = (std::uint64_t{1} << 63) + 1; // huge
    constexpr std::uint64_t            small     = 1;
    QuerySet<ExtendedTypes, TypeParam> qs;

    auto result = qs.insert(std::vector<ExtendedTypes>{
                                    {.big_unsigned_full = above_max, .label = "huge"},
                                    {.big_unsigned_full = small, .label = "small"},
                            })
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    auto selected = qs.template order_by<fields::ExtendedTypes.big_unsigned_full>().select().execute();
    ASSERT_TRUE(selected.has_value()) << selected.error().message();
    ASSERT_EQ(selected.value().size(), 2);
    auto it = selected.value().begin();
    // Correct unsigned order: small (1) first, huge (2^63+1) last.
    EXPECT_EQ(it->label, "small");
    ++it;
    EXPECT_EQ(it->label, "huge");
}

// NOLINTEND(readability-identifier-length,readability-uppercase-literal-suffix,modernize-use-std-numbers)
// NOLINTEND(misc-const-correctness)
