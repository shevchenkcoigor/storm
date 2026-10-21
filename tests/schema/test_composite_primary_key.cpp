#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import std;
import storm_orm_statements_base;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::create_index_sql;
using storm::create_table_sql;
using storm::many_to_many;
using storm::primary;
using storm::primary_autoincrement;
using storm::primary_part;
using storm::RefAction;
using storm::db::DefaultConnection;
using storm::orm::schema::Dialect;
using storm::orm::statements::BaseStatement;

// ── #500: composite primary key — annotation, concept widening, CREATE TABLE DDL ──
// Step 1 of #90. Introduces the primary_part annotation and widens the
// compile-time PK machinery from "exactly one member" to "one or more", WITHOUT
// touching any CRUD path. At the end of this issue a composite-PK model compiles,
// produces correct CREATE TABLE DDL on both dialects, and is still rejected by
// INSERT/UPDATE/DELETE/JOIN (those land in #501-#504).
//
// primary_part is a SEPARATE tag from primary (not "two primary members means
// composite"): that keeps an accidental double-`primary` typo a compile error
// instead of silently becoming a legal composite key.

namespace {
    using storm::orm::statements::ModelPrimaryKeyValid;
    using storm::orm::statements::ModelWithPrimaryKey;

    // ── Models ───────────────────────────────────────────────────────────────

    // Canonical composite-PK model: two primary_part members.
    struct OrderItem {
        [[= primary_part]] int order_id{};
        [[= primary_part]] int product_id{};
        int                    quantity{};
    };

    // Three-part composite, mixed types, to prove the list is not arity-limited
    // and that a non-integer part is accepted.
    struct TripleKey {
        [[= primary_part]] int          region{};
        [[= primary_part]] std::string  sku;
        [[= primary_part]] std::int64_t revision{};
        double                          price{};
    };

    // Single-PK control, unchanged behaviour.
    struct SinglePk {
        [[= primary]] int id{};
        std::string       name;
    };

    // Single-PK control for the SQLite AUTOINCREMENT branch — the one path the
    // composite change must route around rather than through.
    struct AutoincrementModel {
        [[= primary_autoincrement]] int id{};
        std::string                     name;
    };

    // ── Rejected combinations (ModelPrimaryKeyValid) ─────────────────────────

    // primary + primary_part on the same model: two competing PK declarations.
    struct MixedPrimaryAndPart {
        [[= primary]] int      id{};
        [[= primary_part]] int order_id{};
    };

    // primary_autoincrement + primary_part: autoincrement is UNREPRESENTABLE with a
    // composite PK. SQLite rejects both spellings at parse time — a column-level
    // PRIMARY KEY cannot coexist with a table-level PRIMARY KEY (...), and
    // AUTOINCREMENT is only grammatical directly after INTEGER PRIMARY KEY. PG is
    // equivalent (GENERATED AS IDENTITY is single-column). So it must be a
    // compile-time rejection, not a DDL we emit and let the backend refuse.
    struct MixedAutoincrementAndPart {
        [[= primary_autoincrement]] int id{};
        [[= primary_part]] int          order_id{};
    };

    // Exactly one primary_part is a plain PK spelled the wrong way.
    struct LonelyPart {
        [[= primary_part]] int order_id{};
        std::string            name;
    };

    // The accidental double-`primary` typo. Keeping THIS an error is the entire reason
    // primary_part is a separate tag — the widened PK machinery would otherwise treat it
    // as a composite key and silently emit different DDL than before composite support.
    struct DoublePrimary {
        [[= primary]] int a{};
        [[= primary]] int b{};
    };

    // A PK annotation on an m2m container: never emitted as a column, so the key would
    // name something no column definition provides (the #485 class of leak).
    struct PartOnRelation {
        [[= primary_part]] int                                     a{};
        [[= primary_part]][[= many_to_many<>]] std::vector<Person> members;
    };

    // A nullable PK part is not a key: SQLite's legacy NULL quirk even lets duplicate
    // keys through, so the same DDL behaves differently on SQLite and PG.
    struct NullablePart {
        [[= primary_part]] int                a{};
        [[= primary_part]] std::optional<int> b{};
    };

    // The nullable rejection is gated on is_primary_member, so it covers a plain
    // `primary` too. This is a DELIBERATE widening beyond the composite path: a nullable
    // PK was accepted before #500 and is now refused, for the same reason as above. No
    // in-tree model used the shape. Pinned here so the widening stays intentional.
    struct NullableSinglePrimary {
        [[= primary]] std::optional<int> id{};
        std::string                      name;
    };

    // UNIQUE on one part allows at most one row per that part alone, which defeats the
    // composite key it belongs to.
    struct UniquePart {
        [[= primary_part]] int                    a{};
        [[= primary_part]][[= storm::unique]] int b{};
    };

    // A composite key whose parts are FKs — the canonical reason composite keys exist
    // (association tables). The FK part's COLUMN is "warehouse_id", so the key clause
    // must name that, not the bare member.
    struct StockLine {
        [[= primary_part]][[= storm::fk<>]] Person warehouse;
        [[= primary_part]] int                     sku{};
        int                                        qty{};
    };

    // Several FK columns with long names and an ON DELETE clause — the shape that
    // exhausts the fixed buffer slack first if FK columns are sized as regular columns.
    // Guards the CREATE TABLE buffer against silent truncation.
    struct WideFkComposite {
        [[= primary_part]][[= storm::fk<>]] Person                   first_ref;
        [[= primary_part]][[= storm::fk<RefAction::Cascade>]] Person second_ref;
        std::string                                                  some_reasonably_long_label;
        std::string                                                  another_reasonably_long_label;
    };

    // ── is_primary_part_member / is_primary_member ───────────────────────────
    // primary_part is recognised as a PK-carrying annotation, and the two
    // predicates stay distinct: is_primary_part_member is true ONLY for the new tag.

    consteval auto member_named(std::meta::info type, std::string_view name) -> std::meta::info {
        for (const auto member : std::meta::nonstatic_data_members_of(type, std::meta::access_context::unchecked())) {
            if (std::meta::identifier_of(member) == name) {
                return member;
            }
        }
        std::unreachable();
    }

    static_assert(
            storm::meta::is_primary_part_member(member_named(^^OrderItem, "order_id")),
            "a primary_part member must be recognised by is_primary_part_member"
    );
    static_assert(
            !storm::meta::is_primary_part_member(member_named(^^OrderItem, "quantity")),
            "an unannotated member must not be a primary part"
    );
    static_assert(
            !storm::meta::is_primary_part_member(member_named(^^SinglePk, "id")),
            "a plain `primary` member must NOT be reported as a primary_part"
    );
    static_assert(
            storm::meta::is_primary_member(member_named(^^OrderItem, "order_id")),
            "a primary_part member is a primary-key member"
    );

    // ── ModelWithPrimaryKey accepts a composite PK ───────────────────────────

    static_assert(
            storm::orm::statements::ModelWithPrimaryKey<OrderItem>,
            "a composite-PK model must satisfy ModelWithPrimaryKey"
    );
    static_assert(
            storm::orm::statements::ModelWithPrimaryKey<TripleKey>,
            "a 3-part composite-PK model must satisfy ModelWithPrimaryKey"
    );
    static_assert(
            storm::orm::statements::ModelWithPrimaryKey<SinglePk>,
            "a single-PK model must still satisfy ModelWithPrimaryKey"
    );

    // ── ModelPrimaryKeyValid rejections ──────────────────────────────────────
    // Asserted on the gate predicate directly, not as !requires on a public call:
    // a loosely-constrained public API would hard-error instead of yielding false.

    static_assert(ModelPrimaryKeyValid<OrderItem>, "two primary_part members are a valid composite PK");
    static_assert(ModelPrimaryKeyValid<TripleKey>, "three primary_part members are a valid composite PK");
    static_assert(ModelPrimaryKeyValid<SinglePk>, "a single `primary` member is a valid PK");

    static_assert(!ModelPrimaryKeyValid<MixedPrimaryAndPart>, "primary combined with primary_part must be rejected");
    static_assert(
            !ModelPrimaryKeyValid<MixedAutoincrementAndPart>,
            "primary_autoincrement combined with primary_part must be rejected (unrepresentable in SQL)"
    );
    static_assert(
            !ModelPrimaryKeyValid<LonelyPart>, "exactly one primary_part must be rejected (that is a plain `primary`)"
    );
    static_assert(
            !ModelPrimaryKeyValid<DoublePrimary>,
            "two `primary` members must stay an ERROR — that is the whole point of a separate primary_part tag"
    );
    static_assert(!ModelPrimaryKeyValid<PartOnRelation>, "a PK annotation on a relation container must be rejected");
    static_assert(!ModelPrimaryKeyValid<NullablePart>, "a nullable PK part must be rejected");
    static_assert(
            !ModelPrimaryKeyValid<NullableSinglePrimary>,
            "a nullable SINGLE primary is rejected too — deliberate widening beyond the composite path"
    );
    static_assert(!ModelPrimaryKeyValid<UniquePart>, "unique on a single part contradicts the composite key");

    static_assert(ModelPrimaryKeyValid<StockLine>, "a composite key over FK parts is valid");
} // namespace

// The gate is load-bearing at the statement layer: ModelPrimaryKeyValid<T> is ANDed
// into the BaseStatement<T> constraint list, so an invalid PK declaration fails at a
// named constraint rather than deep inside reflection. Wrapped in a variable template
// so the failed-constraint case is a dependent SFINAE soft-fail (naming the
// constrained specialization directly in a namespace-scope `requires` is eagerly
// instantiated by clang-p2996 and hard-errors) — same pattern as #472/#492.
template <class T> constexpr bool base_statement_instantiable = requires { typename BaseStatement<T>; };

static_assert(base_statement_instantiable<OrderItem>, "a composite-PK model must back a statement");
static_assert(base_statement_instantiable<SinglePk>, "a single-PK model must still back a statement");
static_assert(
        !base_statement_instantiable<MixedPrimaryAndPart>, "primary + primary_part must block statement instantiation"
);
static_assert(
        !base_statement_instantiable<MixedAutoincrementAndPart>,
        "primary_autoincrement + primary_part must block statement instantiation"
);
static_assert(!base_statement_instantiable<LonelyPart>, "a lone primary_part must block statement instantiation");
static_assert(!base_statement_instantiable<DoublePrimary>, "two `primary` members must block statement instantiation");
static_assert(!base_statement_instantiable<PartOnRelation>, "a PK on a relation container must block instantiation");
static_assert(!base_statement_instantiable<NullablePart>, "a nullable PK part must block instantiation");
static_assert(!base_statement_instantiable<UniquePart>, "a unique PK part must block instantiation");
static_assert(base_statement_instantiable<StockLine>, "a composite key over FK parts must back a statement");

// ── primary_key_members_ ─────────────────────────────────────────────────────
// The widened PK list sits alongside the existing primary_key_ / pk_name_ statics.

namespace {
    template <typename T> using Base = BaseStatement<T>;

    static_assert(Base<OrderItem>::primary_key_members_.size() == 2, "OrderItem has a 2-part primary key");
    static_assert(Base<TripleKey>::primary_key_members_.size() == 3, "TripleKey has a 3-part primary key");
    static_assert(Base<SinglePk>::primary_key_members_.size() == 1, "a single-PK model has a 1-element PK list");

    static_assert(Base<OrderItem>::has_composite_pk_, "OrderItem's PK is composite");
    static_assert(!Base<SinglePk>::has_composite_pk_, "a single-PK model's PK is not composite");

    // The list is in declaration order — the order the table-level PRIMARY KEY (...)
    // clause must emit, since column order is semantically significant for the index.
    static_assert(
            std::meta::identifier_of(Base<OrderItem>::primary_key_members_[0]) == "order_id",
            "primary_key_members_ must be in declaration order"
    );
    static_assert(
            std::meta::identifier_of(Base<OrderItem>::primary_key_members_[1]) == "product_id",
            "primary_key_members_ must be in declaration order"
    );

    // Single-PK models keep primary_key_ / pk_name_ working unchanged: the ~13 files
    // that read them are untouched by this issue.
    static_assert(
            Base<SinglePk>::primary_key_ == Base<SinglePk>::primary_key_members_[0],
            "for a single-PK model primary_key_ is the sole element of primary_key_members_"
    );
    static_assert(Base<SinglePk>::pk_name_ == "id", "single-PK pk_name_ is unchanged");
} // namespace

// ── CREATE TABLE DDL ─────────────────────────────────────────────────────────

// A composite PK emits a table-level PRIMARY KEY (a, b) clause after the columns,
// and NO column-level PRIMARY KEY — reusing the junction-table DDL pattern. The PK
// part columns are ordinary NOT NULL columns.
//
// Both dialects render "INTEGER" here: OrderItem's parts are plain `int`, and PG's
// width-matched integer mapping (#603) picks INTEGER for a 4-byte signed source —
// the ordinary per-dialect mapping, unchanged by the composite path. Neither carries
// the SQLite AUTOINCREMENT nor the PG GENERATED AS IDENTITY decoration: every part of
// a composite key is caller-supplied, so no part can be DB-generated.
//
// The parts carry `DEFAULT 0` because they are genuinely regular columns on this path
// — the auto-DEFAULT for NOT NULL columns (#413) applies to them like any other. That
// is harmless for a PK: the caller always supplies the full key, and the DEFAULT only
// makes ALTER TABLE ADD COLUMN work on a populated table, which is why #413 emits it.
auto order_item_ddl(std::string_view int_type) -> std::string {
    return std::format(
            "CREATE TABLE OrderItem (\n"
            "    order_id {0} NOT NULL DEFAULT 0,\n"
            "    product_id {0} NOT NULL DEFAULT 0,\n"
            "    quantity {0} NOT NULL DEFAULT 0,\n"
            "    PRIMARY KEY (order_id, product_id)\n"
            ")",
            int_type
    );
}

TEST(CompositePrimaryKeyDDL, SqliteEmitsTableLevelPrimaryKey) {
    EXPECT_EQ(create_table_sql<OrderItem>(), order_item_ddl("INTEGER"));
}

TEST(CompositePrimaryKeyDDL, PostgresEmitsTableLevelPrimaryKey) {
    EXPECT_EQ((create_table_sql<OrderItem, Dialect::PostgreSQL>()), order_item_ddl("INTEGER"));
}

TEST(CompositePrimaryKeyDDL, ThreePartKeyEmitsAllPartsInOrder) {
    const std::string& sql = create_table_sql<TripleKey>();
    EXPECT_TRUE(sql.contains("PRIMARY KEY (region, sku, revision)")) << sql;
    // No column-level PRIMARY KEY anywhere: the table-level clause is the only one.
    EXPECT_EQ(sql.find("PRIMARY KEY"), sql.rfind("PRIMARY KEY")) << sql;
}

TEST(CompositePrimaryKeyDDL, ThreePartKeyPgEmitsAllPartsInOrder) {
    const std::string& sql = create_table_sql<TripleKey, Dialect::PostgreSQL>();
    EXPECT_TRUE(sql.contains("PRIMARY KEY (region, sku, revision)")) << sql;
    EXPECT_TRUE(sql.contains("sku TEXT NOT NULL")) << sql;
    EXPECT_FALSE(sql.contains("IDENTITY")) << sql;
    EXPECT_EQ(sql.find("PRIMARY KEY"), sql.rfind("PRIMARY KEY")) << sql;
}

// An FK part's COLUMN is "<name>_id", so the key clause must name that. Emitting the
// bare member would produce a PRIMARY KEY over a column that does not exist, and the
// DDL would fail at parse time on both backends — and a composite key over FKs is the
// canonical association-table case, not an exotic one.
TEST(CompositePrimaryKeyDDL, FkPartUsesColumnNameNotIdentifier) {
    const std::string& sql = create_table_sql<StockLine>();
    EXPECT_TRUE(sql.contains("PRIMARY KEY (warehouse_id, sku)")) << sql;
    EXPECT_FALSE(sql.contains("PRIMARY KEY (warehouse,")) << sql;
    // The part is still emitted as a normal FK column, REFERENCES intact.
    EXPECT_TRUE(sql.contains("warehouse_id INTEGER NOT NULL REFERENCES Person(id)")) << sql;
}

TEST(CompositePrimaryKeyDDL, FkPartUsesColumnNameNotIdentifierPg) {
    const std::string& sql = create_table_sql<StockLine, Dialect::PostgreSQL>();
    EXPECT_TRUE(sql.contains("PRIMARY KEY (warehouse_id, sku)")) << sql;
    EXPECT_TRUE(sql.contains("warehouse_id BIGINT NOT NULL REFERENCES Person(id)")) << sql;
}

// The FK columns of a composite model must be SIZED as FK columns. Crediting them the
// (much shorter) regular-column suffix under-counts the buffer, and ConstexprString
// silently drops the overflow — so the failure mode is a truncated CREATE TABLE with no
// diagnostic. Long related-type names exhaust the fixed slack first, which is what this
// model is built to do. Reaching this assertion at all means the DDL was not truncated.
TEST(CompositePrimaryKeyDDL, CompositeWithFksIsNotTruncated) {
    const std::string& sql = create_table_sql<WideFkComposite>();
    EXPECT_TRUE(sql.ends_with(")")) << sql;
    EXPECT_TRUE(sql.contains("PRIMARY KEY (first_ref_id, second_ref_id)")) << sql;
    EXPECT_TRUE(sql.contains("second_ref_id INTEGER NOT NULL REFERENCES Person(id) ON DELETE CASCADE")) << sql;
}

TEST(CompositePrimaryKeyDDL, CompositePartsAreNotAutoincrement) {
    const std::string& sql = create_table_sql<OrderItem>();
    EXPECT_FALSE(sql.contains("AUTOINCREMENT")) << sql;
}

// The main regression risk of this issue: single-PK DDL must be BYTE-IDENTICAL to
// before the composite path was added. Person/Message are the long-standing schema
// fixtures asserted verbatim in test_orm_schema.cpp; repeating the assertion here
// pins it against the composite branch specifically.
TEST(CompositePrimaryKeyDDL, SinglePkDdlIsByteIdentical) {
    const std::string expected = "CREATE TABLE SinglePk (\n"
                                 "    id INTEGER PRIMARY KEY,\n"
                                 "    name TEXT NOT NULL\n"
                                 ")";
    EXPECT_EQ(create_table_sql<SinglePk>(), expected);
}

TEST(CompositePrimaryKeyDDL, SinglePkAutoincrementDdlUnchanged) {
    // The column-level AUTOINCREMENT branch is untouched by the composite path. The
    // trailing "PRIMARY KEY (" check pins that a single-PK model never grows a
    // table-level clause.
    const std::string& sql = create_table_sql<AutoincrementModel>();
    EXPECT_TRUE(sql.contains("id INTEGER PRIMARY KEY AUTOINCREMENT")) << sql;
    EXPECT_FALSE(sql.contains("PRIMARY KEY (")) << sql;
}

TEST(CompositePrimaryKeyDDL, SinglePkPgDdlUnchanged) {
    const std::string& sql = create_table_sql<Person, Dialect::PostgreSQL>();
    EXPECT_TRUE(sql.contains("id BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY")) << sql;
    EXPECT_FALSE(sql.contains("PRIMARY KEY (")) << sql;
}

// A composite PK is the table's key — its parts must not also get their own
// single-column CREATE INDEX (the PK is already an index), matching how the
// single-PK path excludes primary_key_ from needs_index().
TEST(CompositePrimaryKeyDDL, CompositePartsGetNoSeparateIndex) {
    const std::vector<std::string>& indexes = create_index_sql<OrderItem>();
    for (const std::string& idx : indexes) {
        EXPECT_FALSE(idx.contains("(order_id)")) << idx;
        EXPECT_FALSE(idx.contains("(product_id)")) << idx;
    }
}

// The DDL is genuinely executable on a live backend, and the composite key is
// actually enforced: a duplicate (order_id, product_id) pair is rejected while a
// row differing in one part is accepted. Runs on SQLite and PostgreSQL.
//
// The fixture anchors the connection on Person but skips its table creation — a
// composite-PK model cannot reach a CRUD path in this issue (that is #501-#504),
// so the table is created from the generated DDL by hand and driven with raw SQL.
template <typename ConnType> class CompositePkExecutionTest : public StormTestFixture<Person, ConnType> {
  public:
    auto on_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {}
};

TYPED_TEST_SUITE(CompositePkExecutionTest, DatabaseTypes);

TYPED_TEST(CompositePkExecutionTest, CompositeDdlExecutesAndEnforcesKey) {
    const auto& conn = DefaultConnection<TypeParam>::get();

    const std::string ddl = storm::test::is_postgresql<TypeParam>() ? create_table_sql<OrderItem, Dialect::PostgreSQL>()
                                                                    : create_table_sql<OrderItem>();
    ASSERT_TRUE(conn->execute(ddl).has_value()) << ddl;

    ASSERT_TRUE(conn->execute("INSERT INTO OrderItem (order_id, product_id, quantity) VALUES (1, 2, 5)").has_value());
    // Differs in the second part — accepted.
    EXPECT_TRUE(conn->execute("INSERT INTO OrderItem (order_id, product_id, quantity) VALUES (1, 3, 7)").has_value());
    // Full duplicate key — rejected by the composite PRIMARY KEY.
    EXPECT_FALSE(conn->execute("INSERT INTO OrderItem (order_id, product_id, quantity) VALUES (1, 2, 9)").has_value());
}

// A composite key over FK parts must be EXECUTABLE, not merely well-formed text. Naming
// the bare member instead of the "<name>_id" column produces DDL the backend rejects
// with "no such column", which only a live execution catches.
TYPED_TEST(CompositePkExecutionTest, FkPartCompositeDdlExecutes) {
    const auto& conn = DefaultConnection<TypeParam>::get();

    const bool        is_pg      = storm::test::is_postgresql<TypeParam>();
    const std::string person_ddl = is_pg ? create_table_sql<Person, Dialect::PostgreSQL>() : create_table_sql<Person>();
    ASSERT_TRUE(conn->execute(person_ddl).has_value()) << person_ddl;

    const std::string ddl = is_pg ? create_table_sql<StockLine, Dialect::PostgreSQL>() : create_table_sql<StockLine>();
    ASSERT_TRUE(conn->execute(ddl).has_value()) << ddl;
}
