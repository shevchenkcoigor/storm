#pragma once

using storm::QuerySet;
using storm::orm::statements::ReturnId;

// DB writer (DashboardDB) + baseline resolution helpers for
// storm_bench_dashboard (Issue #247).
//
// Textual header — #included from the anonymous namespace of main.cpp so
// QuerySet calls and models.hpp stay in the same TU (clang-p2996 does not
// transport reflection annotations across BMI boundaries — memory
// feedback_cpp26_module_reflection_annotations). Do NOT include from any
// other TU.
//
// import std; migration (issue #326): no std #includes here — std types come
// from main.cpp's `import std;` (this header is pulled in after it). A textual
// std #include after the module import re-pulls libc++ headers the std module
// owns and breaks the build (Finding B). std::meta:: usage here is served by the
// textual <meta> main.cpp includes before its imports (Finding A).

namespace {

    // -----------------------------------------------------------------------
    // Utility: shell capture + ISO-8601 timestamp
    // -----------------------------------------------------------------------

    auto run_capture(char const* cmd) -> std::string {
        std::unique_ptr<std::FILE, decltype(&pclose)> pipe{::popen(cmd, "r"), &::pclose};
        if (!pipe)
            return {};
        std::string out;
        char        buf[256];
        if (std::fgets(buf, sizeof(buf), pipe.get()) != nullptr)
            out.assign(buf);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return out;
    }

    auto current_iso8601() -> std::string {
        const auto now = std::chrono::system_clock::now();
        const auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm    tm_utc{};
        ::gmtime_r(&t, &tm_utc);
        char buf[32]{};
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        return std::string{buf};
    }

    // -----------------------------------------------------------------------
    // Baseline resolution
    // -----------------------------------------------------------------------

    // Materialise a QuerySet result (plf::hive) into a std::vector<T> by
    // moving elements out. Single source of truth used across all baseline
    // and state-rebuild query paths.
    template <typename T> auto hive_to_vector_lambda() {
        return [](auto&& hive) {
            return std::forward<decltype(hive)>(hive) | std::views::as_rvalue | std::ranges::to<std::vector<T>>();
        };
    }

    // Execute qs and materialise into std::vector<T>. Returns empty on failure.
    template <typename T, typename QS> auto materialise_select(QS&& qs) -> std::vector<T> {
        auto rows = std::forward<QS>(qs).select().execute().transform(hive_to_vector_lambda<T>());
        return rows ? std::move(*rows) : std::vector<T>{};
    }

    // Fetch at most one BenchResult row from qs.
    template <typename QS> auto fetch_first_result_row(QS&& qs) -> std::vector<bench_dashboard::BenchResult> {
        return materialise_select<bench_dashboard::BenchResult>(std::forward<QS>(qs).limit(1));
    }

    struct ResolvedBaseline {
        std::int64_t run_id{0};
        std::string  label;
        bool         is_raw{false};
    };

    // Take the newest matching BenchRun for `qs` (ordered by id desc).
    template <typename QS> auto newest_run(QS qs) -> ResolvedBaseline {
        auto rows = qs.template order_by<bench_dashboard::fields::BenchRun.id, false>()
                            .limit(1)
                            .select()
                            .execute()
                            .transform(hive_to_vector_lambda<bench_dashboard::BenchRun>());
        if (!rows || rows->empty())
            return {};
        const auto& r   = rows->front();
        const auto  lbl = std::format("Run #{} {} {}", r.id, r.branch, r.git_hash);
        return {.run_id = r.id, .label = lbl, .is_raw = r.is_raw};
    }

    // Newest run matching the boolean flag `field` plus current branch+host.
    // `flag` is the reflected BenchRun bool column (is_full_run or is_raw).
    template <auto Flag>
    auto newest_flagged_run(std::string_view current_branch, std::string_view current_host) -> ResolvedBaseline {
        return newest_run(
                QuerySet<bench_dashboard::BenchRun>()
                        .where(Flag == true)
                        .where(bench_dashboard::fields::BenchRun.branch == std::string{current_branch})
                        .where(bench_dashboard::fields::BenchRun.hostname == std::string{current_host})
        );
    }

    // Newest run with the given id.
    auto run_by_id(std::int64_t id) -> ResolvedBaseline {
        return newest_run(
                QuerySet<bench_dashboard::BenchRun>().where(
                        bench_dashboard::fields::BenchRun.id == static_cast<int>(id)
                )
        );
    }

    // Resolve the baseline selector to a concrete (run_id, label, is_raw).
    // Returns {} when no matching run is found or selector is None.
    auto resolve_baseline(BaselineSelector const& sel, std::string_view current_branch, std::string_view current_host)
            -> ResolvedBaseline {
        if (std::holds_alternative<BaselineAuto>(sel))
            return newest_flagged_run<bench_dashboard::fields::BenchRun.is_full_run>(current_branch, current_host);

        if (const auto* r = std::get_if<BaselineRunId>(&sel))
            return run_by_id(r->id);

        if (const auto* b = std::get_if<BaselineBranch>(&sel))
            return newest_run(
                    QuerySet<bench_dashboard::BenchRun>()
                            .where(bench_dashboard::fields::BenchRun.is_full_run == true)
                            .where(bench_dashboard::fields::BenchRun.branch == b->name)
            );

        if (const auto* rw = std::get_if<BaselineRaw>(&sel)) {
            if (rw->id > 0)
                return run_by_id(rw->id);
            // raw:last — most recent raw run, same branch+host (mirrors auto).
            return newest_flagged_run<bench_dashboard::fields::BenchRun.is_raw>(current_branch, current_host);
        }

        return {};
    }

    // Look up the baseline real_time_ns for (run_id, test_name, dataset_size).
    auto lookup_baseline_ns(std::int64_t baseline_run_id, std::string const& test_name, std::int64_t dataset_size)
            -> std::optional<double> {
        auto rows = fetch_first_result_row(
                QuerySet<bench_dashboard::BenchResult>()
                        .where(bench_dashboard::fields::BenchResult.run_id == static_cast<int>(baseline_run_id))
                        .where(bench_dashboard::fields::BenchResult.test_name == test_name)
                        .where(bench_dashboard::fields::BenchResult.dataset_size == static_cast<int>(dataset_size))
        );
        if (rows.empty())
            return std::nullopt;
        return rows.front().real_time_ns;
    }

    auto compute_delta(double current_ns, double baseline_ns) -> std::optional<double> {
        if (baseline_ns <= 0.0)
            return std::nullopt;
        return (current_ns - baseline_ns) / baseline_ns * 100.0;
    }

    struct BaselineComplexity {
        std::string complexity_class;
        double      complexity_coef{0.0};
    };

    // Look up the baseline BigO row for (run_id, test_name, row_kind=="bigo").
    auto lookup_baseline_complexity(std::int64_t baseline_run_id, std::string const& test_name)
            -> std::optional<BaselineComplexity> {
        auto rows = fetch_first_result_row(
                QuerySet<bench_dashboard::BenchResult>()
                        .where(bench_dashboard::fields::BenchResult.run_id == static_cast<int>(baseline_run_id))
                        .where(bench_dashboard::fields::BenchResult.test_name == test_name)
                        .where(bench_dashboard::fields::BenchResult.row_kind ==
                               std::string{bench_dashboard::wire::kRowKindBigO})
        );
        if (rows.empty())
            return std::nullopt;
        return BaselineComplexity{rows.front().complexity_class, rows.front().complexity_coef};
    }

    auto print_missing_schema_hint(std::string_view db_path) -> void {
        std::fprintf(
                stderr,
                "\n"
                "  The dashboard schema is missing. Bootstrap it with:\n"
                "\n"
                "      cmake --build <build-dir> --target makemigrations   # once, after model changes\n"
                "      atlas migrate apply \\\n"
                "          --dir 'file://benchmarks/dashboard/migrations' \\\n"
                "          --url 'sqlite://%.*s'\n",
                static_cast<int>(db_path.size()),
                db_path.data()
        );
    }

    // -----------------------------------------------------------------------
    // DashboardDB — Storm-side writer
    //
    // Kept textual so models.hpp + QuerySet calls stay in the same TU as
    // `import storm;` (clang-p2996 BMI annotation transport is unreliable).
    // -----------------------------------------------------------------------

    class DashboardDB {
      public:
        // git_hash + branch are volatile: they change whenever the developer
        // checks out a branch or commits while the daemon keeps running. They
        // MUST be re-read per BenchRun, not cached at startup (Issue #267).
        struct GitContext {
            std::string git_hash;
            std::string branch;
        };

        using GitCapture = std::function<GitContext()>;

        // Default: read the working tree via git. Injectable for tests.
        DashboardDB() : git_capture_{&capture_git_context} {}
        explicit DashboardDB(GitCapture git_capture) : git_capture_{std::move(git_capture)} {}

        [[nodiscard]] auto insert_run(std::string_view filter, bool is_full_run, bool is_raw)
                -> std::expected<std::int64_t, std::string> {
            ensure_host_context();

            // Re-read git on every run — the working tree may have moved since
            // the daemon started or since the previous run (Issue #267).
            const GitContext git = git_capture_();

            bench_dashboard::BenchRun row{};
            row.git_hash    = git.git_hash;
            row.branch      = git.branch;
            row.timestamp   = current_iso8601();
            row.hostname    = host_.hostname;
            row.compiler    = host_.compiler;
            row.filter      = std::string{filter};
            row.is_full_run = is_full_run;
            row.is_raw      = is_raw;

            auto rc = QuerySet<bench_dashboard::BenchRun>().insert(row).execute();
            if (!rc)
                return std::unexpected(std::string{rc.error().message()});
            return *rc;
        }

        auto insert_result(std::int64_t run_id, bench_dashboard::wire::ResultMsg const& m)
                -> std::expected<void, std::string> {
            bench_dashboard::BenchResult row{};
            row.run_id           = static_cast<int>(run_id);
            row.test_name        = m.test_name;
            row.category         = m.category;
            row.dataset_size     = static_cast<int>(m.dataset_size);
            row.row_kind         = m.row_kind;
            row.real_time_ns     = m.real_ns;
            row.cpu_time_ns      = m.cpu_ns;
            row.iterations       = static_cast<int>(m.iterations);
            row.items_per_second = m.items_per_second;
            row.complexity_class = m.complexity_class;
            row.complexity_coef  = m.complexity_coef;
            row.rms_pct          = m.rms_pct;

            QuerySet<bench_dashboard::BenchResult> qs;
            auto                                   rc = qs.template insert<ReturnId::No>(row).execute();
            if (!rc)
                return std::unexpected(std::string{rc.error().message()});
            return {};
        }

        // Read the current working-tree git state. Default GitCapture impl.
        static auto capture_git_context() -> GitContext {
            return {.git_hash = run_capture("git rev-parse --short HEAD 2>/dev/null"),
                    .branch   = run_capture("git rev-parse --abbrev-ref HEAD 2>/dev/null")};
        }

      private:
        // Host-level metadata that is stable for the daemon's whole lifetime —
        // unlike git state, hostname and compiler never change mid-run, so they
        // stay cached.
        struct HostContext {
            std::string hostname;
            std::string compiler;
        };

        auto ensure_host_context() -> void {
            if (host_initialised_)
                return;
            host_initialised_ = true;
            host_.hostname    = run_capture("hostname -s 2>/dev/null");
            host_.compiler    = std::format("Clang {}.{}", __clang_major__, __clang_minor__);
        }

        GitCapture  git_capture_;
        HostContext host_{};
        bool        host_initialised_{false};
    };

} // namespace
