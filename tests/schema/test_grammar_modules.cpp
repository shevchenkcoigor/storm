#include <gtest/gtest.h>
#include <meta>

import std;
import storm_orm_field_attr;
import storm_orm_statements_insert_grammar;
import storm_orm_statements_select_grammar;
import storm_orm_statements_upsert_grammar;
// Deliberately NOT importing storm_orm_statements_insert / _select or storm: this TU proves the
// INSERT and SELECT SQL-text builders live in their own grammar modules, reachable without a
// connection type (#671).

namespace {
    using namespace storm::orm::statements;

    struct GrammarItem {
        [[= storm::meta::primary]] int        id{};
        [[= storm::meta::unique]] std::string name;
        int                                   qty{};
    };

    // Upsert-head shapes beyond a DB-generated INTEGER key: caller-assigned key (#586),
    // composite key (#502), and a composite key whose part is an FK (`<name>_id`).
    struct AssignedRow {
        [[= storm::meta::primary_assigned]] std::int64_t ext_id{};
        std::string                                      label;
    };

    struct PartRow {
        [[= storm::meta::primary_part]] int a{};
        [[= storm::meta::primary_part]] int b{};
        int                                 qty{};
    };

    struct FkPartRow {
        [[= storm::meta::primary_part]][[= storm::meta::fk<>]] GrammarItem item;
        [[= storm::meta::primary_part]] int                                sku{};
        int                                                                qty{};
    };

    using Insert = InsertGrammar<GrammarItem>;
    using Select = SelectGrammar<GrammarItem>;
} // namespace

TEST(GrammarModules, InsertGrammarExposesSingleRowText) {
    EXPECT_EQ(Insert::insert_sql_string, "INSERT INTO GrammarItem (name, qty) VALUES (?, ?)");
    EXPECT_EQ(Insert::insert_returning_sql_string, "INSERT INTO GrammarItem (name, qty) VALUES (?, ?) RETURNING id");
    EXPECT_EQ(Insert::returning_suffix, " RETURNING id");
    EXPECT_EQ(Insert::placeholders_count(), 2U);
}

TEST(GrammarModules, InsertGrammarBuildsBulkBody) {
    EXPECT_EQ(Insert::bulk_insert_prefix, "INSERT INTO GrammarItem (name, qty) VALUES ");
    EXPECT_EQ(Insert::build_bulk_insert_body(0), Insert::bulk_insert_prefix);
    EXPECT_EQ(Insert::build_bulk_insert_body(2), "INSERT INTO GrammarItem (name, qty) VALUES (?, ?), (?, ?)");
}

TEST(GrammarModules, SelectGrammarExposesSelectAndLimitText) {
    EXPECT_EQ(Select::select_sql_string, "SELECT id, name, qty FROM GrammarItem");
    EXPECT_EQ(Select::select_limit1_sql_string, "SELECT id, name, qty FROM GrammarItem LIMIT 1");
    EXPECT_EQ(Select::select_limit2_sql_string, "SELECT id, name, qty FROM GrammarItem LIMIT 2");
}

// UpsertGrammar reuses InsertGrammar's INSERT head; the composed statement must be unchanged.
TEST(GrammarModules, UpsertGrammarReusesInsertHead) {
    constexpr auto name = ^^GrammarItem::name;
    EXPECT_EQ(
            UpsertGrammar<GrammarItem>::nothing_sql<name>(),
            "INSERT INTO GrammarItem (name, qty) VALUES (?, ?) ON CONFLICT (name) DO NOTHING RETURNING id"
    );
}

// The shared head must carry every caller-supplied key column, and nothing is RETURNed (#572).
TEST(GrammarModules, UpsertHeadCoversCallerSuppliedKeyShapes) {
    EXPECT_EQ(
            UpsertGrammar<AssignedRow>::nothing_sql<^^AssignedRow::ext_id>(),
            "INSERT INTO AssignedRow (ext_id, label) VALUES (?, ?) ON CONFLICT (ext_id) DO NOTHING"
    );
    EXPECT_EQ(
            (UpsertGrammar<PartRow>::nothing_sql<^^PartRow::a, ^^PartRow::b>()),
            "INSERT INTO PartRow (a, b, qty) VALUES (?, ?, ?) ON CONFLICT (a, b) DO NOTHING"
    );
    EXPECT_EQ(
            (UpsertGrammar<FkPartRow>::nothing_sql<^^FkPartRow::item, ^^FkPartRow::sku>()),
            "INSERT INTO FkPartRow (item_id, sku, qty) VALUES (?, ?, ?) ON CONFLICT (item_id, sku) DO NOTHING"
    );
}
