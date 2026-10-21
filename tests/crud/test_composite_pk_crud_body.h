#pragma once

// Shared test body for test_composite_pk_crud_sqlite.cpp / test_composite_pk_crud_pg.cpp — the two
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
#error "test_composite_pk_crud_body.h: define STORM_SPLIT_TYPES/STORM_SPLIT_TYPE_NAMES before including"
#endif

import storm;
import std;

using storm::db::DefaultConnection;

#include "../../shared/models/person.h" // NOSONAR cpp:S954 — Person, the FK target of StockEntry
#include "test_fixture.h"

// Must follow person.h: StockEntry's FK part names Person.
#include "test_composite_pk_models.h" // NOSONAR cpp:S954
using storm::create_table_sql;
using storm::QuerySet;
using storm::orm::schema::Dialect;
using storm::orm::statements::ReturnId;

// ── #501: composite PK — UPDATE and DELETE executed on live backends ─────────
// The companion of test_composite_pk_sql.cpp, which pins the compile-time gates
// and the generated SQL text. This TU proves the widened WHERE clause and the
// bind arithmetic behave against real SQLite and PostgreSQL: a full-key match
// hits exactly one row, and a partial-key match hits none.
//
// The UPDATE/DELETE fixtures seed through raw SQL (as when they were written,
// pre-#502) so those tests stay independent of the INSERT path they don't
// test. The INSERT suites below (#502) use qs.insert() itself.

// NOLINTBEGIN(readability-implicit-bool-conversion)

namespace {

// Composite-PK fixture. The table is created from the generated DDL, since
// ensure_tables() drives the same schema path. Anchors the connection on
// Person but creates no Person table.
template <typename Model, typename ConnType> class CompositePkFixture : public StormTestFixture<Person, ConnType> {
  public:
    auto on_setup(const std::shared_ptr<ConnType> &conn) -> void override {
        const std::string ddl = storm::test::is_postgresql<ConnType>() ? create_table_sql<Model, Dialect::PostgreSQL>()
                                                                       : create_table_sql<Model>();
        ASSERT_TRUE(conn->execute(ddl).has_value()) << ddl;
    }

    static auto seed(std::string_view sql) -> void {
        const auto conn = DefaultConnection<ConnType>::get();
        ASSERT_TRUE(conn->execute(std::string(sql)).has_value()) << sql;
    }

    static auto row_count() -> int {
        QuerySet<Model, ConnType> qs;
        auto result = qs.count().execute();
        return result.has_value() ? static_cast<int>(result.value()) : -1;
    }

    // The single row matching `filter`, or nullopt when the filter matches
    // nothing. Every "did the right row change?" assertion below reads
    // through this, so the key-matching check is written once.
    static auto find_one(const auto &filter) -> std::optional<Model> {
        QuerySet<Model, ConnType> qs;
        auto rows = qs.where(filter).select().execute();
        if (!rows.has_value() || rows.value().empty()) {
            return std::nullopt;
        }
        return *rows.value().begin();
    }
};

// --- OrderLine: 2-part int key -------------------------------------------

template <typename ConnType> class OrderLineTest : public CompositePkFixture<OrderLine, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        this->seed("INSERT INTO OrderLine (order_id, product_id, quantity, note) VALUES "
                   "(1, 10, 5, 'a'), (1, 20, 7, 'b'), (2, 10, 9, 'c'), (2, 20, 11, 'd')");
    }

    // quantity of one key, or -1 when the row is gone.
    static auto quantity_of(int order_id, int product_id) -> int {
        auto row = OrderLineTest::find_one(fields::OrderLine.order_id == order_id &&
                                           fields::OrderLine.product_id == product_id);
        return row ? row->quantity : -1;
    }
};

} // namespace

TYPED_TEST_SUITE(OrderLineTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// --- DELETE: single row -----------------------------------------------------

TYPED_TEST(OrderLineTest, DeleteSingleMatchesFullKeyOnly) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine target{.order_id = 1, .product_id = 20};
    ASSERT_TRUE(qs.erase(target).execute().has_value());

    EXPECT_EQ(this->row_count(), 3);
    EXPECT_EQ(this->quantity_of(1, 20), -1) << "the full-key row is deleted";
    EXPECT_EQ(this->quantity_of(1, 10), 5) << "a row sharing only order_id survives";
    EXPECT_EQ(this->quantity_of(2, 20), 11) << "a row sharing only product_id survives";
}

// The core failure this issue prevents: with a single-column WHERE, deleting
// (1,20) would also take (1,10) — every row sharing the first key part.
TYPED_TEST(OrderLineTest, DeletePartialKeyMatchDoesNotDelete) {
    QuerySet<OrderLine, TypeParam> qs;
    // order_id matches rows of order 1, but no row of order 1 has product_id 99.
    const OrderLine target{.order_id = 1, .product_id = 99};
    ASSERT_TRUE(qs.erase(target).execute().has_value());
    EXPECT_EQ(this->row_count(), 4) << "no row matches the FULL key, so nothing is deleted";
}

TYPED_TEST(OrderLineTest, DeleteNoMatchIsNotAnError) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine target{.order_id = 77, .product_id = 88};
    EXPECT_TRUE(qs.erase(target).execute().has_value());
    EXPECT_EQ(this->row_count(), 4);
}

// --- DELETE: batch ----------------------------------------------------------

TYPED_TEST(OrderLineTest, DeleteBatchMatchesEachFullKey) {
    QuerySet<OrderLine, TypeParam> qs;
    const std::vector<OrderLine> targets{
        {.order_id = 1, .product_id = 10},
        {.order_id = 2, .product_id = 20},
    };
    ASSERT_TRUE(qs.erase(std::span<const OrderLine>(targets)).execute().has_value());

    // The listed keys CROSS: (1,10) and (2,20) are given, (1,20) and (2,10) are
    // not. A per-column IN list — "order_id IN (1,2) AND product_id IN (10,20)"
    // — would wrongly delete all four. Only a row-value list gets this right.
    EXPECT_EQ(this->row_count(), 2) << "exactly the two listed keys, not their cross product";
    EXPECT_EQ(this->quantity_of(1, 10), -1);
    EXPECT_EQ(this->quantity_of(2, 20), -1);
    EXPECT_EQ(this->quantity_of(1, 20), 7) << "an unlisted key survives";
    EXPECT_EQ(this->quantity_of(2, 10), 9) << "an unlisted key survives";
}

TYPED_TEST(OrderLineTest, DeleteEmptyBatchIsANoOp) {
    QuerySet<OrderLine, TypeParam> qs;
    const std::vector<OrderLine> empty;
    EXPECT_TRUE(qs.erase(std::span<const OrderLine>(empty)).execute().has_value());
    EXPECT_EQ(this->row_count(), 4);
}

// --- UPDATE: single row -----------------------------------------------------

TYPED_TEST(OrderLineTest, UpdateSingleMatchesFullKeyOnly) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine updated{.order_id = 1, .product_id = 20, .quantity = 700, .note = "B"};
    ASSERT_TRUE(qs.update(updated).execute().has_value());

    EXPECT_EQ(this->quantity_of(1, 20), 700) << "the full-key row is updated";
    EXPECT_EQ(this->quantity_of(1, 10), 5) << "a row sharing only order_id is untouched";
    EXPECT_EQ(this->quantity_of(2, 20), 11) << "a row sharing only product_id is untouched";
}

TYPED_TEST(OrderLineTest, UpdatePartialKeyMatchUpdatesNothing) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine updated{.order_id = 1, .product_id = 99, .quantity = 700};
    ASSERT_TRUE(qs.update(updated).execute().has_value());

    EXPECT_EQ(this->quantity_of(1, 10), 5);
    EXPECT_EQ(this->quantity_of(1, 20), 7);
    EXPECT_EQ(this->row_count(), 4) << "UPDATE never inserts";
}

// The bind offsets are what this asserts: the two SET values occupy
// placeholders 1-2, so the key values must land at 3-4. Binding the key at a
// stale offset would either write the wrong row or fail outright.
TYPED_TEST(OrderLineTest, UpdateBindsEveryKeyPartAtTheRightOffset) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine updated{.order_id = 2, .product_id = 10, .quantity = 42, .note = "z"};
    ASSERT_TRUE(qs.update(updated).execute().has_value());

    EXPECT_EQ(this->quantity_of(2, 10), 42) << "the SET values and the key both landed correctly";
    EXPECT_EQ(this->quantity_of(2, 20), 11) << "the second key part was bound, not defaulted";
    EXPECT_EQ(this->quantity_of(1, 10), 5) << "the first key part was bound, not defaulted";
}

// --- UPDATE: batch ----------------------------------------------------------

TYPED_TEST(OrderLineTest, UpdateBatchAppliesPerKey) {
    QuerySet<OrderLine, TypeParam> qs;
    const std::vector<OrderLine> updates{
        {.order_id = 1, .product_id = 10, .quantity = 100, .note = "x"},
        {.order_id = 2, .product_id = 20, .quantity = 200, .note = "y"},
    };
    ASSERT_TRUE(qs.update(std::span<const OrderLine>(updates)).execute().has_value());

    EXPECT_EQ(this->quantity_of(1, 10), 100);
    EXPECT_EQ(this->quantity_of(2, 20), 200);
    EXPECT_EQ(this->quantity_of(1, 20), 7) << "an unlisted key is untouched";
    EXPECT_EQ(this->quantity_of(2, 10), 9) << "an unlisted key is untouched";
}

TYPED_TEST(OrderLineTest, UpdateEmptyBatchIsANoOp) {
    QuerySet<OrderLine, TypeParam> qs;
    const std::vector<OrderLine> empty;
    EXPECT_TRUE(qs.update(std::span<const OrderLine>(empty)).execute().has_value());
    EXPECT_EQ(this->quantity_of(1, 10), 5);
}

// ── Mixed-type key parts (int + std::string) ─────────────────────────────────

namespace {
template <typename ConnType> class InventoryTest : public CompositePkFixture<Inventory, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        this->seed("INSERT INTO Inventory (warehouse, sku, on_hand) VALUES "
                   "(1, 'apple', 5), (1, 'pear', 7), (2, 'apple', 9)");
    }

    static auto on_hand_of(int warehouse, std::string_view sku) -> int {
        auto row = InventoryTest::find_one(fields::Inventory.warehouse == warehouse &&
                                           fields::Inventory.sku == std::string(sku));
        return row ? row->on_hand : -1;
    }
};
} // namespace

TYPED_TEST_SUITE(InventoryTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(InventoryTest, DeleteWithTextKeyPart) {
    QuerySet<Inventory, TypeParam> qs;
    const Inventory target{.warehouse = 1, .sku = "apple"};
    ASSERT_TRUE(qs.erase(target).execute().has_value());

    EXPECT_EQ(this->on_hand_of(1, "apple"), -1);
    EXPECT_EQ(this->on_hand_of(1, "pear"), 7) << "same warehouse, different sku survives";
    EXPECT_EQ(this->on_hand_of(2, "apple"), 9) << "same sku, different warehouse survives";
}

TYPED_TEST(InventoryTest, UpdateWithTextKeyPart) {
    QuerySet<Inventory, TypeParam> qs;
    const Inventory updated{.warehouse = 2, .sku = "apple", .on_hand = 900};
    ASSERT_TRUE(qs.update(updated).execute().has_value());

    EXPECT_EQ(this->on_hand_of(2, "apple"), 900);
    EXPECT_EQ(this->on_hand_of(1, "apple"), 5) << "the text part alone must not match";
}

TYPED_TEST(InventoryTest, DeleteBatchWithTextKeyPart) {
    QuerySet<Inventory, TypeParam> qs;
    const std::vector<Inventory> targets{
        {.warehouse = 1, .sku = "pear"},
        {.warehouse = 2, .sku = "apple"},
    };
    ASSERT_TRUE(qs.erase(std::span<const Inventory>(targets)).execute().has_value());

    EXPECT_EQ(this->on_hand_of(1, "apple"), 5) << "unlisted key survives";
    EXPECT_EQ(this->on_hand_of(1, "pear"), -1);
    EXPECT_EQ(this->on_hand_of(2, "apple"), -1);
}

// ── Three-part key ───────────────────────────────────────────────────────────

namespace {
template <typename ConnType> class LedgerTest : public CompositePkFixture<Ledger, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        this->seed("INSERT INTO Ledger (region, account, period, balance) VALUES "
                   "(1, 'cash', 202601, 10.5), (1, 'cash', 202602, 20.5), (1, 'debt', 202601, 30.5)");
    }

    static auto balance_of(int region, std::string_view account, std::int64_t period) -> double {
        auto row =
            LedgerTest::find_one(fields::Ledger.region == region && fields::Ledger.account == std::string(account) &&
                                 fields::Ledger.period == period);
        return row ? row->balance : -1.0;
    }
};
} // namespace

TYPED_TEST_SUITE(LedgerTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

// Two of the three parts match — the row must NOT be touched. This is the case
// a 2-of-3 AND-join would get wrong.
TYPED_TEST(LedgerTest, DeleteRequiresAllThreeParts) {
    QuerySet<Ledger, TypeParam> qs;
    const Ledger target{.region = 1, .account = "cash", .period = 209912};
    ASSERT_TRUE(qs.erase(target).execute().has_value());

    EXPECT_DOUBLE_EQ(this->balance_of(1, "cash", 202601), 10.5) << "no row matches all three parts";
    EXPECT_DOUBLE_EQ(this->balance_of(1, "cash", 202602), 20.5);
    EXPECT_DOUBLE_EQ(this->balance_of(1, "debt", 202601), 30.5);
}

TYPED_TEST(LedgerTest, DeleteThreePartKey) {
    QuerySet<Ledger, TypeParam> qs;
    const Ledger target{.region = 1, .account = "cash", .period = 202602};
    ASSERT_TRUE(qs.erase(target).execute().has_value());

    EXPECT_DOUBLE_EQ(this->balance_of(1, "cash", 202602), -1.0);
    EXPECT_DOUBLE_EQ(this->balance_of(1, "cash", 202601), 10.5);
    EXPECT_DOUBLE_EQ(this->balance_of(1, "debt", 202601), 30.5);
}

TYPED_TEST(LedgerTest, UpdateThreePartKey) {
    QuerySet<Ledger, TypeParam> qs;
    const Ledger updated{.region = 1, .account = "debt", .period = 202601, .balance = 99.5};
    ASSERT_TRUE(qs.update(updated).execute().has_value());

    EXPECT_DOUBLE_EQ(this->balance_of(1, "debt", 202601), 99.5);
    EXPECT_DOUBLE_EQ(this->balance_of(1, "cash", 202601), 10.5) << "differs in one part — untouched";
}

TYPED_TEST(LedgerTest, DeleteBatchThreePartKey) {
    QuerySet<Ledger, TypeParam> qs;
    const std::vector<Ledger> targets{
        {.region = 1, .account = "cash", .period = 202601},
        {.region = 1, .account = "debt", .period = 202601},
    };
    ASSERT_TRUE(qs.erase(std::span<const Ledger>(targets)).execute().has_value());

    EXPECT_DOUBLE_EQ(this->balance_of(1, "cash", 202601), -1.0);
    EXPECT_DOUBLE_EQ(this->balance_of(1, "debt", 202601), -1.0);
    EXPECT_DOUBLE_EQ(this->balance_of(1, "cash", 202602), 20.5) << "unlisted key survives";
}

// ── FK key parts: the association-table shape ────────────────────────────────
// An FK part binds the REFERENCED row's key, not the whole object, and names the
// "<name>_id" column. Executing it is the only way to catch a mismatch between
// the two — a bare-member column name fails at prepare time, and binding the
// wrong value silently hits the wrong row.

namespace {
template <typename ConnType> class StockEntryTest : public CompositePkFixture<StockEntry, ConnType> {
  public:
    // The FK target table must exist before the referencing table.
    auto on_setup(const std::shared_ptr<ConnType> &conn) -> void override {
        ASSERT_TRUE((storm::test::ensure_tables<ConnType, Person>(conn))) << "Failed to create Person";
        CompositePkFixture<StockEntry, ConnType>::on_setup(conn);
    }

    // Shared by the UPDATE/DELETE fixture and the INSERT fixture (#502).
    static auto seed_persons() -> void {
        QuerySet<Person, ConnType> people;
        for (int i = 1; i <= 2; ++i) {
            const Person person{.id = i, .name = std::format("W{}", i), .age = 30};
            ASSERT_TRUE(people.insert(person).execute().has_value());
        }
    }

    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        StockEntryTest::seed_persons();
        this->seed("INSERT INTO StockEntry (warehouse_id, sku, qty) VALUES (1, 10, 5), (1, 20, 7), (2, 10, 9)");
    }

    // Selects every row and matches the key in C++ rather than in a WHERE clause.
    // Filtering with fields::StockEntry.warehouse would compare the FK MEMBER rather
    // than its "warehouse_id" column — an unrelated gap in the WHERE layer (nothing
    // in the tree filters on an FK member today), and leaning on it here would make
    // this test fail for a reason unrelated to the key binding it exists to check.
    // The tables hold three rows, so the scan costs nothing.
    static auto qty_of(int warehouse_id, int sku) -> int {
        QuerySet<StockEntry, ConnType> qs;
        auto rows = qs.select().execute();
        if (!rows.has_value()) {
            return -1;
        }
        for (const StockEntry &row : rows.value()) {
            if (row.warehouse.id == warehouse_id && row.sku == sku) {
                return row.qty;
            }
        }
        return -1;
    }
};
} // namespace

TYPED_TEST_SUITE(StockEntryTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(StockEntryTest, DeleteByFkCompositeKey) {
    QuerySet<StockEntry, TypeParam> qs;
    const StockEntry target{.warehouse = {.id = 1}, .sku = 20};
    ASSERT_TRUE(qs.erase(target).execute().has_value());

    EXPECT_EQ(this->qty_of(1, 20), -1) << "the full-key row is deleted";
    EXPECT_EQ(this->qty_of(1, 10), 5) << "same warehouse, different sku survives";
    EXPECT_EQ(this->qty_of(2, 10), 9) << "same sku, different warehouse survives";
}

TYPED_TEST(StockEntryTest, UpdateByFkCompositeKey) {
    QuerySet<StockEntry, TypeParam> qs;
    const StockEntry updated{.warehouse = {.id = 2}, .sku = 10, .qty = 900};
    ASSERT_TRUE(qs.update(updated).execute().has_value());

    EXPECT_EQ(this->qty_of(2, 10), 900);
    EXPECT_EQ(this->qty_of(1, 10), 5) << "the sku part alone must not match";
}

TYPED_TEST(StockEntryTest, DeleteBatchByFkCompositeKey) {
    QuerySet<StockEntry, TypeParam> qs;
    const std::vector<StockEntry> targets{
        {.warehouse = {.id = 1}, .sku = 10},
        {.warehouse = {.id = 2}, .sku = 10},
    };
    ASSERT_TRUE(qs.erase(std::span<const StockEntry>(targets)).execute().has_value());

    EXPECT_EQ(this->qty_of(1, 10), -1);
    EXPECT_EQ(this->qty_of(2, 10), -1);
    EXPECT_EQ(this->qty_of(1, 20), 7) << "unlisted key survives";
}

// ── Chunking: a batch that crosses the shrunken chunk boundary ───────────────
// The 999-variable ceiling is spent N params per row, so a 2-part key chunks at
// 399 rows rather than 799. 900 rows forces the max-chunk statement to run
// twice plus a remainder.

namespace {
template <typename ConnType> class LargeBatchTest : public CompositePkFixture<OrderLine, ConnType> {
  public:
    static constexpr int ROWS = 900;

    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        std::string sql = "INSERT INTO OrderLine (order_id, product_id, quantity, note) VALUES ";
        for (int i = 0; i < ROWS; ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += std::format("({}, {}, {}, 'n')", i, i * 2, i);
        }
        this->seed(sql);
    }

    // Keys (i, 2i) for i in [from, ROWS).
    static auto keys_from(int from) -> std::vector<OrderLine> {
        std::vector<OrderLine> targets;
        targets.reserve(static_cast<std::size_t>(ROWS - from));
        for (int i = from; i < ROWS; ++i) {
            targets.push_back({.order_id = i, .product_id = i * 2});
        }
        return targets;
    }
};
} // namespace

TYPED_TEST_SUITE(LargeBatchTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(LargeBatchTest, ChunkedDeleteSpansMultipleChunks) {
    const std::vector<OrderLine> targets = LargeBatchTest<TypeParam>::keys_from(0);
    QuerySet<OrderLine, TypeParam> qs;
    ASSERT_TRUE(qs.erase(std::span<const OrderLine>(targets)).execute().has_value());
    EXPECT_EQ(this->row_count(), 0) << "every key across every chunk is deleted";
}

// The same batch minus one key: the survivor proves the chunked path deletes the
// keys it was given rather than everything it touched.
TYPED_TEST(LargeBatchTest, ChunkedDeleteLeavesUnlistedKeys) {
    const std::vector<OrderLine> targets = LargeBatchTest<TypeParam>::keys_from(1);
    QuerySet<OrderLine, TypeParam> qs;
    ASSERT_TRUE(qs.erase(std::span<const OrderLine>(targets)).execute().has_value());
    EXPECT_EQ(this->row_count(), 1) << "the one unlisted key survives";
}

TYPED_TEST(LargeBatchTest, ChunkedUpdateSpansMultipleChunks) {
    std::vector<OrderLine> updates = LargeBatchTest<TypeParam>::keys_from(0);
    for (OrderLine &row : updates) {
        row.quantity = 1000 + row.order_id;
        row.note = "u";
    }
    QuerySet<OrderLine, TypeParam> qs;
    ASSERT_TRUE(qs.update(std::span<const OrderLine>(updates)).execute().has_value());

    QuerySet<OrderLine, TypeParam> check;
    auto updated = check.where(fields::OrderLine.quantity >= 1000).count().execute();
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated.value(), LargeBatchTest<TypeParam>::ROWS);
}

// ── Single-PK regression ─────────────────────────────────────────────────────
// Person is the long-standing CRUD fixture. Its UPDATE/DELETE behaviour must be
// unchanged by the composite widening.

namespace {
template <typename ConnType> class SinglePkRegressionTest : public StormTestFixture<Person, ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        QuerySet<Person, ConnType> qs;
        for (int i = 1; i <= 3; ++i) {
            const Person person{.id = i, .name = std::format("P{}", i), .age = 20 + i};
            ASSERT_TRUE(qs.insert(person).execute().has_value());
        }
    }

    static auto age_of(int person_id) -> int {
        QuerySet<Person, ConnType> qs;
        auto rows = qs.where(fields::Person.id == person_id).select().execute();
        if (!rows.has_value() || rows.value().empty()) {
            return -1;
        }
        return rows.value().begin()->age;
    }
};
} // namespace

TYPED_TEST_SUITE(SinglePkRegressionTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(SinglePkRegressionTest, SingleDeleteStillWorks) {
    QuerySet<Person, TypeParam> qs;
    const Person target{.id = 2};
    ASSERT_TRUE(qs.erase(target).execute().has_value());
    EXPECT_EQ(this->age_of(2), -1);
    EXPECT_EQ(this->age_of(1), 21);
}

TYPED_TEST(SinglePkRegressionTest, SingleUpdateStillWorks) {
    QuerySet<Person, TypeParam> qs;
    const Person updated{.id = 3, .name = "P3", .age = 99};
    ASSERT_TRUE(qs.update(updated).execute().has_value());
    EXPECT_EQ(this->age_of(3), 99);
    EXPECT_EQ(this->age_of(1), 21);
}

TYPED_TEST(SinglePkRegressionTest, BatchDeleteStillWorks) {
    QuerySet<Person, TypeParam> qs;
    const std::vector<Person> targets{{.id = 1}, {.id = 3}};
    ASSERT_TRUE(qs.erase(std::span<const Person>(targets)).execute().has_value());
    EXPECT_EQ(this->age_of(1), -1);
    EXPECT_EQ(this->age_of(3), -1);
    EXPECT_EQ(this->age_of(2), 22);
}

// ── (#502) INSERT by composite key ───────────────────────────────────────────
// Every key part is caller data: the INSERT carries all columns in declaration
// order, .execute() returns std::expected<void, Error> (a composite key is
// never DB-generated, so there is nothing to RETURN), and a duplicate full key
// surfaces the PRIMARY KEY violation as an Error.

namespace {
// Insert tests need an EMPTY table, so they use the base fixture directly.
template <typename ConnType> class OrderLineInsertTest : public CompositePkFixture<OrderLine, ConnType> {};
template <typename ConnType> class InventoryInsertTest : public CompositePkFixture<Inventory, ConnType> {};
template <typename ConnType> class LedgerInsertTest : public CompositePkFixture<Ledger, ConnType> {};
} // namespace

TYPED_TEST_SUITE(OrderLineInsertTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(InventoryInsertTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);
TYPED_TEST_SUITE(LedgerInsertTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(OrderLineInsertTest, SingleInsertLandsEveryKeyPart) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine row{.order_id = 7, .product_id = 42, .quantity = 3, .note = "a"};
    auto result = qs.insert(row).execute();
    static_assert(std::is_same_v<decltype(result), std::expected<void, typename TypeParam::Error>>,
                  "composite insert has nothing to return — the caller supplied the whole key");
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(this->row_count(), 1);
    auto found = this->find_one(fields::OrderLine.order_id == 7 && fields::OrderLine.product_id == 42);
    ASSERT_TRUE(found.has_value()) << "the full key landed";
    EXPECT_EQ(found->quantity, 3);
    EXPECT_EQ(found->note, "a");
}

// The core failure this issue prevents: pre-#502 the column list dropped the
// FIRST key part while the bind loop skipped it too, so every later value
// shifted one column left — order_id would have received product_id's value.
TYPED_TEST(OrderLineInsertTest, FirstKeyPartIsNotDefaultedOrShifted) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine row{.order_id = 5, .product_id = 6, .quantity = 9, .note = "x"};
    ASSERT_TRUE(qs.insert(row).execute().has_value());

    EXPECT_FALSE(this->find_one(fields::OrderLine.order_id == 6).has_value())
        << "product_id's value must not land in order_id";
    auto found = this->find_one(fields::OrderLine.order_id == 5 && fields::OrderLine.product_id == 6);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->quantity, 9) << "every non-key value landed in its own column";
}

// Redundant but accepted (generic code may spell it out): identical to plain insert().
TYPED_TEST(OrderLineInsertTest, ExplicitReturnIdNoIsAcceptedAndIdentical) {
    using storm::orm::statements::ReturnId;
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine row{.order_id = 1, .product_id = 2, .quantity = 1, .note = "n"};
    auto result = qs.template insert<ReturnId::No>(row).execute();
    static_assert(std::is_same_v<decltype(result), std::expected<void, typename TypeParam::Error>>);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(this->row_count(), 1);
}

TYPED_TEST(OrderLineInsertTest, DuplicateFullKeyIsAnError) {
    QuerySet<OrderLine, TypeParam> qs;
    ASSERT_TRUE(
        qs.insert(OrderLine{.order_id = 1, .product_id = 10, .quantity = 5, .note = "a"}).execute().has_value());

    auto result = qs.insert(OrderLine{.order_id = 1, .product_id = 10, .quantity = 99, .note = "b"}).execute();
    EXPECT_FALSE(result.has_value()) << "a duplicate composite key violates the PRIMARY KEY constraint";
    EXPECT_EQ(this->row_count(), 1) << "the duplicate did not land";
}

// Sharing one part is NOT a duplicate — only the full key is unique.
TYPED_TEST(OrderLineInsertTest, SharedSinglePartIsNotADuplicate) {
    QuerySet<OrderLine, TypeParam> qs;
    ASSERT_TRUE(
        qs.insert(OrderLine{.order_id = 1, .product_id = 10, .quantity = 1, .note = "a"}).execute().has_value());
    EXPECT_TRUE(
        qs.insert(OrderLine{.order_id = 1, .product_id = 20, .quantity = 2, .note = "b"}).execute().has_value());
    EXPECT_TRUE(
        qs.insert(OrderLine{.order_id = 2, .product_id = 10, .quantity = 3, .note = "c"}).execute().has_value());
    EXPECT_EQ(this->row_count(), 3);
}

TYPED_TEST(OrderLineInsertTest, BatchInsertLandsEveryRow) {
    QuerySet<OrderLine, TypeParam> qs;
    const std::vector<OrderLine> rows{
        {.order_id = 1, .product_id = 10, .quantity = 5, .note = "a"},
        {.order_id = 1, .product_id = 20, .quantity = 7, .note = "b"},
        {.order_id = 2, .product_id = 10, .quantity = 9, .note = "c"},
    };
    ASSERT_TRUE(qs.insert(std::span<const OrderLine>(rows)).execute().has_value());
    EXPECT_EQ(this->row_count(), 3);

    auto row = this->find_one(fields::OrderLine.order_id == 2 && fields::OrderLine.product_id == 10);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->quantity, 9);
}

TYPED_TEST(OrderLineInsertTest, EmptyBatchIsANoOp) {
    QuerySet<OrderLine, TypeParam> qs;
    const std::vector<OrderLine> empty;
    EXPECT_TRUE(qs.insert(std::span<const OrderLine>(empty)).execute().has_value());
    EXPECT_EQ(this->row_count(), 0);
}

// The 999-parameter ceiling divides by ALL fields now that the key columns are
// in the statement: OrderLine binds 4 params/row, so one bulk statement caps at
// 999/4 == 249 rows and 250 forces the chunked path (one max chunk + remainder).
TYPED_TEST(OrderLineInsertTest, BatchAtTheChunkBoundary) {
    constexpr int ROWS = 250;
    std::vector<OrderLine> rows;
    rows.reserve(ROWS);
    for (int i = 0; i < ROWS; ++i) {
        rows.push_back({.order_id = i, .product_id = i * 2, .quantity = i, .note = "n"});
    }
    QuerySet<OrderLine, TypeParam> qs;
    ASSERT_TRUE(qs.insert(std::span<const OrderLine>(rows)).execute().has_value());
    EXPECT_EQ(this->row_count(), ROWS);

    auto last =
        this->find_one(fields::OrderLine.order_id == ROWS - 1 && fields::OrderLine.product_id == (ROWS - 1) * 2);
    ASSERT_TRUE(last.has_value()) << "the row past the chunk boundary landed with its full key";
    EXPECT_EQ(last->quantity, ROWS - 1);
}

// ── Mixed-type key parts (int + std::string) ─────────────────────────────────

TYPED_TEST(InventoryInsertTest, TextKeyPartBindsPerType) {
    QuerySet<Inventory, TypeParam> qs;
    ASSERT_TRUE(qs.insert(Inventory{.warehouse = 1, .sku = "apple", .on_hand = 5}).execute().has_value());
    ASSERT_TRUE(qs.insert(Inventory{.warehouse = 1, .sku = "pear", .on_hand = 7}).execute().has_value());

    auto row = this->find_one(fields::Inventory.warehouse == 1 && fields::Inventory.sku == std::string("apple"));
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->on_hand, 5);
}

TYPED_TEST(InventoryInsertTest, DuplicateTextKeyIsAnError) {
    QuerySet<Inventory, TypeParam> qs;
    ASSERT_TRUE(qs.insert(Inventory{.warehouse = 1, .sku = "apple", .on_hand = 5}).execute().has_value());
    EXPECT_FALSE(qs.insert(Inventory{.warehouse = 1, .sku = "apple", .on_hand = 9}).execute().has_value());
    EXPECT_EQ(this->row_count(), 1);
}

// ── Three-part key ───────────────────────────────────────────────────────────

TYPED_TEST(LedgerInsertTest, ThreePartKeyInsert) {
    QuerySet<Ledger, TypeParam> qs;
    ASSERT_TRUE(
        qs.insert(Ledger{.region = 1, .account = "cash", .period = 202601, .balance = 10.5}).execute().has_value());
    // Differs in exactly ONE part each — none is a duplicate of the first row.
    EXPECT_TRUE(
        qs.insert(Ledger{.region = 2, .account = "cash", .period = 202601, .balance = 1.0}).execute().has_value());
    EXPECT_TRUE(
        qs.insert(Ledger{.region = 1, .account = "debt", .period = 202601, .balance = 2.0}).execute().has_value());
    EXPECT_TRUE(
        qs.insert(Ledger{.region = 1, .account = "cash", .period = 202602, .balance = 3.0}).execute().has_value());
    EXPECT_EQ(this->row_count(), 4);
}

// ── FK key parts: the association-table shape ────────────────────────────────
// An FK part binds the REFERENCED row's key into the "<name>_id" column.

namespace {
// Inherits StockEntryTest's Person-table setup and qty_of matcher; overrides
// the seeding so the StockEntry table starts EMPTY for the INSERT tests.
template <typename ConnType> class StockEntryInsertTest : public StockEntryTest<ConnType> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType> & /*conn*/) -> void override {
        StockEntryTest<ConnType>::seed_persons();
    }
};
} // namespace

TYPED_TEST_SUITE(StockEntryInsertTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(StockEntryInsertTest, FkKeyPartBindsTheReferencedKey) {
    QuerySet<StockEntry, TypeParam> qs;
    ASSERT_TRUE(qs.insert(StockEntry{.warehouse = {.id = 2}, .sku = 10, .qty = 5}).execute().has_value());

    EXPECT_EQ(this->qty_of(2, 10), 5) << "warehouse_id bound the referenced key, not a defaulted value";
    EXPECT_EQ(this->qty_of(1, 10), -1) << "no row for the other warehouse";
}

TYPED_TEST(StockEntryInsertTest, DuplicateFkKeyIsAnError) {
    QuerySet<StockEntry, TypeParam> qs;
    ASSERT_TRUE(qs.insert(StockEntry{.warehouse = {.id = 1}, .sku = 10, .qty = 5}).execute().has_value());
    EXPECT_FALSE(qs.insert(StockEntry{.warehouse = {.id = 1}, .sku = 10, .qty = 9}).execute().has_value());
    EXPECT_EQ(this->row_count(), 1);
}

// ── (#503) UPSERT by composite key ───────────────────────────────────────────
// on_conflict<> targeting the FULL composite-PK column set. A composite key is
// never DB-generated (#502), so there is nothing to RETURN: both DO NOTHING and
// DO UPDATE resolve to std::expected<void, Error> on a composite model, unlike
// the std::optional<int64_t>/int64_t single-PK proxies.

namespace {
template <typename ConnType> class OrderLineUpsertTest : public CompositePkFixture<OrderLine, ConnType> {};
} // namespace

TYPED_TEST_SUITE(OrderLineUpsertTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(OrderLineUpsertTest, DoNothingSkipsOnConflict) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine first{.order_id = 1, .product_id = 10, .quantity = 5, .note = "a"};
    auto inserted = qs.insert(first)
                        .template on_conflict<fields::OrderLine.order_id, fields::OrderLine.product_id>()
                        .nothing()
                        .execute();
    static_assert(std::is_same_v<decltype(inserted), std::expected<void, typename TypeParam::Error>>,
                  "a composite key has nothing to RETURN, even for DO NOTHING");
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(this->row_count(), 1);

    const OrderLine conflicting{.order_id = 1, .product_id = 10, .quantity = 99, .note = "b"};
    auto skipped = qs.insert(conflicting)
                       .template on_conflict<fields::OrderLine.order_id, fields::OrderLine.product_id>()
                       .nothing()
                       .execute();
    ASSERT_TRUE(skipped.has_value());
    EXPECT_EQ(this->row_count(), 1) << "the conflicting insert must be skipped, not applied";

    auto row = this->find_one(fields::OrderLine.order_id == 1 && fields::OrderLine.product_id == 10);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->quantity, 5) << "DO NOTHING must not overwrite the existing row";
}

TYPED_TEST(OrderLineUpsertTest, DoUpdateOverwritesListedColumn) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine first{.order_id = 2, .product_id = 20, .quantity = 5, .note = "a"};
    auto seeded = qs.insert(first)
                      .template on_conflict<fields::OrderLine.order_id, fields::OrderLine.product_id>()
                      .template update<fields::OrderLine.quantity>()
                      .execute();
    ASSERT_TRUE(seeded.has_value());

    const OrderLine conflicting{.order_id = 2, .product_id = 20, .quantity = 99, .note = "b"};
    auto updated = qs.insert(conflicting)
                       .template on_conflict<fields::OrderLine.order_id, fields::OrderLine.product_id>()
                       .template update<fields::OrderLine.quantity>()
                       .execute();
    static_assert(std::is_same_v<decltype(updated), std::expected<void, typename TypeParam::Error>>,
                  "a composite key has nothing to RETURN for DO UPDATE either");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(this->row_count(), 1);

    auto row = this->find_one(fields::OrderLine.order_id == 2 && fields::OrderLine.product_id == 20);
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->quantity, 99) << "the listed column is overwritten by the conflicting insert";
    EXPECT_EQ(row->note, "a") << "an unlisted column is preserved from the original row";
}

// A key that shares only ONE part is not a conflict at all — both rows land.
TYPED_TEST(OrderLineUpsertTest, PartialKeyMatchIsNotAConflict) {
    QuerySet<OrderLine, TypeParam> qs;
    const OrderLine first{.order_id = 3, .product_id = 30, .quantity = 1, .note = "a"};
    auto first_result = qs.insert(first)
                            .template on_conflict<fields::OrderLine.order_id, fields::OrderLine.product_id>()
                            .nothing()
                            .execute();
    ASSERT_TRUE(first_result.has_value());

    const OrderLine same_order{.order_id = 3, .product_id = 31, .quantity = 2, .note = "b"};
    auto second_result = qs.insert(same_order)
                             .template on_conflict<fields::OrderLine.order_id, fields::OrderLine.product_id>()
                             .nothing()
                             .execute();
    ASSERT_TRUE(second_result.has_value());
    EXPECT_EQ(this->row_count(), 2) << "sharing only order_id is not a full-key conflict";
}

// Mixed-type key parts (int + std::string) — proves the conflict target and the
// bound VALUES both dispatch per part type, not just for an all-int key.
namespace {
template <typename ConnType> class InventoryUpsertTest : public CompositePkFixture<Inventory, ConnType> {};
} // namespace

TYPED_TEST_SUITE(InventoryUpsertTest, STORM_SPLIT_TYPES, STORM_SPLIT_TYPE_NAMES);

TYPED_TEST(InventoryUpsertTest, DoUpdateOverwritesListedColumnWithTextKeyPart) {
    QuerySet<Inventory, TypeParam> qs;
    const Inventory first{.warehouse = 1, .sku = "apple", .on_hand = 5};
    auto seeded = qs.insert(first)
                      .template on_conflict<fields::Inventory.warehouse, fields::Inventory.sku>()
                      .template update<fields::Inventory.on_hand>()
                      .execute();
    ASSERT_TRUE(seeded.has_value());

    const Inventory conflicting{.warehouse = 1, .sku = "apple", .on_hand = 900};
    auto updated = qs.insert(conflicting)
                       .template on_conflict<fields::Inventory.warehouse, fields::Inventory.sku>()
                       .template update<fields::Inventory.on_hand>()
                       .execute();
    static_assert(std::is_same_v<decltype(updated), std::expected<void, typename TypeParam::Error>>,
                  "a composite key has nothing to RETURN, even with a text key part");
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(this->row_count(), 1);

    auto row = this->find_one(fields::Inventory.warehouse == 1 && fields::Inventory.sku == std::string("apple"));
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->on_hand, 900) << "the listed column is overwritten by the conflicting insert";
}

TYPED_TEST(InventoryUpsertTest, DoNothingSkipsOnConflictWithTextKeyPart) {
    QuerySet<Inventory, TypeParam> qs;
    const Inventory first{.warehouse = 2, .sku = "pear", .on_hand = 3};
    auto inserted =
        qs.insert(first).template on_conflict<fields::Inventory.warehouse, fields::Inventory.sku>().nothing().execute();
    ASSERT_TRUE(inserted.has_value());

    const Inventory conflicting{.warehouse = 2, .sku = "pear", .on_hand = 999};
    auto skipped = qs.insert(conflicting)
                       .template on_conflict<fields::Inventory.warehouse, fields::Inventory.sku>()
                       .nothing()
                       .execute();
    ASSERT_TRUE(skipped.has_value());

    auto row = this->find_one(fields::Inventory.warehouse == 2 && fields::Inventory.sku == std::string("pear"));
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->on_hand, 3) << "DO NOTHING must not overwrite the existing row";
}

// NOLINTEND(readability-implicit-bool-conversion)
