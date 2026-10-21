// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,concurrency-mt-unsafe)
// storm_bench_dashboard — Phase 3 (Issue #247).
//
// Phase 1 verified the SQLite schema is current. Phase 2 added the live socket
// stream. Phase 3 turns the binary into a real TUI: every NDJSON datagram is
// parsed, persisted to SQLite via Storm (`QuerySet<BenchRun>` /
// `QuerySet<BenchResult>` — Storm dogfoods itself), and rendered in an
// alt-screen ANSI view with categorised, colour-coded, newest-first results.
// Multiple `storm_bench` invocations stack in the same dashboard process
// (newest on top); older sessions auto-collapse and re-expand on number keys.
//
// Schema management is out of scope: this binary never auto-creates tables.
// Migrations are run explicitly via `cmake --build . --target migrate-bench`
// (Atlas-backed, see docs/development/MIGRATIONS.md). If a required table is
// missing the binary prints the migration command and exits non-zero — same
// philosophy as the application schema flow.
//
// File layout: this TU stays small by delegating most of the implementation
// to textual headers `#include`d from the anonymous namespace below
// (args.hpp, backup.hpp, db.hpp, events.hpp). The textual approach is
// load-bearing — it keeps the model definitions in models.hpp and the
// QuerySet calls in the same TU, which is required because clang-p2996
// does not transport reflection annotations across BMI boundaries
// (memory feedback_cpp26_module_reflection_annotations).

// import std; migration (issue #326): <meta> MUST precede the imports. The
// textual dashboard headers below (db.hpp/events.hpp/models.hpp) use the `^^`
// splice operator and storm reflection annotations; import std; does not export
// std::meta:: (Finding A), and a textual <meta> after `import std;`/`import
// storm;` would redefine std-module entities (Finding B). Putting it first
// satisfies both. import std; supplies all other std headers for this TU and the
// textual headers it includes.
#include <meta>

// POSIX signal API (`struct sigaction`, `sigaction()`, `sigemptyset`, SIGINT/
// SIGTERM) is NOT part of import std; — those are POSIX extensions, not standard
// C++. Keep <csignal> textual, and before the imports so it does not re-pull
// libc++ headers after the std module (Finding B).
#include <csignal>

import storm;
import std;
import storm.bench_dashboard.socket_server;
import storm.bench_dashboard.tui;
import storm.bench_dashboard.wire;

#include "models.hpp" // textual — must follow `import storm;`

// import std; migration (issue #326): std headers (<atomic>, <string>, <csignal>,
// …) are no longer #included here — they come from `import std;` above. Keeping
// them as textual includes after the imports would re-pull libc++ headers the std
// module already owns, causing "requires clause differs in template
// redeclaration" (issue #326 Finding B). <csignal>'s `sigaction`/`SIGINT` come
// from import std; (it re-exports the C compatibility names).

namespace {

    // SIGINT / SIGTERM → flip the flag the main loop already checks for `q`.
    // Keeps the alt-screen + termios teardown on the normal exit path so a
    // Ctrl-C doesn't leave the user with a wrecked terminal.
    std::atomic<bool> g_should_quit{false}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    extern "C" auto handle_quit_signal(int /*sig*/) -> void {
        g_should_quit.store(true);
    }

    auto install_signal_handlers() -> void {
        struct sigaction sa{};
        sa.sa_handler = &handle_quit_signal;
        // Unqualified: sigemptyset is a real function on Linux/glibc but a macro on
        // macOS's <signal.h> (`#define sigemptyset(set) (*(set) = 0, 0)`), and `::`
        // in front of a macro invocation does not parse.
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; // intentionally not SA_RESTART — we want poll() to wake on signal
        ::sigaction(SIGINT, &sa, nullptr);
        ::sigaction(SIGTERM, &sa, nullptr);
    }

} // namespace

// Textual headers — included after `import storm;` + models.hpp so their
// QuerySet calls see the same models with their reflection annotations.
// Order matters: backup needs Options, db needs Options + tui types,
// events needs DashboardDB + baseline helpers from db.
using storm::db::DefaultConnection;
#include "args.hpp"
#include "backup.hpp"
#include "db.hpp"
#include "events.hpp"

namespace {

    // -----------------------------------------------------------------------
    // main() helpers — stage gates + event loop
    // -----------------------------------------------------------------------

    // Print an error of the form "<prefix> '<arg>': <reason>" and return rc.
    // Single source of truth for the path/connection/io error pattern.
    auto fail_with(int rc, char const* prefix, std::string_view arg, std::string_view reason) -> int {
        std::
                fprintf( // NOLINT(cppcoreguidelines-pro-type-vararg)
                        stderr,
                        "storm_bench_dashboard: %s '%.*s': %.*s\n",
                        prefix,
                        static_cast<int>(arg.size()),
                        arg.data(),
                        static_cast<int>(reason.size()),
                        reason.data()
                );
        return rc;
    }

    // mkdir -p the parent of `db_path` so SQLite's open() doesn't fail with
    // ENOENT on first run. Returns 0 on success, 1 on filesystem error.
    auto ensure_db_parent_dir(std::string_view db_path) -> int {
        const auto parent = std::filesystem::path{db_path}.parent_path();
        if (parent.empty()) {
            return 0;
        }
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return fail_with(1, "cannot create", parent.string(), ec.message());
        }
        return 0;
    }

    // Open the SQLite store. Returns 0 on success, 1 on connection failure.
    auto open_db_connection(Options const& opts) -> int {
        auto rc = DefaultConnection<>::set(opts.db_path);
        if (!rc) {
            return fail_with(1, "cannot open", opts.db_path, std::string{rc.error().message()});
        }
        return 0;
    }

    // Verify a single table by issuing `count()`. Returns 0 on success or 2
    // on missing schema (already printed).
    auto check_table_exists(auto&& count_result, std::string_view db_path) -> int {
        if (count_result) {
            return 0;
        }
        std::
                fprintf( // NOLINT(cppcoreguidelines-pro-type-vararg)
                        stderr,
                        "storm_bench_dashboard: schema check failed: %s\n",
                        std::string{count_result.error().message()}.c_str()
                );
        print_missing_schema_hint(db_path);
        return 2;
    }

    auto check_schema(Options const& opts) -> int {
        if (const int rc = check_table_exists(QuerySet<bench_dashboard::BenchRun>().count().execute(), opts.db_path);
            rc != 0) {
            return rc;
        }
        return check_table_exists(QuerySet<bench_dashboard::BenchResult>().count().execute(), opts.db_path);
    }

    // Each BenchRun re-reads git_hash/branch from the working tree at insert
    // time (Issue #267), so the daemon's CWD MUST be inside the storm git work
    // tree. Fail loudly at startup otherwise — a daemon outside the repo would
    // silently stamp every run with empty git metadata.
    auto check_inside_git_worktree() -> int {
        if (run_capture("git rev-parse --is-inside-work-tree 2>/dev/null") == "true") {
            return 0;
        }
        std::
                fprintf( // NOLINT(cppcoreguidelines-pro-type-vararg)
                        stderr,
                        "storm_bench_dashboard: current directory is not inside the storm git work tree.\n"
                        "  Each bench run records the live git branch/hash, which needs a git working tree.\n"
                        "  Start the dashboard from within the storm repository checkout.\n"
                );
        return 4;
    }

    // Open the AF_UNIX listener. Returns 0 on success, 3 on bind failure.
    auto setup_socket(Options const& opts, bench_dashboard::SocketServer& server) -> int {
        const std::string_view socket_path = opts.socket_path.empty() ? bench_dashboard::wire::default_socket_path()
                                                                      : std::string_view{opts.socket_path};
        if (auto err = server.open(socket_path); !err.empty()) {
            std::
                    fprintf( // NOLINT(cppcoreguidelines-pro-type-vararg)
                            stderr,
                            "storm_bench_dashboard: %s\n",
                            err.c_str()
                    );
            return 3;
        }
        return 0;
    }

    // Resolve the baseline selector now that the DB is open, store the result
    // on `state`, and warn when --baseline auto matched nothing.
    auto resolve_and_log_baseline(Options const& opts, bench_dashboard::tui::DashboardState& state) -> void {
        const std::string current_branch = run_capture("git rev-parse --abbrev-ref HEAD 2>/dev/null");
        const std::string current_host   = run_capture("hostname -s 2>/dev/null");
        auto              resolved       = resolve_baseline(opts.baseline, current_branch, current_host);
        state.baseline_run_id            = resolved.run_id;
        state.baseline_label             = std::move(resolved.label);
        state.baseline_is_raw            = resolved.is_raw;
        if (resolved.run_id == 0 && std::holds_alternative<BaselineAuto>(opts.baseline)) {
            std::
                    fprintf( // NOLINT(cppcoreguidelines-pro-type-vararg)
                            stderr,
                            "storm_bench_dashboard: no baseline found for branch '%s' on host '%s' — running without "
                            "comparison\n",
                            current_branch.c_str(),
                            current_host.c_str()
                    );
        }
    }

    // Translate a single key event into state mutations. Returns true when
    // the user asked to quit. `dirty` is set when the next frame must redraw.
    auto handle_key_event(
            bench_dashboard::tui::KeyEvent        key,
            bench_dashboard::tui::DashboardState& state,
            std::int64_t&                         active_run_id,
            bool&                                 dirty
    ) -> bool {
        using Key = bench_dashboard::tui::Key;
        switch (key.kind) {
        case Key::Quit:
            return true;
        case Key::Refresh:
            rebuild_state_from_db(state);
            active_run_id       = 0;
            state.scroll_offset = 0;
            dirty               = true;
            return false;
        case Key::Digit:
            bench_dashboard::tui::toggle_session(state, key.digit - '0');
            dirty = true;
            return false;
        case Key::ScrollDown:
            ++state.scroll_offset;
            dirty = true;
            return false;
        case Key::ScrollUp:
            if (state.scroll_offset > 0) {
                --state.scroll_offset;
            }
            dirty = true;
            return false;
        case Key::None:
            return false;
        }
        return false;
    }

    // Drain whatever datagrams have queued on the socket since last poll.
    // Returns false on a fatal apply_event error so the caller can stop.
    auto drain_socket_datagrams(
            bench_dashboard::SocketServer&        server,
            bench_dashboard::tui::DashboardState& state,
            DashboardDB&                          db,
            std::int64_t&                         active_run_id,
            bool&                                 dirty
    ) -> bool {
        while (true) {
            auto datagram = server.recv_one(/*timeout_ms=*/0);
            if (!datagram) {
                return true;
            }
            auto msg = bench_dashboard::wire::parse(*datagram);
            if (!msg) {
                std::
                        fprintf( // NOLINT(cppcoreguidelines-pro-type-vararg)
                                stderr,
                                "\nstorm_bench_dashboard: skipped unparseable datagram (%zu bytes)\n",
                                datagram->size()
                        );
                continue;
            }
            if (!apply_event(*msg, state, db, active_run_id)) {
                return false;
            }
            dirty = true;
        }
    }

    // Spinner: redraw at least every ~5 ticks (~500 ms) so the active
    // session's glyph rotates even when no events arrive.
    auto tick_spinner(bench_dashboard::tui::DashboardState& state, std::size_t& since_last_redraw, bool& dirty)
            -> void {
        ++since_last_redraw;
        if (since_last_redraw >= 5) {
            ++state.spinner_tick;
            dirty             = true;
            since_last_redraw = 0;
        }
    }

    // SocketServer doesn't expose its fd today, so stdin and the socket are
    // driven independently: read_key polls stdin for 100 ms, then recv_one(0)
    // drains all queued datagrams. A single thread suffices.
    auto
    run_event_loop(bench_dashboard::tui::DashboardState& state, DashboardDB& db, bench_dashboard::SocketServer& server)
            -> void {
        std::int64_t active_run_id     = 0;
        std::size_t  since_last_redraw = 0;
        while (true) {
            if (g_should_quit.load()) {
                return;
            }
            const auto key   = bench_dashboard::tui::read_key(/*timeout_ms=*/100);
            bool       dirty = false;
            if (handle_key_event(key, state, active_run_id, dirty)) {
                return;
            }
            if (!drain_socket_datagrams(server, state, db, active_run_id, dirty)) {
                return;
            }
            tick_spinner(state, since_last_redraw, dirty);
            if (dirty) {
                bench_dashboard::tui::write_full_frame(bench_dashboard::tui::render(state));
            }
        }
    }

    // Run any one-shot subcommand requested via CLI flags, returning the
    // exit code. Returns nullopt when no subcommand was selected and the
    // normal interactive flow should run.
    auto try_run_subcommand(Options const& opts) -> std::optional<int> {
        if (opts.upload_backup) {
            return upload_backup(opts);
        }
        if (opts.restore_backup) {
            return restore_backup(opts);
        }
        return std::nullopt;
    }

} // namespace

auto main(int argc, char** argv) -> int { // NOLINT(bugprone-exception-escape)
    const auto opts = parse_args(argc, argv);

    // One-shot backup actions short-circuit before any TUI / DB-connection
    // setup. They must NOT install signal handlers (Ctrl-C should propagate
    // straight to gh) and must NOT call DefaultConnection::set.
    if (auto code = try_run_subcommand(opts); code.has_value()) {
        return *code;
    }

    install_signal_handlers();

    if (const int rc = check_inside_git_worktree(); rc != 0) {
        return rc;
    }
    if (const int rc = ensure_db_parent_dir(opts.db_path); rc != 0) {
        return rc;
    }
    if (const int rc = open_db_connection(opts); rc != 0) {
        return rc;
    }
    if (const int rc = check_schema(opts); rc != 0) {
        return rc;
    }

    bench_dashboard::SocketServer server;
    if (const int rc = setup_socket(opts, server); rc != 0) {
        return rc;
    }

    DashboardDB                          db;
    bench_dashboard::tui::DashboardState state;
    state.order_arrival        = opts.order_arrival;
    state.regression_threshold = opts.regression_threshold;
    state.complexity_threshold = opts.complexity_threshold;

    resolve_and_log_baseline(opts, state);

    // Pre-load history so the first frame shows past runs even before any
    // bench process connects.
    rebuild_state_from_db(state);

    const bench_dashboard::tui::TerminalGuard guard;
    bench_dashboard::tui::write_full_frame(bench_dashboard::tui::render(state));

    run_event_loop(state, db, server);
    return 0;
}
// NOLINTEND(cppcoreguidelines-pro-type-vararg,concurrency-mt-unsafe)
