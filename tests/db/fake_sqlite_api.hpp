#pragma once

// Shared fake sqlite3 API policy (#760/#784). Include AFTER `import storm_db_sqlite;`
// and `<sqlite3.h>` (a textual include placed before the imports).
//
// FakeSqliteApi forwards to the real RealApi, substituting a result only where a
// test armed one. Counting rules mirror the removed preloaded mock: global call
// counters, fail-on-call-N checked first, then a step sequence, then a fixed code.
// A step sequence returns its codes without stepping the real statement; an
// un-stepped statement reads back as NULL columns, as the mock's dummy handles did.
// The plan is thread_local: a thread a test spawns starts UNARMED, so an injected failure
// silently does nothing there. All tests here are single-threaded.

namespace fake_sqlite {

    struct FakePlan {
        // Fixed return codes. SQLITE_OK means "forward to real SQLite".
        int open_rc         = SQLITE_OK;
        int busy_timeout_rc = SQLITE_OK;
        int exec_rc         = SQLITE_OK;
        int prepare_rc      = SQLITE_OK;
        int step_rc         = SQLITE_OK;
        int bind_int_rc     = SQLITE_OK;
        int bind_int64_rc   = SQLITE_OK;
        int bind_double_rc  = SQLITE_OK;
        int bind_text_rc    = SQLITE_OK;
        int bind_null_rc    = SQLITE_OK;
        int bind_blob_rc    = SQLITE_OK;

        // Fail on the Nth call (1-based, counted since the last reset); -1 = never.
        int prepare_fail_on    = -1;
        int prepare_fail_rc    = SQLITE_OK;
        int step_fail_on       = -1;
        int step_fail_rc       = SQLITE_OK;
        int bind_int_fail_on   = -1;
        int bind_int_fail_rc   = SQLITE_OK;
        int bind_int64_fail_on = -1;
        int bind_int64_fail_rc = SQLITE_OK;
        int bind_text_fail_on  = -1;
        int bind_text_fail_rc  = SQLITE_OK;

        std::vector<int> step_sequence;
        std::size_t      step_sequence_index = 0;
        bool             expanded_sql_null   = false;
        std::string      exec_error_message  = "fake exec error";

        int prepare_calls     = 0;
        int step_calls        = 0;
        int exec_calls        = 0;
        int bind_int_calls    = 0;
        int bind_int64_calls  = 0;
        int bind_double_calls = 0;
        int bind_text_calls   = 0;
        int bind_null_calls   = 0;
        int bind_blob_calls   = 0;
    };

    struct FakeSqliteApi {
        using Real = storm::db::sqlite::RealApi;

        static inline thread_local FakePlan plan{};

        static auto reset() -> void {
            plan = FakePlan{};
        }

        static auto open_v2(const char* path, sqlite3** db, int flags, const char* vfs) noexcept -> int {
            return plan.open_rc != SQLITE_OK ? plan.open_rc : Real::open_v2(path, db, flags, vfs);
        }
        static auto busy_timeout(sqlite3* db, int timeout_ms) noexcept -> int {
            return plan.busy_timeout_rc != SQLITE_OK ? plan.busy_timeout_rc : Real::busy_timeout(db, timeout_ms);
        }
        static auto exec(sqlite3* db, const char* sql, char** errmsg) noexcept -> int {
            ++plan.exec_calls;
            if (plan.exec_rc == SQLITE_OK) {
                return Real::exec(db, sql, errmsg);
            }
            if (errmsg != nullptr) {
                *errmsg = sqlite3_mprintf("%s", plan.exec_error_message.c_str()); // freed by sqlite3_free
            }
            return plan.exec_rc;
        }
        static auto prepare_v2(sqlite3* db, const char* sql, int len, sqlite3_stmt** out) noexcept -> int {
            ++plan.prepare_calls;
            if (plan.prepare_fail_on == plan.prepare_calls) {
                *out = nullptr;
                return plan.prepare_fail_rc;
            }
            if (plan.prepare_rc != SQLITE_OK) {
                *out = nullptr;
                return plan.prepare_rc;
            }
            return Real::prepare_v2(db, sql, len, out);
        }
        static auto step(sqlite3_stmt* stmt) noexcept -> int {
            ++plan.step_calls;
            if (plan.step_fail_on == plan.step_calls) {
                return plan.step_fail_rc;
            }
            if (!plan.step_sequence.empty()) {
                if (plan.step_sequence_index < plan.step_sequence.size()) {
                    return plan.step_sequence[plan.step_sequence_index++];
                }
                return SQLITE_DONE;
            }
            return plan.step_rc != SQLITE_OK ? plan.step_rc : Real::step(stmt);
        }
        static auto bind_int(sqlite3_stmt* stmt, int idx, int value) noexcept -> int {
            ++plan.bind_int_calls;
            if (plan.bind_int_fail_on == plan.bind_int_calls) {
                return plan.bind_int_fail_rc;
            }
            return plan.bind_int_rc != SQLITE_OK ? plan.bind_int_rc : Real::bind_int(stmt, idx, value);
        }
        static auto bind_int64(sqlite3_stmt* stmt, int idx, std::int64_t value) noexcept -> int {
            ++plan.bind_int64_calls;
            if (plan.bind_int64_fail_on == plan.bind_int64_calls) {
                return plan.bind_int64_fail_rc;
            }
            return plan.bind_int64_rc != SQLITE_OK ? plan.bind_int64_rc : Real::bind_int64(stmt, idx, value);
        }
        static auto bind_double(sqlite3_stmt* stmt, int idx, double value) noexcept -> int {
            ++plan.bind_double_calls;
            return plan.bind_double_rc != SQLITE_OK ? plan.bind_double_rc : Real::bind_double(stmt, idx, value);
        }
        static auto bind_text(sqlite3_stmt* stmt, int idx, std::string_view value) noexcept -> int {
            ++plan.bind_text_calls;
            if (plan.bind_text_fail_on == plan.bind_text_calls) {
                return plan.bind_text_fail_rc;
            }
            return plan.bind_text_rc != SQLITE_OK ? plan.bind_text_rc : Real::bind_text(stmt, idx, value);
        }
        static auto bind_null(sqlite3_stmt* stmt, int idx) noexcept -> int {
            ++plan.bind_null_calls;
            return plan.bind_null_rc != SQLITE_OK ? plan.bind_null_rc : Real::bind_null(stmt, idx);
        }
        static auto bind_blob(sqlite3_stmt* stmt, int idx, const void* data, std::size_t size) noexcept -> int {
            ++plan.bind_blob_calls;
            return plan.bind_blob_rc != SQLITE_OK ? plan.bind_blob_rc : Real::bind_blob(stmt, idx, data, size);
        }
        static auto expanded_sql(sqlite3_stmt* stmt) noexcept -> char* {
            return plan.expanded_sql_null ? nullptr : Real::expanded_sql(stmt);
        }
    };

    using FakeConn = storm::db::sqlite::BasicConnection<FakeSqliteApi>;

    // Builder-style facade with the removed MockSqlite3Config's call shapes, so a
    // ported test body reads the same as the original.
    struct FakeSqliteConfig {
        static auto reset() -> void {
            FakeSqliteApi::reset();
        }
        static auto open_returns(int rc) -> void {
            FakeSqliteApi::plan.open_rc = rc;
        }
        static auto busy_timeout_returns(int rc) -> void {
            FakeSqliteApi::plan.busy_timeout_rc = rc;
        }
        static auto exec_returns(int rc) -> void {
            FakeSqliteApi::plan.exec_rc = rc;
        }
        static auto exec_error_message(std::string_view msg) -> void {
            FakeSqliteApi::plan.exec_error_message = std::string{msg};
        }
        static auto prepare_returns(int rc) -> void {
            FakeSqliteApi::plan.prepare_rc = rc;
        }
        static auto step_returns(int rc) -> void {
            FakeSqliteApi::plan.step_rc = rc;
            FakeSqliteApi::plan.step_sequence.clear(); // the removed mock cleared the sequence too
            FakeSqliteApi::plan.step_sequence_index = 0;
        }
        static auto step_returns_sequence(std::vector<int> codes) -> void {
            FakeSqliteApi::plan.step_sequence       = std::move(codes);
            FakeSqliteApi::plan.step_sequence_index = 0;
        }
        static auto bind_int_returns(int rc) -> void {
            FakeSqliteApi::plan.bind_int_rc = rc;
        }
        static auto bind_int64_returns(int rc) -> void {
            FakeSqliteApi::plan.bind_int64_rc = rc;
        }
        static auto bind_double_returns(int rc) -> void {
            FakeSqliteApi::plan.bind_double_rc = rc;
        }
        static auto bind_text_returns(int rc) -> void {
            FakeSqliteApi::plan.bind_text_rc = rc;
        }
        static auto bind_null_returns(int rc) -> void {
            FakeSqliteApi::plan.bind_null_rc = rc;
        }
        static auto bind_blob_returns(int rc) -> void {
            FakeSqliteApi::plan.bind_blob_rc = rc;
        }
        static auto prepare_fails_on_call(int call, int rc) -> void {
            FakeSqliteApi::plan.prepare_fail_on = call;
            FakeSqliteApi::plan.prepare_fail_rc = rc;
        }
        static auto step_fails_on_call(int call, int rc) -> void {
            FakeSqliteApi::plan.step_fail_on = call;
            FakeSqliteApi::plan.step_fail_rc = rc;
        }
        static auto bind_int_fails_on_call(int call, int rc) -> void {
            FakeSqliteApi::plan.bind_int_fail_on = call;
            FakeSqliteApi::plan.bind_int_fail_rc = rc;
        }
        static auto bind_int64_fails_on_call(int call, int rc) -> void {
            FakeSqliteApi::plan.bind_int64_fail_on = call;
            FakeSqliteApi::plan.bind_int64_fail_rc = rc;
        }
        static auto bind_text_fails_on_call(int call, int rc) -> void {
            FakeSqliteApi::plan.bind_text_fail_on = call;
            FakeSqliteApi::plan.bind_text_fail_rc = rc;
        }
        static auto expanded_sql_returns_null() -> void {
            FakeSqliteApi::plan.expanded_sql_null = true;
        }
        static auto get_prepare_call_count() -> int {
            return FakeSqliteApi::plan.prepare_calls;
        }
        static auto get_step_call_count() -> int {
            return FakeSqliteApi::plan.step_calls;
        }
        static auto get_exec_call_count() -> int {
            return FakeSqliteApi::plan.exec_calls;
        }
        static auto get_bind_int_call_count() -> int {
            return FakeSqliteApi::plan.bind_int_calls;
        }
        static auto get_bind_text_call_count() -> int {
            return FakeSqliteApi::plan.bind_text_calls;
        }
    };

} // namespace fake_sqlite
