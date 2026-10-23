#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
#include "test_seed_helpers.h"
using storm::db::DefaultConnection;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertOptions;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::ReturnId;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateStatement;

// Tests for the public transaction API (#415):
//   - TransactionGuard re-exported from storm.cppm
//   - storm::begin(conn) thin factory returning the guard
//   - success path COMMITs, early-return / scope-exit ROLLBACKs
//   - batch ops nested inside an outer transaction skip their inner BEGIN (#9)
template <typename ConnType> class PublicTransactionApiTest : public StormTestFixture<Person, ConnType> {
  protected:
    static auto countPersons() -> int {
        auto result = (SelectStatement<Person, ConnType>{}).all().count().execute();
        return result.has_value() ? static_cast<int>(result.value()) : -1;
    }

    static auto conn() -> std::shared_ptr<ConnType> {
        return DefaultConnection<ConnType>::get();
    }

    // Build `count` distinct Person rows for batch tests.
    static auto make_people(int count) -> std::vector<Person> {
        std::vector<Person> people;
        people.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            people.push_back(Person{.name = std::format("Person{}", i), .age = 20 + i});
        }
        return people;
    }

    // InsertOptions that force chunking (so the inner TransactionGuard runs).
    static auto chunked_opts() -> InsertOptions {
        InsertOptions opts;
        opts.batch_size = 2; // chunks of 2 → multiple inner BEGIN/COMMIT attempts.
        return opts;
    }

    // Inserts `count` rows (committed) and returns them with their assigned PKs so
    // update(span)/erase(span) can address existing rows.
    static auto seed_with_ids(int count) -> std::vector<Person> {
        using ReturnId = ReturnId;
        auto rows      = make_people(count);
        auto ids       = (InsertStatement<Person, ConnType>{})
                           .template query<ReturnId::Yes>(std::span<const Person>(rows))
                           .execute();
        EXPECT_TRUE(ids.has_value()) << "seed insert failed";
        if (ids.has_value()) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                rows[i].id = ids.value()[i];
            }
        }
        return rows;
    }

    // seed_with_ids + bump every row's age by 100 (the local copies, not yet
    // persisted) so update(span) has a visible change to write and assert.
    static auto seed_bumped(int count) -> std::vector<Person> {
        auto rows = seed_with_ids(count);
        for (auto& p : rows) {
            p.age += 100;
        }
        return rows;
    }
};

TYPED_TEST_SUITE(PublicTransactionApiTest, DatabaseTypes);

// The guard type is reachable through the public storm:: namespace (re-export).
TYPED_TEST(PublicTransactionApiTest, GuardTypeIsPublic) {
    auto txn = storm::begin(this->conn());
    ASSERT_TRUE(txn.has_value()) << "begin should start a transaction";
    auto commit_result = txn->commit();
    EXPECT_TRUE(commit_result.has_value()) << "explicit commit should succeed";
}

// Success path: several QuerySet ops run atomically, commit persists them.
TYPED_TEST(PublicTransactionApiTest, CommitPersistsMultipleOps) {
    EXPECT_EQ(this->countPersons(), 0);

    auto txn = storm::begin(this->conn());
    ASSERT_TRUE(txn.has_value());

    Person const alice{.name = "Alice", .age = 30};
    Person const bob{.name = "Bob", .age = 25};
    ASSERT_TRUE((InsertStatement<Person, TypeParam>{}).query(alice).execute().has_value());
    ASSERT_TRUE((InsertStatement<Person, TypeParam>{}).query(bob).execute().has_value());

    ASSERT_TRUE(txn->commit().has_value());
    EXPECT_EQ(this->countPersons(), 2) << "both inserts visible after commit";
}

// Failure path: dropping the guard without commit ROLLBACKs everything.
TYPED_TEST(PublicTransactionApiTest, ScopeExitWithoutCommitRollsBack) {
    EXPECT_EQ(this->countPersons(), 0);

    {
        auto txn = storm::begin(this->conn());
        ASSERT_TRUE(txn.has_value());
        Person const alice{.name = "Alice", .age = 30};
        ASSERT_TRUE((InsertStatement<Person, TypeParam>{}).query(alice).execute().has_value());
        // No commit() — guard destructor must ROLLBACK here.
    }

    EXPECT_EQ(this->countPersons(), 0) << "insert must be rolled back on scope exit";
}

// #9 fix: a CHUNKED batch insert issues its own inner BEGIN/COMMIT. Forcing a
// small batch_size (3 chunks of 2 rows) drives that transaction-wrapped path.
// Inside an outer transaction it must NOT raise a nested-BEGIN error.
TYPED_TEST(PublicTransactionApiTest, ChunkedBatchNestedInTransactionDoesNotConflict) {
    auto txn = storm::begin(this->conn());
    ASSERT_TRUE(txn.has_value());

    auto const batch  = this->make_people(5); // 3 chunks (2+2+1) via chunked_opts().
    auto       result = (InsertStatement<Person, TypeParam>{})
                          .query(std::span<const Person>(batch), this->chunked_opts())
                          .execute();
    ASSERT_TRUE(result.has_value()) << "nested chunked batch must not raise a nested-BEGIN error";

    ASSERT_TRUE(txn->commit().has_value());
    EXPECT_EQ(this->countPersons(), 5) << "all chunked rows committed atomically with the outer txn";
}

// #9 fix continued: rolling back the outer transaction must discard the rows the
// nested chunked batch wrote (proving the inner op did NOT self-commit per chunk).
TYPED_TEST(PublicTransactionApiTest, OuterRollbackDiscardsNestedChunkedBatch) {
    {
        auto txn = storm::begin(this->conn());
        ASSERT_TRUE(txn.has_value());
        auto const batch = this->make_people(4); // 2 chunks via chunked_opts().
        ASSERT_TRUE((InsertStatement<Person, TypeParam>{})
                            .query(std::span<const Person>(batch), this->chunked_opts())
                            .execute()
                            .has_value());
        // No commit — outer rollback on scope exit.
    }

    EXPECT_EQ(this->countPersons(), 0) << "nested chunked batch must not have self-committed";
}

// ── storm::transaction(conn, body) scope helper ────────────────────────────────
// Convenience layer over the guard: runs body inside a transaction, COMMITs when
// body returns a value, ROLLBACKs (and propagates) when body returns unexpected.

// Success: body's ops commit together and the returned value is forwarded.
TYPED_TEST(PublicTransactionApiTest, TransactionScopeCommitsOnSuccess) {
    using Error = typename TypeParam::Error;

    auto result = storm::transaction(this->conn(), [&](auto& /*txn*/) -> std::expected<int, Error> {
        if (auto r = (InsertStatement<Person, TypeParam>{}).query(Person{.name = "Alice", .age = 30}).execute(); !r) {
            return std::unexpected(r.error());
        }
        if (auto r = (InsertStatement<Person, TypeParam>{}).query(Person{.name = "Bob", .age = 25}).execute(); !r) {
            return std::unexpected(r.error());
        }
        return 42; // body's value is forwarded out on commit.
    });

    ASSERT_TRUE(result.has_value()) << "scope should commit and forward the body value";
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(this->countPersons(), 2) << "both inserts committed by the scope";
}

// Failure: body returns unexpected → scope ROLLBACKs and propagates the error.
TYPED_TEST(PublicTransactionApiTest, TransactionScopeRollsBackOnUnexpected) {
    using Error = typename TypeParam::Error;

    auto result = storm::transaction(this->conn(), [&](auto& /*txn*/) -> std::expected<void, Error> {
        if (auto r = (InsertStatement<Person, TypeParam>{}).query(Person{.name = "Alice", .age = 30}).execute(); !r) {
            return std::unexpected(r.error());
        }
        return std::unexpected(Error{-1, "deliberate failure after a write"});
    });

    ASSERT_FALSE(result.has_value()) << "scope must propagate the body's error";
    EXPECT_EQ(this->countPersons(), 0) << "the write before the failure must be rolled back";
}

// Nested chunked batch inside the scope cooperates (no nested-BEGIN conflict, #9).
TYPED_TEST(PublicTransactionApiTest, TransactionScopeNestsChunkedBatch) {
    using Error = typename TypeParam::Error;

    auto result = storm::transaction(this->conn(), [&](auto& /*txn*/) -> std::expected<void, Error> {
        auto const batch = this->make_people(5); // 3 chunks via chunked_opts().
        return (InsertStatement<Person, TypeParam>{})
                .query(std::span<const Person>(batch), this->chunked_opts())
                .execute();
    });

    ASSERT_TRUE(result.has_value()) << "nested chunked batch must not conflict inside the scope";
    EXPECT_EQ(this->countPersons(), 5);
}

// ── nested batch UPDATE / ERASE inside an outer transaction (#9 / #414) ─────────
// insert is covered above; update(span) (any size > 1) and chunked erase(span)
// (> MAX_CHUNK_SIZE) drive their own inner TransactionGuard too and must nest.

// update(span) of >1 row wraps its writes in a TransactionGuard. Inside an outer
// transaction it must not raise a nested-BEGIN error, and the outer commit persists.
TYPED_TEST(PublicTransactionApiTest, NestedBatchUpdateCooperatesAndCommits) {
    auto rows = this->seed_bumped(3);

    auto txn = storm::begin(this->conn());
    ASSERT_TRUE(txn.has_value());
    ASSERT_TRUE((UpdateStatement<Person, TypeParam>{}).query(std::span<const Person>(rows)).execute().has_value())
            << "nested batch update must not raise a nested-BEGIN error";
    ASSERT_TRUE(txn->commit().has_value());

    auto bumped = (SelectStatement<Person, TypeParam>{}).where(fields::Person.age >= 100).count().execute();
    ASSERT_TRUE(bumped.has_value());
    EXPECT_EQ(bumped.value(), 3) << "all updated rows persisted with the outer commit";
}

// Outer rollback must discard the nested batch update (the inner guard must not
// have self-committed).
TYPED_TEST(PublicTransactionApiTest, OuterRollbackDiscardsNestedBatchUpdate) {
    auto rows = this->seed_bumped(3);

    {
        auto txn = storm::begin(this->conn());
        ASSERT_TRUE(txn.has_value());
        ASSERT_TRUE((UpdateStatement<Person, TypeParam>{}).query(std::span<const Person>(rows)).execute().has_value());
        // No commit — outer rollback on scope exit.
    }

    auto bumped = (SelectStatement<Person, TypeParam>{}).where(fields::Person.age >= 100).count().execute();
    ASSERT_TRUE(bumped.has_value());
    EXPECT_EQ(bumped.value(), 0) << "nested batch update must not have self-committed";
}

// chunked erase(span) (> MAX_CHUNK_SIZE, 799) wraps multiple DELETEs in a
// TransactionGuard. Inside an outer transaction it must nest and the outer commit
// persists the deletions.
TYPED_TEST(PublicTransactionApiTest, NestedChunkedEraseCooperatesAndCommits) {
    auto rows = this->seed_with_ids(850); // > 799 → chunked erase
    ASSERT_EQ(this->countPersons(), 850);

    auto txn = storm::begin(this->conn());
    ASSERT_TRUE(txn.has_value());
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query(std::span<const Person>(rows)).execute().has_value())
            << "nested chunked erase must not raise a nested-BEGIN error";
    ASSERT_TRUE(txn->commit().has_value());

    EXPECT_EQ(this->countPersons(), 0) << "all rows erased and persisted with the outer commit";
}
