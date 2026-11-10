#pragma once

// Shared fake libpq API policy (#787). Include AFTER `import storm_db_postgresql;`
// and `<libpq-fe.h>` (a textual include placed before the imports).
//
// FakePqApi forwards to the real RealApi (a live PostgreSQL server), substituting a
// result only where a test armed one. Counting rules mirror the removed preloaded
// libpq mock: per-function call counters, fail-on-call-N checked first, then a fixed
// override. An injected failed result is a genuine PGresult made by
// PQmakeEmptyPGresult, so the direct PQresultStatus/PQclear calls in production handle
// it exactly as a server-side failure; the statement is never sent to the server.
// status/error_message always read the real handle first, then substitute an armed
// value, so a read after PQfinish stays a genuine use-after-free ASAN reports (#351).
// The per-column reads (PQgetvalue/PQgetlength/PQgetisnull) are not part of the
// policy: extraction tests read real server values.
// The plan is thread_local: a thread a test spawns starts UNARMED. All tests here
// are single-threaded.

namespace fake_pq {

    struct FakePlan {
        // Connection
        bool                       connectdb_null = false;
        std::optional<int>         status;        // PQstatus override
        std::optional<std::string> error_message; // PQerrorMessage override

        // PQprepare: null result, failed status, or null on the Nth call (1-based).
        bool               prepare_null = false;
        std::optional<int> prepare_status;
        int                prepare_fail_on = -1;

        // PQexecPrepared: failed status.
        std::optional<int> exec_prepared_status;

        // PQexec: exec_null returns null on EVERY call, the Nth included. Otherwise
        // exec_fail_on fails only the Nth call (1-based) with PGRES_FATAL_ERROR, and
        // exec_status fails every remaining call with that status.
        bool               exec_null = false;
        std::optional<int> exec_status;
        int                exec_fail_on = -1;

        int         connectdb_calls     = 0;
        int         prepare_calls       = 0;
        int         exec_calls          = 0;
        int         exec_prepared_calls = 0;
        int         deallocate_calls    = 0;
        std::string last_exec_query;
        std::string last_prepare_query;
        std::string last_prepare_name;
    };

    struct FakePqApi {
        using Real = storm::db::postgresql::RealApi;

        static inline thread_local FakePlan plan{};

        static auto reset() -> void {
            plan = FakePlan{};
        }

        static auto connectdb(const char* conninfo) noexcept -> PGconn* {
            ++plan.connectdb_calls;
            return plan.connectdb_null ? nullptr : Real::connectdb(conninfo);
        }
        // Real call first, even when overridden: it is what keeps a read of a freed
        // handle observable to ASAN.
        static auto status(const PGconn* conn) noexcept -> ConnStatusType {
            const ConnStatusType real = Real::status(conn);
            return plan.status ? static_cast<ConnStatusType>(*plan.status) : real;
        }
        static auto error_message(const PGconn* conn) noexcept -> const char* {
            const char* real = Real::error_message(conn);
            return plan.error_message ? plan.error_message->c_str() : real;
        }
        static auto prepare(PGconn* conn, const char* stmt_name, const char* query) noexcept -> PGresult* {
            ++plan.prepare_calls;
            plan.last_prepare_query = query;
            plan.last_prepare_name  = stmt_name;
            if (plan.prepare_fail_on == plan.prepare_calls || plan.prepare_null) {
                return nullptr;
            }
            if (plan.prepare_status) {
                return PQmakeEmptyPGresult(conn, static_cast<ExecStatusType>(*plan.prepare_status));
            }
            return Real::prepare(conn, stmt_name, query);
        }
        static auto exec(PGconn* conn, const char* query) noexcept -> PGresult* {
            ++plan.exec_calls;
            plan.last_exec_query = query;
            if (std::string_view{query}.starts_with("DEALLOCATE")) {
                ++plan.deallocate_calls;
            }
            if (plan.exec_fail_on == plan.exec_calls) {
                return plan.exec_null ? nullptr : PQmakeEmptyPGresult(conn, PGRES_FATAL_ERROR);
            }
            if (plan.exec_null) {
                return nullptr;
            }
            if (plan.exec_status) {
                return PQmakeEmptyPGresult(conn, static_cast<ExecStatusType>(*plan.exec_status));
            }
            return Real::exec(conn, query);
        }
        static auto exec_prepared(
                PGconn*            conn,
                const char*        stmt_name,
                int                n_params,
                const char* const* values,
                const int*         lengths,
                const int*         formats,
                int                result_format
        ) noexcept -> PGresult* {
            ++plan.exec_prepared_calls;
            if (plan.exec_prepared_status) {
                return PQmakeEmptyPGresult(conn, static_cast<ExecStatusType>(*plan.exec_prepared_status));
            }
            return Real::exec_prepared(conn, stmt_name, n_params, values, lengths, formats, result_format);
        }
    };

    using FakeConn = storm::db::postgresql::BasicConnection<FakePqApi>;

    // Facade with the removed MockPqConfig's call shapes, so a ported test body reads
    // the same as the original.
    struct FakePqConfig {
        static auto reset() -> void {
            FakePqApi::reset();
        }
        static auto connectdb_returns_null() -> void {
            FakePqApi::plan.connectdb_null = true;
        }
        static auto status_returns(int status) -> void {
            FakePqApi::plan.status = status;
        }
        static auto error_message(std::string_view msg) -> void {
            FakePqApi::plan.error_message = std::string{msg};
        }
        static auto prepare_returns_null() -> void {
            FakePqApi::plan.prepare_null = true;
        }
        static auto prepare_status(int status) -> void {
            FakePqApi::plan.prepare_status = status;
        }
        static auto prepare_fails_on_call(int call) -> void {
            FakePqApi::plan.prepare_fail_on = call;
        }
        static auto exec_prepared_status(int status) -> void {
            FakePqApi::plan.exec_prepared_status = status;
        }
        static auto exec_returns_null() -> void {
            FakePqApi::plan.exec_null = true;
        }
        static auto exec_status(int status) -> void {
            FakePqApi::plan.exec_status = status;
        }
        static auto exec_fails_on_call(int call) -> void {
            FakePqApi::plan.exec_fail_on = call;
        }
        static auto get_connectdb_call_count() -> int {
            return FakePqApi::plan.connectdb_calls;
        }
        static auto get_prepare_call_count() -> int {
            return FakePqApi::plan.prepare_calls;
        }
        static auto get_exec_call_count() -> int {
            return FakePqApi::plan.exec_calls;
        }
        static auto get_exec_prepared_call_count() -> int {
            return FakePqApi::plan.exec_prepared_calls;
        }
        static auto get_deallocate_call_count() -> int {
            return FakePqApi::plan.deallocate_calls;
        }
        static auto get_last_exec_query() -> std::string {
            return FakePqApi::plan.last_exec_query;
        }
        static auto get_last_prepare_query() -> std::string {
            return FakePqApi::plan.last_prepare_query;
        }
        static auto get_last_prepare_name() -> std::string {
            return FakePqApi::plan.last_prepare_name;
        }
    };

} // namespace fake_pq
