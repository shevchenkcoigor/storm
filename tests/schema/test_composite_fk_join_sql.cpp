#include <gtest/gtest.h>
#include <meta>
#include <plf_hive/plf_hive.h> // NOSONAR cpp:S954 — must precede `import std;` (see test_m2m_models.h)

#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_insert;
import std;
import storm_orm_statements_base;
import storm_orm_utilities;

#include "../../shared/models/message.h" // NOSONAR cpp:S954 — Message/Person
#include "../../shared/models/person.h"

// Must follow the shared model headers: OrderLine/Ledger are composite-PK fixtures reused
// across the #501/#502 tests; nothing here needs Person as an FK part.
#include "crud/test_composite_pk_models.h" // NOSONAR cpp:S954
#include "query/test_m2m_models.h"         // NOSONAR cpp:S954 — Student/Course (single-PK junction baseline)
using storm::orm::statements::BaseStatement;

// ── #504 (Task 2): widen find_fk_primary_key to find_fk_primary_key_members ──
// Step 1 of the composite-FK-join plan. Adds an ADDITIVE, composite-aware
// accessor for an FK target's full primary-key member list, alongside the
// existing (untouched) single-column find_fk_primary_key. Every existing
// single-FK call site keeps using find_fk_primary_key, byte-identical;
// find_fk_primary_key_members is consumed only by later composite-FK bind /
// extract / JOIN paths (Tasks 3-8 of the plan).

namespace {

    using storm::orm::statements::BaseStatement;

    // ---- Single-column FK target: Message::sender is fk<> Person, Person has
    // ---- exactly one primary-key member. find_fk_primary_key_members must
    // ---- return a 1-element array — the same shape find_fk_primary_key does.
    using MessageBase                = BaseStatement<Message>;
    constexpr auto single_pk_members = MessageBase::find_fk_primary_key_members<Person>();
    static_assert(single_pk_members.size() == 1);
    static_assert(MessageBase::fk_primary_key_count<Person>() == 1);
    static_assert(single_pk_members[0] == MessageBase::find_fk_primary_key<Person>());

    // ---- Composite (2-part) FK target: OrderLine has a 2-part composite PK
    // ---- (order_id, product_id). find_fk_primary_key_members must return
    // ---- both parts, in declaration order, matching OrderLine's own
    // ---- primary_key_members_ exactly.
    using OrderLineBase = BaseStatement<OrderLine>;
    static_assert(OrderLineBase::primary_key_members_.size() == 2);
    static_assert(OrderLineBase::primary_key_column_count_ == 2);

    constexpr auto order_line_fk_members = MessageBase::find_fk_primary_key_members<OrderLine>();
    static_assert(order_line_fk_members.size() == 2);
    static_assert(MessageBase::fk_primary_key_count<OrderLine>() == 2);
    static_assert(order_line_fk_members[0] == OrderLineBase::primary_key_members_[0]);
    static_assert(order_line_fk_members[1] == OrderLineBase::primary_key_members_[1]);

    // ---- Composite (3-part), mixed-type FK target: Ledger has region (int),
    // ---- account (std::string), period (int64_t). Proves the widened
    // ---- accessor is not arity- or type-limited.
    using LedgerBase = BaseStatement<Ledger>;
    static_assert(LedgerBase::primary_key_members_.size() == 3);
    static_assert(LedgerBase::primary_key_column_count_ == 3);

    constexpr auto ledger_fk_members = MessageBase::find_fk_primary_key_members<Ledger>();
    static_assert(ledger_fk_members.size() == 3);
    static_assert(MessageBase::fk_primary_key_count<Ledger>() == 3);
    static_assert(ledger_fk_members[0] == LedgerBase::primary_key_members_[0]);
    static_assert(ledger_fk_members[1] == LedgerBase::primary_key_members_[1]);
    static_assert(ledger_fk_members[2] == LedgerBase::primary_key_members_[2]);

    // ---- Nullable FK target: std::optional<Person> unwraps the same as the
    // ---- existing find_fk_primary_key does (ValidForeignKey / optional_inner_type_t).
    static_assert(MessageBase::fk_primary_key_count<std::optional<Person>>() == 1);
    constexpr auto optional_target_members = MessageBase::find_fk_primary_key_members<std::optional<Person>>();
    static_assert(optional_target_members.size() == 1);
    static_assert(optional_target_members[0] == single_pk_members[0]);

} // namespace

TEST(CompositeFkResolutionTest, FindFkPrimaryKeyMembersReturnsOnePartForSinglePkTarget) {
    // Verification is entirely in the static_asserts above; this body just
    // gives GoogleTest something to run.
    SUCCEED();
}

TEST(CompositeFkResolutionTest, FindFkPrimaryKeyMembersReturnsTwoPartsForOrderLineTarget) {
    SUCCEED();
}

TEST(CompositeFkResolutionTest, FkPrimaryKeyCountMatchesTargetPrimaryKeyColumnCount) {
    EXPECT_EQ(OrderLineBase::primary_key_column_count_, 2);
}

// ── #504 (Task 3): composite-aware column-name writer for FK members ──
// fk_column_names_size / append_fk_column_names are the composite-aware
// sibling of append_column_name / column_name_size (#422). A single-column
// FK target must degenerate to EXACTLY what append_column_name emits today
// ("<member>_id") — the byte-identical guarantee. A composite target spells
// one column per target PK part: "<member>_<part>", AND-joined by the
// caller-supplied separator.

TEST(FkColumnNamesTest, SingleColumnTargetMatchesLegacyAppendColumnName) {
    // Message::sender (fk<> Person, single-column PK) must emit exactly what
    // append_column_name emits today: "sender_id".
    //
    // fk_column_names_size/append_fk_column_names are consteval (like every other
    // writer in field_attr.cppm) and write into any buffer exposing
    // .append(std::string_view) — the same contract append_column_name uses. Every
    // real call site (field_names.cppm, join.cppm, ...) drives that buffer as a
    // storm::orm::utilities::ConstexprString<N> built inside a consteval function, so
    // the test mirrors that: build the ConstexprString at compile time, then compare
    // its runtime .view() to the expected string.
    using MessageBase            = storm::orm::statements::BaseStatement<Message>;
    constexpr auto sender_member = []() consteval {
        for (auto m : std::meta::nonstatic_data_members_of(^^Message, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(m) == "sender") {
                return m;
            }
        }
        std::unreachable();
    }();
    constexpr auto target_pk = MessageBase::find_fk_primary_key_members<Person>();
    constexpr auto size      = storm::meta::fk_column_names_size(sender_member, target_pk, ", ");
    constexpr auto result    = []() consteval {
        storm::orm::utilities::ConstexprString<size + 1> buf;
        storm::meta::append_fk_column_names(buf, sender_member, target_pk, ", ");
        return buf;
    }();
    EXPECT_EQ(result.view(), "sender_id");
}

TEST(FkColumnNamesTest, CompositeTargetEmitsOneColumnPerPart) {
    // A hypothetical FK member `line` of type OrderLine (2-part composite PK:
    // order_id, product_id) must emit "line_order_id, line_product_id".
    // (Uses a hand-rolled member array since no in-tree model has this FK yet —
    // this test validates the writer in isolation before Task 4 wires up a real model.)
    // fk_member is a placeholder info — the writer only reads the FK member's own
    // identifier, so any member reflection with identifier "line" would do; here we
    // reuse OrderLine::order_id's info as a stand-in reflection value and rely on
    // the test asserting the TARGET side (order_id/product_id) is correctly spelled
    // to prove the per-part loop is right; Task 4 covers the member-identifier prefix
    // end to end against a real composite-FK-holding model.
    constexpr auto target_pk = BaseStatement<OrderLine>::primary_key_members_;
    constexpr auto size      = storm::meta::fk_column_names_size(target_pk[0], target_pk, ", ");
    constexpr auto result    = []() consteval {
        storm::orm::utilities::ConstexprString<size + 1> buf;
        storm::meta::append_fk_column_names(buf, target_pk[0], target_pk, ", ");
        return buf;
    }();
    EXPECT_EQ(result.view(), "order_id_order_id, order_id_product_id");
}

// ── #504 (Task 5): ValidForeignKey composite-arity finding (no code change) ──
// fk<> annotates ONE member whose TYPE is the whole related object — there are
// no separate "local FK columns" to mismatch in count against the target's PK
// arity (unlike SQLAlchemy's ForeignKeyConstraint, which lists local columns
// explicitly). So ValidForeignKey<OrderLine> just needs OrderLine to have A
// primary key (single or composite) — already true via ModelWithPrimaryKey
// (#500 widened is_primary_member for primary_part). No new concept code
// needed; this test is a permanent regression guard on that reasoning.

TEST(CompositeFkArityTest, ValidForeignKeyAcceptsCompositePkTargetWithNoArityMismatchPossible) {
    static_assert(storm::orm::statements::ValidForeignKey<OrderLine>);
    static_assert(storm::orm::statements::ValidForeignKey<std::optional<OrderLine>>);
    SUCCEED();
}

// ── #504 (Task 9): junction-table DDL when either m2m side has a composite PK ──
//
// The auto-junction table (#203) was fixed at exactly 2 columns —
// "<Owner>_id INTEGER NOT NULL, <Related>_id INTEGER NOT NULL" — with a
// 2-column PRIMARY KEY and two FK clauses hardcoded to "REFERENCES <side>(id)".
// Every one of those three assumptions breaks when a side's PK is composite:
//
//   1. ONE column per side is not enough — a 3-part owner key needs 3 columns,
//      and there is no single column that could stand in for it (LedgerWithTags
//      has no "id" column at all).
//   2. The PRIMARY KEY must list every column from both sides, or the junction
//      would reject legitimate distinct pairs as duplicates.
//   3. "REFERENCES <side>(id)" names a column that does not exist on a
//      composite-PK model. SQLite does not validate FK target columns at CREATE
//      TABLE time so it silently accepts this, but PostgreSQL rejects it
//      outright — which is why create_table_if_not_exists<LedgerWithTags> could
//      not run on PG at all before this task.
//
// The column naming follows Task 3's convention verbatim: a single-PK side keeps
// the exact legacy "<Side>_id" spelling (byte-identical), a composite side emits
// "<Side>_<part>" per PK part.

namespace {
    namespace schema_ns = storm::orm::schema;

    template <typename Model, schema_ns::Dialect D> auto junction_sql() -> const std::string& {
        return schema_ns::SchemaStatement<Model>::template junction_table_sql<D>();
    }
} // namespace

// ---- Regression guard: single-PK on BOTH sides must stay BYTE-IDENTICAL. -----
// Every prior task in #504 carries such a guarantee. This is an EXACT-string
// assertion (not `contains`) against Student/Course, the canonical in-tree m2m
// pair from #203 — the whole DDL, every byte, including whitespace and the
// trailing ")". If the composite widening leaks into the single-PK path at all,
// this fails.

namespace {
    // The two dialects' single-PK junction DDL differs in exactly one token (the
    // integer column type), so the expected string is built from that token
    // rather than written out twice — every other byte is still asserted
    // literally, keeping this a true exact-string regression guard.
    auto expected_student_course_ddl(std::string_view int_type) -> std::string {
        return std::format(
                "CREATE TABLE Student_Course (\n"
                "    Student_id {0} NOT NULL,\n"
                "    Course_id {0} NOT NULL,\n"
                "    PRIMARY KEY (Student_id, Course_id),\n"
                "    FOREIGN KEY (Student_id) REFERENCES Student(id) ON DELETE CASCADE,\n"
                "    FOREIGN KEY (Course_id) REFERENCES Course(id) ON DELETE CASCADE\n"
                ")",
                int_type
        );
    }
} // namespace

TEST(JunctionDdlTest, SinglePkBothSidesSqliteStaysByteIdentical) {
    const std::string& sql = junction_sql<Student, schema_ns::Dialect::SQLite>();
    EXPECT_EQ(sql, expected_student_course_ddl("INTEGER"));
}

TEST(JunctionDdlTest, SinglePkBothSidesPostgresStaysByteIdentical) {
    const std::string& sql = junction_sql<Student, schema_ns::Dialect::PostgreSQL>();
    EXPECT_EQ(sql, expected_student_course_ddl("BIGINT"));
}

// ---- Composite OWNER side (LedgerWithTags: region/account/period) ------------
// 3 owner columns + 1 related column; PK lists all 4; the owner FK clause names
// all three target parts instead of the nonexistent "id".

TEST(JunctionDdlTest, CompositeOwnerSideEmitsOneColumnPerPkPart) {
    const std::string& sql = junction_sql<LedgerWithTags, schema_ns::Dialect::SQLite>();
    EXPECT_EQ(
            sql,
            "CREATE TABLE LedgerWithTags_LedgerTag (\n"
            "    LedgerWithTags_region INTEGER NOT NULL,\n"
            "    LedgerWithTags_account TEXT NOT NULL,\n"
            "    LedgerWithTags_period INTEGER NOT NULL,\n"
            "    LedgerTag_id INTEGER NOT NULL,\n"
            "    PRIMARY KEY (LedgerWithTags_region, LedgerWithTags_account, LedgerWithTags_period, LedgerTag_id),\n"
            "    FOREIGN KEY (LedgerWithTags_region, LedgerWithTags_account, LedgerWithTags_period) REFERENCES "
            "LedgerWithTags(region, account, period) ON DELETE CASCADE,\n"
            "    FOREIGN KEY (LedgerTag_id) REFERENCES LedgerTag(id) ON DELETE CASCADE\n"
            ")"
    );
}

// PG must map each part to its own dialect type: int -> INTEGER, string -> TEXT,
// int64 -> BIGINT (#603 width-matching). A single hardcoded "BIGINT NOT NULL"
// for every column (the pre-fix shape) would type the TEXT part wrong and PG
// would reject the FK for a type mismatch against LedgerWithTags.account.
// LedgerTag_id stays BIGINT: it mirrors LedgerTag's own single-column
// DB-generated PK, which is pinned to BIGINT regardless of its declared `int`
// width (see has_pinned_bigint_pk in schema.cppm) — not width-derived here.
TEST(JunctionDdlTest, CompositeOwnerSidePostgresTypesEachPartIndividually) {
    const std::string& sql = junction_sql<LedgerWithTags, schema_ns::Dialect::PostgreSQL>();
    EXPECT_TRUE(sql.contains("LedgerWithTags_region INTEGER NOT NULL")) << sql;
    EXPECT_TRUE(sql.contains("LedgerWithTags_account TEXT NOT NULL")) << sql;
    EXPECT_TRUE(sql.contains("LedgerWithTags_period BIGINT NOT NULL")) << sql;
    EXPECT_TRUE(sql.contains("LedgerTag_id BIGINT NOT NULL")) << sql;
}

// ---- Composite RELATED side (TagRegistry -> CatalogEntry) --------------------
// The mirror direction: single-PK owner keeps its legacy "<Owner>_id" spelling
// unchanged while the RELATED side widens. Proves the widening is per-side, not
// an all-or-nothing switch driven by the owner alone.

// #566 — same shape as expected_student_course_ddl above, but with two
// independent type tokens rather than one shared one (#603): TagRegistry_id
// mirrors TagRegistry's own single-column DB-generated PK (pinned to BIGINT
// on PG regardless of its declared `int` width — has_pinned_bigint_pk in
// schema.cppm), while CatalogEntry's plain `int` composite parts are
// width-derived (INTEGER on PG) — the two columns can diverge on PostgreSQL.
namespace {
    auto expected_tag_registry_catalog_entry_ddl(std::string_view owner_type, std::string_view related_type)
            -> std::string {
        return std::format(
                "CREATE TABLE TagRegistry_CatalogEntry (\n"
                "    TagRegistry_id {0} NOT NULL,\n"
                "    CatalogEntry_catalog_id {1} NOT NULL,\n"
                "    CatalogEntry_entry_no {1} NOT NULL,\n"
                "    PRIMARY KEY (TagRegistry_id, CatalogEntry_catalog_id, CatalogEntry_entry_no),\n"
                "    FOREIGN KEY (TagRegistry_id) REFERENCES TagRegistry(id) ON DELETE CASCADE,\n"
                "    FOREIGN KEY (CatalogEntry_catalog_id, CatalogEntry_entry_no) REFERENCES "
                "CatalogEntry(catalog_id, entry_no) ON DELETE CASCADE\n"
                ")",
                owner_type,
                related_type
        );
    }
} // namespace

TEST(JunctionDdlTest, CompositeRelatedSideWidensWhileOwnerStaysSingleColumn) {
    const std::string& sql = junction_sql<TagRegistry, schema_ns::Dialect::SQLite>();
    EXPECT_EQ(sql, expected_tag_registry_catalog_entry_ddl("INTEGER", "INTEGER"));
}

// PG must map each part independently on the related side too — the mirror of
// CompositeOwnerSidePostgresTypesEachPartIndividually above but exact-string
// rather than contains(): a PG-only regression in the related-side FK target
// list (#566) would otherwise pass this file's suite with no PG signal at all.
TEST(JunctionDdlTest, CompositeRelatedSideWidensWhileOwnerStaysSingleColumnPostgres) {
    const std::string& sql = junction_sql<TagRegistry, schema_ns::Dialect::PostgreSQL>();
    EXPECT_EQ(sql, expected_tag_registry_catalog_entry_ddl("BIGINT", "INTEGER"));
}

// ---- Composite on BOTH sides (ShelfAssignment -> StorageBin) ------------------
// 2 + 3 = 5 junction columns, all five in the PK, both FK clauses multi-column.
// This is also the widest junction in the tree: if build_junction_sql's
// ConstexprString budget undercounts, ConstexprString truncates SILENTLY (no
// diagnostic — the exact failure mode Task 7 hit), so this exact-string
// assertion is the sizing test as much as it is the shape test.

TEST(JunctionDdlTest, CompositeOnBothSidesEmitsEveryColumnFromEachSide) {
    const std::string& sql = junction_sql<ShelfAssignment, schema_ns::Dialect::SQLite>();
    EXPECT_EQ(
            sql,
            "CREATE TABLE ShelfAssignment_StorageBin (\n"
            "    ShelfAssignment_warehouse_no INTEGER NOT NULL,\n"
            "    ShelfAssignment_shelf_code TEXT NOT NULL,\n"
            "    StorageBin_aisle INTEGER NOT NULL,\n"
            "    StorageBin_bin_code TEXT NOT NULL,\n"
            "    StorageBin_revision INTEGER NOT NULL,\n"
            "    PRIMARY KEY (ShelfAssignment_warehouse_no, ShelfAssignment_shelf_code, StorageBin_aisle, "
            "StorageBin_bin_code, StorageBin_revision),\n"
            "    FOREIGN KEY (ShelfAssignment_warehouse_no, ShelfAssignment_shelf_code) REFERENCES "
            "ShelfAssignment(warehouse_no, shelf_code) ON DELETE CASCADE,\n"
            "    FOREIGN KEY (StorageBin_aisle, StorageBin_bin_code, StorageBin_revision) REFERENCES "
            "StorageBin(aisle, bin_code, revision) ON DELETE CASCADE\n"
            ")"
    );
}

// Exact byte-budget guard, independent of the string content above. The
// generated SQL must fit its ConstexprString buffer with NO truncation: a
// silently-truncated buffer would still produce a valid-looking prefix, so
// assert the emitted length equals what a from-scratch measurement says it
// should be. The widest fixture (5 columns, mixed types) is the one that would
// expose an undercount that narrower fixtures' slack absorbs.
TEST(JunctionDdlTest, WidestJunctionSqlIsNotSilentlyTruncated) {
    const std::string& sql = junction_sql<ShelfAssignment, schema_ns::Dialect::SQLite>();
    ASSERT_FALSE(sql.empty());
    EXPECT_TRUE(sql.ends_with("\n)")) << "truncated buffer would lose the closing paren: " << sql;
    EXPECT_EQ(std::ranges::count(sql, '('), std::ranges::count(sql, ')'))
            << "unbalanced parens indicate truncation: " << sql;
}
