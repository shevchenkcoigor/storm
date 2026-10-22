#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"
#include "plf_hive/plf_hive.h"

// NOLINTBEGIN(misc-const-correctness)

import storm_defaults;
import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;
import storm_orm_fields;
import std;

#include "../../shared/models/message.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"
#include "test_fixture.h"
using storm::create_table_sql;
using storm::field_specs_for;
using storm::many_to_many;
using storm::primary;
using storm::primary_autoincrement;
using storm::db::DefaultConnection;
using storm::meta::selector_info;
using storm::orm::schema::SchemaStatement;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;
using storm::orm::statements::UpdateStatement;

// ============================================================================
// PK column name must come from the member's identifier, not a hardcoded "id"
// literal (#506). Every DML path (base.cppm's pk_name_) already reads the real
// identifier; the schema generator's CREATE TABLE branch — and the FK REFERENCES
// clause naming the TARGET model's PK — did not, so a model whose PK member is
// not called `id` got DDL and queries that disagreed ("no such column").
// ============================================================================
using storm::orm::schema::Dialect;

namespace {

    // Single-PK model whose PK member is NOT named "id".
    struct Widget {
        [[= primary]] int widget_id{};
        std::string       name;
    };

    // Single-PK model using primary_autoincrement with a non-"id" PK name — the
    // AUTOINCREMENT branch must also use the real identifier.
    struct Gadget {
        [[= primary_autoincrement]] int gadget_id{};
        std::string                     name;
    };

    // FK TARGET whose PK member is not named "id". The REFERENCING model's FK
    // column must emit "REFERENCES Owner(owner_id)", not "REFERENCES Owner(id)".
    struct Owner {
        [[= primary]] int owner_id{};
        std::string       label;
    };

    struct Item {
        [[= primary]] int       id{};
        [[= storm::fk<>]] Owner owner;
        std::string             description;
    };

    // A deliberately long PK member name — proves the consteval size calculations
    // (column_size_budget's pk_size, col_def_buffer, calculate_column_defs_size)
    // were widened for a variable-length PK identifier rather than the fixed
    // literal "id", so this must build (and render untruncated) rather than
    // overflow the ConstexprString buffer at compile time.
    inline constexpr std::string_view kLongPkName =
            "this_is_a_deliberately_very_long_primary_key_identifier_that_exceeds_one_hundred_characters_long";

    struct LongPkNameRecord {
        [[= primary]] int
                    this_is_a_deliberately_very_long_primary_key_identifier_that_exceeds_one_hundred_characters_long{};
        std::string name;
    };

    // A deliberately long PK name on an FK TARGET — proves fk_references_len /
    // max_fk_references_len were widened for a variable-length target PK
    // identifier too (not just the referencing side).
    struct LongPkTarget {
        [[= primary]] int
                    this_is_a_deliberately_very_long_primary_key_identifier_that_exceeds_one_hundred_characters_long{};
        std::string label;
    };

    struct RefersToLongPkTarget {
        [[= primary]] int              id{};
        [[= storm::fk<>]] LongPkTarget target;
    };

    // ── m2m auto-junction, non-"id" PKs on BOTH sides (#519) ────────────────
    // The junction's own columns are <Model>_id by construction, but the
    // target-side "REFERENCES <Model>(...)" must name each model's REAL PK
    // member — the same failure class as #506, in the junction DDL path.
    struct Badge {
        [[= primary]] int badge_id{};
        std::string       label;
    };

    struct Hero {
        [[= primary]] int                       hero_id{};
        std::string                             name;
        [[= many_to_many<>]] std::vector<Badge> badges;
    };

    // Mixed shape: owner PK is "id", related PK is not — proves the two sides
    // are resolved independently rather than from one shared lookup.
    struct Sticker {
        [[= primary]] int sticker_id{};
        std::string       label;
    };

    struct Notebook {
        [[= primary]] int                         id{};
        std::string                               title;
        [[= many_to_many<>]] std::vector<Sticker> stickers;
    };

    // Both sides PK-named "id" — the byte-identical regression guard.
    struct Tag {
        [[= primary]] int id{};
        std::string       label;
    };

    struct Post {
        [[= primary]] int                     id{};
        std::string                           title;
        [[= many_to_many<>]] std::vector<Tag> tags;
    };

    // Long PK names on both junction sides — proves build_junction_sql's
    // consteval buffer folds in the two PK identifier lengths instead of
    // budgeting for the fixed 2-char literal "id".
    struct LongPkRelated {
        [[= primary]] int
                    this_is_a_deliberately_very_long_primary_key_identifier_that_exceeds_one_hundred_characters_long{};
        std::string label;
    };

    struct LongPkOwner {
        [[= primary]] int
                this_is_a_deliberately_very_long_primary_key_identifier_that_exceeds_one_hundred_characters_long{};
        [[= many_to_many<>]] std::vector<LongPkRelated> links;
    };

    template <typename Model, Dialect D> auto junction_sql() -> const std::string& {
        return SchemaStatement<Model>::template junction_table_sql<D>();
    }

} // namespace

// fields:: selector proxies (#518). At global scope, NOT nested inside the
// anonymous namespace above: a nested `namespace fields` makes every
// unqualified `fields::` reference in this TU ambiguous against the global one.
namespace fields {

    struct WidgetT;
    consteval {
        std::meta::define_aggregate(^^WidgetT, field_specs_for(^^Widget));
    }
    inline constexpr WidgetT Widget{};

    struct ItemT;
    consteval {
        std::meta::define_aggregate(^^ItemT, field_specs_for(^^Item));
    }
    inline constexpr ItemT Item{};

    struct HeroT;
    consteval {
        std::meta::define_aggregate(^^HeroT, field_specs_for(^^Hero));
    }
    inline constexpr HeroT Hero{};
} // namespace fields

// ============================================================================
// DDL — PK column name (SQLite + PostgreSQL)
// ============================================================================

TEST(PkColumnNameTest, NonIdPkNameEmittedSqlite) {
    const std::string& sql = create_table_sql<Widget>();
    EXPECT_NE(sql.find("widget_id INTEGER PRIMARY KEY"), std::string::npos)
            << "Expected PK column named 'widget_id' in: " << sql;
    EXPECT_EQ(sql.find("    id "), std::string::npos) << "Must not fall back to the literal 'id' in: " << sql;
}

TEST(PkColumnNameTest, NonIdPkNameEmittedPostgres) {
    const std::string& sql = create_table_sql<Widget, Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("widget_id BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY"), std::string::npos)
            << "Expected PK column named 'widget_id' in: " << sql;
}

TEST(PkColumnNameTest, NonIdPkNameWithAutoincrementSqlite) {
    const std::string& sql = create_table_sql<Gadget>();
    EXPECT_NE(sql.find("gadget_id INTEGER PRIMARY KEY AUTOINCREMENT"), std::string::npos)
            << "Expected AUTOINCREMENT PK column named 'gadget_id' in: " << sql;
}

// Regression guard: Person's PK is named "id" — DDL must stay byte-identical.
TEST(PkColumnNameTest, IdNamedPkUnchanged) {
    const std::string& sql = create_table_sql<Person>();
    EXPECT_NE(sql.find("    id INTEGER PRIMARY KEY,\n"), std::string::npos)
            << "Person's id-named PK DDL must be byte-identical: " << sql;
}

// A long PK name must not overflow the consteval size calculations (widened for
// a variable-length PK identifier, not the fixed literal "id") — proves the
// buffer, not just the column, and that it renders untruncated rather than
// std::unreachable()-ing at compile time (which would fail the build).
TEST(PkColumnNameTest, LongPkNameNotTruncatedSqlite) {
    const std::string& sql = create_table_sql<LongPkNameRecord>();
    EXPECT_NE(sql.find(std::string(kLongPkName) + " INTEGER PRIMARY KEY"), std::string::npos)
            << "Long PK name was truncated in:\n"
            << sql;
}

TEST(PkColumnNameTest, LongPkNameNotTruncatedPostgres) {
    const std::string& sql = create_table_sql<LongPkNameRecord, Dialect::PostgreSQL>();
    EXPECT_NE(
            sql.find(std::string(kLongPkName) + " BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY"),
            std::string::npos
    ) << "Long PK name was truncated in:\n"
      << sql;
}

// ============================================================================
// DDL — FK REFERENCES clause names the target's real PK (SQLite + PostgreSQL)
// ============================================================================

TEST(PkColumnNameTest, FkReferencesTargetNonIdPkSqlite) {
    const std::string& sql = create_table_sql<Item>();
    EXPECT_NE(sql.find("owner_id INTEGER NOT NULL REFERENCES Owner(owner_id)"), std::string::npos)
            << "Expected REFERENCES Owner(owner_id) in: " << sql;
}

TEST(PkColumnNameTest, FkReferencesTargetNonIdPkPostgres) {
    const std::string& sql = create_table_sql<Item, Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("owner_id BIGINT NOT NULL REFERENCES Owner(owner_id)"), std::string::npos)
            << "Expected REFERENCES Owner(owner_id) in: " << sql;
}

// Regression guard: Message REFERENCES Person(id) — target's PK is "id" — must
// stay byte-identical.
TEST(PkColumnNameTest, FkReferencesIdNamedTargetUnchanged) {
    const std::string& sql = create_table_sql<Message>();
    EXPECT_NE(sql.find("sender_id INTEGER NOT NULL REFERENCES Person(id)"), std::string::npos)
            << "Expected REFERENCES Person(id) unchanged in: " << sql;
}

// A long PK name on the FK TARGET must not overflow fk_references_len /
// max_fk_references_len (widened for a variable-length target PK identifier).
TEST(PkColumnNameTest, FkReferencesLongTargetPkNotTruncatedSqlite) {
    const std::string& sql = create_table_sql<RefersToLongPkTarget>();
    EXPECT_NE(sql.find("REFERENCES LongPkTarget(" + std::string(kLongPkName) + ")"), std::string::npos)
            << "Long target PK name was truncated in:\n"
            << sql;
}

TEST(PkColumnNameTest, FkReferencesLongTargetPkNotTruncatedPostgres) {
    const std::string& sql = create_table_sql<RefersToLongPkTarget, Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("REFERENCES LongPkTarget(" + std::string(kLongPkName) + ")"), std::string::npos)
            << "Long target PK name was truncated in:\n"
            << sql;
}

// ============================================================================
// CRUD round-trip — non-"id" PK model (TYPED_TEST: SQLite + PostgreSQL)
//
// Proves DDL and DML agree: the generated column name and the WHERE/UPDATE/
// DELETE clause name (base.cppm's pk_name_, already correct pre-#506) refer to
// the same column.
// ============================================================================

template <typename ConnType> class NonIdPkCrudTest : public StormTestFixture<Widget, ConnType> {};

TYPED_TEST_SUITE(NonIdPkCrudTest, DatabaseTypes);

TYPED_TEST(NonIdPkCrudTest, InsertSelectUpdateDeleteRoundTrip) {
    auto inserted = (InsertStatement<Widget, TypeParam>{}).query(Widget{.widget_id = 0, .name = "Sprocket"}).execute();
    ASSERT_TRUE(inserted.has_value()) << "Insert failed: "
                                      << (inserted.has_value() ? std::string{} : inserted.error().message());
    const auto new_id = static_cast<int>(inserted.value());

    auto selected = (SelectStatement<Widget, TypeParam>{}).where(fields::Widget.widget_id == new_id).execute();
    ASSERT_TRUE(selected.has_value()) << "Select by widget_id failed: "
                                      << (selected.has_value() ? std::string{} : selected.error().message());
    ASSERT_EQ(selected->size(), 1U);
    EXPECT_EQ(selected->begin()->name, "Sprocket");

    auto updated = (UpdateStatement<Widget, TypeParam>{})
                           .template query_where<selector_info<fields::Widget.name>()>(
                                   Widget{.name = "Cog"}, fields::Widget.widget_id == new_id
                           )
                           .execute();
    ASSERT_TRUE(updated.has_value()) << "Update by widget_id failed: "
                                     << (updated.has_value() ? std::string{} : updated.error().message());

    auto reselected = (SelectStatement<Widget, TypeParam>{}).where(fields::Widget.widget_id == new_id).execute();
    ASSERT_TRUE(reselected.has_value());
    ASSERT_EQ(reselected->size(), 1U);
    EXPECT_EQ(reselected->begin()->name, "Cog");

    auto deleted = (EraseStatement<Widget, TypeParam>{}).query_where(fields::Widget.widget_id == new_id).execute();
    ASSERT_TRUE(deleted.has_value()) << "Delete by widget_id failed: "
                                     << (deleted.has_value() ? std::string{} : deleted.error().message());

    auto empty = (SelectStatement<Widget, TypeParam>{}).where(fields::Widget.widget_id == new_id).execute();
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->size(), 0U);
}

// ============================================================================
// FK-target-with-non-"id"-PK — executable DDL + working insert/join (TYPED_TEST)
// ============================================================================

template <typename ConnType> class FkTargetNonIdPkTest : public StormTestFixture<Owner, ConnType, Item> {};

TYPED_TEST_SUITE(FkTargetNonIdPkTest, DatabaseTypes);

TYPED_TEST(FkTargetNonIdPkTest, InsertWithFkToNonIdPkTargetSucceeds) {
    auto owner_result = (InsertStatement<Owner, TypeParam>{}).query(Owner{.owner_id = 0, .label = "Acme"}).execute();
    ASSERT_TRUE(owner_result.has_value())
            << "Owner insert failed: " << (owner_result.has_value() ? std::string{} : owner_result.error().message());
    const auto owner_id = static_cast<int>(owner_result.value());

    auto item_result = (InsertStatement<Item, TypeParam>{})
                               .query(Item{.id = 0, .owner = Owner{.owner_id = owner_id}, .description = "Widget A"})
                               .execute();
    ASSERT_TRUE(item_result.has_value()) << "Item insert (FK to non-id PK target) failed: "
                                         << (item_result.has_value() ? std::string{} : item_result.error().message());

    auto joined = (SelectStatement<Item, TypeParam>{}).all().template join<fields::Item.owner>().execute();
    ASSERT_TRUE(joined.has_value()) << "Join on FK to non-id PK target failed: "
                                    << (joined.has_value() ? std::string{} : joined.error().message());
    ASSERT_EQ(joined->size(), 1U);
    EXPECT_EQ(joined->begin()->owner.label, "Acme");
}

// ============================================================================
// m2m auto-junction — REFERENCES clause names each side's real PK (#519)
//
// The junction's own columns stay <Model>_id (internally consistent by
// construction); only the target-side REFERENCES was hardcoded to "id".
// The junction FK DDL is dialect-independent, so each case asserts on both.
// ============================================================================

TEST(PkColumnNameTest, JunctionReferencesNonIdPkBothSides) {
    for (const std::string& sql : {junction_sql<Hero, Dialect::SQLite>(), junction_sql<Hero, Dialect::PostgreSQL>()}) {
        EXPECT_TRUE(sql.contains("FOREIGN KEY (Hero_id) REFERENCES Hero(hero_id) ON DELETE CASCADE")) << sql;
        EXPECT_TRUE(sql.contains("FOREIGN KEY (Badge_id) REFERENCES Badge(badge_id) ON DELETE CASCADE")) << sql;
        EXPECT_FALSE(sql.contains("(id)")) << "Must not fall back to the literal 'id': " << sql;
    }
}

TEST(PkColumnNameTest, JunctionReferencesResolvesEachSideIndependently) {
    for (const std::string& sql :
         {junction_sql<Notebook, Dialect::SQLite>(), junction_sql<Notebook, Dialect::PostgreSQL>()}) {
        // Owner PK is "id", related PK is "sticker_id" — neither side may borrow the other's.
        EXPECT_TRUE(sql.contains("FOREIGN KEY (Notebook_id) REFERENCES Notebook(id) ON DELETE CASCADE")) << sql;
        EXPECT_TRUE(sql.contains("FOREIGN KEY (Sticker_id) REFERENCES Sticker(sticker_id) ON DELETE CASCADE")) << sql;
    }
}

// Regression guard: Post/Tag both have "id"-named PKs — the junction DDL must
// stay byte-identical to its pre-#519 form.
TEST(PkColumnNameTest, JunctionIdNamedPksUnchanged) {
    EXPECT_EQ(
            (junction_sql<Post, Dialect::SQLite>()),
            "CREATE TABLE Post_Tag (\n"
            "    Post_id INTEGER NOT NULL,\n"
            "    Tag_id INTEGER NOT NULL,\n"
            "    PRIMARY KEY (Post_id, Tag_id),\n"
            "    FOREIGN KEY (Post_id) REFERENCES Post(id) ON DELETE CASCADE,\n"
            "    FOREIGN KEY (Tag_id) REFERENCES Tag(id) ON DELETE CASCADE\n"
            ")"
    );
}

// Long PK names on both sides must not overflow build_junction_sql's consteval
// buffer. The budget is measured by rendering append_junction_sql into ClauseSizer
// (#504), so it tracks the real identifier automatically — this pins that the
// sizer and the emitter stay the same code: ConstexprString::append truncates
// SILENTLY, so a sizer that missed the PK term would drop the tail unnoticed.
TEST(PkColumnNameTest, JunctionLongPkNamesNotTruncated) {
    const std::string expected_pk(kLongPkName);
    for (const std::string& sql :
         {junction_sql<LongPkOwner, Dialect::SQLite>(), junction_sql<LongPkOwner, Dialect::PostgreSQL>()}) {
        EXPECT_TRUE(sql.contains("REFERENCES LongPkOwner(" + expected_pk + ") ON DELETE CASCADE")) << sql;
        EXPECT_TRUE(sql.contains("REFERENCES LongPkRelated(" + expected_pk + ") ON DELETE CASCADE")) << sql;
        EXPECT_TRUE(sql.ends_with(")")) << "junction SQL truncated: " << sql;
    }
}

// ============================================================================
// m2m junction with non-"id" PKs — executable DDL end-to-end (TYPED_TEST)
//
// The DDL assertions above prove the SQL text; this proves the DB accepts it:
// with the hardcoded "id" the CREATE TABLE names a column that does not exist,
// so the junction insert (and its FK enforcement) fails at runtime.
// ============================================================================

template <typename ConnType> class M2MNonIdPkTest : public StormTestFixture<Hero, ConnType, Badge> {
  public:
    // Seeds one Hero + one Badge and links them through the junction. Returns
    // false on a fatal failure so the caller can bail (ASSERT_* cannot cross a
    // helper's return).
    static auto seed_linked_pair(const std::shared_ptr<ConnType>& conn) -> bool {
        auto hero_id = (InsertStatement<Hero, ConnType>{}).query(Hero{.hero_id = 0, .name = "Rook"}).execute();
        EXPECT_TRUE(hero_id.has_value());
        auto badge_id = (InsertStatement<Badge, ConnType>{}).query(Badge{.badge_id = 0, .label = "Gold"}).execute();
        EXPECT_TRUE(badge_id.has_value());
        if (!hero_id.has_value() || !badge_id.has_value()) {
            return false;
        }
        auto ins = conn->execute(
                std::format(
                        "INSERT INTO Hero_Badge (Hero_id, Badge_id) VALUES ({}, {})", hero_id.value(), badge_id.value()
                )
        );
        EXPECT_TRUE(ins.has_value()) << ins.error().message();
        return ins.has_value();
    }
};

TYPED_TEST_SUITE(M2MNonIdPkTest, DatabaseTypes);

TYPED_TEST(M2MNonIdPkTest, JunctionDdlIsExecutableAndEnforcesFks) {
    auto conn = DefaultConnection<TypeParam>::get();
    ASSERT_TRUE(TestFixture::seed_linked_pair(conn)) << "junction insert must succeed on executable DDL";

    // The FK really points at Hero(hero_id)/Badge(badge_id) — a dangling pair is rejected.
    auto dangling = conn->execute("INSERT INTO Hero_Badge (Hero_id, Badge_id) VALUES (99999, 99999)");
    EXPECT_FALSE(dangling.has_value()) << "Junction FK must reject a dangling owner/related key";
}

TYPED_TEST(M2MNonIdPkTest, EagerLoadThroughNonIdPkJunction) {
    auto conn = DefaultConnection<TypeParam>::get();
    ASSERT_TRUE(TestFixture::seed_linked_pair(conn));

    auto loaded = (SelectStatement<Hero, TypeParam>{}).all().template join<fields::Hero.badges>().execute();
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message();
    ASSERT_EQ(loaded->size(), 1U);
    ASSERT_EQ(loaded->begin()->badges.size(), 1U);
    EXPECT_EQ(loaded->begin()->badges.begin()->label, "Gold");
}

// NOLINTEND(misc-const-correctness)
