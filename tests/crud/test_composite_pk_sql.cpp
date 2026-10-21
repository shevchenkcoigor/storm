#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_statements_insert;
import std;
// UpdateGrammar / EraseGrammar are not re-exported by `storm` — same direct
// import the m2m and reverse-FK SET-gate assertions use.
import storm_orm_statements_update_grammar;
import storm_orm_statements_erase_grammar;
import storm_orm_statements_upsert_grammar;
import storm_orm_statements_erase;

#include "../../shared/models/person.h" // NOSONAR cpp:S954 — Person, the FK target of StockEntry
#include "../../shared/models/timestamped_record.h"

// Must follow person.h: StockEntry's FK part names Person.
#include "test_composite_pk_models.h" // NOSONAR cpp:S954
using storm::orm::statements::InsertStatement;
using storm::orm::statements::ReturnId;

using storm::orm::statements::default_return_id;
using storm::orm::statements::ReturnId;
using storm::orm::statements::ReturnIdSupported;

// ── #501: composite PK — compile-time gates and generated SQL text ───────────
// Step 2 of #90, on top of #500 (annotation + DDL). Widens the by-key WHERE
// clause from "pk = ?" to "a = ? AND b = ?", with the bind arithmetic and the
// consteval sizing that follow from it.
//
// Three distinct things change; this TU pins the two static ones, and
// test_composite_pk_crud.cpp drives the third against live backends:
//   (1) SQL TEXT — one column name becomes N, AND-joined; the bulk DELETE IN
//       list becomes a row-value list "(a,b) IN ((?,?),(?,?))".
//   (2) THE SET-TARGET GATE — is_settable_member must exclude EVERY PK member,
//       not just the first, or a composite PK column becomes writable.
//   (3) BIND ARITHMETIC — N values per key, bound after the SET values.
//
// INSERT joined in #502: a composite key is never DB-generated, so every part
// is caller data — all key columns appear in the INSERT, and no RETURNING is
// emitted (there is no generated value to return).

// NOLINTBEGIN(readability-implicit-bool-conversion)

namespace {

    using storm::orm::statements::EraseGrammar;
    using storm::orm::statements::UpdateGrammar;
    using storm::orm::statements::UpsertGrammar;

    // ── (2) SET-target gate: every PK member is excluded ──────────────────────
    // update_grammar.cppm's is_settable_member() used `Member != primary_key_`,
    // which excludes only the FIRST PK member. On a composite model that leaves
    // every other part writable — `UPDATE ... SET product_id=? WHERE order_id=?
    // AND product_id=?` would rewrite part of the very key it matches on.
    //
    // Asserted on the gate predicate directly rather than as `!requires` on the
    // public update<>() call: that API is loosely constrained, so a bad member
    // would hard-error instead of yielding false.

    static_assert(
            !UpdateGrammar<OrderLine>::is_settable_member<^^OrderLine::order_id>(),
            "the FIRST part of a composite PK must not be a SET target"
    );
    static_assert(
            !UpdateGrammar<OrderLine>::is_settable_member<^^OrderLine::product_id>(),
            "EVERY part of a composite PK must be excluded from SET, not just the first"
    );
    static_assert(
            UpdateGrammar<OrderLine>::is_settable_member<^^OrderLine::quantity>(),
            "a non-key column is still a valid SET target"
    );
    static_assert(
            !UpdateGrammar<Ledger>::is_settable_member<^^Ledger::period>(),
            "the LAST part of a 3-part composite PK must be excluded from SET"
    );
    static_assert(
            UpdateGrammar<Ledger>::is_settable_member<^^Ledger::balance>(),
            "a non-key column of a 3-part-key model is a valid SET target"
    );
    // Single-PK behaviour unchanged.
    static_assert(
            !UpdateGrammar<Widget>::is_settable_member<^^Widget::id>(), "a single primary key is not a SET target"
    );
    static_assert(
            UpdateGrammar<Widget>::is_settable_member<^^Widget::name>(), "a single-PK model's plain column is settable"
    );

    // The same widening is required in upsert_grammar.cppm, which carries its own
    // copy of the gate (the issue's DoD names it explicitly). ON CONFLICT
    // targeting a composite key is #503; excluding the key parts from the SET
    // list is this issue's half of it.
    static_assert(
            !UpsertGrammar<OrderLine>::is_settable_member<^^OrderLine::product_id>(),
            "upsert's SET gate must exclude every PK member too"
    );
    static_assert(
            UpsertGrammar<OrderLine>::is_settable_member<^^OrderLine::quantity>(),
            "upsert's SET gate still accepts a non-key column"
    );

    template <typename T> auto single_delete_sql() -> std::string {
        return std::string(EraseGrammar<T>::build_single_delete_sql_array());
    }

    // Connection-free: the statement text is derived purely from reflection, so the
    // shape can be asserted for any row count without a live database. Asserted through
    // EraseGrammar rather than EraseStatement — the grammar is where the text is built,
    // and EraseStatement is at the cpp:S1448 method ceiling.
    template <typename T> auto bulk_delete_sql(std::size_t rows) -> std::string {
        return EraseGrammar<T>::bulk_delete_sql_for(rows);
    }

} // namespace

// ── (1) SQL text: DELETE ─────────────────────────────────────────────────────
// The single-row DELETE WHERE clause is AND-joined across the key, in
// DECLARATION order (the same order primary_key_members_ holds and the bind
// loop follows).

TEST(CompositePkDeleteSql, SingleRowWhereIsAndJoined) {
    EXPECT_EQ(single_delete_sql<OrderLine>(), "DELETE FROM OrderLine WHERE order_id = ? AND product_id = ?");
}

TEST(CompositePkDeleteSql, ThreePartWhereIsAndJoinedInDeclarationOrder) {
    EXPECT_EQ(single_delete_sql<Ledger>(), "DELETE FROM Ledger WHERE region = ? AND account = ? AND period = ?");
}

// The single-PK DELETE text must be byte-identical to before this issue.
TEST(CompositePkDeleteSql, SinglePkTextIsByteIdentical) {
    EXPECT_EQ(single_delete_sql<Widget>(), "DELETE FROM Widget WHERE id = ?");
}

// Bulk DELETE uses a row-value IN list for a composite key: the single-PK
// "id IN (?,?,?)" becomes "(a,b) IN ((?,?),(?,?),(?,?))". Both SQLite (>= 3.15;
// the project floor is 3.35) and PostgreSQL accept this form, and the execution
// tests in test_composite_pk_crud.cpp prove it against live backends.
TEST(CompositePkDeleteSql, BulkUsesRowValueInList) {
    EXPECT_EQ(
            bulk_delete_sql<OrderLine>(3), "DELETE FROM OrderLine WHERE (order_id, product_id) IN ((?,?),(?,?),(?,?))"
    );
}

TEST(CompositePkDeleteSql, BulkThreePartRowValueInList) {
    EXPECT_EQ(bulk_delete_sql<Ledger>(2), "DELETE FROM Ledger WHERE (region, account, period) IN ((?,?,?),(?,?,?))");
}

TEST(CompositePkDeleteSql, BulkSinglePkInListIsByteIdentical) {
    EXPECT_EQ(bulk_delete_sql<Widget>(3), "DELETE FROM Widget WHERE id IN (?,?,?)");
}

// The grammar builds a genuine one-element IN list for count == 1; it is
// EraseStatement::get_bulk_delete_sql that short-circuits a one-object batch to the
// single-row form, so this pins the grammar's own shape rather than that fast path.
TEST(CompositePkDeleteSql, BulkOfOneIsAOneElementList) {
    EXPECT_EQ(bulk_delete_sql<OrderLine>(1), "DELETE FROM OrderLine WHERE (order_id, product_id) IN ((?,?))");
    EXPECT_EQ(bulk_delete_sql<Widget>(1), "DELETE FROM Widget WHERE id IN (?)");
}

// An FK part's COLUMN is "<name>_id", so the key clause must name that. Emitting the
// bare member would produce a WHERE over a column that does not exist, and the
// statement would fail at prepare time on both backends. A composite key over FKs is
// the canonical association-table case, not an exotic one — the same reason #500
// asserts this for the DDL.
TEST(CompositePkDeleteSql, FkKeyPartUsesColumnNameNotIdentifier) {
    EXPECT_EQ(single_delete_sql<StockEntry>(), "DELETE FROM StockEntry WHERE warehouse_id = ? AND sku = ?");
}

TEST(CompositePkDeleteSql, FkKeyPartBulkUsesColumnName) {
    EXPECT_EQ(bulk_delete_sql<StockEntry>(2), "DELETE FROM StockEntry WHERE (warehouse_id, sku) IN ((?,?),(?,?))");
}

// ── (1) SQL text: UPDATE ─────────────────────────────────────────────────────
// SET excludes every key part; WHERE is AND-joined across all of them.

TEST(CompositePkUpdateSql, SetExcludesEveryKeyPartAndWhereIsAndJoined) {
    EXPECT_EQ(
            UpdateGrammar<OrderLine>::update_sql_string,
            "UPDATE OrderLine SET quantity=?, note=? WHERE order_id = ? AND product_id = ?"
    );
}

TEST(CompositePkUpdateSql, ThreePartKeyUpdate) {
    EXPECT_EQ(
            UpdateGrammar<Ledger>::update_sql_string,
            "UPDATE Ledger SET balance=? WHERE region = ? AND account = ? AND period = ?"
    );
}

TEST(CompositePkUpdateSql, SinglePkTextIsByteIdentical) {
    EXPECT_EQ(UpdateGrammar<Widget>::update_sql_string, "UPDATE Widget SET name=?, weight=? WHERE id = ?");
}

// FK key parts name their "_id" column in the UPDATE WHERE clause too, and stay out
// of the SET list like any other key part.
TEST(CompositePkUpdateSql, FkKeyPartUsesColumnNameNotIdentifier) {
    EXPECT_EQ(
            UpdateGrammar<StockEntry>::update_sql_string,
            "UPDATE StockEntry SET qty=? WHERE warehouse_id = ? AND sku = ?"
    );
}

static_assert(
        !UpdateGrammar<StockEntry>::is_settable_member<^^StockEntry::warehouse>(),
        "an FK part of a composite key is not a SET target"
);

// ── Chunking: the parameter budget shrinks by the key width ──────────────────
// The 999-variable ceiling is spent N params per row for an N-part key, so the
// effective chunk size is MAX_CHUNK_SIZE/N, not MAX_CHUNK_SIZE.

namespace {
    template <typename T> constexpr std::size_t chunk_rows = EraseGrammar<T>::MAX_CHUNK_ROWS;
} // namespace

TEST(CompositePkChunking, RowCapacityShrinksWithKeyWidth) {
    EXPECT_EQ(chunk_rows<Widget>, 799U) << "single-PK chunking is unchanged";
    EXPECT_EQ(chunk_rows<OrderLine>, 799U / 2) << "a 2-part key costs 2 parameters per row";
    EXPECT_EQ(chunk_rows<Ledger>, 799U / 3) << "a 3-part key costs 3 parameters per row";
}

TEST(CompositePkChunking, RowCapacityStaysWithinTheParameterBudget) {
    EXPECT_LE(chunk_rows<OrderLine> * 2U, 999U);
    EXPECT_LE(chunk_rows<Ledger> * 3U, 999U);
}

// ── (#502) SQL text: INSERT ──────────────────────────────────────────────────
// A composite PK has no auto-generation mechanism (AUTOINCREMENT and
// GENERATED ... AS IDENTITY are single-integer-column features), so every part
// is caller data: all key columns join the column list and placeholder list in
// DECLARATION order, and no RETURNING clause is emitted.

namespace {

    // SingleInsertQuery<R>::sql() is static: the text derives purely
    // from reflection, so no live connection is involved (same rationale as the
    // EraseGrammar asserts above).
    template <typename T, ReturnId R = default_return_id<T>()> auto insert_sql() -> std::string {
        return InsertStatement<T, storm::db::sqlite::Connection>::template SingleInsertQuery<R>::sql();
    }

} // namespace

TEST(CompositePkInsertSql, EveryKeyPartJoinsTheColumnList) {
    EXPECT_EQ(
            (insert_sql<OrderLine, ReturnId::No>()),
            "INSERT INTO OrderLine (order_id, product_id, quantity, note) VALUES (?, ?, ?, ?)"
    );
}

TEST(CompositePkInsertSql, ThreePartKeyInDeclarationOrder) {
    EXPECT_EQ(
            (insert_sql<Ledger, ReturnId::No>()),
            "INSERT INTO Ledger (region, account, period, balance) VALUES (?, ?, ?, ?)"
    );
}

// An FK part's COLUMN is "<name>_id" — emitting the bare member would INSERT
// into a column that does not exist. Same reason #500 asserts this for DDL.
TEST(CompositePkInsertSql, FkKeyPartUsesColumnNameNotIdentifier) {
    EXPECT_EQ(
            (insert_sql<StockEntry, ReturnId::No>()), "INSERT INTO StockEntry (warehouse_id, sku, qty) VALUES (?, ?, ?)"
    );
}

TEST(CompositePkInsertSql, MixedTypeKeyParts) {
    EXPECT_EQ(
            (insert_sql<Inventory, ReturnId::No>()), "INSERT INTO Inventory (warehouse, sku, on_hand) VALUES (?, ?, ?)"
    );
}

// Single-PK INSERT text must be byte-identical to before this issue, on both
// the void and the RETURNING variants.
TEST(CompositePkInsertSql, SinglePkTextIsByteIdentical) {
    EXPECT_EQ((insert_sql<Widget, ReturnId::No>()), "INSERT INTO Widget (name, weight) VALUES (?, ?)");
    EXPECT_EQ(insert_sql<Widget>(), "INSERT INTO Widget (name, weight) VALUES (?, ?) RETURNING id");
}

// ── (#502) ReturnId gate ─────────────────────────────────────────────────────
// ReturnId::Yes means "return the DB-generated key", which a composite model
// does not have. Unconstrained it would compile and emit "RETURNING <first
// part>" — the issue's rejected option C (silently lossy) through a back door.
// Asserted on the gate predicate, not via an ill-formed insert<Yes>() call —
// the established negative-compile pattern (a TU cannot contain the ill-formed
// call it asserts about).

namespace {

    static_assert(!ReturnIdSupported<OrderLine, ReturnId::Yes>, "a composite model has no DB-generated key to return");
    static_assert(ReturnIdSupported<OrderLine, ReturnId::No>, "opting OUT of the id is valid on every model shape");
    static_assert(ReturnIdSupported<Widget, ReturnId::Yes>, "single-PK models keep the RETURNING path");
    static_assert(ReturnIdSupported<Widget, ReturnId::No>, "single-PK models keep the explicit void path");
    static_assert(!ReturnIdSupported<Ledger, ReturnId::Yes>, "the gate is not arity-limited (3-part key)");
    static_assert(!ReturnIdSupported<StockEntry, ReturnId::Yes>, "an FK-part composite key is gated too");

    // Plain insert() picks its default from the model shape: void for composite
    // (nothing to return), id for single-PK (unchanged).
    static_assert(
            default_return_id<OrderLine>() == ReturnId::No,
            "plain insert() on a composite model resolves to the void path"
    );
    static_assert(
            default_return_id<Widget>() == ReturnId::Yes, "plain insert() on a single-PK model still returns the id"
    );

} // namespace

// ── (#503) UPSERT — ON CONFLICT target and RETURNING ────────────────────────
// Step 4 of #90, on top of #500/#501/#502. Two things change: (1)
// ConflictTargetUnique must accept the FULL composite-PK column set as a
// conflict target (a strict subset stays a compile error — the interesting
// negative case), and (2) RETURNING follows #502's decision: a composite key
// is never DB-generated, so there is nothing to RETURN.

namespace {

    using storm::orm::statements::ConflictTargetUnique;
    using storm::orm::statements::UpsertGrammar;

} // namespace

// ── ConflictTargetUnique: composite PK as a conflict target ─────────────────

static_assert(
        ConflictTargetUnique<OrderLine, ^^OrderLine::order_id, ^^OrderLine::product_id>,
        "the full composite-PK column set, in declaration order, is a valid conflict target"
);
static_assert(
        ConflictTargetUnique<Ledger, ^^Ledger::region, ^^Ledger::account, ^^Ledger::period>,
        "a 3-part composite PK is a valid conflict target too"
);
static_assert(
        ConflictTargetUnique<StockEntry, ^^StockEntry::warehouse, ^^StockEntry::sku>,
        "an FK-part composite PK is a valid conflict target"
);

// The interesting negative case: a STRICT SUBSET of the composite PK is not
// unique on its own and must be rejected at this named gate.
static_assert(
        !ConflictTargetUnique<OrderLine, ^^OrderLine::order_id>,
        "a single column of a composite PK is not unique on its own"
);
static_assert(
        !ConflictTargetUnique<Ledger, ^^Ledger::region, ^^Ledger::account>,
        "a 2-of-3 subset of a composite PK is still not a unique constraint"
);
// Single-PK behaviour is unchanged: the PK itself is never a valid INSERT
// conflict target (INSERT omits it for a single-PK model).
static_assert(!ConflictTargetUnique<Widget, ^^Widget::id>, "single-PK ConflictTargetUnique behaviour is unchanged");

// ── RETURNING: composite models emit none ────────────────────────────────────

TEST(CompositePkUpsertSql, NothingSqlHasNoReturning) {
    const std::string& sql = UpsertGrammar<OrderLine>::nothing_sql<^^OrderLine::order_id, ^^OrderLine::product_id>();
    EXPECT_NE(sql.find("ON CONFLICT (order_id, product_id) DO NOTHING"), std::string::npos) << sql;
    EXPECT_EQ(sql.find("RETURNING"), std::string::npos) << "a composite key has nothing to RETURN: " << sql;
}

TEST(CompositePkUpsertSql, UpdateSqlHasNoReturning) {
    const std::string sql = UpsertGrammar<OrderLine>::update_sql<^^OrderLine::order_id, ^^OrderLine::product_id>(
            UpsertGrammar<OrderLine>::build_excluded_set_clause<^^OrderLine::quantity>()
    );
    EXPECT_NE(
            sql.find("ON CONFLICT (order_id, product_id) DO UPDATE SET quantity=excluded.quantity"), std::string::npos
    ) << sql;
    EXPECT_EQ(sql.find("RETURNING"), std::string::npos) << "a composite key has nothing to RETURN: " << sql;
}

// An FK part's COLUMN is "<name>_id" in the conflict target too — same reason
// #500/#501/#502 assert this for DDL/WHERE/INSERT.
TEST(CompositePkUpsertSql, FkKeyPartConflictTargetUsesColumnName) {
    const std::string target =
            std::string(UpsertGrammar<StockEntry>::build_conflict_target<^^StockEntry::warehouse, ^^StockEntry::sku>());
    EXPECT_EQ(target, "(warehouse_id, sku)");
}

// Single-PK upsert SQL must stay byte-identical: RETURNING <pk> is still emitted.
TEST(CompositePkUpsertSql, SinglePkStillEmitsReturning) {
    const std::string& nothing = UpsertGrammar<Widget>::nothing_sql<^^Widget::name>();
    EXPECT_TRUE(nothing.ends_with("RETURNING id")) << nothing;

    const std::string update = UpsertGrammar<Widget>::update_sql<^^Widget::name>(
            UpsertGrammar<Widget>::build_excluded_set_clause<^^Widget::weight>()
    );
    EXPECT_TRUE(update.ends_with("RETURNING id")) << update;
}

// ── #542: the auto_update SET tail uses the canonical column-name writer ─────
// Both tails appended identifier_of(member) directly while the explicit-pack
// branch beside them used meta::append_column_name (#422). The spellings differ
// only for an FK member, which ValidTimestampField bars from carrying a timestamp
// annotation — so no model can observe the difference and these assertions hold
// equally before and after the fix. They pin the emitted text so a later change to
// either writer cannot silently move the auto_update tail off the shared spelling.
TEST(AutoUpdateSetTail, UpdateSetClauseText) {
    constexpr auto set = UpdateGrammar<TimestampedRecord>::build_conditional_set_clause<^^TimestampedRecord::name>();
    // created_at is auto_create (INSERT only), so only updated_at joins the tail.
    EXPECT_EQ(std::string(set), "name=?, updated_at=?");
}

TEST(AutoUpdateSetTail, UpsertExcludedSetClauseText) {
    constexpr auto set = UpsertGrammar<TimestampedRecord>::build_excluded_set_clause<^^TimestampedRecord::name>();
    EXPECT_EQ(std::string(set), "name=excluded.name, updated_at=?");
}

// NOLINTEND(readability-implicit-bool-conversion)
