#pragma once

using storm::QuerySet;

// Event dispatch + state rebuild for storm_bench_dashboard (Issue #247).
//
// Textual header — #included after db.hpp from main.cpp's anonymous
// namespace. Depends on: DashboardDB, baseline helpers (db.hpp),
// wire::ResultMsg, tui::DashboardState. Do NOT include from any other TU.
//
// import std; migration (issue #326): no std #includes here — std types come
// from main.cpp's `import std;` (this header is pulled in after it). A textual
// std #include after the module import breaks the build (Finding B). std::meta::
// usage is served by the textual <meta> main.cpp includes before its imports.

namespace {

    // -----------------------------------------------------------------------
    // Baseline enrichment
    // -----------------------------------------------------------------------

    // Attach per-size delta to a measurement row when a baseline is active.
    // When the baseline is a raw run, also attach efficiency_pct (raw_ns / current_ns).
    auto enrich_measurement(bench_dashboard::wire::ResultMsg& msg, std::int64_t baseline_run_id, bool baseline_is_raw)
            -> void {
        if (baseline_run_id == 0)
            return;
        msg.baseline_looked_up = true;
        if (auto base_ns = lookup_baseline_ns(baseline_run_id, msg.test_name, msg.dataset_size); base_ns.has_value()) {
            msg.delta_pct = compute_delta(msg.real_ns, *base_ns);
            if (baseline_is_raw && msg.real_ns > 0.0)
                msg.efficiency_pct = *base_ns / msg.real_ns * 100.0; // raw_ns / current_ns * 100
        }
    }

    // Attach baseline complexity class + coef to a BigO row when a baseline is active.
    auto enrich_bigo(bench_dashboard::wire::ResultMsg& msg, std::int64_t baseline_run_id) -> void {
        if (baseline_run_id == 0)
            return;
        msg.baseline_looked_up = true;
        if (auto base = lookup_baseline_complexity(baseline_run_id, msg.test_name); base.has_value()) {
            msg.baseline_class   = base->complexity_class;
            msg.baseline_coef    = base->complexity_coef;
            msg.shape_regression = (msg.complexity_class != base->complexity_class);
        }
    }

    // Issue #346: a _stddev / _cv aggregate row reuses items_per_second for a
    // non-throughput stat, so dividing it against a baseline row produces a
    // meaningless "% of raw". Skip enrichment for those two; only mean/median
    // aggregates (and measurement rows) carry a real throughput. Mirrors
    // bench_dashboard::aggregate_carries_throughput (row_classify.hpp), inlined
    // here because this header is pulled in after `import std;` and cannot
    // re-include wire.hpp without breaking the build (Finding B).
    auto aggregate_carries_throughput(std::string_view test_name) -> bool {
        // NOSONAR(cpp:S4144) source of truth: row_classify.hpp::aggregate_carries_throughput
        return !(test_name.ends_with("_stddev") || test_name.ends_with("_cv"));
    }

    auto enrich_with_baseline(bench_dashboard::wire::ResultMsg& msg, std::int64_t baseline_run_id, bool baseline_is_raw)
            -> void {
        if (msg.row_kind == bench_dashboard::wire::kRowKindMeasurement ||
            (msg.row_kind == bench_dashboard::wire::kRowKindAggregate && aggregate_carries_throughput(msg.test_name)) ||
            msg.row_kind.empty())
            enrich_measurement(msg, baseline_run_id, baseline_is_raw);
        else if (msg.row_kind == bench_dashboard::wire::kRowKindBigO)
            enrich_bigo(msg, baseline_run_id);
    }

    // -----------------------------------------------------------------------
    // Wire event handlers
    // -----------------------------------------------------------------------

    auto handle_run_start(
            bench_dashboard::wire::ResultMsg const& msg,
            bench_dashboard::tui::DashboardState&   state,
            DashboardDB&                            db,
            std::int64_t&                           active_run_id
    ) -> bool {
        auto rc = db.insert_run(msg.filter, msg.is_full_run, msg.is_raw);
        if (!rc) {
            std::fprintf(stderr, "\nstorm_bench_dashboard: insert_run failed: %s\n", rc.error().c_str());
            return false;
        }
        active_run_id  = *rc;
        auto& sess     = bench_dashboard::tui::open_session(state, msg.filter, msg.is_full_run, msg.is_raw);
        sess.run_id    = active_run_id;
        sess.timestamp = current_iso8601();
        return true;
    }

    // Open a synthetic full-run session for a Result that arrived before any RunStart.
    auto open_synthetic_session(
            bench_dashboard::wire::ResultMsg const& msg,
            bench_dashboard::tui::DashboardState&   state,
            DashboardDB&                            db,
            std::int64_t&                           active_run_id
    ) -> bool {
        auto rc = db.insert_run(msg.filter, /*is_full_run=*/true, /*is_raw=*/false);
        if (!rc) {
            std::fprintf(stderr, "\nstorm_bench_dashboard: synthetic insert_run failed: %s\n", rc.error().c_str());
            return false;
        }
        active_run_id = *rc;
        auto& sess    = bench_dashboard::tui::open_session(state, "", true, /*is_raw=*/false);
        sess.run_id   = active_run_id;
        return true;
    }

    auto handle_result(
            bench_dashboard::wire::ResultMsg const& msg,
            bench_dashboard::tui::DashboardState&   state,
            DashboardDB&                            db,
            std::int64_t&                           active_run_id
    ) -> bool {
        if (active_run_id == 0 && !open_synthetic_session(msg, state, db, active_run_id))
            return false;

        auto enriched = msg;
        enrich_with_baseline(enriched, state.baseline_run_id, state.baseline_is_raw);

        if (auto rc = db.insert_result(active_run_id, enriched); !rc)
            std::fprintf(stderr, "\nstorm_bench_dashboard: insert_result failed: %s\n", rc.error().c_str());
        if (!state.sessions.empty())
            bench_dashboard::tui::add_result(
                    state.sessions.front(), enriched, state.regression_threshold, state.baseline_is_raw
            );
        return true;
    }

    // Apply one parsed wire message to TUI state + DB. Returns false on fatal DB error.
    auto apply_event(
            bench_dashboard::wire::ResultMsg const& msg,
            bench_dashboard::tui::DashboardState&   state,
            DashboardDB&                            db,
            std::int64_t&                           active_run_id
    ) -> bool {
        using bench_dashboard::wire::MessageKind;
        switch (msg.kind) {
        case MessageKind::RunStart:
            return handle_run_start(msg, state, db, active_run_id);
        case MessageKind::Result:
            return handle_result(msg, state, db, active_run_id);
        case MessageKind::RunComplete:
            bench_dashboard::tui::mark_complete(state);
            active_run_id = 0;
            return true;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // State rebuild from DB (used on startup + 'r' keypress)
    // -----------------------------------------------------------------------

    auto build_result_msg_from_row(bench_dashboard::BenchResult& r) -> bench_dashboard::wire::ResultMsg {
        bench_dashboard::wire::ResultMsg m{};
        m.kind             = bench_dashboard::wire::MessageKind::Result;
        m.test_name        = std::move(r.test_name);
        m.category         = std::move(r.category);
        m.dataset_size     = r.dataset_size;
        m.row_kind         = std::move(r.row_kind);
        m.real_ns          = r.real_time_ns;
        m.cpu_ns           = r.cpu_time_ns;
        m.iterations       = r.iterations;
        m.items_per_second = r.items_per_second;
        m.complexity_class = std::move(r.complexity_class);
        m.complexity_coef  = r.complexity_coef;
        m.rms_pct          = r.rms_pct;
        return m;
    }

    auto build_session_from_run(bench_dashboard::BenchRun& run, bool is_first) -> bench_dashboard::tui::Session {
        bench_dashboard::tui::Session sess{};
        sess.filter      = std::move(run.filter);
        sess.timestamp   = std::move(run.timestamp);
        sess.is_full_run = run.is_full_run;
        sess.is_raw      = run.is_raw;
        sess.complete    = true;
        sess.expanded    = is_first;
        sess.run_id      = run.id;
        return sess;
    }

    auto load_results_for_run(int run_id) -> std::vector<bench_dashboard::BenchResult> {
        return materialise_select<bench_dashboard::BenchResult>(
                QuerySet<bench_dashboard::BenchResult>()
                        .where(bench_dashboard::fields::BenchResult.run_id == run_id)
                        .order_by<bench_dashboard::fields::BenchResult.id>()
        );
    }

    auto replay_run_results_into_session(
            bench_dashboard::tui::Session&            sess,
            std::vector<bench_dashboard::BenchResult> rows,
            std::int64_t                              baseline_run_id,
            bool                                      baseline_is_raw,
            double                                    regression_threshold
    ) -> void {
        for (auto& r : rows) {
            auto m = build_result_msg_from_row(r);
            enrich_with_baseline(m, baseline_run_id, baseline_is_raw);
            bench_dashboard::tui::add_result(sess, m, regression_threshold, baseline_is_raw);
        }
    }

    auto rebuild_state_from_db(bench_dashboard::tui::DashboardState& state) -> void {
        state.sessions.clear();

        auto runs = QuerySet<bench_dashboard::BenchRun>()
                            .order_by<bench_dashboard::fields::BenchRun.id, false>()
                            .select()
                            .execute()
                            .transform(hive_to_vector_lambda<bench_dashboard::BenchRun>());
        if (!runs)
            return;

        for (auto& run : *runs) {
            auto sess = build_session_from_run(run, /*is_first=*/state.sessions.empty());
            replay_run_results_into_session(
                    sess,
                    load_results_for_run(run.id),
                    state.baseline_run_id,
                    state.baseline_is_raw,
                    state.regression_threshold
            );
            state.sessions.push_back(std::move(sess));
        }
    }

} // namespace
