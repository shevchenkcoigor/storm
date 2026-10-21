#include <sqlite3.h>

#include <gtest/gtest.h>

import storm_db_sqlite;
import std;
import storm_db_sqlite;

using storm::db::sqlite::Statement;

// ============================================================================
// Statement move semantics (issue #354)
//
// Statement caches a raw sqlite3_stmt* in raw_ parallel to the owning
// unique_ptr stmt_. With defaulted move ops, the move transferred stmt_ (source
// stmt_ -> null) but COPIED raw_, leaving the moved-from source aliasing the
// statement now owned by the destination. These tests pin the invariant that a
// moved-from Statement exposes no usable handle (raw_ == nullptr), so calling a
// method on it can never touch the destination's live statement.
// ============================================================================

namespace {

    // Prepares one statement against a private in-memory db. The db is kept
    // alive for the fixture's lifetime so the prepared statement stays valid.
    class StatementMoveTest : public ::testing::Test {
      public:
        void SetUp() override {
            ASSERT_EQ(sqlite3_open(":memory:", &db_), SQLITE_OK);
        }

        void TearDown() override {
            sqlite3_close_v2(db_);
        }

        [[nodiscard]] auto prepare(const char* sql) const -> sqlite3_stmt* {
            sqlite3_stmt* raw = nullptr;
            EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr), SQLITE_OK);
            return raw;
        }

        sqlite3* db_ = nullptr;
    };

    // sqlite3_stmt is incomplete; viewing handles as void* keeps GTest's
    // EqHelper off the bool-converting overload it picks for incomplete ptrs.
    auto as_void(sqlite3_stmt* p) -> const void* {
        return p;
    }

    // The destination must own the live statement; the moved-from source must
    // null its cached raw_ so it can never alias the destination's statement.
    TEST_F(StatementMoveTest, MoveCtorNullsSourceHandle) {
        sqlite3_stmt* raw = prepare("SELECT 1");
        Statement     src{raw};
        ASSERT_EQ(src.handle(), raw);

        Statement dst{std::move(src)};

        EXPECT_EQ(as_void(dst.handle()), as_void(raw));
        // Intentional moved-from read: the test asserts the move nulled the source.
        const void* moved_src = as_void(src.handle()); // NOLINT(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
        EXPECT_EQ(moved_src, nullptr);
    }

    // Same invariant for move assignment.
    TEST_F(StatementMoveTest, MoveAssignNullsSourceHandle) {
        sqlite3_stmt* raw_src = prepare("SELECT 1");
        sqlite3_stmt* raw_dst = prepare("SELECT 2");
        Statement     src{raw_src};
        Statement     dst{raw_dst};

        dst = std::move(src);

        EXPECT_EQ(as_void(dst.handle()), as_void(raw_src));
        // Intentional moved-from read: the test asserts the move nulled the source.
        const void* moved_src = as_void(src.handle()); // NOLINT(clang-analyzer-cplusplus.Move,bugprone-use-after-move)
        EXPECT_EQ(moved_src, nullptr);
    }

} // namespace
