#include <gtest/gtest.h>

#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

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
import std;

#include "../../shared/models/cascade_child.h" // NOSONAR cpp:S954
#include "../../shared/models/message.h"
#include "../../shared/models/person.h"
#include "../../shared/models/restrict_child.h"
#include "../../shared/models/set_null_child.h"
#include "../../shared/models/task.h"
#include "test_fixture.h"
using storm::create_table_sql;
using storm::primary;
using storm::orm::schema::Dialect;
using storm::orm::schema::SchemaStatement;
using storm::orm::statements::EraseStatement;
using storm::orm::statements::InsertStatement;
using storm::orm::statements::SelectStatement;

// ============================================================================
// Referential Integrity — FK REFERENCES (#412)
//
// Always-on: every fk<...> column emits REFERENCES <Related>(id). Base
// FKs use plain REFERENCES (= RESTRICT / NO ACTION, the SQL default). The
// related table name is the FK field's C++ type identifier; the PK is always
// "id". Both dialects.
// ============================================================================

// Test: a single FK column emits REFERENCES to its related table (SQLite).
TEST(SchemaFkRefTest, MessageFkEmitsReferencesSqlite) {
    const std::string& sql = create_table_sql<Message>();
    EXPECT_NE(sql.find("sender_id INTEGER NOT NULL REFERENCES Person(id)"), std::string::npos)
            << "Expected FK column to carry REFERENCES Person(id) in: " << sql;
}

// Test: same column emits REFERENCES in the PostgreSQL dialect.
TEST(SchemaFkRefTest, MessageFkEmitsReferencesPostgres) {
    const std::string& sql = create_table_sql<Message, Dialect::PostgreSQL>();
    EXPECT_NE(sql.find("sender_id BIGINT NOT NULL REFERENCES Person(id)"), std::string::npos)
            << "Expected FK column to carry REFERENCES Person(id) in: " << sql;
}

// Test: multiple FKs to the same table each get their own REFERENCES (SQLite).
TEST(SchemaFkRefTest, TaskBothFksEmitReferencesSqlite) {
    const std::string& sql = create_table_sql<Task>();
    EXPECT_NE(sql.find("assignee_id INTEGER NOT NULL REFERENCES Person(id)"), std::string::npos)
            << "Expected assignee FK to reference Person(id) in: " << sql;
    EXPECT_NE(sql.find("reviewer_id INTEGER NOT NULL REFERENCES Person(id)"), std::string::npos)
            << "Expected reviewer FK to reference Person(id) in: " << sql;
}

// Test: a nullable FK (optional<Related>) still emits REFERENCES, without NOT NULL.
struct NullableFkRow {
    [[= primary]] int                       id{};
    [[= storm::fk<>]] std::optional<Person> owner;
    std::string                             label;
};

TEST(SchemaFkRefTest, NullableFkEmitsReferencesWithoutNotNull) {
    const std::string& sql = create_table_sql<NullableFkRow>();
    EXPECT_NE(sql.find("owner_id INTEGER REFERENCES Person(id)"), std::string::npos)
            << "Nullable FK should be 'owner_id INTEGER REFERENCES Person(id)' (no NOT NULL) in: " << sql;
    const std::size_t owner_pos = sql.find("owner_id INTEGER");
    ASSERT_NE(owner_pos, std::string::npos);
    EXPECT_EQ(sql.substr(owner_pos, 16).find("NOT NULL"), std::string::npos)
            << "Nullable FK must not carry NOT NULL, got: " << sql.substr(owner_pos, 40);
}

// ============================================================================
// Per-FK ON DELETE policy — SQL generation (#431)
//
// fk<RefAction::...> appends the chosen ON DELETE clause to the FK's REFERENCES
// (RESTRICT = default = no clause). SET NULL requires a nullable FK. Both dialects.
// ============================================================================

TEST(SchemaFkPolicyTest, CascadeEmitsOnDeleteCascade) {
    for (std::string_view sql :
         {std::string_view(create_table_sql<CascadeChild>()),
          std::string_view(create_table_sql<CascadeChild, Dialect::PostgreSQL>())}) {
        EXPECT_TRUE(sql.contains("REFERENCES Person(id) ON DELETE CASCADE"))
                << "CASCADE FK must emit ON DELETE CASCADE in: " << sql;
    }
}

TEST(SchemaFkPolicyTest, SetNullEmitsOnDeleteSetNullOnNullableFk) {
    const std::string& sql = create_table_sql<SetNullChild>();
    EXPECT_TRUE(sql.contains("owner_id INTEGER REFERENCES Person(id) ON DELETE SET NULL"))
            << "SET NULL FK must be nullable and emit ON DELETE SET NULL in: " << sql;
    EXPECT_FALSE(sql.contains("owner_id INTEGER NOT NULL"))
            << "SET NULL FK column must be nullable (no NOT NULL) in: " << sql;
}

// fk<RefAction::Restrict> is the SQL default, so it emits a plain REFERENCES with NO
// ON DELETE clause — byte-identical to a bare fk<>. The DB still enforces RESTRICT.
TEST(SchemaFkPolicyTest, RestrictEmitsPlainReferencesNoClause) {
    const std::string& sql = create_table_sql<RestrictChild>();
    EXPECT_TRUE(sql.contains("owner_id INTEGER NOT NULL REFERENCES Person(id)"))
            << "RESTRICT FK must emit a plain REFERENCES in: " << sql;
    EXPECT_FALSE(sql.contains("ON DELETE")) << "RESTRICT is the default — no ON DELETE clause expected in: " << sql;
}

// An unannotated FK (Message::sender) must NOT gain an ON DELETE clause — the
// pre-#431 DDL stays byte-identical (plain REFERENCES = RESTRICT default).
TEST(SchemaFkPolicyTest, UnannotatedFkHasNoOnDeleteClause) {
    const std::string& sql = create_table_sql<Message>();
    EXPECT_FALSE(sql.contains("ON DELETE")) << "Unannotated FK must not emit an ON DELETE clause in: " << sql;
}

// ============================================================================
// Per-FK ON DELETE policy — runtime enforcement (#431)
//
// TYPED_TEST over SQLite + PostgreSQL. CASCADE deletes children; SET NULL nulls
// the child FK; RESTRICT blocks the parent delete.
// ============================================================================

template <typename ConnType>
class FkPolicyTest : public StormTestFixture<Person, ConnType, CascadeChild, SetNullChild, RestrictChild> {
  public:
    static auto seed_person() -> int {
        auto result =
                (InsertStatement<Person, ConnType>{}).query(Person{.id = 0, .name = "Alice", .age = 30}).execute();
        EXPECT_TRUE(result.has_value()) << (result.has_value() ? std::string{} : result.error().message());
        return result.has_value() ? static_cast<int>(result.value()) : 0;
    }
};

TYPED_TEST_SUITE(FkPolicyTest, DatabaseTypes);

// CASCADE: deleting the parent removes the child rows that reference it.
TYPED_TEST(FkPolicyTest, CascadeDeletesChildren) {
    int const alice_id = TestFixture::seed_person();

    ASSERT_TRUE((InsertStatement<CascadeChild, TypeParam>{})
                        .query(CascadeChild{.id = 0, .owner = Person{.id = alice_id}, .label = "c"})
                        .execute()
                        .has_value());
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.id == alice_id).execute().has_value())
            << "CASCADE must allow deleting the referenced parent";

    EXPECT_EQ((SelectStatement<CascadeChild, TypeParam>{}).all().count().execute(), 0)
            << "CASCADE must delete the child rows";
}

// SET NULL: deleting the parent nulls the child FK (child row survives).
TYPED_TEST(FkPolicyTest, SetNullNullsChildFk) {
    int const alice_id = TestFixture::seed_person();

    ASSERT_TRUE((InsertStatement<SetNullChild, TypeParam>{})
                        .query(SetNullChild{.id = 0, .owner = Person{.id = alice_id}, .label = "s"})
                        .execute()
                        .has_value());
    ASSERT_TRUE((EraseStatement<Person, TypeParam>{}).query_where(fields::Person.id == alice_id).execute().has_value())
            << "SET NULL must allow deleting the referenced parent";

    auto rows = (SelectStatement<SetNullChild, TypeParam>{}).all().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1U) << "SET NULL must keep the child row";
    EXPECT_FALSE(rows->begin()->owner.has_value()) << "SET NULL must null the child FK";
}

// RESTRICT: deleting a still-referenced parent is rejected.
TYPED_TEST(FkPolicyTest, RestrictBlocksParentDelete) {
    int const alice_id = TestFixture::seed_person();

    ASSERT_TRUE((InsertStatement<RestrictChild, TypeParam>{})
                        .query(RestrictChild{.id = 0, .owner = Person{.id = alice_id}, .label = "r"})
                        .execute()
                        .has_value());
    auto del = (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.id == alice_id).execute();
    EXPECT_FALSE(del.has_value()) << "RESTRICT must block deleting a referenced parent";
}

// Junction-table FOREIGN KEY tests (#412) live in tests/query/test_many_to_many.cpp,
// where the Student/Course m2m models already compile (they pull in plf_hive, which
// cannot be included in an `import std;` TU like this one).

// ============================================================================
// Referential Integrity — parent tables created first (#412)
//
// With always-on REFERENCES, a referencing table cannot be created before its
// FK target. create_table_if_not_exists<T> must create the referenced parent
// tables first. PG rejects a REFERENCES to a non-existent table; SQLite tolerated
// it. A fresh in-memory SQLite connection where neither table pre-exists is the
// minimal reproduction.
// ============================================================================

TEST(SchemaParentOrderTest, CreatesReferencedParentTableFirst) {
    using Connection = storm::db::sqlite::Connection;
    auto conn        = std::make_shared<Connection>(Connection::open(":memory:").value());

    // Message REFERENCES Person — Person must be created first. Creating only Message
    // must still succeed and leave BOTH tables present.
    auto created = SchemaStatement<Message>::create_table_if_not_exists(conn);
    ASSERT_TRUE(created.has_value()) << "create_table_if_not_exists<Message> failed: "
                                     << (created.has_value() ? std::string{} : created.error().message());

    for (std::string_view table : {"Person", "Message"}) {
        auto stmt = conn->prepare(std::format("SELECT 1 FROM {} LIMIT 1", table));
        EXPECT_TRUE(stmt.has_value()) << "Expected table '" << table << "' to exist after create_table_if_not_exists";
    }
}

// ============================================================================
// Referential Integrity — runtime enforcement (#412)
//
// TYPED_TEST over SQLite + PostgreSQL. With REFERENCES emitted AND (SQLite)
// PRAGMA foreign_keys = ON, the database must reject a dangling-FK insert and
// a delete of a referenced parent.
// ============================================================================

template <typename ConnType> class ReferentialIntegrityTest : public StormTestFixture<Person, ConnType, Task> {
  public:
    // Insert one Person and return its generated id (the shared setup for the
    // valid-FK and referenced-parent-delete cases).
    static auto seed_person() -> int {
        auto result =
                (InsertStatement<Person, ConnType>{}).query(Person{.id = 0, .name = "Alice", .age = 30}).execute();
        EXPECT_TRUE(result.has_value()) << (result.has_value() ? std::string{} : result.error().message());
        return result.has_value() ? static_cast<int>(result.value()) : 0;
    }
};

TYPED_TEST_SUITE(ReferentialIntegrityTest, DatabaseTypes);

// Test: inserting a child whose FK points at a non-existent parent is rejected.
TYPED_TEST(ReferentialIntegrityTest, RejectsInsertWithDanglingFk) {
    // No Person rows exist (fresh table) — assignee/reviewer id 999 are dangling.
    Task const orphan{.id = 0, .assignee = Person{.id = 999}, .reviewer = Person{.id = 999}, .description = "dangling"};

    auto result = (InsertStatement<Task, TypeParam>{}).query(orphan).execute();
    EXPECT_FALSE(result.has_value()) << "Insert with a dangling FK must be rejected by the database";
}

// Test: a valid FK insert still succeeds (the constraint does not over-reject).
TYPED_TEST(ReferentialIntegrityTest, AcceptsInsertWithValidFk) {
    int const alice_id = TestFixture::seed_person();

    Task const valid{
            .id = 0, .assignee = Person{.id = alice_id}, .reviewer = Person{.id = alice_id}, .description = "v"
    };
    auto result = (InsertStatement<Task, TypeParam>{}).query(valid).execute();
    EXPECT_TRUE(result.has_value()) << "Valid FK insert must succeed: "
                                    << (result.has_value() ? std::string{} : result.error().message());
}

// Test: deleting a parent that is still referenced is rejected (RESTRICT default).
TYPED_TEST(ReferentialIntegrityTest, RejectsDeleteOfReferencedParent) {
    int const alice_id = TestFixture::seed_person();

    Task const ref{.id = 0, .assignee = Person{.id = alice_id}, .reviewer = Person{.id = alice_id}, .description = "r"};
    ASSERT_TRUE((InsertStatement<Task, TypeParam>{}).query(ref).execute().has_value());

    // Alice is still referenced by the task — RESTRICT must block the delete.
    auto del = (EraseStatement<Person, TypeParam>{}).query_where(fields::Person.id == alice_id).execute();
    EXPECT_FALSE(del.has_value()) << "Deleting a referenced parent must be rejected (RESTRICT)";
}

// NOLINTEND(misc-const-correctness)
