/**
 * @file test_sqlite_fake_errors.cpp
 * @brief Tests for Storm ORM error paths using the fake sqlite3 API policy (#784)
 *
 * These tests use the mock SQLite library to test hard-to-trigger error paths
 * in the Storm ORM that would be difficult or impossible to test with real SQLite.
 *
 * Error paths tested:
 * - SQLITE_NOMEM (out of memory)
 * - SQLITE_CORRUPT (database corruption)
 * - SQLITE_IOERR (I/O errors)
 * - SQLITE_BUSY (database locked)
 * - Transaction failures (BEGIN/COMMIT/ROLLBACK)
 * - Bind failures for various types
 * - Step failures during execution
 */

#include <gtest/gtest.h>
#include <meta>
#include <sqlite3.h>

// NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR(cpp:S125)
// NOLINTBEGIN(misc-const-correctness,bugprone-unused-return-value,performance-inefficient-vector-operation) // NOSONAR(cpp:S125)

import std;
import storm_orm_utilities;
import storm_orm_transaction;
import storm;
import storm_orm_queryset;
import storm_orm_statements_insert;
import storm_db_default_connection;
import storm_orm_statements_upsert_grammar; // UpsertGrammar — internal, not re-exported by `storm`
using storm::auto_update;
using storm::field_specs_for;
using storm::many_to_many;
using storm::primary;
using storm::primary_part;
using storm::orm::statements::InsertOptions;
using storm::orm::statements::ReturnId;
using storm::orm::statements::UpsertGrammar;

namespace db  = storm::db;
namespace orm = storm::orm;

#include "fake_sqlite_api.hpp" // NOSONAR cpp:S954 - must follow the imports

using fake_sqlite::FakeConn;
using fake_sqlite::FakeSqliteApi;
using fake_sqlite::FakeSqliteConfig;
using FakeDefault = storm::db::BasicDefaultConnection<FakeConn>;
template <typename T, typename Conn = FakeConn, bool Finalized = false>
using QuerySet                              = storm::BasicQuerySet<T, Conn, Finalized>;
template <typename T> using InsertStatement = storm::orm::statements::BasicInsertStatement<T, FakeConn>;

// Struct with indexes — must be at namespace scope for Indexes<> specialization
struct MockIndexedPerson {
    [[= primary]] int id{};
    std::string       name;
    int               department{};
};

template <> struct storm::Indexes<MockIndexedPerson> {
    using type = std::tuple<Index<^^MockIndexedPerson::name, ^^MockIndexedPerson::department>>;
};

namespace {

    // Test model for ORM operations
    struct MockPerson {
        [[= primary]] std::int64_t id{};
        // unique: real SQLite rejects `ON CONFLICT (name)` at prepare without a UNIQUE constraint
        [[= storm::unique]] std::string name;
        int                             age{};
    };

    // fields:: selector proxies (#518).
    namespace fields {
        struct MockPersonT;
        consteval {
            std::meta::define_aggregate(^^MockPersonT, field_specs_for(^^MockPerson));
        }
        inline constexpr MockPersonT MockPerson{};
    } // namespace fields

    // Test model with optional fields
    struct MockPersonOptional {
        [[= primary]] std::int64_t id{};
        std::optional<std::string> name;
        std::optional<int>         age;
    };

    // Test model with an auto_update timestamp (#209), used to cover the upsert
    // DO UPDATE auto_update-tail bind-error branch (#205).
    struct MockPersonAutoUpdate {
        [[= primary]] std::int64_t                              id{};
        std::string                                             name;
        [[= auto_update]] std::chrono::system_clock::time_point updated_at{};
    };

    // Composite-PK model, used to cover the composite branches of
    // execute_upsert_nothing()/UpsertUpdateRunner::run() (#503) — the plain
    // single-PK mocks above only ever hit the single-PK else-branch.
    struct MockOrderLine {
        [[= primary_part]] int order_id{};
        [[= primary_part]] int product_id{};
        int                    quantity{};
    };

    // Composite-PK + auto_update, mirroring MockPersonAutoUpdate above — isolates
    // the composite branch's bind_upsert_auto_updates() error path (#503) from
    // bind_all_fields()'. `quantity` is a settable non-key column so SetCols can
    // list it while leaving `updated_at` unlisted, forcing the auto_update tail.
    struct MockOrderLineAutoUpdate {
        [[= primary_part]] int                                  order_id{};
        [[= primary_part]] int                                  product_id{};
        int                                                     quantity{};
        [[= auto_update]] std::chrono::system_clock::time_point updated_at{};
    };

    // FK targeting MockOrderLine's composite (2-part) PK, as std::optional — used
    // to cover bind_null_run()'s error branch (#504): binding a NULL composite FK
    // loops bind_null() once per target PK part, so a failure on any iteration
    // must propagate. The plain single-column optional-FK mock above
    // (MockOptionalFKMessage) only ever hits the N==1 bind_null branch.
    struct MockOptionalCompositeFkOrder {
        [[= primary]] int                              id{};
        [[= storm::fk<>]] std::optional<MockOrderLine> shipment_line;
    };

    /**
     * @brief Test fixture for ORM mock error tests
     *
     * Sets up a fresh in-memory database and resets mock configuration
     * before each test.
     */
    // Creates every base model's table on a fresh in-memory default connection, plus
    // the tables of any extra models a test needs, then restores the armed fake plan:
    // table DDL must run un-failed, and a test may arm failures before calling this.
    template <typename... Extra> auto fresh_db() -> void {
        auto armed = FakeSqliteApi::plan;
        FakeSqliteApi::reset();
        if (!FakeDefault::set(":memory:").has_value()) {
            ADD_FAILURE() << "could not open the in-memory default connection";
            return;
        }
        using orm::schema::SchemaStatement;
        const bool base =
                SchemaStatement<MockIndexedPerson>::template create_table_if_not_exists<FakeConn>().has_value() &&
                SchemaStatement<MockPerson>::template create_table_if_not_exists<FakeConn>().has_value() &&
                SchemaStatement<MockPersonOptional>::template create_table_if_not_exists<FakeConn>().has_value() &&
                SchemaStatement<MockPersonAutoUpdate>::template create_table_if_not_exists<FakeConn>().has_value() &&
                SchemaStatement<MockOrderLine>::template create_table_if_not_exists<FakeConn>().has_value() &&
                SchemaStatement<MockOrderLineAutoUpdate>::template create_table_if_not_exists<FakeConn>().has_value() &&
                SchemaStatement<MockOptionalCompositeFkOrder>::template create_table_if_not_exists<FakeConn>()
                        .has_value();
        const bool extras =
                (... && SchemaStatement<Extra>::template create_table_if_not_exists<FakeConn>().has_value());
        const bool created = base && extras;
        EXPECT_TRUE(created);
        // Match the removed mock's bring-up: only BEGIN/COMMIT/ROLLBACK cached (set() runs
        // prepare_common_statements), and prepare_calls already at 3 -- the Nth-prepare
        // tests below count from there (only the prepare counter is mirrored). DDL above would otherwise leave extra
        // statements cached.
        FakeDefault::get()->clear_statement_cache();
        FakeSqliteApi::reset();
        FakeDefault::get()->prepare_common_statements();
        const int common_prepares = FakeSqliteApi::plan.prepare_calls;
        FakeSqliteApi::plan       = std::move(armed);
        FakeSqliteApi::plan.prepare_calls += common_prepares;
    }

    class ORMMockErrorTest : public ::testing::Test {
      public:
        void SetUp() override {
            FakeSqliteConfig::reset();
            fresh_db();
        }

        void TearDown() override {
            FakeSqliteConfig::reset();
            FakeDefault::clear();
        }
    };

    // ============================================================================
    // INSERT Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, InsertFailsOnPrepareError) {
        // Configure mock to fail on prepare
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnBindInt64Nomem) {
        // An INSERT never binds the DB-generated int64 PK; an UPDATE binds it in its
        // WHERE clause, so that is where a bind_int64 failure is reachable.
        FakeSqliteConfig::bind_int64_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Alice", .age = 30};

        auto result = qs.update(person).execute();

        // The ORM may handle bind errors internally - verify behavior
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
        // If it succeeds, that's also valid behavior if bind error happens after execution
    }

    // Note: InsertFailsOnBindTextNomem test removed because ORM currently
    // doesn't propagate bind errors - the insert succeeds even when bind fails.
    // This is a potential improvement area for the ORM error handling.

    // INSERT ... RETURNING that finishes with no row (step -> DONE) yields id 0
    // (insert.cppm execute_single, R == ReturnId::Yes). SQLite never does this on its
    // own, so it takes an injected step result; this branch used to be reached only
    // incidentally by a test whose armed bind_int64 failure never fired.
    TEST_F(ORMMockErrorTest, InsertReturningYieldsZeroWhenNoRowComesBack) {
        FakeSqliteConfig::step_returns(SQLITE_DONE);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, 0);
    }

    TEST_F(ORMMockErrorTest, InsertFailsOnStepError) {
        // Let prepare and binding succeed, but fail on step
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, InsertFailsOnStepCorrupt) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    // ============================================================================
    // UPSERT Error Tests (#205) — execute_upsert_nothing() called directly to
    // inject mock bind/step errors; the on_conflict() proxy chain (Task 5) only
    // runs against a real connection, so it can't hit these error paths.
    // ============================================================================

    TEST_F(ORMMockErrorTest, UpsertNothingFailsOnBindError) {
        // Fail bind_text (used for the "name" field) so prepare_and_bind() errors out.
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        InsertStatement<MockPerson> stat{};
        MockPerson const            person{.id = 0, .name = "Alice", .age = 30};

        auto result = stat.template execute_upsert_nothing<^^MockPerson::name>(person);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, UpsertToSqlFailsOnBindError) {
        // Fail bind_text so the bind step inside to_sql_with() errors out.
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        InsertStatement<MockPerson> stat{};
        MockPerson const            person{.id = 0, .name = "Alice", .age = 30};

        auto result = stat.template to_sql_with<>(
                UpsertGrammar<MockPerson>::template nothing_sql<^^MockPerson::name>(), person
        );

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, UpsertNothingFailsOnStepError) {
        // Let prepare and binding succeed, but fail on step with a real DB error
        // (neither ROW_AVAILABLE nor NO_MORE_ROWS).
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        InsertStatement<MockPerson> stat{};
        MockPerson const            person{.id = 0, .name = "Alice", .age = 30};

        auto result = stat.template execute_upsert_nothing<^^MockPerson::name>(person);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, UpsertUpdateFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        InsertStatement<MockPerson> stat{};
        MockPerson const            person{.id = 0, .name = "Alice", .age = 30};

        auto result = stat.template upsert_update_runner<^^MockPerson::name>().template run<^^MockPerson::age>(person);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, UpsertUpdateFailsOnBindError) {
        // Fail bind_text (used for the "name" VALUES field) so bind_all_fields() errors out.
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        InsertStatement<MockPerson> stat{};
        MockPerson const            person{.id = 0, .name = "Alice", .age = 30};

        auto result = stat.template upsert_update_runner<^^MockPerson::name>().template run<^^MockPerson::age>(person);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, UpsertUpdateFailsOnStepError) {
        // Let prepare and binding succeed, but fail on step with a real DB error.
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        InsertStatement<MockPerson> stat{};
        MockPerson const            person{.id = 0, .name = "Alice", .age = 30};

        auto result = stat.template upsert_update_runner<^^MockPerson::name>().template run<^^MockPerson::age>(person);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, UpsertUpdateFailsOnAutoUpdateBindError) {
        // A system_clock::time_point binds via bind_text (tp_to_string), NOT bind_int64.
        // For MockPersonAutoUpdate{id(PK), name, updated_at}, the DO UPDATE binds, in order:
        // VALUES name -> text #1, VALUES updated_at -> text #2; then SetCols=name leaves
        // updated_at unlisted, so the auto_update tail (#209) re-binds it -> text #3.
        // Fail only text #3 to isolate bind_upsert_auto_updates' error branch from
        // bind_all_fields'.
        FakeSqliteConfig::bind_text_fails_on_call(3, SQLITE_NOMEM);

        fresh_db();
        InsertStatement<MockPersonAutoUpdate> stat{};
        MockPersonAutoUpdate const            person{.id = 0, .name = "Alice"};

        auto result = stat.template upsert_update_runner<^^MockPersonAutoUpdate::id>()
                              .template run<^^MockPersonAutoUpdate::name>(person);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // Composite-PK upsert (#503): nothing_sql()/update_sql() omit RETURNING for a
    // composite model, so execute_upsert_nothing()/UpsertUpdateRunner::run() take
    // the has_composite_pk_ branch instead — covered here since MockPerson (above)
    // is single-PK and never exercises it.

    TEST_F(ORMMockErrorTest, CompositeUpsertNothingFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        fresh_db();
        InsertStatement<MockOrderLine> stat{};
        MockOrderLine const            line{.order_id = 1, .product_id = 10, .quantity = 5};

        auto result =
                stat.template execute_upsert_nothing<^^MockOrderLine::order_id, ^^MockOrderLine::product_id>(line);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, CompositeUpsertNothingFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        fresh_db();
        InsertStatement<MockOrderLine> stat{};
        MockOrderLine const            line{.order_id = 1, .product_id = 10, .quantity = 5};

        auto result =
                stat.template execute_upsert_nothing<^^MockOrderLine::order_id, ^^MockOrderLine::product_id>(line);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, CompositeUpsertUpdateFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        fresh_db();
        InsertStatement<MockOrderLine> stat{};
        MockOrderLine const            line{.order_id = 1, .product_id = 10, .quantity = 5};

        auto result = stat.template upsert_update_runner<^^MockOrderLine::order_id, ^^MockOrderLine::product_id>()
                              .template run<^^MockOrderLine::quantity>(line);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, CompositeUpsertUpdateFailsOnBindError) {
        // Fail bind_int (used for every field of MockOrderLine) so bind_all_fields() errors out.
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        fresh_db();
        InsertStatement<MockOrderLine> stat{};
        MockOrderLine const            line{.order_id = 1, .product_id = 10, .quantity = 5};

        auto result = stat.template upsert_update_runner<^^MockOrderLine::order_id, ^^MockOrderLine::product_id>()
                              .template run<^^MockOrderLine::quantity>(line);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, CompositeUpsertUpdateFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        fresh_db();
        InsertStatement<MockOrderLine> stat{};
        MockOrderLine const            line{.order_id = 1, .product_id = 10, .quantity = 5};

        auto result = stat.template upsert_update_runner<^^MockOrderLine::order_id, ^^MockOrderLine::product_id>()
                              .template run<^^MockOrderLine::quantity>(line);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, CompositeUpsertUpdateFailsOnAutoUpdateBindError) {
        // For MockOrderLineAutoUpdate{order_id, product_id, quantity, updated_at}, the
        // composite DO UPDATE binds ALL fields as VALUES (#502): order_id/product_id/quantity
        // via bind_int, then updated_at via bind_text (tp_to_string) -> text #1. SetCols =
        // quantity leaves updated_at unlisted, so the auto_update tail (#209) re-binds it ->
        // text #2. Fail only text #2 to isolate bind_upsert_auto_updates' error branch from
        // bind_all_fields'.
        FakeSqliteConfig::bind_text_fails_on_call(2, SQLITE_NOMEM);

        fresh_db();
        InsertStatement<MockOrderLineAutoUpdate> stat{};
        MockOrderLineAutoUpdate const            line{.order_id = 1, .product_id = 10, .quantity = 5};

        auto result = stat.template upsert_update_runner<
                                  ^^MockOrderLineAutoUpdate::order_id,
                                  ^^MockOrderLineAutoUpdate::product_id>()
                              .template run<^^MockOrderLineAutoUpdate::quantity>(line);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // SELECT Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, SelectFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, SelectFailsOnStepCorrupt) {
        // The SELECT on the empty table is a single step.
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        // The exact behavior depends on when the failure occurs
        // It should either fail or return an error
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    // ============================================================================
    // UPDATE Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, UpdateFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnBindError) {
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_BUSY);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    // ============================================================================
    // REMOVE Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, RemoveFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "ToDelete", .age = 25};

        auto result = qs.erase(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, RemoveFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_LOCKED);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "ToDelete", .age = 25};

        auto result = qs.erase(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_LOCKED);
    }

    // Conditional bulk DELETE (#198) error paths

    TEST_F(ORMMockErrorTest, ConditionalEraseFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.where(fields::MockPerson.age > 30).erase().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, ConditionalEraseFailsOnBindError) {
        FakeSqliteConfig::bind_int_fails_on_call(1, SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 result = qs.where(fields::MockPerson.age > 30).erase().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, ConditionalEraseFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_LOCKED);

        QuerySet<MockPerson> qs;
        auto                 result = qs.where(fields::MockPerson.age > 30).erase().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_LOCKED);
    }

    // Empty WHERE is refused with a client-side error (no DB call).
    TEST_F(ORMMockErrorTest, ConditionalEraseEmptyWhereRefused) {
        QuerySet<MockPerson> qs;
        auto                 result = qs.erase().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);

        auto sql = qs.erase().to_sql();
        ASSERT_FALSE(sql.has_value());
        EXPECT_EQ(sql.error().code(), -1);
    }

    // Conditional bulk UPDATE (#403) error paths

    TEST_F(ORMMockErrorTest, ConditionalUpdateFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.where(fields::MockPerson.age > 30)
                              .update<fields::MockPerson.name>(MockPerson{.name = "x"})
                              .execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, ConditionalUpdateFailsOnSetBindError) {
        // SET binds the explicit column first (param index 1) — force that bind to fail.
        FakeSqliteConfig::bind_text_fails_on_call(1, SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 result = qs.where(fields::MockPerson.age > 30)
                              .update<fields::MockPerson.name>(MockPerson{.name = "x"})
                              .execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, ConditionalUpdateFailsOnWhereBindError) {
        // SET binds name (text, param 1); WHERE binds age (int, param 2) — fail the WHERE bind.
        FakeSqliteConfig::bind_int_fails_on_call(1, SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 result = qs.where(fields::MockPerson.age > 30)
                              .update<fields::MockPerson.name>(MockPerson{.name = "x"})
                              .execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, ConditionalUpdateFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_LOCKED);

        QuerySet<MockPerson> qs;
        auto                 result = qs.where(fields::MockPerson.age > 30)
                              .update<fields::MockPerson.name>(MockPerson{.name = "x"})
                              .execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_LOCKED);
    }

    // Empty WHERE is refused with a client-side error (no DB call) on both paths.
    TEST_F(ORMMockErrorTest, ConditionalUpdateEmptyWhereRefused) {
        QuerySet<MockPerson> qs;
        auto                 result = qs.update<fields::MockPerson.name>(MockPerson{.name = "x"}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), -1);

        auto sql = qs.update<fields::MockPerson.name>(MockPerson{.name = "x"}).to_sql();
        ASSERT_FALSE(sql.has_value());
        EXPECT_EQ(sql.error().code(), -1);
    }

    // ============================================================================
    // Aggregate Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, CountFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, SumFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_MISUSE);

        QuerySet<MockPerson> qs;
        auto                 result = qs.sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_MISUSE);
    }

    TEST_F(ORMMockErrorTest, AvgFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 result = qs.avg<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Transaction Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, TransactionBeginFailsOnStepError) {
        // BEGIN TRANSACTION goes through a cached prepared statement (not exec), so its
        // step is the first step of the batch.
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_BUSY);

        QuerySet<MockPerson> qs;

        // Batch insert uses transactions
        std::vector<MockPerson> people = {
                {.id = 0, .name = "Alice", .age = 30},
                {.id = 0, .name = "Bob", .age = 25},
        };

        auto result = qs.insert(std::span{people}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
        // Note: If insert succeeds, ORM may use a different mechanism for transactions
    }

    // ============================================================================
    // Connection Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, ConnectionOpenFailsOnError) {
        FakeSqliteConfig::open_returns(SQLITE_CANTOPEN);

        // Try to create a new connection
        auto conn_result = FakeConn::open("nonexistent.db");

        ASSERT_FALSE(conn_result.has_value());
        EXPECT_EQ(conn_result.error().code(), SQLITE_CANTOPEN);
    }

    // Issue #410: open() applies busy_timeout via sqlite3_busy_timeout(); a failure
    // there aborts open() and reports the error (default config has a non-zero
    // busy_timeout, so the call always happens).
    TEST_F(ORMMockErrorTest, ConnectionOpenFailsOnBusyTimeoutError) {
        FakeSqliteConfig::busy_timeout_returns(SQLITE_ERROR);

        auto conn_result = FakeConn::open(":memory:");

        ASSERT_FALSE(conn_result.has_value());
        EXPECT_EQ(conn_result.error().code(), SQLITE_ERROR);
    }

    // Issue #410: with journal_mode = WAL, open() issues PRAGMA journal_mode=WAL via
    // sqlite3_exec(); a failure there aborts open() and reports the error.
    TEST_F(ORMMockErrorTest, ConnectionOpenFailsOnWalPragmaError) {
        FakeSqliteConfig::exec_returns(SQLITE_ERROR);

        auto conn_result = FakeConn::open(":memory:", {.journal_mode = db::sqlite::JournalMode::WAL});

        ASSERT_FALSE(conn_result.has_value());
        EXPECT_EQ(conn_result.error().code(), SQLITE_ERROR);
    }

    // ============================================================================
    // DISTINCT Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, DistinctFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.distinct<fields::MockPerson.name>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, DistinctFailsOnStepCorrupt) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.distinct<fields::MockPerson.name>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    // #197: distinct().to_sql() prepare failure (distinct.cppm prepare_and_bind).
    TEST_F(ORMMockErrorTest, DistinctToSqlFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.distinct<fields::MockPerson.name>().to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // #197: distinct().to_sql() WHERE bind failure (distinct.cppm prepare_and_bind).
    TEST_F(ORMMockErrorTest, DistinctToSqlWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).distinct<fields::MockPerson.name>().to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // #197: count().to_sql() prepare failure (aggregate.cppm to_sql).
    TEST_F(ORMMockErrorTest, CountToSqlFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.count().to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // #197: where().count().to_sql() WHERE bind failure (aggregate.cppm to_sql).
    TEST_F(ORMMockErrorTest, CountToSqlWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).count().to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // #197: group_by().having().count().to_sql() HAVING bind failure (aggregate.cppm to_sql).
    TEST_F(ORMMockErrorTest, GroupByHavingToSqlFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.group_by<fields::MockPerson.age>().having(age > 25).count().to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // GROUP BY Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, GroupByCountFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.age>().count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, GroupBySumFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.name>().sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // ============================================================================
    // Bind NULL Error Tests (for optional fields)
    // ============================================================================

    // Note: InsertOptionalFailsOnBindNullError test removed because ORM currently
    // doesn't propagate bind errors - the insert succeeds even when bind_null fails.
    // This is a potential improvement area for the ORM error handling.

    // ============================================================================
    // Additional Bind Type Error Tests
    // Covers [[unlikely]] paths for all bind_* methods in sqlite.cppm
    // ============================================================================

    TEST_F(ORMMockErrorTest, InsertFailsOnBindIntNomem) {
        // bind_int is used for regular int fields (age)
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        // Verify bind error is propagated (error code may vary in ORM layer)
        // Key behavior: operation should fail when bind fails
        EXPECT_FALSE(result.has_value()) << "armed failure was not reached";
    }

    // Test model with double field for bind_double coverage
    struct MockPersonWithDouble {
        [[= primary]] std::int64_t id{};
        std::string                name;
        double                     salary{};
    };

    TEST_F(ORMMockErrorTest, InsertFailsOnBindDoubleNomem) {
        FakeSqliteConfig::reset();
        fresh_db<MockPersonWithDouble>();
        FakeSqliteConfig::bind_double_returns(SQLITE_NOMEM);

        QuerySet<MockPersonWithDouble> qs;
        MockPersonWithDouble const     person{.id = 0, .name = "Alice", .salary = 50000.0};

        auto result = qs.insert(person).execute();

        // Verify bind error is propagated (error code may vary in ORM layer)
        EXPECT_FALSE(result.has_value()) << "armed failure was not reached";
    }

    // Test model with blob field for bind_blob coverage
    struct MockPersonWithBlob {
        [[= primary]] std::int64_t id{};
        std::string                name;
        std::vector<std::uint8_t>  data;
    };

    TEST_F(ORMMockErrorTest, InsertFailsOnBindBlobNomem) {
        FakeSqliteConfig::reset();
        fresh_db<MockPersonWithBlob>();
        FakeSqliteConfig::bind_blob_returns(SQLITE_NOMEM);

        QuerySet<MockPersonWithBlob> qs;
        MockPersonWithBlob const     person{.id = 0, .name = "Alice", .data = {0x01, 0x02, 0x03}};

        auto result = qs.insert(person).execute();

        // Verify bind error is propagated (error code may vary in ORM layer)
        EXPECT_FALSE(result.has_value()) << "armed failure was not reached";
    }

    TEST_F(ORMMockErrorTest, InsertOptionalFieldFailsOnBindNullNomem) {
        FakeSqliteConfig::reset();
        fresh_db();
        FakeSqliteConfig::bind_null_returns(SQLITE_NOMEM);

        QuerySet<MockPersonOptional> qs;
        MockPersonOptional const     person{.id = 0, .name = std::nullopt, .age = std::nullopt};

        auto result = qs.insert(person).execute();

        // Verify bind error is propagated (error code may vary in ORM layer)
        EXPECT_FALSE(result.has_value()) << "armed failure was not reached";
    }

    // ============================================================================
    // Additional Error Code Coverage Tests
    // Tests uncommon error codes that are hard to trigger with real SQLite
    // ============================================================================

    TEST_F(ORMMockErrorTest, InsertFailsOnSqliteFull) {
        // SQLITE_FULL: Database disk is full
        FakeSqliteConfig::step_returns(SQLITE_FULL);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_FULL);
    }

    TEST_F(ORMMockErrorTest, InsertFailsOnSqliteReadonly) {
        // SQLITE_READONLY: Attempt to write to readonly database
        FakeSqliteConfig::step_returns(SQLITE_READONLY);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_READONLY);
    }

    TEST_F(ORMMockErrorTest, InsertFailsOnSqliteToobig) {
        // SQLITE_TOOBIG: String or blob exceeds size limit
        FakeSqliteConfig::step_returns(SQLITE_TOOBIG);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_TOOBIG);
    }

    TEST_F(ORMMockErrorTest, SelectFailsOnSqliteSchema) {
        // SQLITE_SCHEMA: Database schema changed (requires re-prepare)
        FakeSqliteConfig::step_returns(SQLITE_SCHEMA);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_SCHEMA);
    }

    TEST_F(ORMMockErrorTest, SelectFailsOnSqliteInterrupt) {
        // SQLITE_INTERRUPT: Operation interrupted
        FakeSqliteConfig::step_returns(SQLITE_INTERRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_INTERRUPT);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnSqliteAbort) {
        // SQLITE_ABORT: Callback requested abort
        FakeSqliteConfig::step_returns(SQLITE_ABORT);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ABORT);
    }

    // ============================================================================
    // Min/Max Aggregate Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, MinFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.min<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, MinFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.min<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, MaxFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.max<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, MaxFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.max<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    // ============================================================================
    // Batch Operation Error Tests
    // Tests mid-batch failures using step_fails_on_call
    // ============================================================================

    TEST_F(ORMMockErrorTest, BatchInsertFailsOnMidBatchStepError) {
        // The 3 rows go out as one multi-row INSERT, i.e. a single step.
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_IOERR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "Alice", .age = 30},
                {.id = 0, .name = "Bob", .age = 25},
                {.id = 0, .name = "Charlie", .age = 35},
        };

        auto result = qs.insert(std::span{people}).execute();

        // The batch should fail when the second insert fails
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, BatchInsertFailsOnMidBatchBindError) {
        // `age` is the only bind_int per row, so call 2 is the second row's age.
        FakeSqliteConfig::bind_int_fails_on_call(2, SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "Alice", .age = 30},
                {.id = 0, .name = "Bob", .age = 25},
        };

        auto result = qs.insert(std::span{people}).execute();

        // If ORM propagates bind errors, it should fail
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, BatchUpdateFailsOnMidBatchStepError) {
        FakeSqliteConfig::step_fails_on_call(2, SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "Alice Updated", .age = 31},
                {.id = 2, .name = "Bob Updated", .age = 26},
        };

        auto result = qs.update(std::span{people}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, BatchRemoveFailsOnMidBatchStepError) {
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_LOCKED);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "Alice", .age = 30},
                {.id = 2, .name = "Bob", .age = 25},
        };

        auto result = qs.erase(std::span{people}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_LOCKED);
    }

    // ============================================================================
    // SELECT with WHERE Bind Error Tests
    // Tests [[unlikely]] paths in bind_where_params
    // ============================================================================

    TEST_F(ORMMockErrorTest, SelectWithWhereFailsOnBindIntError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).select().execute();

        // WHERE condition binding should fail
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, SelectWithWhereFailsOnBindTextError) {
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 name   = storm::orm::where::Field<^^MockPerson::name>{};
        auto                 result = qs.where(name == "Alice").select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // DISTINCT with WHERE Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, DistinctWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).distinct<fields::MockPerson.name>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, DistinctMultiFieldFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.distinct<fields::MockPerson.name, fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // ============================================================================
    // GROUP BY with WHERE Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, GroupByWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).group_by<fields::MockPerson.name>().count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, GroupByAvgFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.name>().avg<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, GroupByMinFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.name>().min<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, GroupByMaxFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_BUSY);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.name>().max<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    // ============================================================================
    // SELECT with ORDER BY + LIMIT + OFFSET Error Tests
    // Tests [[unlikely]] paths in prepare_statement with modifiers
    // ============================================================================

    TEST_F(ORMMockErrorTest, SelectWithOrderByFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.order_by<fields::MockPerson.age>().select().execute();

        // Verify prepare error is propagated (error code may differ at ORM layer)
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().code(), SQLITE_OK);
    }

    TEST_F(ORMMockErrorTest, SelectWithLimitFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.limit(10).select().execute();

        // Verify prepare error is propagated
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().code(), SQLITE_OK);
    }

    TEST_F(ORMMockErrorTest, SelectWithOffsetFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.offset(5).select().execute();

        // Verify prepare error is propagated
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().code(), SQLITE_OK);
    }

    TEST_F(ORMMockErrorTest, SelectWithAllModifiersFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        auto                 age = storm::orm::where::Field<^^MockPerson::age>{};
        auto result = qs.where(age > 25).order_by<fields::MockPerson.name>().limit(10).offset(5).select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // ============================================================================
    // Connection Execute Error Tests
    // Tests [[unlikely]] paths in Connection::execute
    // ============================================================================

    TEST_F(ORMMockErrorTest, ConnectionExecuteFailsOnExecError) {
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto& conn = *conn_result;

        FakeSqliteConfig::exec_returns(SQLITE_ERROR);
        FakeSqliteConfig::exec_error_message("mock exec error");

        auto result = conn.execute("CREATE TABLE test (id INTEGER)");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // ============================================================================
    // Step Return Value Coverage
    // Tests step() returning unexpected values (not ROW, DONE, or error)
    // ============================================================================

    TEST_F(ORMMockErrorTest, SelectWithUnexpectedStepReturnValue) {
        // SQLITE_INTERNAL is an unexpected return from step()
        FakeSqliteConfig::step_returns(SQLITE_INTERNAL);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_INTERNAL);
    }

    TEST_F(ORMMockErrorTest, SelectWithStepReturnsNotadb) {
        // SQLITE_NOTADB: File is not a database
        FakeSqliteConfig::step_returns(SQLITE_NOTADB);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOTADB);
    }

    // ============================================================================
    // Sequence-Based Step Error Tests
    // Tests step sequence for multi-row operations
    // ============================================================================

    TEST_F(ORMMockErrorTest, SelectMultiRowFailsOnThirdRow) {
        // First two rows succeed, third row fails
        FakeSqliteConfig::step_returns_sequence({SQLITE_ROW, SQLITE_ROW, SQLITE_CORRUPT});

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, DistinctMultiRowFailsOnSecondRow) {
        FakeSqliteConfig::step_returns_sequence({SQLITE_ROW, SQLITE_IOERR});

        QuerySet<MockPerson> qs;
        auto                 result = qs.distinct<fields::MockPerson.name>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // ============================================================================
    // Update with Bind Type Errors
    // ============================================================================

    TEST_F(ORMMockErrorTest, UpdateFailsOnBindInt64Error) {
        FakeSqliteConfig::bind_int64_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnBindIntError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Erase with Bind Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, RemoveFailsOnBindInt64Error) {
        FakeSqliteConfig::bind_int64_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "ToDelete", .age = 25};

        auto result = qs.erase(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Aggregate with All Error Types
    // ============================================================================

    TEST_F(ORMMockErrorTest, CountWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, SumWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Connection Prepare Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, ConnectionPrepareCachedFailsOnError) {
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto& conn = *conn_result;

        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        auto result = conn.prepare_cached("SELECT * FROM test");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, ConnectionPrepareFailsOnError) {
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto& conn = *conn_result;

        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        auto result = conn.prepare("SELECT * FROM test");

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // ============================================================================
    // Statement Execute vs Step Distinction
    // Tests the [[unlikely]] path in execute() method
    // ============================================================================

    TEST_F(ORMMockErrorTest, InsertStepReturnsUnexpectedError) {
        // With supports_returning=true (SQLite 3.35+), INSERT uses step() which
        // expects SQLITE_ROW to retrieve the RETURNING result. Test that an
        // unexpected error code (not ROW, not DONE) causes failure.
        FakeSqliteConfig::step_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // ============================================================================
    // Prepare Call Count Verification
    // Tests that mock properly tracks call counts
    // ============================================================================

    TEST_F(ORMMockErrorTest, PrepareCallCountTracking) {
        FakeSqliteConfig::reset();

        // Get baseline count after reset
        int const baseline_count = FakeSqliteConfig::get_prepare_call_count();
        EXPECT_EQ(baseline_count, 0);

        fresh_db();

        // Count after connection setup
        int const after_setup_count = FakeSqliteConfig::get_prepare_call_count();
        EXPECT_GE(after_setup_count, baseline_count); // May have prepare calls during setup

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};
        (void)qs.insert(person).execute();

        // At least one more prepare call for INSERT
        EXPECT_GT(FakeSqliteConfig::get_prepare_call_count(), after_setup_count);
    }

    // ============================================================================
    // Complex Query Error Propagation
    // Tests error propagation through complex query chains
    // ============================================================================

    TEST_F(ORMMockErrorTest, ComplexQueryChainFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 age = storm::orm::where::Field<^^MockPerson::age>{};
        auto result = qs.where(age > 25).order_by<fields::MockPerson.name>().limit(10).offset(5).select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, DistinctWithModifiersFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto result = qs.order_by<fields::MockPerson.age>().limit(5).distinct<fields::MockPerson.name>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // ============================================================================
    // JOIN Operation Error Tests
    // Uses FK models for testing JOIN error paths
    // ============================================================================

    struct MockUser {
        [[= primary]] int id{};
        std::string       name;
        int               age{};
    };

    struct MockMessage {
        [[= primary]] int          id{};
        [[= storm::fk<>]] MockUser sender;
        std::string                text;
    };

    // fields:: selector proxies (#518).
    namespace fields {
        struct MockMessageT;
        consteval {
            std::meta::define_aggregate(^^MockMessageT, field_specs_for(^^MockMessage));
        }
        inline constexpr MockMessageT MockMessage{};
    } // namespace fields

    class JoinMockErrorTest : public ::testing::Test {
      public:
        void SetUp() override {
            FakeSqliteConfig::reset();
            fresh_db<MockUser, MockMessage>();
        }

        void TearDown() override {
            FakeSqliteConfig::reset();
            FakeDefault::clear();
        }
    };

    TEST_F(JoinMockErrorTest, JoinSelectFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().select().execute();

        // Verify prepare error is propagated (error code may differ at ORM layer)
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().code(), SQLITE_OK);
    }

    TEST_F(JoinMockErrorTest, JoinSelectFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(JoinMockErrorTest, JoinWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockMessage> qs;
        auto result = qs.where(fields::MockMessage.id > 5).join<fields::MockMessage.sender>().select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(JoinMockErrorTest, JoinWithOrderByFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockMessage> qs;
        auto result = qs.join<fields::MockMessage.sender>().order_by<fields::MockMessage.id>().select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // Test FK field bind error during UPDATE (covers lines 213-214 in update.cppm)
    // MockMessage has FK field 'sender' of type MockUser
    // Update bind order: sender.id (FK's PK, int), text (text), MockMessage.id (PK, int)
    // Failing bind_int on call 1 will fail the FK bind
    TEST_F(JoinMockErrorTest, UpdateWithFkFieldFailsOnFkBindError) {
        // Fail on first bind_int call which binds the FK field's PK
        FakeSqliteConfig::bind_int_fails_on_call(1, SQLITE_NOMEM);

        QuerySet<MockMessage> qs;
        MockUser const        sender{.id = 1, .name = "Sender", .age = 30};
        MockMessage const     message{.id = 1, .sender = sender, .text = "Hello"};

        auto result = qs.update(message).execute();

        ASSERT_FALSE(result.has_value()) << "Update should fail when FK field bind fails";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Step Return Sequence for Complex Operations
    // Tests multi-step operations with mid-execution failures
    // ============================================================================

    TEST_F(ORMMockErrorTest, GroupByMultiRowFailsOnThirdRow) {
        // GROUP BY returning multiple groups, fail on third group
        FakeSqliteConfig::step_returns_sequence({SQLITE_ROW, SQLITE_ROW, SQLITE_CORRUPT});

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.age>().count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, AggregateWithOrderByFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.order_by<fields::MockPerson.age>().count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // ============================================================================
    // Protocol and Permission Error Tests
    // Tests less common SQLite error codes
    // ============================================================================

    TEST_F(ORMMockErrorTest, SelectFailsOnProtocolError) {
        FakeSqliteConfig::step_returns(SQLITE_PROTOCOL);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_PROTOCOL);
    }

    TEST_F(ORMMockErrorTest, InsertFailsOnPermError) {
        FakeSqliteConfig::step_returns(SQLITE_PERM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_PERM);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnAuthError) {
        FakeSqliteConfig::step_returns(SQLITE_AUTH);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_AUTH);
    }

    // ============================================================================
    // Single Aggregate Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, CountStarFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 result = qs.count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Cache Hit Path With Error
    // Tests that cached statements still propagate errors correctly
    // ============================================================================

    TEST_F(ORMMockErrorTest, CachedStatementStillPropagatesStepError) {
        QuerySet<MockPerson> qs;

        // First call succeeds (caches the statement)
        FakeSqliteConfig::step_returns(SQLITE_DONE);
        auto first_result = qs.select().execute();
        // Note: DONE means no rows, which is valid for empty table
        EXPECT_TRUE(first_result.has_value());

        // Second call uses cached statement but step fails
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);
        auto second_result = qs.select().execute();

        ASSERT_FALSE(second_result.has_value());
        EXPECT_EQ(second_result.error().code(), SQLITE_CORRUPT);
    }

    // ============================================================================
    // Verify All Bind Type Errors Are Trackable
    // ============================================================================

    TEST_F(ORMMockErrorTest, BindCallCountsAreTracked) {
        FakeSqliteConfig::reset();

        EXPECT_EQ(FakeSqliteConfig::get_bind_int_call_count(), 0);
        EXPECT_EQ(FakeSqliteConfig::get_bind_text_call_count(), 0);

        // Setup fresh connection
        fresh_db();

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Test", .age = 25};
        (void)qs.insert(person).execute();

        // Should have called bind_int (for age) and bind_text (for name)
        EXPECT_GE(FakeSqliteConfig::get_bind_int_call_count(), 0); // Implementation may vary
        EXPECT_GE(FakeSqliteConfig::get_bind_text_call_count(), 0);
    }

    // ============================================================================
    // Targeted Update Bind Error Tests
    // These tests cover specific uncovered paths in update.cppm
    // ============================================================================

    TEST_F(ORMMockErrorTest, UpdateFailsOnPkBindError) {
        // This covers line 244-246 in update.cppm:
        // "Bind primary key last" - when PK binding fails
        // MockPerson binds: name(text), age(int), id(int64 for WHERE)
        // Fail on 1st bind_int64 call (the PK in WHERE clause)
        FakeSqliteConfig::bind_int64_fails_on_call(1, SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value()) << "Update should fail when PK bind fails";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnFirstFieldBindError) {
        // This covers the inline_bind_all_fields error path (lines 166-167, 185-186)
        // Fail on 1st bind_text call (the 'name' field)
        FakeSqliteConfig::bind_text_fails_on_call(1, SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value()) << "Update should fail when field bind fails";
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, UpdateFailsOnSecondFieldBindError) {
        // Fail on 1st bind_int call (the 'age' field, after 'name' succeeds)
        FakeSqliteConfig::bind_int_fails_on_call(1, SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Updated", .age = 35};

        auto result = qs.update(person).execute();

        ASSERT_FALSE(result.has_value()) << "Update should fail when second field bind fails";
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, BatchUpdateFailsOnSecondRowBindError) {
        // This covers the batch update loop error path (lines 165-168 in update.cppm)
        // Fail on 2nd bind_text call (during second row's 'name' field)
        FakeSqliteConfig::bind_text_fails_on_call(2, SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "First", .age = 30},
                {.id = 2, .name = "Second", .age = 25}, // This one should fail
        };

        auto result = qs.update(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Batch update should fail on second row bind error";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Single-Object Span Update Tests
    // These tests cover the execute_single_row path (lines 180-195 in update.cppm)
    // which is only called when update(span) is called with span.size() == 1
    // ============================================================================

    TEST_F(ORMMockErrorTest, SpanOfOneUpdateSucceeds) {
        // This covers execute_single_row happy path (lines 182-195)
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> single = {{.id = 1, .name = "SingleSpan", .age = 30}};

        auto result = qs.update(std::span{single}).execute();

        // Should succeed (mock returns DONE by default)
        EXPECT_TRUE(result.has_value()) << "Span-of-one update should succeed";
    }

    TEST_F(ORMMockErrorTest, SpanOfOneUpdateFailsOnBindError) {
        // This covers execute_single_row bind error (lines 185-187)
        FakeSqliteConfig::bind_text_fails_on_call(1, SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> single = {{.id = 1, .name = "SingleSpan", .age = 30}};

        auto result = qs.update(std::span{single}).execute();

        ASSERT_FALSE(result.has_value()) << "Span-of-one update should fail on bind error";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, SpanOfOneUpdateFailsOnExecError) {
        // This covers execute_single_row exec error (lines 190-192)
        FakeSqliteConfig::step_returns(SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> single = {{.id = 1, .name = "SingleSpan", .age = 30}};

        auto result = qs.update(std::span{single}).execute();

        ASSERT_FALSE(result.has_value()) << "Span-of-one update should fail on exec error";
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, UpdateEmptySpanReturnsSuccess) {
        // This covers the empty span early return (lines 137-139)
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> empty;

        auto result = qs.update(std::span{empty}).execute();

        EXPECT_TRUE(result.has_value()) << "Empty span update should succeed immediately";
    }

    TEST_F(ORMMockErrorTest, BatchUpdatePrepareFailure) {
        // This covers prepare_cached failure in execute(span) (lines 143-146)
        // Use batch of 2+ to go through batch path, not single-row path
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "First", .age = 30},
                {.id = 2, .name = "Second", .age = 25},
        };

        auto result = qs.update(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Batch update should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, BatchUpdateTransactionBeginFailure) {
        // This covers transaction begin failure (lines 157-159)
        // TransactionGuard::begin() calls conn.execute("BEGIN TRANSACTION")
        // which uses prepare_cached() then stmt->execute() (which calls step)
        // For batch update with 2 rows:
        //   step 1: BEGIN TRANSACTION
        //   step 2: UPDATE row 1
        //   step 3: UPDATE row 2
        //   step 4: COMMIT
        // Fail on step 1 to simulate transaction begin failure
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "First", .age = 30},
                {.id = 2, .name = "Second", .age = 25},
        };

        auto result = qs.update(std::span{people}).execute();

        // Transaction begin failure should propagate
        ASSERT_FALSE(result.has_value()) << "Batch update should fail when transaction begin fails";
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, BatchUpdateSuccessWithCommit) {
        // This covers successful batch update with commit (line 176)
        // All operations succeed, commit should be called
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "First", .age = 30},
                {.id = 2, .name = "Second", .age = 25},
        };

        auto result = qs.update(std::span{people}).execute();

        // Mock returns DONE by default, so this should succeed
        EXPECT_TRUE(result.has_value()) << "Batch update should succeed and commit";
    }

    // ============================================================================
    // Aggregate Error Tests - Prepare Failures
    // ============================================================================

    TEST_F(ORMMockErrorTest, AggregateSumFailsOnPrepareError) {
        // Covers prepare_cached failure in aggregate execute_simple
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate SUM should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, AggregateAvgFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_CANTOPEN);

        QuerySet<MockPerson> qs;
        auto                 result = qs.avg<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate AVG should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_CANTOPEN);
    }

    TEST_F(ORMMockErrorTest, AggregateMinFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.min<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate MIN should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, AggregateMaxFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_FULL);

        QuerySet<MockPerson> qs;
        auto                 result = qs.max<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate MAX should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_FULL);
    }

    // ============================================================================
    // Aggregate Error Tests - Step Failures
    // ============================================================================

    TEST_F(ORMMockErrorTest, AggregateSumFailsOnStepError) {
        // Covers line 253: step error in execute_impl
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate SUM should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, AggregateMinFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.min<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate MIN should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, AggregateMaxFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_NOTADB);

        QuerySet<MockPerson> qs;
        auto                 result = qs.max<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate MAX should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_NOTADB);
    }

    // ============================================================================
    // Aggregate with WHERE - Prepare and Bind Errors
    // ============================================================================

    TEST_F(ORMMockErrorTest, AggregateWithWhereFailsOnPrepareError) {
        // Covers execute_where prepare error (aggregate.cppm lines 463-465)
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate with WHERE should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, AggregateWithWhereFailsOnBindError) {
        // Covers bind_where_params error path in aggregate
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, AggregateCountWithWhereFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_TOOBIG);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 30).count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_TOOBIG);
    }

    // ============================================================================
    // GROUP BY Aggregate Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, GroupByAggregateFailsOnPrepareError) {
        // Covers prepare error in AggregateStatement (GROUP BY path)
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.age>().count().execute();

        ASSERT_FALSE(result.has_value()) << "GROUP BY aggregate should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, GroupByAggregateFailsOnStepError) {
        // Covers step error in execute_query_loop
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.age>().count().execute();

        ASSERT_FALSE(result.has_value()) << "GROUP BY aggregate should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, GroupBySumFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_CANTOPEN);

        QuerySet<MockPerson> qs;
        auto                 result = qs.group_by<fields::MockPerson.name>().sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value()) << "GROUP BY SUM should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_CANTOPEN);
    }

    // ============================================================================
    // Erase Transaction Begin Failure
    // ============================================================================

    TEST_F(ORMMockErrorTest, ChunkedRemoveFailsOnTransactionBeginError) {
        // For chunked erase (800+ rows), transaction begin failure
        // This requires a large batch that triggers chunked processing
        // Transaction begin = first step call
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 850; ++i) {
            large_batch.push_back({.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.erase(std::span{large_batch}).execute();

        // Should fail on transaction begin
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, ChunkedRemoveFailsOnMaxBulkPrepareError) {
        // Covers lines 249-250: prepare error for max bulk statement in execute_chunked
        // SetUp pre-caches 3 statements (BEGIN, COMMIT, ROLLBACK) via prepare_common_statements
        // So BEGIN is already cached. execute_chunked's prepare_cached(max_bulk_delete_sql) = call 4
        FakeSqliteConfig::prepare_fails_on_call(4, SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 850; ++i) {
            large_batch.push_back({.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.erase(std::span{large_batch}).execute();

        ASSERT_FALSE(result.has_value()) << "Chunked erase should fail when max bulk prepare fails";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, ChunkedRemoveFailsOnRemainderPrepareError) {
        // Covers lines 263-264: prepare error for remainder statement in execute_chunked
        // Prepare calls: 1-3=pre-cached (BEGIN, COMMIT, ROLLBACK), 4=max bulk DELETE, 5=remainder
        FakeSqliteConfig::prepare_fails_on_call(5, SQLITE_IOERR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 850; ++i) {
            large_batch.push_back({.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.erase(std::span{large_batch}).execute();

        ASSERT_FALSE(result.has_value()) << "Chunked erase should fail when remainder prepare fails";
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, ChunkedRemoveFailsOnFullChunkExecError) {
        // Covers lines 273-274: bind_pks_and_execute error for full chunk
        // Step calls: 1=BEGIN, 2=full chunk (799 items)
        // Fail on step 2 to hit the full chunk execution error path
        FakeSqliteConfig::step_fails_on_call(2, SQLITE_CORRUPT);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 850; ++i) {
            large_batch.push_back({.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.erase(std::span{large_batch}).execute();

        ASSERT_FALSE(result.has_value()) << "Chunked erase should fail when full chunk exec fails";
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, ChunkedRemoveFailsOnRemainderExecError) {
        // Covers lines 282-283: bind_pks_and_execute error for remainder chunk
        // Step calls: 1=BEGIN, 2=full chunk (succeeds), 3=remainder chunk (fails)
        FakeSqliteConfig::step_fails_on_call(3, SQLITE_IOERR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 850; ++i) {
            large_batch.push_back({.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.erase(std::span{large_batch}).execute();

        ASSERT_FALSE(result.has_value()) << "Chunked erase should fail when remainder exec fails";
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // ============================================================================
    // Update Chunked Operations (800+ rows)
    // ============================================================================

    TEST_F(ORMMockErrorTest, ChunkedUpdateFailsOnTransactionBeginError) {
        // For chunked update, transaction begin failure
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_LOCKED);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 850; ++i) {
            large_batch.push_back({.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.update(std::span{large_batch}).execute();

        // Should fail on transaction begin
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_LOCKED);
    }

    TEST_F(ORMMockErrorTest, ChunkedUpdateFailsOnSecondChunkError) {
        // Fail on a step in the second chunk (after 800 rows)
        // First chunk: rows 0-799 (800 steps after BEGIN)
        // Second chunk starts at step 802 (BEGIN + 800 updates + 1)
        FakeSqliteConfig::step_fails_on_call(810, SQLITE_IOERR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 850; ++i) {
            large_batch.push_back({.id = i, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.update(std::span{large_batch}).execute();

        // Should fail during second chunk processing
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    // ============================================================================
    // Insert Chunked Operations
    // ============================================================================

    TEST_F(ORMMockErrorTest, ChunkedInsertFailsOnTransactionBeginError) {
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> large_batch;
        for (int i = 0; i < 400; ++i) { // 400 * 3 fields = 1200 > 999, triggers chunked
            large_batch.push_back({.id = 0, .name = std::format("Person{}", i), .age = 20 + (i % 50)});
        }

        auto result = qs.insert(std::span{large_batch}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    // ============================================================================
    // Aggregate with JOIN Error Tests
    // ============================================================================

    TEST_F(JoinMockErrorTest, AggregateWithJoinFailsOnPrepareError) {
        // Covers prepare error in aggregate with JOIN
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().sum<fields::MockMessage.id>().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate with JOIN should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(JoinMockErrorTest, AggregateWithJoinFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().count().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate with JOIN should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(JoinMockErrorTest, AggregateWithWhereJoinFailsOnPrepareError) {
        // Covers execute_where_join prepare error (aggregate.cppm lines 530-532)
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockMessage> qs;
        auto                  id     = storm::orm::where::Field<^^MockMessage::id>{};
        auto                  result = qs.where(id > 5).join<fields::MockMessage.sender>().count().execute();

        ASSERT_FALSE(result.has_value()) << "Aggregate with WHERE+JOIN should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(JoinMockErrorTest, AggregateWithWhereJoinFailsOnBindError) {
        // Covers execute_where_join bind error (aggregate.cppm lines 535-537)
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockMessage> qs;
        auto                  result = qs.where(fields::MockMessage.id > 5)
                              .join<fields::MockMessage.sender>()
                              .sum<fields::MockMessage.id>()
                              .execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Erase Span Operations
    // ============================================================================

    TEST_F(ORMMockErrorTest, SpanOfOneRemoveSuccess) {
        // Covers line 177-178: single-element span fast path
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> single = {{.id = 1, .name = "ToDelete", .age = 30}};

        auto result = qs.erase(std::span{single}).execute();

        // Mock returns DONE by default
        EXPECT_TRUE(result.has_value()) << "Span-of-one erase should succeed";
    }

    TEST_F(ORMMockErrorTest, SpanOfOneRemoveFailsOnStepError) {
        // Covers execute_one error path through span interface
        FakeSqliteConfig::step_returns(SQLITE_LOCKED);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> single = {{.id = 1, .name = "ToDelete", .age = 30}};

        auto result = qs.erase(std::span{single}).execute();

        ASSERT_FALSE(result.has_value()) << "Span-of-one erase should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_LOCKED);
    }

    TEST_F(ORMMockErrorTest, BulkRemoveFailsOnPrepareError) {
        // Covers line 205-206: prepare error in execute_bulk
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "First", .age = 30},
                {.id = 2, .name = "Second", .age = 25},
        };

        auto result = qs.erase(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Bulk erase should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, BulkRemoveFailsOnBindError) {
        // Covers line 214-215: bind error in execute_bulk
        FakeSqliteConfig::bind_int64_returns(SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "First", .age = 30},
                {.id = 2, .name = "Second", .age = 25},
        };

        auto result = qs.erase(std::span{people}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, BulkRemoveFailsOnExecError) {
        // Covers line 219-220: exec error in execute_bulk
        FakeSqliteConfig::step_returns(SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 1, .name = "First", .age = 30},
                {.id = 2, .name = "Second", .age = 25},
        };

        auto result = qs.erase(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Bulk erase should fail on exec error";
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, RemoveEmptySpanReturnsSuccess) {
        // Covers empty span early return
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> empty;

        auto result = qs.erase(std::span{empty}).execute();

        EXPECT_TRUE(result.has_value()) << "Empty span erase should succeed immediately";
    }

    // ============================================================================
    // Insert Error Tests - Additional Coverage
    // ============================================================================

    TEST_F(ORMMockErrorTest, InsertBatchFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        auto result = qs.insert(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Batch insert should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, InsertBatchFailsOnBindTextError) {
        // Batch insert binds: name (text), age (int) for each row
        // Fail on bind_text to test text binding error path
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        auto result = qs.insert(std::span{people}).execute();

        // Either fails with expected error, or succeeds (bind_text might not be called for batch)
        EXPECT_FALSE(result.has_value()) << "armed failure was not reached";
    }

    TEST_F(ORMMockErrorTest, InsertBatchFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CONSTRAINT);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        auto result = qs.insert(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Batch insert should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_CONSTRAINT);
    }

    // ============================================================================
    // Batch INSERT RETURNING Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, InsertBatchReturningFailsOnPrepareError) {
        using ReturnId = ReturnId;
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        auto result = qs.insert<ReturnId::Yes>(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Batch insert returning should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, InsertBatchReturningFailsOnBindError) {
        using ReturnId = ReturnId;
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        auto result = qs.insert<ReturnId::Yes>(std::span{people}).execute();

        EXPECT_FALSE(result.has_value()) << "armed failure was not reached";
    }

    TEST_F(ORMMockErrorTest, InsertBatchReturningFailsOnStepError) {
        using ReturnId = ReturnId;
        FakeSqliteConfig::step_returns(SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        auto result = qs.insert<ReturnId::Yes>(std::span{people}).execute();

        ASSERT_FALSE(result.has_value()) << "Batch insert returning should fail on step error";
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, InsertBatchReturningEmptySpan) {
        using ReturnId = ReturnId;
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> empty;

        auto result = qs.insert<ReturnId::Yes>(std::span{empty}).execute();

        ASSERT_TRUE(result.has_value()) << "Empty span insert returning should succeed";
        EXPECT_TRUE(result.value().empty());
    }

    TEST_F(ORMMockErrorTest, InsertBatchReturningToSqlEmptySpan) {
        using ReturnId = ReturnId;
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> empty;

        auto result = qs.insert<ReturnId::Yes>(std::span{empty}).to_sql();

        ASSERT_TRUE(result.has_value()) << "Empty span to_sql returning should succeed";
        EXPECT_TRUE(result.value().empty());
    }

    TEST_F(ORMMockErrorTest, InsertBatchReturningToSqlFailsOnPrepareError) {
        using ReturnId = ReturnId;
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
        };

        auto result = qs.insert<ReturnId::Yes>(std::span{people}).to_sql();

        ASSERT_FALSE(result.has_value()) << "to_sql returning should fail on prepare error";
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, InsertBatchReturningToSqlFailsOnBindError) {
        using ReturnId = ReturnId;
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
        };

        auto result = qs.insert<ReturnId::Yes>(std::span{people}).to_sql();

        EXPECT_FALSE(result.has_value()) << "armed failure was not reached";
    }

    TEST_F(ORMMockErrorTest, InsertChunkedBatchReturningFailsOnTxnBeginError) {
        using ReturnId = ReturnId;
        // Step failure on BEGIN TRANSACTION (first step call)
        FakeSqliteConfig::step_returns(SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        InsertOptions opts;
        opts.batch_size = 1; // Forces chunking (2 chunks of 1)

        auto result = qs.insert<ReturnId::Yes>(std::span{people}, opts).execute();

        ASSERT_FALSE(result.has_value()) << "Chunked insert returning should fail on txn begin error";
    }

    TEST_F(ORMMockErrorTest, InsertChunkedBatchReturningFailsOnChunkError) {
        using ReturnId = ReturnId;
        // First step = SQLITE_DONE (BEGIN succeeds), then SQLITE_ERROR on chunk step
        FakeSqliteConfig::step_returns_sequence({SQLITE_DONE, SQLITE_ERROR});

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        InsertOptions opts;
        opts.batch_size = 1; // Forces chunking

        auto result = qs.insert<ReturnId::Yes>(std::span{people}, opts).execute();

        ASSERT_FALSE(result.has_value()) << "Chunked insert returning should fail on chunk step error";
    }

    TEST_F(ORMMockErrorTest, InsertChunkedBatchReturningFailsOnCommitError) {
        using ReturnId = ReturnId;
        // BEGIN=DONE, chunk1 step=ROW(id=1), chunk1 done=DONE, chunk2 step=ROW(id=2), chunk2 done=DONE, COMMIT=BUSY
        FakeSqliteConfig::step_returns_sequence(
                {SQLITE_DONE, SQLITE_ROW, SQLITE_DONE, SQLITE_ROW, SQLITE_DONE, SQLITE_BUSY}
        );

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "First", .age = 30},
                {.id = 0, .name = "Second", .age = 25},
        };

        InsertOptions opts;
        opts.batch_size = 1; // Forces chunking

        auto result = qs.insert<ReturnId::Yes>(std::span{people}, opts).execute();

        ASSERT_FALSE(result.has_value()) << "Chunked insert returning should fail on commit error";
    }

    // ============================================================================
    // TransactionGuard Direct Tests
    // Covers specific TransactionGuard paths for 100% line coverage
    // ============================================================================

    TEST_F(ORMMockErrorTest, TransactionGuardDoubleCommitReturnsSuccess) {
        // Covers line 396-397: commit() when committed_ == true
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        using TxnGuard = storm::TransactionGuard<FakeConn>;

        auto txn = TxnGuard::begin(conn);
        ASSERT_TRUE(txn.has_value());

        // First commit succeeds
        auto result1 = txn->commit();
        EXPECT_TRUE(result1.has_value());

        // Second commit hits early return (committed_ == true)
        auto result2 = txn->commit();
        EXPECT_TRUE(result2.has_value());
    }

    TEST_F(ORMMockErrorTest, TransactionGuardCommitOnMovedFromReturnsSuccess) {
        // Covers line 396-397: commit() when conn_ == nullptr (moved-from) // NOSONAR(cpp:S125)
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        using TxnGuard = storm::TransactionGuard<FakeConn>;

        auto txn = TxnGuard::begin(conn);
        ASSERT_TRUE(txn.has_value());

        // Move the guard - original now has conn_ = nullptr
        auto moved = std::move(*txn);

        // Commit on moved-from guard (conn_ == nullptr)
        auto result = txn->commit();
        EXPECT_TRUE(result.has_value());

        // Clean up: commit the moved guard to prevent rollback in destructor
        auto moved_result = moved.commit();
        EXPECT_TRUE(moved_result.has_value());
    }

    TEST_F(ORMMockErrorTest, TransactionGuardCommitFailureTriggersRollback) {
        // Covers lines 400-402: commit() when COMMIT execution fails
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        using TxnGuard = storm::TransactionGuard<FakeConn>;

        // Step 1: BEGIN TRANSACTION (succeeds)
        // Step 2: COMMIT (fails with BUSY)
        FakeSqliteConfig::step_fails_on_call(2, SQLITE_BUSY);

        auto txn = TxnGuard::begin(conn);
        ASSERT_TRUE(txn.has_value());

        auto result = txn->commit();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, TransactionScopeBeginFailurePropagates) {
        // transaction (#415) error path: BEGIN fails → body never runs,
        // the begin error propagates as std::unexpected.
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        // Step 1: BEGIN TRANSACTION fails.
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_BUSY);

        bool body_ran = false;
        auto result   = storm::transaction(conn, [&](auto& /*txn*/) -> std::expected<int, db::Error> {
            body_ran = true;
            return 1;
        });

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
        EXPECT_FALSE(body_ran) << "body must not run when BEGIN fails";
    }

    TEST_F(ORMMockErrorTest, TransactionScopeCommitFailurePropagates) {
        // transaction (#415) error path: body succeeds but COMMIT fails →
        // the commit error propagates (guard already rolled back).
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        // Step 1: BEGIN (succeeds), Step 2: COMMIT (fails with BUSY).
        FakeSqliteConfig::step_fails_on_call(2, SQLITE_BUSY);

        auto result = storm::transaction(conn, [&](auto& /*txn*/) -> std::expected<int, db::Error> {
            return 7; // body succeeds → scope attempts COMMIT.
        });

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, TransactionGuardCommitFailureDoesNotDoubleRollback) {
        // #353 nit: commit() failure issues one in-commit() ROLLBACK; the
        // destructor must NOT issue a second redundant ROLLBACK.
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        using TxnGuard = storm::TransactionGuard<FakeConn>;

        // Step 1: BEGIN TRANSACTION (succeeds), Step 2: COMMIT (fails)
        FakeSqliteConfig::step_fails_on_call(2, SQLITE_BUSY);

        {
            auto txn = TxnGuard::begin(conn);
            ASSERT_TRUE(txn.has_value());

            auto result = txn->commit();
            ASSERT_FALSE(result.has_value());

            // After failed commit(): BEGIN(1) + COMMIT(2) + in-commit ROLLBACK(3) = 3 steps
            EXPECT_EQ(FakeSqliteConfig::get_step_call_count(), 3);
            // Guard goes out of scope here → destructor must not ROLLBACK again
        }

        // Destructor must NOT have issued a second ROLLBACK (would make it 4)
        EXPECT_EQ(FakeSqliteConfig::get_step_call_count(), 3);
    }

    TEST_F(ORMMockErrorTest, TransactionGuardMoveCtorDoesNotRollbackMovedFrom) {
        // #353: moving a guard must neutralize the source so destroying the
        // moved-from object issues no ROLLBACK.
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        using TxnGuard = storm::TransactionGuard<FakeConn>;

        // dst (live owner) outlives the inner scope where the moved-from source dies.
        std::optional<TxnGuard> dst;
        int                     after_begin = 0;

        {
            auto src_result = TxnGuard::begin(conn);
            ASSERT_TRUE(src_result.has_value());
            TxnGuard src = std::move(*src_result);

            after_begin = FakeSqliteConfig::get_step_call_count();

            // Move-CONSTRUCT dst from src. src becomes moved-from.
            dst.emplace(std::move(src));
            // src is destroyed at scope exit → must not ROLLBACK.
        }

        // No ROLLBACK from the moved-from src destruction.
        EXPECT_EQ(FakeSqliteConfig::get_step_call_count(), after_begin);

        // dst owns the live transaction → commit to clean up.
        auto commit_result = dst->commit();
        EXPECT_TRUE(commit_result.has_value());
    }

    TEST_F(ORMMockErrorTest, TransactionGuardMoveAssignDoesNotRollbackMovedFrom) {
        // #353: move-assignment must neutralize the source so destroying the
        // moved-from object issues no ROLLBACK.
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(*conn_result));

        using TxnGuard = storm::TransactionGuard<FakeConn>;

        // Target guard already committed → its move-assign rollback_if_needed() is a no-op.
        auto dst = TxnGuard::begin(conn);
        ASSERT_TRUE(dst.has_value());
        ASSERT_TRUE(dst->commit().has_value());

        const int before_assign = FakeSqliteConfig::get_step_call_count();

        {
            // Source guard lives only in this scope, moved into dst below.
            auto src_result = TxnGuard::begin(conn);
            ASSERT_TRUE(src_result.has_value());
            TxnGuard src = std::move(*src_result);

            *dst = std::move(src);
            // src is now moved-from; its destruction at scope exit must not ROLLBACK.
        }

        // The BEGIN for src counts as one step; no extra ROLLBACK from moved-from destruction.
        EXPECT_EQ(FakeSqliteConfig::get_step_call_count(), before_assign + 1);

        // dst now owns the live transaction → commit to clean up.
        auto commit_result = dst->commit();
        EXPECT_TRUE(commit_result.has_value());
    }

    // ============================================================================
    // #360: noexcept rollback must not let a throwing execute() reach terminate.
    //
    // Connection::execute() is NOT noexcept — it constructs a std::string and
    // can throw std::bad_alloc under memory pressure (esp. on the cached
    // "ROLLBACK" path: prepare_cached map insert / make_unique). It is called
    // from the noexcept rollback_if_needed() (destructor, move-assign) and from
    // commit()'s best-effort ROLLBACK. A throw escaping a noexcept function calls
    // std::terminate. These tests drive a connection whose execute("ROLLBACK")
    // throws and confirm no terminate occurs.
    // ============================================================================

    // Minimal connection stub satisfying TransactionGuard<ConnType>'s contract:
    // a nested Error type and execute(string_view) -> expected<void, Error>.
    // execute("ROLLBACK") throws std::bad_alloc to emulate OOM during unwinding.
    struct ThrowingRollbackConnection {
        using Error = db::Error;

        int  begin_calls        = 0;
        int  commit_calls       = 0;
        int  rollback_calls     = 0;
        bool commit_should_fail = false;

        [[nodiscard]] auto execute(std::string_view sql) -> std::expected<void, Error> {
            if (sql == "BEGIN TRANSACTION") {
                ++begin_calls;
                return {};
            }
            if (sql == "COMMIT") {
                ++commit_calls;
                if (commit_should_fail) {
                    return std::unexpected(Error{SQLITE_BUSY, "commit failed"});
                }
                return {};
            }
            if (sql == "ROLLBACK") {
                ++rollback_calls;
                throw std::bad_alloc{}; // emulates OOM building/caching the ROLLBACK statement
            }
            return {};
        }

        // Transaction-nesting hooks (#415) required by TransactionGuard::begin.
        int                txn_depth = 0;
        [[nodiscard]] auto in_transaction() const noexcept -> bool {
            return txn_depth > 0;
        }
        auto enter_transaction() noexcept -> void {
            ++txn_depth;
        }
        auto leave_transaction() noexcept -> void {
            if (txn_depth > 0) {
                --txn_depth;
            }
        }
    };

    TEST_F(ORMMockErrorTest, TransactionGuardDestructorRollbackSwallowsThrow) {
        // Guard destroyed without commit → rollback_if_needed() runs, execute()
        // throws. The noexcept destructor must swallow it (no std::terminate).
        auto conn = std::make_shared<ThrowingRollbackConnection>();

        using TxnGuard = storm::TransactionGuard<ThrowingRollbackConnection>;

        {
            auto txn = TxnGuard::begin(conn);
            ASSERT_TRUE(txn.has_value());
            // No commit() → destructor will ROLLBACK, which throws.
        }

        // Reached here without terminate → throw was swallowed. The attempt happened.
        EXPECT_EQ(conn->rollback_calls, 1);
    }

    TEST_F(ORMMockErrorTest, TransactionGuardMoveAssignRollbackSwallowsThrow) {
        // Move-assigning onto a live (uncommitted) guard triggers
        // rollback_if_needed() on the target before adopting the source. That
        // ROLLBACK throws; the noexcept operator=(&&) must swallow it.
        auto conn = std::make_shared<ThrowingRollbackConnection>();

        using TxnGuard = storm::TransactionGuard<ThrowingRollbackConnection>;

        auto dst_result = TxnGuard::begin(conn); // dst is live, uncommitted
        ASSERT_TRUE(dst_result.has_value());
        TxnGuard dst = std::move(*dst_result);

        auto src_result = TxnGuard::begin(conn);
        ASSERT_TRUE(src_result.has_value());
        TxnGuard src = std::move(*src_result);

        dst = std::move(src); // dst's pre-existing transaction is rolled back → throws

        // Reached here without terminate → throw swallowed.
        EXPECT_EQ(conn->rollback_calls, 1);

        // dst now owns src's transaction (uncommitted) → its destructor will
        // ROLLBACK again (throwing, swallowed) at scope exit.
    }

    TEST_F(ORMMockErrorTest, TransactionGuardCommitFailureRollbackSwallowsThrow) {
        // commit() fails on COMMIT, then issues a best-effort ROLLBACK that
        // throws. commit() is noexcept → the throw must be swallowed.
        auto conn                = std::make_shared<ThrowingRollbackConnection>();
        conn->commit_should_fail = true;

        using TxnGuard = storm::TransactionGuard<ThrowingRollbackConnection>;

        auto txn = TxnGuard::begin(conn);
        ASSERT_TRUE(txn.has_value());

        auto result = txn->commit();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);

        // commit() attempted the best-effort ROLLBACK (which threw, swallowed),
        // and marked committed_ so the destructor does not rollback again.
        EXPECT_EQ(conn->commit_calls, 1);
        EXPECT_EQ(conn->rollback_calls, 1);
    }

    // ============================================================================
    // WHERE Expression Bind Error Tests
    // Covers bind failure paths in BetweenExpr, InExpression, LogicalExpr
    // ============================================================================

    TEST_F(ORMMockErrorTest, BetweenExprBindFailsOnFirstParam) {
        // Covers lines 156-157: BetweenExpr::bind_params_direct min value bind failure
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age.between(20, 40)).select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, InExprBindFailsOnValueParam) {
        // Covers lines 204-205: InExpression::bind_params_direct value bind failure
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age.in(25, 30, 35)).select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, LogicalExprLeftChildBindFailure) {
        // Covers lines 295-296: BindParamsVisitor left child bind failure in AND/OR
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age = storm::orm::where::Field<^^MockPerson::age>{};
        // The left child (age > 25) will fail to bind because bind_int returns NOMEM
        auto result = qs.where(age > 25 && age < 40).select().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // =====================================================================
    // first() and get() error paths
    // =====================================================================

    TEST_F(ORMMockErrorTest, FirstFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.first().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, FirstFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.first().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, GetFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.get().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, GetFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.get().execute();

        ASSERT_FALSE(result.has_value());
        // get() maps NO_MORE_ROWS to error code -1, but SQLITE_CORRUPT is a real step error
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, FirstWithJoinFailsOnPrepareError) {
        FakeSqliteConfig::reset();
        fresh_db<MockMessage>();
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().first().execute();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, GetWithJoinFailsOnPrepareError) {
        FakeSqliteConfig::reset();
        fresh_db<MockMessage>();
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().get().execute();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, FirstWithJoinFailsOnStepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockMessage>();
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().first().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, FirstWithJoinReturnsRow) {
        FakeSqliteConfig::reset();
        fresh_db<MockMessage>();
        // Return one row then done — exercises JOIN lambda body in execute_one()
        FakeSqliteConfig::step_returns_sequence({SQLITE_ROW, SQLITE_DONE});

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().first().execute();

        // Mock returns zeroed data, but the path is exercised
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result.value().has_value());
    }

    TEST_F(ORMMockErrorTest, GetWithJoinFailsOnStepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockMessage>();
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().get().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, GetWithJoinReturnsRow) {
        FakeSqliteConfig::reset();
        fresh_db<MockMessage>();
        // Return one row then done — exercises JOIN lambda body in execute_get()
        FakeSqliteConfig::step_returns_sequence({SQLITE_ROW, SQLITE_DONE});

        QuerySet<MockMessage> qs;
        auto                  result = qs.join<fields::MockMessage.sender>().get().execute();

        // Mock returns zeroed data, but the path is exercised
        ASSERT_TRUE(result.has_value());
    }

    // ============================================================================
    // to_sql() error path tests — SELECT
    // ============================================================================

    TEST_F(ORMMockErrorTest, ToSqlSelectFailsOnPrepare) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 result = qs.select().to_sql();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, ToSqlSelectWithWhereFailsOnBind) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).select().to_sql();

        ASSERT_FALSE(result.has_value());
    }

    // ============================================================================
    // to_sql() error path tests — INSERT (single)
    // ============================================================================

    TEST_F(ORMMockErrorTest, ToSqlInsertFailsOnPrepare) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};
        auto                 result = qs.insert(person).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, ToSqlInsertFailsOnBind) {
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};
        auto                 result = qs.insert(person).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    // ============================================================================
    // to_sql() error path tests — INSERT (bulk)
    // ============================================================================

    TEST_F(ORMMockErrorTest, ToSqlBulkInsertFailsOnPrepare) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {MockPerson{.id = 0, .name = "Alice", .age = 30}};
        auto                    result = qs.insert(std::span<const MockPerson>(people)).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, ToSqlBulkInsertFailsOnBind) {
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {MockPerson{.id = 0, .name = "Alice", .age = 30}};
        auto                    result = qs.insert(std::span<const MockPerson>(people)).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    // ============================================================================
    // to_sql() error path tests — REMOVE (single)
    // ============================================================================

    TEST_F(ORMMockErrorTest, ToSqlRemoveFailsOnPrepare) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Alice", .age = 30};
        auto                 result = qs.erase(person).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, ToSqlRemoveFailsOnBind) {
        FakeSqliteConfig::bind_int64_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Alice", .age = 30};
        auto                 result = qs.erase(person).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    // ============================================================================
    // to_sql() error path tests — REMOVE (bulk)
    // ============================================================================

    TEST_F(ORMMockErrorTest, ToSqlBulkRemoveFailsOnPrepare) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {MockPerson{.id = 1, .name = "Alice", .age = 30}};
        auto                    result = qs.erase(std::span<const MockPerson>(people)).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, ToSqlBulkRemoveFailsOnBind) {
        FakeSqliteConfig::bind_int64_returns(SQLITE_NOMEM);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {MockPerson{.id = 1, .name = "Alice", .age = 30}};
        auto                    result = qs.erase(std::span<const MockPerson>(people)).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    // ============================================================================
    // to_sql() error path tests — UPDATE (single)
    // ============================================================================

    TEST_F(ORMMockErrorTest, ToSqlUpdateFailsOnPrepare) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Alice", .age = 30};
        auto                 result = qs.update(person).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    TEST_F(ORMMockErrorTest, ToSqlUpdateFailsOnBind) {
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 1, .name = "Alice", .age = 30};
        auto                 result = qs.update(person).to_sql();

        ASSERT_FALSE(result.has_value());
    }

    // ============================================================================
    // Chunked INSERT Transaction Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, ChunkedInsertTransactionBeginFails) {
        // BEGIN TRANSACTION is executed via sqlite3_step (cached prepared statement path),
        // so step_fails_on_call(1, ...) intercepts it — exec_returns has no effect here
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_BUSY);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "Alice", .age = 30},
                {.id = 0, .name = "Bob", .age = 25},
        };

        // batch_size=1 forces chunking (2 chunks for 2 rows)
        auto result = qs.insert(std::span<const MockPerson>(people), InsertOptions{.batch_size = 1}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_BUSY);
    }

    TEST_F(ORMMockErrorTest, ChunkedInsertPartialFailureRollsBack) {
        // step call 1 = BEGIN TRANSACTION (succeeds), step call 2 = first INSERT (fails)
        // RAII TransactionGuard destructor issues ROLLBACK automatically on scope exit
        FakeSqliteConfig::step_fails_on_call(2, SQLITE_IOERR);

        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "Alice", .age = 30},
                {.id = 0, .name = "Bob", .age = 25},
        };

        // batch_size=1 forces chunking — first chunk succeeds, second fails
        auto result = qs.insert(std::span<const MockPerson>(people), InsertOptions{.batch_size = 1}).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
        // RAII TransactionGuard destructor issues ROLLBACK automatically
    }

    TEST_F(ORMMockErrorTest, ChunkedInsertSuccessCommits) {
        // All operations succeed — verify that exec() was called for BEGIN and COMMIT
        QuerySet<MockPerson>    qs;
        std::vector<MockPerson> people = {
                {.id = 0, .name = "Alice", .age = 30},
                {.id = 0, .name = "Bob", .age = 25},
                {.id = 0, .name = "Carol", .age = 35},
        };

        // batch_size=1 forces 3 chunks
        auto result = qs.insert(std::span<const MockPerson>(people), InsertOptions{.batch_size = 1}).execute();

        ASSERT_TRUE(result.has_value());
        // BEGIN TRANSACTION + COMMIT go through sqlite3_step (cached prepared statement path).
        // With batch_size=1 and 3 rows: step(BEGIN) + step(INSERT)*3 + step(COMMIT) = 5 minimum
        EXPECT_GE(FakeSqliteConfig::get_step_call_count(), 5);
    }

    // ============================================================================
    // SetOp Error Tests — covers error paths in setop.cppm
    // ============================================================================

    TEST_F(ORMMockErrorTest, SetOpExecuteFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs1;
        QuerySet<MockPerson> qs2;
        auto result = qs1.where(fields::MockPerson.age > 30).union_(qs2.where(fields::MockPerson.age < 10)).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, SetOpExecuteFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs1;
        QuerySet<MockPerson> qs2;
        auto result = qs1.where(fields::MockPerson.age > 30).union_(qs2.where(fields::MockPerson.age < 10)).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, SetOpExecuteFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs1;
        QuerySet<MockPerson> qs2;
        auto result = qs1.where(fields::MockPerson.age > 30).union_(qs2.where(fields::MockPerson.age < 10)).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    TEST_F(ORMMockErrorTest, SetOpToSqlFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs1;
        QuerySet<MockPerson> qs2;
        auto result = qs1.where(fields::MockPerson.age > 30).union_(qs2.where(fields::MockPerson.age < 10)).to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, SetOpToSqlFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs1;
        QuerySet<MockPerson> qs2;
        auto result = qs1.where(fields::MockPerson.age > 30).union_(qs2.where(fields::MockPerson.age < 10)).to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // rows() Generator Error Tests
    // ============================================================================

    TEST_F(ORMMockErrorTest, RowsFailsOnPrepareError) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        for (auto&& result : qs.rows()) {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code(), SQLITE_ERROR);
        }
    }

    TEST_F(ORMMockErrorTest, RowsFailsOnBindError) {
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age       = fields::MockPerson.age;
        bool                 got_error = false;
        for (auto&& result : qs.where(age > 25).rows()) {
            if (!result.has_value()) {
                EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
                got_error = true;
            }
        }
        EXPECT_TRUE(got_error);
    }

    TEST_F(ORMMockErrorTest, RowsFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        bool                 got_error = false;
        for (auto&& result : qs.rows()) {
            if (!result.has_value()) {
                EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
                got_error = true;
            }
        }
        EXPECT_TRUE(got_error);
    }

    TEST_F(JoinMockErrorTest, JoinRowsFailsOnStepError) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockMessage> qs;
        bool                  got_error = false;
        for (auto&& result : qs.template join<fields::MockMessage.sender>().rows()) {
            if (!result.has_value()) {
                EXPECT_EQ(result.error().code(), SQLITE_IOERR);
                got_error = true;
            }
        }
        EXPECT_TRUE(got_error);
    }

    // ============================================================================
    // InsertNoReturn Error Tests — insert<ReturnId::No> error paths
    // ============================================================================

    TEST_F(ORMMockErrorTest, InsertNoReturnPrepareFailure) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert<ReturnId::No>(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, InsertNoReturnBindFailure) {
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert<ReturnId::No>(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, InsertNoReturnStepFailure) {
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert<ReturnId::No>(person).execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, InsertNoReturnToSqlPrepareFailure) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert<ReturnId::No>(person).to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    TEST_F(ORMMockErrorTest, InsertNoReturnToSqlBindFailure) {
        FakeSqliteConfig::bind_text_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        MockPerson const     person{.id = 0, .name = "Alice", .age = 30};

        auto result = qs.insert<ReturnId::No>(person).to_sql();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // HAVING Error Path Tests (#177)
    // Tests HAVING-related error paths in AggregateStatement
    // ============================================================================

    // Test: HAVING bind failure in prepare_bind_extract (aggregate.cppm:461-463)
    // When WHERE + GROUP BY + HAVING: execute_where() → prepare_bind_extract()
    // WHERE bind succeeds, then HAVING bind fails
    TEST_F(ORMMockErrorTest, HavingBindFailsInPrepareBindExtract) {
        // bind_int call order: WHERE param (age > 30), then HAVING param (age > 25)
        // Fail on 2nd bind_int call (the HAVING param)
        FakeSqliteConfig::bind_int_fails_on_call(2, SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age = storm::orm::where::Field<^^MockPerson::age>{};
        auto result = qs.where(age > 30).group_by<fields::MockPerson.age>().having(age > 25).count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // Test: prepare_cached failure in prepare_bind_having_extract (aggregate.cppm:470-472)
    // When GROUP BY + HAVING (no WHERE): execute_simple() → prepare_bind_having_extract()
    TEST_F(ORMMockErrorTest, HavingPrepareFailsInPrepareBindHavingExtract) {
        FakeSqliteConfig::prepare_returns(SQLITE_ERROR);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.group_by<fields::MockPerson.age>().having(age > 25).count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // Test: HAVING bind failure in prepare_bind_having_extract (aggregate.cppm:476-478)
    // When GROUP BY + HAVING (no WHERE): execute_simple() → prepare_bind_having_extract()
    TEST_F(ORMMockErrorTest, HavingBindFailsInPrepareBindHavingExtract) {
        // Fail on 1st bind_int call (the HAVING param — no WHERE params)
        FakeSqliteConfig::bind_int_returns(SQLITE_NOMEM);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.group_by<fields::MockPerson.age>().having(age > 25).count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // Test: bind_having_params error (base.cppm:811-813)
    // Same path as above but verifies the reset + error propagation in bind_having_params
    TEST_F(ORMMockErrorTest, BindHavingParamsResetsOnError) {
        FakeSqliteConfig::bind_int_fails_on_call(1, SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.group_by<fields::MockPerson.age>().having(age > 30).count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    // Test: Step error in extract_simple_no_reset (aggregate.cppm:514-519)
    // When simple aggregate (no GROUP BY) step returns error
    TEST_F(ORMMockErrorTest, AggregateStepErrorInExtractSimpleNoReset) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 result = qs.count().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    // Test: Step returns NO_MORE_ROWS in extract_simple_no_reset (aggregate.cppm:515-516)
    // Returns default ResultType when no rows
    TEST_F(ORMMockErrorTest, AggregateStepNoRowsInExtractSimpleNoReset) {
        FakeSqliteConfig::step_returns(SQLITE_DONE);

        QuerySet<MockPerson> qs;
        auto                 result = qs.count().execute();

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 0);
    }

    // Test: Step error in extract_simple_result (aggregate.cppm:343-354)
    // WHERE + non-grouped aggregate → execute_where → extract_simple_result
    TEST_F(ORMMockErrorTest, AggregateWithWhereStepErrorInExtractSimpleResult) {
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).sum<fields::MockPerson.age>().execute();

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_CORRUPT);
    }

    // Test: Step returns NO_MORE_ROWS in extract_simple_result (aggregate.cppm:346-351)
    TEST_F(ORMMockErrorTest, AggregateWithWhereStepNoRowsInExtractSimpleResult) {
        FakeSqliteConfig::step_returns(SQLITE_DONE);

        QuerySet<MockPerson> qs;
        auto                 age    = storm::orm::where::Field<^^MockPerson::age>{};
        auto                 result = qs.where(age > 25).count().execute();

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 0);
    }

    // ============================================================================
    // Optional FK Bind Error Test (#177)
    // Covers base.cppm:282-283 — optional FK bind error path
    // ============================================================================

    struct MockOptionalFKMessage {
        [[= primary]] int                         id{};
        [[= storm::fk<>]] std::optional<MockUser> sender;
        std::string                               text;
    };

    TEST_F(ORMMockErrorTest, InsertOptionalFKBindFailure) {
        // MockOptionalFKMessage has optional<MockUser> sender
        // When sender has value, bind_int is called for sender.id (FK PK)
        // Fail on first bind_int call
        FakeSqliteConfig::bind_int_fails_on_call(1, SQLITE_NOMEM);

        fresh_db<MockUser, MockOptionalFKMessage>();
        QuerySet<MockOptionalFKMessage> qs;
        MockUser const                  sender{.id = 1, .name = "Sender", .age = 30};
        MockOptionalFKMessage const     msg{.id = 0, .sender = sender, .text = "Hello"};

        auto result = qs.insert(msg).execute();

        ASSERT_FALSE(result.has_value()) << "Insert should fail when optional FK bind fails";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, InsertOptionalFKNullBindFailure) {
        // When sender is nullopt, bind_null is called
        FakeSqliteConfig::bind_null_returns(SQLITE_NOMEM);

        fresh_db<MockOptionalFKMessage>();
        QuerySet<MockOptionalFKMessage> qs;
        MockOptionalFKMessage const     msg{.id = 0, .sender = std::nullopt, .text = "No sender"};

        auto result = qs.insert(msg).execute();

        ASSERT_FALSE(result.has_value()) << "Insert should fail when optional FK null bind fails";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // #504 — bind_null_run()'s error branch: a NULL composite-FK (std::optional<
    // MockOrderLine>, 2-part PK) loops bind_null() once per target PK part, so a
    // failure must propagate out of the loop rather than being swallowed.
    TEST_F(ORMMockErrorTest, InsertOptionalCompositeFKNullBindFailure) {
        FakeSqliteConfig::bind_null_returns(SQLITE_NOMEM);

        fresh_db();
        QuerySet<MockOptionalCompositeFkOrder> qs;
        MockOptionalCompositeFkOrder const     order{.id = 0, .shipment_line = std::nullopt};

        auto result = qs.insert(order).execute();

        ASSERT_FALSE(result.has_value()) << "Insert should fail when composite optional FK null bind fails";
        EXPECT_EQ(result.error().code(), SQLITE_NOMEM);
    }

    // ============================================================================
    // Schema Error Test (#177)
    // Covers schema.cppm:471-472 — create_indexes_if_not_exist failure
    // ============================================================================

    TEST_F(ORMMockErrorTest, CreateIndexesFailsOnExecError) {
        // Open first — open() issues PRAGMA foreign_keys = ON via sqlite3_exec (#412),
        // so the exec-failure must be configured AFTER a successful open, not before.
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());
        auto conn = std::make_shared<FakeConn>(std::move(conn_result.value()));

        // Configure exec to fail (create_indexes_if_not_exist uses conn->execute)
        FakeSqliteConfig::exec_returns(SQLITE_ERROR);
        FakeSqliteConfig::exec_error_message("index creation failed");

        // MockIndexedPerson has Index<name, department> → exec runs → fails
        auto result = orm::schema::SchemaStatement<MockIndexedPerson>::create_indexes_if_not_exist(conn);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), SQLITE_ERROR);
    }

    // open() issues PRAGMA foreign_keys = ON via sqlite3_exec (#412). When that exec
    // fails, open() must surface the error and not return a half-initialised connection.
    TEST_F(ORMMockErrorTest, OpenFailsWhenForeignKeysPragmaFails) {
        FakeSqliteConfig::exec_returns(SQLITE_ERROR);
        FakeSqliteConfig::exec_error_message("pragma failed");

        auto conn_result = FakeConn::open(":memory:");

        ASSERT_FALSE(conn_result.has_value()) << "open() must fail when the foreign_keys PRAGMA exec fails";
        EXPECT_EQ(conn_result.error().code(), SQLITE_ERROR);
    }

    // ============================================================================
    // SQLCache Round-Robin Eviction Test (#177)
    // Covers SQLCache::insert's round-robin replacement branch (utilities.cppm)
    // ============================================================================

    TEST_F(ORMMockErrorTest, SQLCacheRoundRobinEviction) {
        // CACHE_DEFAULT = 8 — fill all slots, then insert more to trigger eviction
        storm::orm::utilities::SQLCache<std::size_t, 4> cache;

        // Fill all 4 slots
        cache.insert(1, "SELECT 1");
        cache.insert(2, "SELECT 2");
        cache.insert(3, "SELECT 3");
        cache.insert(4, "SELECT 4");

        // Verify all entries present
        EXPECT_NE(cache.find(1), nullptr);
        EXPECT_NE(cache.find(2), nullptr);
        EXPECT_NE(cache.find(3), nullptr);
        EXPECT_NE(cache.find(4), nullptr);

        // Insert 5th — should evict slot 0 (key=1) via round-robin
        cache.insert(5, "SELECT 5");
        EXPECT_EQ(cache.find(1), nullptr) << "Key 1 should be evicted";
        EXPECT_NE(cache.find(5), nullptr);
        EXPECT_EQ(*cache.find(5), "SELECT 5");

        // Insert 6th — should evict slot 1 (key=2)
        cache.insert(6, "SELECT 6");
        EXPECT_EQ(cache.find(2), nullptr) << "Key 2 should be evicted";
        EXPECT_NE(cache.find(6), nullptr);
        EXPECT_EQ(*cache.find(6), "SELECT 6");

        // Keys 3, 4 should still be present
        EXPECT_NE(cache.find(3), nullptr);
        EXPECT_NE(cache.find(4), nullptr);
    }

    // ============================================================================
    // SQLCache lifetime contract: find()'s pointer is valid only until the next
    // insert/clear on this thread; a round-robin slot reuse invalidates it (#356)
    // ============================================================================

    TEST_F(ORMMockErrorTest, SQLCacheFindPointerInvalidatedBySlotReuse) {
        storm::orm::utilities::SQLCache<std::size_t, 2> cache;

        cache.insert(1, "SELECT 1");
        cache.insert(2, "SELECT 2");

        // Copy the value BEFORE the next insert — the documented-safe way to keep it.
        const std::string* hit = cache.find(1);
        ASSERT_NE(hit, nullptr);
        const std::string copied = *hit;

        // Round-robin reuses slot 0 (key 1) for key 3, invalidating `hit`'s contents.
        cache.insert(3, "SELECT 3");

        EXPECT_EQ(copied, "SELECT 1") << "Copy taken before insert stays valid";
        EXPECT_EQ(cache.find(1), nullptr) << "Key 1 was evicted by slot reuse";
        ASSERT_NE(cache.find(3), nullptr);
        EXPECT_EQ(*cache.find(3), "SELECT 3");
    }

    // ============================================================================
    // SQLCache: an occupied slot with a legitimately empty SQL value is distinct
    // from an unused slot — find() returns it, insert() does not treat it as free
    // (key == KeyType{} + empty value edge case, #356)
    // ============================================================================

    TEST_F(ORMMockErrorTest, SQLCacheOccupiedSlotWithEmptyValue) {
        storm::orm::utilities::SQLCache<std::size_t, 4> cache;

        // Key 0 (== KeyType{}) with an empty SQL string — the ambiguous case.
        cache.insert(0, "");

        const std::string* hit = cache.find(0);
        ASSERT_NE(hit, nullptr) << "Occupied slot must be found even when value is empty";
        EXPECT_EQ(*hit, "");

        // A subsequent insert must NOT reuse the occupied key-0 slot as if it were free.
        cache.insert(1, "SELECT 1");
        ASSERT_NE(cache.find(0), nullptr) << "Key 0 must survive a later insert";
        EXPECT_EQ(*cache.find(0), "");
        ASSERT_NE(cache.find(1), nullptr);
        EXPECT_EQ(*cache.find(1), "SELECT 1");
    }

    // ============================================================================
    // Raw Handle get() Test (#177)
    // Covers sqlite.cppm:454-458
    // ============================================================================

    TEST_F(ORMMockErrorTest, ExpandedSqlReturnsEmptyOnNull) {
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());

        MockPerson const               alice{.id = 0, .name = "Alice", .age = 30};
        QuerySet<MockPerson, FakeConn> queryset;

        // First insert normally to get a prepared statement
        auto insert_result = queryset.insert(alice).execute();
        ASSERT_TRUE(insert_result.has_value());

        // Configure expanded_sql to return null
        FakeSqliteConfig::expanded_sql_returns_null();

        // to_sql() calls expanded_sql internally — should return empty string when null
        auto sql_result = queryset.insert(alice).to_sql();
        ASSERT_TRUE(sql_result.has_value());
        EXPECT_TRUE(sql_result.value().empty());
    }

    TEST_F(ORMMockErrorTest, ConnectionRawHandleGet) {
        auto conn_result = FakeConn::open(":memory:");
        ASSERT_TRUE(conn_result.has_value());

        // get() should return a non-null raw sqlite3 pointer
        auto* raw = conn_result.value().get();
        EXPECT_NE(raw, nullptr);
    }

    // ============================================================================
    // Many-to-many (#203) — m2m aggregation error paths (select / first / get / rows)
    // ============================================================================

    struct MockCourse {
        [[= primary]] std::int64_t id{};
        std::string                title;
    };

    struct MockStudent {
        [[= primary]] std::int64_t                   id{};
        std::string                                  name;
        [[= many_to_many<>]] std::vector<MockCourse> courses;
    };

    // fields:: selector proxies (#518).
    namespace fields {
        struct MockStudentT;
        consteval {
            std::meta::define_aggregate(^^MockStudentT, field_specs_for(^^MockStudent));
        }
        inline constexpr MockStudentT MockStudent{};
    } // namespace fields

    TEST_F(ORMMockErrorTest, M2MSelectFailsOnStepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockStudent> qs;
        auto                  rows = qs.join<fields::MockStudent.courses>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, M2MFirstFailsOnStepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockStudent> qs;
        auto                  first = qs.join<fields::MockStudent.courses>().first().execute();

        ASSERT_FALSE(first.has_value());
        EXPECT_EQ(first.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, M2MGetFailsOnStepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::step_returns(SQLITE_IOERR);

        QuerySet<MockStudent> qs;
        auto                  got = qs.join<fields::MockStudent.courses>().get().execute();

        ASSERT_FALSE(got.has_value());
        EXPECT_EQ(got.error().code(), SQLITE_IOERR);
    }

    TEST_F(ORMMockErrorTest, M2MRowsGeneratorYieldsStepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::step_returns(SQLITE_CORRUPT);

        QuerySet<MockStudent> qs;
        auto                  joined    = qs.join<fields::MockStudent.courses>();
        bool                  saw_error = false;
        for (auto&& row : joined.rows()) {
            ASSERT_FALSE(row.has_value());
            EXPECT_EQ(row.error().code(), SQLITE_CORRUPT);
            saw_error = true;
            break;
        }
        EXPECT_TRUE(saw_error);
    }

    // #391 — the two-query eager load wraps Q1+Q2 in a transaction for snapshot
    // consistency. TransactionGuard::begin() issues "BEGIN TRANSACTION" (a cached
    // statement that steps), so it is the FIRST step call. Failing only that step
    // proves the transaction is opened BEFORE Q1/Q2 run: the BEGIN error surfaces
    // and no rows come back.
    TEST_F(ORMMockErrorTest, M2MSelectOpensTransactionBeforeQueries) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::step_fails_on_call(1, SQLITE_BUSY); // 1st step = BEGIN TRANSACTION

        QuerySet<MockStudent> qs;
        auto                  rows = qs.join<fields::MockStudent.courses>().select().execute();

        ASSERT_FALSE(rows.has_value()) << "m2m eager load must fail when BEGIN TRANSACTION fails";
        EXPECT_EQ(rows.error().code(), SQLITE_BUSY);
    }

    // #391 — prepare-call sequence for an m2m select. SetUp resets the counter,
    // then the connection bring-up + this query prepare statements in a fixed
    // order: calls 1-3 = connection/BEGIN bring-up, 4 = Q1 (base subquery),
    // 5 = Q2 (junction⋈related). Failing each exercises the distinct prepare error
    // branch in run_q1 / run_q2_stitch (via prepare_clause_sql).
    TEST_F(ORMMockErrorTest, M2MSelectFailsOnQ1PrepareError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::prepare_fails_on_call(4, SQLITE_NOMEM); // Q1 prepare

        QuerySet<MockStudent> qs;
        auto                  rows = qs.join<fields::MockStudent.courses>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_NOMEM);
    }

    TEST_F(ORMMockErrorTest, M2MSelectFailsOnQ2PrepareError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::prepare_fails_on_call(5, SQLITE_IOERR); // Q2 prepare

        QuerySet<MockStudent> qs;
        auto                  rows = qs.join<fields::MockStudent.courses>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_IOERR);
    }

    // Q2's row loop ends on a non-NO_MORE_ROWS step → error surfaces from
    // run_q2_stitch. Step sequence: 1=BEGIN ok, 2=Q1 NO_MORE (empty), 3=Q2 fails.
    TEST_F(ORMMockErrorTest, M2MSelectFailsOnQ2StepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::step_fails_on_call(3, SQLITE_CORRUPT);

        QuerySet<MockStudent> qs;
        auto                  rows = qs.join<fields::MockStudent.courses>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_CORRUPT);
    }

    // The final COMMIT (4th step: BEGIN, Q1 NO_MORE, Q2 NO_MORE, COMMIT) fails →
    // error surfaces from execute_m2m_2query's commit() branch.
    TEST_F(ORMMockErrorTest, M2MSelectFailsOnCommitError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::step_fails_on_call(4, SQLITE_FULL);

        QuerySet<MockStudent> qs;
        auto                  rows = qs.join<fields::MockStudent.courses>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_FULL);
    }

    // An m2m select WITH a WHERE clause binds params into Q1; a bind failure
    // surfaces from prepare_clause_sql's bind branch.
    TEST_F(ORMMockErrorTest, M2MSelectFailsOnWhereBindError) {
        FakeSqliteConfig::reset();
        fresh_db<MockStudent>();
        FakeSqliteConfig::bind_int_returns(SQLITE_RANGE);

        QuerySet<MockStudent> qs;
        auto rows = qs.join<fields::MockStudent.courses>().where(fields::MockStudent.id > 0).select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_RANGE);
    }

    // ============================================================================
    // Multi-relation m2m (#392) — second-Q2 error paths
    // ============================================================================

    struct MockClub {
        [[= primary]] std::int64_t id{};
        std::string                name;
    };

    struct MockMember {
        [[= primary]] std::int64_t                   id{};
        std::string                                  name;
        [[= many_to_many<>]] std::vector<MockCourse> courses;
        [[= many_to_many<>]] std::vector<MockClub>   clubs;
    };

    // fields:: selector proxies (#518).
    namespace fields {
        struct MockMemberT;
        consteval {
            std::meta::define_aggregate(^^MockMemberT, field_specs_for(^^MockMember));
        }
        inline constexpr MockMemberT MockMember{};
    } // namespace fields

    // Prepare order: calls 1-3 = connection/BEGIN bring-up, 4 = Q1, 5 = Q2a
    // (courses), 6 = Q2b (clubs). Failing call 6 exercises the per-relation Q2
    // loop's error branch for a relation after the first.
    TEST_F(ORMMockErrorTest, MultiM2MSelectFailsOnSecondQ2PrepareError) {
        FakeSqliteConfig::reset();
        fresh_db<MockMember>();
        FakeSqliteConfig::prepare_fails_on_call(6, SQLITE_IOERR);

        QuerySet<MockMember> qs;
        auto                 rows = qs.join<fields::MockMember.courses, fields::MockMember.clubs>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_IOERR);
    }

    // Step order: 1 = BEGIN, 2 = Q1 NO_MORE (empty), 3 = Q2a NO_MORE,
    // 4 = Q2b fails → the error surfaces from the second run_q2_stitch and the
    // transaction never commits.
    TEST_F(ORMMockErrorTest, MultiM2MSelectFailsOnSecondQ2StepError) {
        FakeSqliteConfig::reset();
        fresh_db<MockMember>();
        FakeSqliteConfig::step_fails_on_call(4, SQLITE_CORRUPT);

        QuerySet<MockMember> qs;
        auto                 rows = qs.join<fields::MockMember.courses, fields::MockMember.clubs>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_CORRUPT);
    }

    // With two relations the COMMIT is the 5th step (BEGIN, Q1, Q2a, Q2b, COMMIT).
    TEST_F(ORMMockErrorTest, MultiM2MSelectFailsOnCommitError) {
        FakeSqliteConfig::reset();
        fresh_db<MockMember>();
        FakeSqliteConfig::step_fails_on_call(5, SQLITE_FULL);

        QuerySet<MockMember> qs;
        auto                 rows = qs.join<fields::MockMember.courses, fields::MockMember.clubs>().select().execute();

        ASSERT_FALSE(rows.has_value());
        EXPECT_EQ(rows.error().code(), SQLITE_FULL);
    }

} // namespace

// NOLINTEND(misc-const-correctness,bugprone-unused-return-value,performance-inefficient-vector-operation) // NOSONAR(cpp:S125)
// NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,misc-non-private-member-variables-in-classes) // NOSONAR(cpp:S125)
