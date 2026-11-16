#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

// #828: table and column names that are SQL reserved words. Storm quotes every identifier it
// emits (lowercased, so the name is the one PostgreSQL's case folding would give it anyway),
// which makes `Group`, `Order`, `desc`, `Union` … ordinary model and member names.
//
// `desc` is reserved on PostgreSQL only; `Group`, `Order`, `Union`, `Select`, `Where`, `Table`,
// `From`, `Limit`, `All` and `Having` on both backends. Every statement path names the table or
// one of those columns, and only executing the statement shows whether the name was quoted —
// hence TYPED_TEST on both backends rather than SQL-text assertions alone.
//
// The mixed-case half of the same upstream scenarios (SQLAlchemy test_quote.py) is
// tests/query/test_mixed_case_identifiers.cpp (#822).

import storm;
import std;

using storm::QuerySet;

#include "test_fixture.h" // NOSONAR cpp:S954

struct Order;

struct Group {
    [[= storm::primary_assigned]] int                          Select{};
    int                                                        desc{};
    int                                                        Union{};
    int                                                        ASC{};
    [[ = storm::unique, = storm::max_length<10> ]] std::string Where;
    [[= storm::indexed]] int                                   order{};
    [[= storm::reverse_fk<^^Order>]] std::vector<Order>        Having;

    using storm_indexes = std::tuple<storm::Index<^^Group::desc, ^^Group::Union>>;
};

struct Order {
    [[= storm::primary_assigned]] int              Limit{};
    int                                            From{};
    [[= storm::fk<>]] Group                        Table;
    [[= storm::many_to_many<>]] std::vector<Group> All;
};

// A DATABASE-generated reserved key: the only shape that emits RETURNING "<pk>".
struct Check {
    [[= storm::primary]] int Default{};
    int                      Values{};
};

// A composite key whose parts are reserved words: table-level PRIMARY KEY, by-key WHERE,
// row-value IN on bulk delete.
struct Join {
    [[= storm::primary_part]] int Inner{};
    [[= storm::primary_part]] int Outer{};
    int                           On{};
};

// Declared with mixed case and no reserved words: the shape every pre-#828 database has.
struct LegacyTag {
    [[= storm::primary_assigned]] int TagId{};
    int                               Weight{};
};

struct LegacyOwner {
    [[= storm::primary_assigned]] int                  OwnerId{};
    [[= storm::indexed]] int                           Rank{};
    [[= storm::many_to_many<>]] std::vector<LegacyTag> Tags;
};

struct LegacyItem {
    [[= storm::primary_assigned]] int ItemId{};
    [[= storm::fk<>]] LegacyOwner     Owner;
};

namespace fields {
    struct GroupT;
    struct OrderT;
    struct CheckT;
    struct JoinT;
    struct LegacyOwnerT;
    struct LegacyItemT;
    consteval {
        std::meta::define_aggregate(^^GroupT, storm::field_specs_for(^^Group));
        std::meta::define_aggregate(^^OrderT, storm::field_specs_for(^^Order));
        std::meta::define_aggregate(^^CheckT, storm::field_specs_for(^^Check));
        std::meta::define_aggregate(^^JoinT, storm::field_specs_for(^^Join));
        std::meta::define_aggregate(^^LegacyOwnerT, storm::field_specs_for(^^LegacyOwner));
        std::meta::define_aggregate(^^LegacyItemT, storm::field_specs_for(^^LegacyItem));
    }
    inline constexpr GroupT       Group{};
    inline constexpr OrderT       Order{};
    inline constexpr CheckT       Check{};
    inline constexpr JoinT        Join{};
    inline constexpr LegacyOwnerT LegacyOwner{};
    inline constexpr LegacyItemT  LegacyItem{};
} // namespace fields

namespace {

    using GroupRow = std::tuple<int, int, int, int, std::string, int>;

    auto as_tuple(const Group& row) -> GroupRow {
        return {row.Select, row.desc, row.Union, row.ASC, row.Where, row.order};
    }

    auto keys_of(const auto& rows) -> std::vector<int> {
        std::vector<int> keys;
        for (const auto& row : rows) {
            keys.emplace_back(row.Select);
        }
        return keys;
    }

} // namespace

// Seeds three Group rows, one Order row per group, and two junction rows for Order 10.
template <typename ConnType>
class ReservedWordIdentifierTest : public StormTestFixture<Group, ConnType, Order, Check, Join> {
  public:
    auto on_after_setup(const std::shared_ptr<ConnType>& conn) -> void override {
        const std::vector<Group> groups{
                {.Select = 1, .desc = 2, .Union = 3, .ASC = 4, .Where = "a", .order = 5},
                {.Select = 2, .desc = 2, .Union = 30, .ASC = 40, .Where = "b", .order = 50},
                {.Select = 4, .desc = 3, .Union = 300, .ASC = 400, .Where = "c", .order = 500},
        };
        auto inserted_groups = QuerySet<Group, ConnType>().insert(groups).execute();
        ASSERT_TRUE(inserted_groups.has_value()) << inserted_groups.error().message();

        const std::vector<Order> orders{
                {.Limit = 10, .From = 7, .Table = {.Select = 1}},
                {.Limit = 20, .From = 7, .Table = {.Select = 1}},
                {.Limit = 40, .From = 8, .Table = {.Select = 4}},
        };
        auto inserted_orders = QuerySet<Order, ConnType>().insert(orders).execute();
        ASSERT_TRUE(inserted_orders.has_value()) << inserted_orders.error().message();

        // The auto-junction is Storm's own table: "<owner>_<related>", one "<side>_id" per side.
        auto linked = conn->execute(R"(INSERT INTO "order_group" ("order_id", "group_id") VALUES (10, 1), (10, 2))");
        ASSERT_TRUE(linked.has_value()) << linked.error().message();
    }
};

TYPED_TEST_SUITE(ReservedWordIdentifierTest, DatabaseTypes);

TYPED_TEST(ReservedWordIdentifierTest, BatchInsertedRowsSelectBack) {
    auto result = QuerySet<Group, TypeParam>().template order_by<fields::Group.Select>().select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::vector<GroupRow> rows;
    for (const auto& row : result.value()) {
        rows.emplace_back(as_tuple(row));
    }
    EXPECT_EQ(rows, (std::vector<GroupRow>{{1, 2, 3, 4, "a", 5}, {2, 2, 30, 40, "b", 50}, {4, 3, 300, 400, "c", 500}}));
}

TYPED_TEST(ReservedWordIdentifierTest, SingleRowInsertSelectsBack) {
    QuerySet<Group, TypeParam> qs;
    auto                       inserted =
            qs.insert(Group{.Select = 7, .desc = 8, .Union = 9, .ASC = 10, .Where = "z", .order = 11}).execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    auto result = qs.where(fields::Group.Select == 7).select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1);
    EXPECT_EQ(as_tuple(*result.value().begin()), (GroupRow{7, 8, 9, 10, "z", 11}));
}

TYPED_TEST(ReservedWordIdentifierTest, WhereOnReservedColumns) {
    QuerySet<Group, TypeParam> qs;

    auto compared = qs.where(fields::Group.desc == 2 && fields::Group.Union >= 30).select().execute();
    ASSERT_TRUE(compared.has_value()) << compared.error().message();
    EXPECT_EQ(keys_of(compared.value()), (std::vector<int>{2}));

    auto in_list = qs.where(fields::Group.ASC.in(4, 400)).template order_by<fields::Group.Select>().select().execute();
    ASSERT_TRUE(in_list.has_value()) << in_list.error().message();
    EXPECT_EQ(keys_of(in_list.value()), (std::vector<int>{1, 4}));

    auto between = qs.where(fields::Group.order.between(10, 100)).select().execute();
    ASSERT_TRUE(between.has_value()) << between.error().message();
    EXPECT_EQ(keys_of(between.value()), (std::vector<int>{2}));

    auto like = qs.where(fields::Group.Where.like("c%")).select().execute();
    ASSERT_TRUE(like.has_value()) << like.error().message();
    EXPECT_EQ(keys_of(like.value()), (std::vector<int>{4}));

    // COLLATE BINARY/NOCASE/RTRIM are SQLite collation names; PostgreSQL has none of them.
    if constexpr (!storm::test::is_postgresql<TypeParam>()) {
        auto collated = qs.where(fields::Group.Where.collate(storm::Collate::Binary) == "b").select().execute();
        ASSERT_TRUE(collated.has_value()) << collated.error().message();
        EXPECT_EQ(keys_of(collated.value()), (std::vector<int>{2}));
    }
}

TYPED_TEST(ReservedWordIdentifierTest, OrderByLimitOffsetOnReservedColumn) {
    auto result = QuerySet<Group, TypeParam>()
                          .template order_by<fields::Group.Union, false>()
                          .limit(2)
                          .offset(1)
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(keys_of(result.value()), (std::vector<int>{2, 1}));
}

TYPED_TEST(ReservedWordIdentifierTest, ProjectionAndDistinctOfReservedColumns) {
    QuerySet<Group, TypeParam> qs;

    auto projected = qs.template order_by<fields::Group.Select>()
                             .template values<fields::Group.desc, fields::Group.Union>()
                             .execute();
    ASSERT_TRUE(projected.has_value()) << projected.error().message();
    const std::vector<std::tuple<int, int>> rows(projected.value().begin(), projected.value().end());
    EXPECT_EQ(rows, (std::vector<std::tuple<int, int>>{{2, 3}, {2, 30}, {3, 300}}));

    auto distinct = qs.template distinct<fields::Group.desc>().execute();
    ASSERT_TRUE(distinct.has_value()) << distinct.error().message();
    std::vector<int> values(distinct.value().begin(), distinct.value().end());
    std::ranges::sort(values);
    EXPECT_EQ(values, (std::vector<int>{2, 3}));
}

TYPED_TEST(ReservedWordIdentifierTest, AggregatesOverReservedColumns) {
    QuerySet<Group, TypeParam> qs;

    auto count = qs.where(fields::Group.desc == 2).count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 2);

    auto sum = qs.template sum<fields::Group.Union>().execute();
    ASSERT_TRUE(sum.has_value()) << sum.error().message();
    EXPECT_EQ(sum.value(), 333);

    auto max = qs.template max<fields::Group.ASC>().execute();
    ASSERT_TRUE(max.has_value()) << max.error().message();
    ASSERT_TRUE(max.value().has_value());
    EXPECT_DOUBLE_EQ(*max.value(), 400.0);

    auto grouped = qs.template group_by<fields::Group.desc>().having(fields::Group.desc > 2).count().execute();
    ASSERT_TRUE(grouped.has_value()) << grouped.error().message();
    EXPECT_EQ(grouped.value().size(), 1);
}

TYPED_TEST(ReservedWordIdentifierTest, UpdateByObjectAndByFilter) {
    QuerySet<Group, TypeParam> qs;

    auto by_object =
            qs.update(Group{.Select = 1, .desc = 20, .Union = 3, .ASC = 4, .Where = "a2", .order = 5}).execute();
    ASSERT_TRUE(by_object.has_value()) << by_object.error().message();

    auto by_filter =
            qs.where(fields::Group.desc == 3).template update<fields::Group.Union>(Group{.Union = 99}).execute();
    ASSERT_TRUE(by_filter.has_value()) << by_filter.error().message();

    auto result = qs.template order_by<fields::Group.Select>().select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    std::vector<GroupRow> rows;
    for (const auto& row : result.value()) {
        rows.emplace_back(as_tuple(row));
    }
    EXPECT_EQ(
            rows, (std::vector<GroupRow>{{1, 20, 3, 4, "a2", 5}, {2, 2, 30, 40, "b", 50}, {4, 3, 99, 400, "c", 500}})
    );
}

TYPED_TEST(ReservedWordIdentifierTest, UpsertOnReservedConflictTarget) {
    QuerySet<Group, TypeParam> qs;

    // "b" already exists on row 2: the conflict rewrites that row's Union instead of inserting.
    auto upserted = qs.insert(Group{.Select = 9, .desc = 9, .Union = 77, .ASC = 9, .Where = "b", .order = 9})
                            .template on_conflict<fields::Group.Where>()
                            .template update<fields::Group.Union>()
                            .execute();
    ASSERT_TRUE(upserted.has_value()) << upserted.error().message();

    auto ignored = qs.insert(Group{.Select = 9, .desc = 9, .Union = 1, .ASC = 9, .Where = "b", .order = 9})
                           .template on_conflict<fields::Group.Where>()
                           .nothing()
                           .execute();
    ASSERT_TRUE(ignored.has_value()) << ignored.error().message();

    auto result = qs.where(fields::Group.Where == "b").select().execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result.value().size(), 1);
    EXPECT_EQ(as_tuple(*result.value().begin()), (GroupRow{2, 2, 77, 40, "b", 50}));
}

TYPED_TEST(ReservedWordIdentifierTest, EraseByFilterAndWipe) {
    QuerySet<Order, TypeParam> orders;

    auto erased = orders.where(fields::Order.From == 7).erase().execute();
    ASSERT_TRUE(erased.has_value()) << erased.error().message();

    auto remaining = orders.select().execute();
    ASSERT_TRUE(remaining.has_value()) << remaining.error().message();
    ASSERT_EQ(remaining.value().size(), 1);
    EXPECT_EQ(remaining.value().begin()->Limit, 40);

    auto wiped = orders.erase_all().execute();
    ASSERT_TRUE(wiped.has_value()) << wiped.error().message();

    auto count = orders.count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 0);
}

// FK join: the ON clause names the reserved target table, its reserved PK, and the "table_id"
// FK column; the WHERE names a reserved column of the base table.
TYPED_TEST(ReservedWordIdentifierTest, ForeignKeyJoinBetweenReservedTables) {
    auto result = QuerySet<Order, TypeParam>()
                          .template join<fields::Order.Table>()
                          .where(fields::Order.From == 7)
                          .template order_by<fields::Order.Limit>()
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::vector<std::tuple<int, GroupRow>> rows;
    for (const auto& order : result.value()) {
        rows.emplace_back(order.Limit, as_tuple(order.Table));
    }
    EXPECT_EQ(rows, (std::vector<std::tuple<int, GroupRow>>{{10, {1, 2, 3, 4, "a", 5}}, {20, {1, 2, 3, 4, "a", 5}}}));
}

// Many-to-many: the auto-junction table and its columns are composed from two reserved table
// names.
TYPED_TEST(ReservedWordIdentifierTest, ManyToManyAutoJunctionBetweenReservedTables) {
    auto result = QuerySet<Order, TypeParam>()
                          .template left_join<fields::Order.All>()
                          .template order_by<fields::Order.Limit>()
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::vector<std::tuple<int, std::vector<int>>> rows;
    for (const auto& order : result.value()) {
        auto linked = keys_of(order.All);
        std::ranges::sort(linked);
        rows.emplace_back(order.Limit, linked);
    }
    EXPECT_EQ(rows, (std::vector<std::tuple<int, std::vector<int>>>{{10, {1, 2}}, {20, {}}, {40, {}}}));

    auto counted = QuerySet<Order, TypeParam>().template join<fields::Order.All>().count().execute();
    ASSERT_TRUE(counted.has_value()) << counted.error().message();
    EXPECT_EQ(counted.value(), 2);
}

// Reverse FK: Q2 reads the owning table directly, keyed on its "table_id" column.
TYPED_TEST(ReservedWordIdentifierTest, ReverseForeignKeyBetweenReservedTables) {
    auto result = QuerySet<Group, TypeParam>()
                          .template left_join<fields::Group.Having>()
                          .template order_by<fields::Group.Select>()
                          .select()
                          .execute();
    ASSERT_TRUE(result.has_value()) << result.error().message();

    std::vector<std::tuple<int, std::size_t>> rows;
    for (const auto& group : result.value()) {
        rows.emplace_back(group.Select, group.Having.size());
    }
    EXPECT_EQ(rows, (std::vector<std::tuple<int, std::size_t>>{{1, 2}, {2, 0}, {4, 1}}));
}

// A DB-generated key named after a reserved word: INSERT ... RETURNING "default".
TYPED_TEST(ReservedWordIdentifierTest, ReturningAReservedGeneratedKey) {
    QuerySet<Check, TypeParam> qs;

    auto first = qs.insert(Check{.Values = 5}).execute();
    ASSERT_TRUE(first.has_value()) << first.error().message();
    EXPECT_GT(first.value(), 0);

    const std::vector<Check> batch{{.Values = 6}, {.Values = 7}};
    auto                     inserted = qs.insert(batch).execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    auto stored = qs.where(fields::Check.Default == static_cast<int>(first.value())).select().execute();
    ASSERT_TRUE(stored.has_value()) << stored.error().message();
    ASSERT_EQ(stored.value().size(), 1);
    EXPECT_EQ(stored.value().begin()->Values, 5);

    auto erased = qs.erase(*stored.value().begin()).execute();
    ASSERT_TRUE(erased.has_value()) << erased.error().message();

    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 2);
}

// Composite key with reserved parts: every by-key statement names both parts.
TYPED_TEST(ReservedWordIdentifierTest, CompositeKeyWithReservedParts) {
    QuerySet<Join, TypeParam> qs;

    const std::vector<Join> rows{
            {.Inner = 1, .Outer = 1, .On = 10}, {.Inner = 1, .Outer = 2, .On = 20}, {.Inner = 2, .Outer = 1, .On = 30}
    };
    auto inserted = qs.insert(rows).execute();
    ASSERT_TRUE(inserted.has_value()) << inserted.error().message();

    auto updated = qs.update(Join{.Inner = 1, .Outer = 2, .On = 99}).execute();
    ASSERT_TRUE(updated.has_value()) << updated.error().message();

    auto erased_one = qs.erase(Join{.Inner = 2, .Outer = 1}).execute();
    ASSERT_TRUE(erased_one.has_value()) << erased_one.error().message();

    auto remaining = qs.template order_by<fields::Join.Outer>().select().execute();
    ASSERT_TRUE(remaining.has_value()) << remaining.error().message();
    std::vector<std::tuple<int, int, int>> seen;
    for (const auto& row : remaining.value()) {
        seen.emplace_back(row.Inner, row.Outer, row.On);
    }
    EXPECT_EQ(seen, (std::vector<std::tuple<int, int, int>>{{1, 1, 10}, {1, 2, 99}}));

    const std::vector<Join> rest{{.Inner = 1, .Outer = 1}, {.Inner = 1, .Outer = 2}};
    auto                    erased_rest = qs.erase(std::span<const Join>(rest)).execute();
    ASSERT_TRUE(erased_rest.has_value()) << erased_rest.error().message();

    auto count = qs.count().execute();
    ASSERT_TRUE(count.has_value()) << count.error().message();
    EXPECT_EQ(count.value(), 0);
}

// max_length on a reserved column is a CHECK naming that column on SQLite and a VARCHAR on
// PostgreSQL; both must reject an over-long value.
TYPED_TEST(ReservedWordIdentifierTest, MaxLengthOnReservedColumnIsEnforced) {
    auto too_long =
            QuerySet<Group, TypeParam>().insert(Group{.Select = 8, .Where = "longer than ten characters"}).execute();
    EXPECT_FALSE(too_long.has_value());
}

namespace {

    template <typename ConnType> constexpr auto dialect_of() -> storm::orm::schema::Dialect {
        using storm::orm::schema::Dialect;
        return storm::test::is_postgresql<ConnType>() ? Dialect::PostgreSQL : Dialect::SQLite;
    }

    // The DDL a pre-#828 Storm emitted, as far as the database can tell: no quotes, and a
    // spelling that is not lowercase. Uppercasing the whole statement is the harshest case —
    // PostgreSQL folds it back to lowercase, SQLite stores it uppercase.
    auto as_legacy_ddl(std::string_view sql) -> std::string {
        std::string legacy;
        for (const char c : sql) {
            if (c != '"') {
                legacy.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
        }
        return legacy;
    }

    template <typename Model, typename ConnType> auto legacy_table_ddl() -> std::string {
        if constexpr (storm::test::is_postgresql<ConnType>()) {
            return as_legacy_ddl(storm::create_table_sql<Model, storm::orm::schema::Dialect::PostgreSQL>());
        } else {
            return as_legacy_ddl(storm::create_table_sql<Model>());
        }
    }

} // namespace

// Tables, junction and index created by unquoted DDL (as every database made before #828 has
// them) keep working: Storm's quoted lowercase names resolve to them on both backends.
template <typename ConnType> class LegacySchemaIdentifierTest : public StormTestFixture<LegacyTag, ConnType> {
  public:
    auto on_setup(const std::shared_ptr<ConnType>& conn) -> void override {
        using Schema = storm::orm::schema::SchemaStatement<LegacyOwner>;
        std::vector<std::string> ddl{
                legacy_table_ddl<LegacyTag, ConnType>(),
                legacy_table_ddl<LegacyOwner, ConnType>(),
                legacy_table_ddl<LegacyItem, ConnType>(),
        };
        for (const auto& junction : Schema::template junction_table_sqls<dialect_of<ConnType>()>()) {
            ddl.emplace_back(as_legacy_ddl(junction));
        }
        for (const auto& index : storm::create_index_sql<LegacyOwner>()) {
            ddl.emplace_back(as_legacy_ddl(index));
        }
        for (const auto& statement : ddl) {
            auto created = conn->execute(statement);
            ASSERT_TRUE(created.has_value()) << statement << "\n" << created.error().message();
        }
        // The current schema generator must find every one of them already there.
        ASSERT_TRUE((storm::test::ensure_tables<ConnType, LegacyTag, LegacyOwner, LegacyItem>(conn)));
    }
};

TYPED_TEST_SUITE(LegacySchemaIdentifierTest, DatabaseTypes);

TYPED_TEST(LegacySchemaIdentifierTest, StatementsReachTablesCreatedUnquoted) {
    auto conn = storm::db::DefaultConnection<TypeParam>::get();

    auto tag = QuerySet<LegacyTag, TypeParam>().insert(LegacyTag{.TagId = 1, .Weight = 3}).execute();
    ASSERT_TRUE(tag.has_value()) << tag.error().message();
    auto owner = QuerySet<LegacyOwner, TypeParam>().insert(LegacyOwner{.OwnerId = 5, .Rank = 2}).execute();
    ASSERT_TRUE(owner.has_value()) << owner.error().message();
    auto item = QuerySet<LegacyItem, TypeParam>().insert(LegacyItem{.ItemId = 9, .Owner = {.OwnerId = 5}}).execute();
    ASSERT_TRUE(item.has_value()) << item.error().message();
    auto linked = conn->execute("INSERT INTO LegacyOwner_LegacyTag (LegacyOwner_id, LegacyTag_id) VALUES (5, 1)");
    ASSERT_TRUE(linked.has_value()) << linked.error().message();

    auto joined = QuerySet<LegacyItem, TypeParam>()
                          .template join<fields::LegacyItem.Owner>()
                          .where(fields::LegacyItem.ItemId == 9)
                          .select()
                          .execute();
    ASSERT_TRUE(joined.has_value()) << joined.error().message();
    ASSERT_EQ(joined.value().size(), 1);
    EXPECT_EQ(joined.value().begin()->Owner.Rank, 2);

    auto owners = QuerySet<LegacyOwner, TypeParam>()
                          .template join<fields::LegacyOwner.Tags>()
                          .where(fields::LegacyOwner.Rank == 2)
                          .select()
                          .execute();
    ASSERT_TRUE(owners.has_value()) << owners.error().message();
    ASSERT_EQ(owners.value().size(), 1);
    ASSERT_EQ(owners.value().begin()->Tags.size(), 1);
    EXPECT_EQ(owners.value().begin()->Tags.begin()->Weight, 3);
}

// The emitted text itself: every identifier quoted and lowercased, on both dialects.
TEST(ReservedWordIdentifierSql, CreateTableQuotesEveryIdentifier) {
    using storm::orm::schema::Dialect;

    const std::string& sqlite = storm::create_table_sql<Order>();
    EXPECT_TRUE(sqlite.starts_with(R"(CREATE TABLE "order" ()")) << sqlite;
    EXPECT_TRUE(sqlite.contains(R"("limit" INTEGER PRIMARY KEY)")) << sqlite;
    EXPECT_TRUE(sqlite.contains(R"("from" INTEGER NOT NULL)")) << sqlite;
    EXPECT_TRUE(sqlite.contains(R"("table_id" INTEGER NOT NULL REFERENCES "group"("select"))")) << sqlite;

    const std::string& pg = storm::create_table_sql<Group, Dialect::PostgreSQL>();
    EXPECT_TRUE(pg.starts_with(R"(CREATE TABLE "group" ()")) << pg;
    EXPECT_TRUE(pg.contains(R"("select" BIGINT PRIMARY KEY)")) << pg;
    EXPECT_TRUE(pg.contains(R"("desc" INTEGER NOT NULL)")) << pg;
    EXPECT_TRUE(pg.contains(R"("union" INTEGER NOT NULL)")) << pg;
    EXPECT_TRUE(pg.contains(R"("asc" INTEGER NOT NULL)")) << pg;
    EXPECT_FALSE(pg.contains("having")) << pg; // a relation container is not a column
    EXPECT_TRUE(pg.contains(R"("where" VARCHAR(10) NOT NULL UNIQUE)")) << pg;

    const std::string& checked = storm::create_table_sql<Group>();
    EXPECT_TRUE(checked.contains(R"(CHECK(length("where") <= 10))")) << checked;

    const std::string& composite = storm::create_table_sql<Join>();
    EXPECT_TRUE(composite.starts_with(R"(CREATE TABLE "join" ()")) << composite;
    EXPECT_TRUE(composite.contains(R"(PRIMARY KEY ("inner", "outer"))")) << composite;
}

// A composed name is quoted ONCE, as a whole: "idx_<table>_<field>", "<owner>_<related>".
TEST(ReservedWordIdentifierSql, ComposedNamesAreQuotedAsOneIdentifier) {
    using storm::orm::schema::Dialect;
    using storm::orm::schema::SchemaStatement;

    const auto& indexes = storm::create_index_sql<Group>();
    const auto  has     = [&indexes](std::string_view sql) { return std::ranges::find(indexes, sql) != indexes.end(); };
    EXPECT_TRUE(has(R"(CREATE UNIQUE INDEX IF NOT EXISTS "idx_group_where" ON "group"("where"))"));
    EXPECT_TRUE(has(R"(CREATE INDEX IF NOT EXISTS "idx_group_order" ON "group"("order"))"));
    EXPECT_TRUE(has(R"(CREATE INDEX IF NOT EXISTS "idx_group_desc_union" ON "group"("desc", "union"))"));

    const auto& order_indexes = storm::create_index_sql<Order>();
    EXPECT_NE(
            std::ranges::find(
                    order_indexes, R"(CREATE INDEX IF NOT EXISTS "idx_order_table_id" ON "order"("table_id"))"
            ),
            order_indexes.end()
    );

    const std::string& junction = SchemaStatement<Order>::template junction_table_sql<Dialect::SQLite>();
    EXPECT_TRUE(junction.starts_with(R"(CREATE TABLE "order_group" ()")) << junction;
    EXPECT_TRUE(junction.contains(R"(PRIMARY KEY ("order_id", "group_id"))")) << junction;
    EXPECT_TRUE(junction.contains(R"(FOREIGN KEY ("order_id") REFERENCES "order"("limit") ON DELETE CASCADE)"))
            << junction;
    EXPECT_TRUE(junction.contains(R"(FOREIGN KEY ("group_id") REFERENCES "group"("select") ON DELETE CASCADE)"))
            << junction;
}

// NOLINTEND(misc-const-correctness)
