#pragma once

// Shared test body for test_crud_sqlite.cpp / test_crud_pg.cpp — the two
// single-backend TUs of a compile-time TU split (see test_db_helpers.h,
// DatabaseTypesSqliteHalf/DatabaseTypesPgHalf). Splitting a 2-backend TU into
// two lets ninja compile them in parallel instead of serially instantiating
// both backends in one TU; keeping the body here (instead of duplicating it
// into both .cpp files) removes the risk of the two halves silently drifting.
//
// The includer must #define STORM_SPLIT_TYPES / STORM_SPLIT_TYPE_NAMES to one
// backend's ::testing::Types<> alias / NameGenerator before #include-ing this
// file, and #undef both afterward. Never include this file directly.
#if !defined(STORM_SPLIT_TYPES) || !defined(STORM_SPLIT_TYPE_NAMES)
#error "test_crud_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

#include <sqlite3.h>

import storm;
import std;

using storm::db::DefaultConnection;

#include "../../shared/models/people_25.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "../../shared/models/simple_record.h"
#include "test_fixture.h"

#include "test_seed_helpers.h"
using storm::QuerySet;

// Common base fixture for Erase/Update tests — shared setup + helpers.
template <typename ConnType> class PersonCrudTestBase : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        std::vector<Person> const initial = {
            {.name = "Alice", .age = 30},
            {.name = "Bob", .age = 25},
            {.name = "Charlie", .age = 35},
        };
        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(initial)));
    }

    static auto insert_persons_range(QuerySet<Person, ConnType> &qs, int from, int to) -> void {
        std::vector<Person> batch;
        batch.reserve(static_cast<std::size_t>(to) - static_cast<std::size_t>(from) + 1U);
        for (int i = from; i <= to; i++) {
            batch.push_back(Person{.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 60)});
        }
        auto result = qs.insert(std::span<const Person>(batch)).execute();
        ASSERT_TRUE(result.has_value()) << "Failed to insert test data";
    }

    static auto countPersons() -> int {
        QuerySet<Person, ConnType> qs;
        auto result = qs.count().execute();
        if (!result.has_value()) {
            return -1;
        }
        return static_cast<int>(result.value());
    }

    static auto personExists(int person_id) -> bool {
        const QuerySet<Person, ConnType> qs;
        auto result = qs.where(fields::Person.id == person_id).select().execute();
        return result.has_value() && !result.value().empty();
    }
};

// Test QuerySet.erase() functionality
template <typename ConnType> class QuerySetEraseTest : public PersonCrudTestBase<ConnType> {};
TYPED_TEST_SUITE(QuerySetEraseTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(QuerySetEraseTest, DatabaseSetup) {
    // Verify database was created and populated correctly
    EXPECT_TRUE(DefaultConnection<TypeParam>::has()) << "Should have default connection";
    EXPECT_EQ(this->countPersons(), 3) << "Should have 3 persons in database";

    // Verify specific persons exist
    EXPECT_TRUE(this->personExists(1)) << "Alice should exist";
    EXPECT_TRUE(this->personExists(2)) << "Bob should exist";
    EXPECT_TRUE(this->personExists(3)) << "Charlie should exist";
}

TYPED_TEST(QuerySetEraseTest, EraseMultiplePersonsSequentially) {
    QuerySet<Person, TypeParam> queryset;

    // Create person objects to erase
    Person const alice{.id = 1, .name = "Alice", .age = 30};
    Person const bob{.id = 2, .name = "Bob", .age = 25};

    // Verify initial state
    EXPECT_EQ(this->countPersons(), 3) << "Should have 3 persons initially";

    auto result1 = queryset.erase(alice).execute();
    ASSERT_TRUE(result1.has_value()) << "First erase should succeed";
    EXPECT_EQ(this->countPersons(), 2) << "Should have 2 persons after first removal";

    auto result2 = queryset.erase(bob).execute();
    ASSERT_TRUE(result2.has_value()) << "Second erase should succeed";
    EXPECT_EQ(this->countPersons(), 1) << "Should have 1 person after second removal";

    // Verify only Charlie remains
    EXPECT_FALSE(this->personExists(1)) << "Alice should be removed";
    EXPECT_FALSE(this->personExists(2)) << "Bob should be removed";
    EXPECT_TRUE(this->personExists(3)) << "Charlie should still exist";
}

TYPED_TEST(QuerySetEraseTest, EraseBatchPerformance) {
    QuerySet<Person, TypeParam> queryset;

    const int num_records = 1000;
    this->insert_persons_range(queryset, 4, num_records);

    // Measure individual removes
    auto start_individual = std::chrono::steady_clock::now();
    for (int i = 1; i <= 50; i++) {
        Person const person{.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 60)};
        auto result = queryset.erase(person).execute();
        ASSERT_TRUE(result.has_value()) << "Individual erase should succeed";
    }
    auto end_individual = std::chrono::steady_clock::now();
    auto duration_individual =
        std::chrono::duration_cast<std::chrono::microseconds>(end_individual - start_individual).count();

    // Prepare batch for batch erase
    std::vector<Person> batch;
    for (int i = 51; i <= 100; i++) {
        batch.emplace_back(i, std::format("Person{}", i), 20 + (i % 60));
    }

    // Measure batch erase
    auto start_batch = std::chrono::steady_clock::now();
    auto result = queryset.erase(std::span<const Person>(batch)).execute();
    ASSERT_TRUE(result.has_value()) << "Batch erase should succeed";
    auto end_batch = std::chrono::steady_clock::now();
    auto duration_batch = std::chrono::duration_cast<std::chrono::microseconds>(end_batch - start_batch).count();

    // Log performance comparison (batch should be faster)
    std::cout << "\nPerformance Comparison (50 deletes):" << '\n';
    std::cout << "  Individual removes: " << duration_individual << " μs" << '\n';
    std::cout << "  Batch erase: " << duration_batch << " μs" << '\n';
    std::cout << "  Speedup: " << std::fixed << std::setprecision(2)
              << static_cast<double>(duration_individual) / static_cast<double>(duration_batch) << "x" << '\n';

    // Verify correct deletions
    EXPECT_EQ(this->countPersons(), num_records - 100) << "Should have correct number of persons after removes";
}

// Test chunked erase (>799 rows to trigger execute_chunked path)
TYPED_TEST(QuerySetEraseTest, EraseBatchChunked) {
    QuerySet<Person, TypeParam> queryset;

    // MAX_CHUNK_SIZE is 799, so >799 triggers chunked path
    this->insert_persons_range(queryset, 4, 1000);

    // Verify initial state
    EXPECT_EQ(this->countPersons(), 1000) << "Should have 1000 persons initially";

    // Create batch of >799 persons to erase (triggers chunked path)
    std::vector<Person> chunked_batch;
    for (int i = 1; i <= 850; i++) {
        chunked_batch.emplace_back(i, std::format("Person{}", i), 20 + (i % 60));
    }

    // Erase chunked batch - should use execute_chunked with transaction
    auto result = queryset.erase(std::span<const Person>(chunked_batch)).execute();

    // Verify removal was successful
    if (!result.has_value()) {
        std::cout << "Error: " << result.error().message() << '\n';
    }
    ASSERT_TRUE(result.has_value()) << "Chunked batch erase operation should succeed: "
                                    << (result.has_value() ? "" : result.error().message());

    // Verify correct number of persons removed
    EXPECT_EQ(this->countPersons(), 150) << "Should have 150 persons after chunked batch removal";

    // Spot-check boundary values (avoid 1000 per-row queries which are slow on PostgreSQL)
    EXPECT_FALSE(this->personExists(1)) << "First removed person should be gone";
    EXPECT_FALSE(this->personExists(425)) << "Mid-range removed person should be gone";
    EXPECT_FALSE(this->personExists(850)) << "Last removed person should be gone";
    EXPECT_TRUE(this->personExists(851)) << "First remaining person should still exist";
    EXPECT_TRUE(this->personExists(1000)) << "Last remaining person should still exist";
}

// Test chunked erase with remainder (tests both full chunks and remainder processing)
TYPED_TEST(QuerySetEraseTest, EraseBatchChunkedWithRemainder) {
    QuerySet<Person, TypeParam> queryset;

    // MAX_CHUNK_SIZE is 799, so 1650 rows = 2 full chunks (1598) + 52 remainder
    this->insert_persons_range(queryset, 4, 1800);

    // Verify initial state
    EXPECT_EQ(this->countPersons(), 1800) << "Should have 1800 persons initially";

    // Create batch of 1650 persons (2 full chunks + 52 remainder)
    std::vector<Person> chunked_batch;
    for (int i = 1; i <= 1650; i++) {
        chunked_batch.emplace_back(i, std::format("Person{}", i), 20 + (i % 60));
    }

    // Erase chunked batch - should use execute_chunked with remainder
    auto result = queryset.erase(std::span<const Person>(chunked_batch)).execute();

    // Verify removal was successful
    ASSERT_TRUE(result.has_value()) << "Chunked batch erase with remainder should succeed: "
                                    << (result.has_value() ? "" : result.error().message());

    // Verify correct number of persons removed
    EXPECT_EQ(this->countPersons(), 150) << "Should have 150 persons after chunked batch removal";

    // Spot-check boundary values (avoid 1800 per-row queries which are slow on PostgreSQL)
    EXPECT_FALSE(this->personExists(1)) << "First removed person should be gone";
    EXPECT_FALSE(this->personExists(825)) << "Mid-range removed person should be gone";
    EXPECT_FALSE(this->personExists(1650)) << "Last removed person should be gone";
    EXPECT_TRUE(this->personExists(1651)) << "First remaining person should still exist";
    EXPECT_TRUE(this->personExists(1800)) << "Last remaining person should still exist";
}

// Test QuerySet.update() functionality
template <typename ConnType> class QuerySetUpdateTest : public PersonCrudTestBase<ConnType> {
  protected:
    // Helper function to get person by ID using the ORM
    static auto getPerson(int person_id) -> std::optional<Person> {
        const QuerySet<Person, ConnType> qs;
        auto result = qs.where(fields::Person.id == person_id).select().execute();
        if (!result.has_value() || result.value().empty()) {
            return std::nullopt;
        }
        return *result.value().begin();
    }
};

TYPED_TEST_SUITE(QuerySetUpdateTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(QuerySetUpdateTest, DatabaseSetup) {
    // Verify database was created and populated correctly
    EXPECT_TRUE(DefaultConnection<TypeParam>::has()) << "Should have default connection";
    EXPECT_EQ(this->countPersons(), 3) << "Should have 3 persons in database";

    // Verify specific persons exist with correct values
    auto alice = this->getPerson(1);
    ASSERT_TRUE(alice.has_value()) << "Alice should exist";
    EXPECT_EQ(alice->name, "Alice");
    EXPECT_EQ(alice->age, 30);

    auto bob = this->getPerson(2);
    ASSERT_TRUE(bob.has_value()) << "Bob should exist";
    EXPECT_EQ(bob->name, "Bob");
    EXPECT_EQ(bob->age, 25);

    auto charlie = this->getPerson(3);
    ASSERT_TRUE(charlie.has_value()) << "Charlie should exist";
    EXPECT_EQ(charlie->name, "Charlie");
    EXPECT_EQ(charlie->age, 35);
}

TYPED_TEST(QuerySetUpdateTest, UpdateMultipleTimes) {
    QuerySet<Person, TypeParam> queryset;

    // Update Alice multiple times
    Person const alice_v1{.id = 1, .name = "Alice A", .age = 31};
    auto result1 = queryset.update(alice_v1).execute();
    ASSERT_TRUE(result1.has_value()) << "First update should succeed";

    auto check1 = this->getPerson(1);
    ASSERT_TRUE(check1.has_value());
    EXPECT_EQ(check1->name, "Alice A");
    EXPECT_EQ(check1->age, 31);

    Person const alice_v2{.id = 1, .name = "Alice B", .age = 32};
    auto result2 = queryset.update(alice_v2).execute();
    ASSERT_TRUE(result2.has_value()) << "Second update should succeed";

    auto check2 = this->getPerson(1);
    ASSERT_TRUE(check2.has_value());
    EXPECT_EQ(check2->name, "Alice B");
    EXPECT_EQ(check2->age, 32);

    Person const alice_v3{.id = 1, .name = "Alice C", .age = 33};
    auto result3 = queryset.update(alice_v3).execute();
    ASSERT_TRUE(result3.has_value()) << "Third update should succeed";

    auto check3 = this->getPerson(1);
    ASSERT_TRUE(check3.has_value());
    EXPECT_EQ(check3->name, "Alice C");
    EXPECT_EQ(check3->age, 33);
}

TYPED_TEST(QuerySetUpdateTest, UpdateCachedStatementReuse) {
    QuerySet<Person, TypeParam> queryset;

    // Perform multiple updates to verify statement caching works correctly
    for (int i = 0; i < 10; ++i) {
        Person const updated_alice{.id = 1, .name = std::format("Alice V{}", i), .age = 30 + i};
        auto result = queryset.update(updated_alice).execute();
        ASSERT_TRUE(result.has_value()) << "Update iteration " << i << " should succeed";

        auto alice = this->getPerson(1);
        ASSERT_TRUE(alice.has_value());
        EXPECT_EQ(alice->name, std::format("Alice V{}", i));
        EXPECT_EQ(alice->age, 30 + i);
    }

    // Verify final state
    auto final_alice = this->getPerson(1);
    ASSERT_TRUE(final_alice.has_value());
    EXPECT_EQ(final_alice->name, "Alice V9");
    EXPECT_EQ(final_alice->age, 39);
}

// =============================================================================
// Transaction Edge Cases (from test_coverage_gaps.cpp)
// =============================================================================

template <typename ConnType> class TransactionTest : public StormTestFixture<SimpleRecord, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        qs = std::make_unique<QuerySet<SimpleRecord, ConnType>>();
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<SimpleRecord, ConnType>::TearDown();
    }

    std::unique_ptr<QuerySet<SimpleRecord, ConnType>> qs;
};

TYPED_TEST_SUITE(TransactionTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(TransactionTest, MultiRowUpdateInTransaction) {
    std::vector<SimpleRecord> const people = {{0, "P1", 1}, {0, "P2", 2}, {0, "P3", 3}};

    auto insert_result = this->qs->insert(std::span<const SimpleRecord>(people)).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto select_result = this->qs->select().execute();
    ASSERT_TRUE(select_result.has_value());

    std::vector<SimpleRecord> updates;
    for (const auto &p : select_result.value()) {
        updates.push_back({p.id, p.name, p.value + 100});
    }

    auto update_result = this->qs->update(std::span<const SimpleRecord>(updates)).execute();
    ASSERT_TRUE(update_result.has_value());

    auto verify_result = this->qs->select().execute();
    ASSERT_TRUE(verify_result.has_value());
    for (const auto &p : verify_result.value()) {
        EXPECT_GT(p.value, 100);
    }
}

TYPED_TEST(TransactionTest, EmptyBatchOperations) {
    std::vector<SimpleRecord> empty;

    auto insert_result = this->qs->insert(std::span<const SimpleRecord>(empty)).execute();
    ASSERT_TRUE(insert_result.has_value());

    auto update_result = this->qs->update(std::span<const SimpleRecord>(empty)).execute();
    ASSERT_TRUE(update_result.has_value());

    auto remove_result = this->qs->erase(std::span<const SimpleRecord>(empty)).execute();
    ASSERT_TRUE(remove_result.has_value());
}

TYPED_TEST(TransactionTest, SingleRowOperations) {
    SimpleRecord const p1{0, "Single", 42};
    auto insert_result = this->qs->insert(p1).execute();
    ASSERT_TRUE(insert_result.has_value());

    std::int64_t const id = insert_result.value();

    SimpleRecord const updated{static_cast<int>(id), "Updated", 99};
    auto update_result = this->qs->update(updated).execute();
    ASSERT_TRUE(update_result.has_value());

    auto select_result = this->qs->select().execute();
    ASSERT_TRUE(select_result.has_value());
    EXPECT_EQ(select_result.value().begin()->value, 99);

    auto remove_result = this->qs->erase(updated).execute();
    ASSERT_TRUE(remove_result.has_value());

    auto empty_result = this->qs->select().execute();
    ASSERT_TRUE(empty_result.has_value());
    EXPECT_TRUE(empty_result.value().empty());
}

// =============================================================================
// Query Reset and Reuse Coverage (from test_coverage_gaps.cpp)
// =============================================================================

template <typename ConnType> class QueryResetTest : public StormTestFixture<Person, ConnType> {
  protected:
    auto on_after_setup(const std::shared_ptr<ConnType> &) -> void override {
        qs = std::make_unique<QuerySet<Person, ConnType>>();

        ASSERT_TRUE((storm::test::batch_insert<Person, ConnType>(
            std::vector<Person>(storm::test::PEOPLE_25.begin(), storm::test::PEOPLE_25.end()))));
    }

    auto TearDown() -> void override {
        qs = nullptr;
        StormTestFixture<Person, ConnType>::TearDown();
    }

  public:
    std::unique_ptr<QuerySet<Person, ConnType>> qs;
};

TYPED_TEST_SUITE(QueryResetTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(QueryResetTest, ResetClearsAllState) {
    auto filtered = this->qs->where(fields::Person.age > 25);

    auto result1 = filtered.template order_by<fields::Person.name>().limit(2).offset(1).select().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_LE(result1.value().size(), 2);

    // Original qs is unaffected — no reset needed
    auto result2 = this->qs->select().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 25);
}

TYPED_TEST(QueryResetTest, ReuseSameQuerySetMultipleTimes) {
    auto result1 = this->qs->where(fields::Person.age > 35).select().execute();
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1.value().size(), 9);

    // Original qs is unaffected by where() — still returns all rows
    auto result2 = this->qs->select().execute();
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2.value().size(), 25);

    // Can create a different filter without reset
    auto result3 = this->qs->where(fields::Person.age < 35).select().execute();
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(result3.value().size(), 14);
}

TYPED_TEST(QueryResetTest, ResetBetweenDifferentOperations) {
    auto count1 = this->qs->count().execute();
    ASSERT_TRUE(count1.has_value());
    EXPECT_EQ(count1.value(), 25);

    (*this->qs).reset();

    auto sum1 = this->qs->template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum1.has_value());
    EXPECT_EQ(sum1.value(), 829);

    (*this->qs).reset();

    auto avg1 = this->qs->template avg<fields::Person.age>().execute();
    ASSERT_TRUE(avg1.has_value());
    ASSERT_TRUE(avg1.value().has_value());
    EXPECT_NEAR(avg1.value().value(), 33.16, 0.01);

    (*this->qs).reset();

    auto min1 = this->qs->template min<fields::Person.age>().execute();
    ASSERT_TRUE(min1.has_value());
    ASSERT_TRUE(min1.value().has_value());
    EXPECT_EQ(min1.value().value(), 22);

    (*this->qs).reset();

    auto max1 = this->qs->template max<fields::Person.age>().execute();
    ASSERT_TRUE(max1.has_value());
    ASSERT_TRUE(max1.value().has_value());
    EXPECT_EQ(max1.value().value(), 48);
}

TYPED_TEST(QueryResetTest, AggregatesWithWhere) {
    auto count = this->qs->where(fields::Person.age >= 35).count().execute();
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(count.value(), 11);

    (*this->qs).reset();

    auto sum = this->qs->where(fields::Person.age >= 35).template sum<fields::Person.age>().execute();
    ASSERT_TRUE(sum.has_value());
    EXPECT_EQ(sum.value(), 442);
}
