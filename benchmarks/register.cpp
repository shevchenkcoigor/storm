// register.cpp — owns the storm-importing side of the bench bridge.
//
// Imports storm + storm_benchmark_query, walks BENCHMARK_TESTS, and emits a
// `RegisteredBenchmark` entry per SELECT-family test into a process-wide
// vector consumed by main.cpp. main.cpp does the actual gbench registration;
// this TU never sees `<benchmark/benchmark.h>`.
//
// Why this split exists: see benchmarks/CMakeLists.txt — putting both
// imports and benchmark.h in one TU produces a PCM-cache hash divergence
// vs libstorm.a (project_pcm_hash_divergence) and trips a fatal at any
// `import storm` site.
//
// Trampoline lifetime: each (Model, test) pair owns one process-lifetime
// `QueryBenchmark<Model, test>` instance via a function-local static. setup(N)
// re-binds dataset and rebuilds the Storm terminal; run() executes the cached
// terminal once. That keeps the gbench loop body tight (no per-iteration
// allocation, no map/lookup, just an indirect call).

// `<tuple>` and models.hpp must be visible textually before any `import` —
// reflection annotations on the model structs are blind across BMI boundaries
// (clang-p2996 #262, feedback_cpp26_module_reflection_annotations) so any TU
// that instantiates an ORM template parameterized on a model type must see
// the annotations textually.
#include <sqlite3.h> // m2m bench fixture (#391) clears tables via raw sqlite3_exec
#include <tuple>
#include "models.hpp"
#include "m2m_models.hpp" // m2m bench models — after models.hpp (needs import storm; + <string>/<vector>)

#include "bench_register.h"
#include "benchmark_tests.hpp" // BENCHMARK_TESTS — must stay textual

import storm;
import std;
import storm_benchmark_crud;
import storm_benchmark_query;
import storm_benchmark_registry;
import storm_benchmark_schema;
import storm_benchmark_sizes;

namespace storm::benchmark {

    auto registered_benchmarks() -> std::vector<RegisteredBenchmark>& {
        static std::vector<RegisteredBenchmark> v;
        return v;
    }

    auto initialize_db() -> bool {
        auto open = storm::db::DefaultConnection<>::set(":memory:");
        if (!open.has_value()) {
            return false;
        }
        const auto& conn = storm::db::DefaultConnection<>::get();
        if (!storm::orm::schema::SchemaStatement<Person>::create_table_if_not_exists(conn).has_value()) {
            return false;
        }
        if (!storm::orm::schema::SchemaStatement<BenchPerson>::create_table_if_not_exists(conn).has_value()) {
            return false;
        }
        if (!storm::orm::schema::SchemaStatement<User>::create_table_if_not_exists(conn).has_value()) {
            return false;
        }
        if (!storm::orm::schema::SchemaStatement<FKMessage>::create_table_if_not_exists(conn).has_value()) {
            return false;
        }
        // m2m bench tables (#391). SchemaStatement<BStudent> auto-creates the
        // BStudent_BCourse junction table (#203).
        using storm::benchmark::m2m::BCourse;
        using storm::benchmark::m2m::BStudent;
        if (!storm::orm::schema::SchemaStatement<BCourse>::create_table_if_not_exists(conn).has_value()) {
            return false;
        }
        return storm::orm::schema::SchemaStatement<BStudent>::create_table_if_not_exists(conn).has_value();
    }

} // namespace storm::benchmark

namespace {

    using storm::benchmark::BenchmarkTest;
    using storm::benchmark::CrudBenchmark;
    using storm::benchmark::QueryBenchmark;
    using storm::benchmark::RegisteredBenchmark;

    consteval auto is_crud(BenchmarkTest const& t) -> bool {
        constexpr std::array<std::string_view, 4> crud = {"insert", "insert_no_return", "update_pk", "delete_pk"};
        return std::ranges::any_of(crud, [&](std::string_view op) { return t.operation.view() == op; });
    }

    struct RangeSpec {
        // Range mode (sized=true) → RangeMultiplier+Range
        // Args mode (sized=false, !args.empty()) → explicit Arg() per element
        // Single mode (sized=false, args.empty()) → single Arg(lo)
        bool         sized;
        std::int64_t lo;
        std::int64_t hi;
        int          multiplier;
    };

    consteval auto range_for(BenchmarkTest const& t) -> RangeSpec {
        if (t.size_profile.view() == "dataset_standard") {
            return {.sized = true, .lo = 100, .hi = 100'000, .multiplier = 10};
        }
        if (t.size_profile.view() == "dataset_small") {
            return {.sized = true, .lo = 10'000, .hi = 50'000, .multiplier = 5};
        }
        return {.sized = false, .lo = t.dataset_size, .hi = t.dataset_size, .multiplier = 1};
    }

    // CRUD profiles use non-power-of-N sizes (1,10,100,500,1000,...) — gbench's
    // RangeMultiplier can't span them, so we emit each as an explicit Arg.
    // Returns a runtime span pointing into the inline-constexpr arrays in
    // storm_benchmark_sizes (program-lifetime storage).
    inline auto args_for(BenchmarkTest const& t) -> std::span<const int> {
        const std::string_view sp = t.size_profile.view();
        if (sp == "batch_standard") {
            return {storm::benchmark::sizes::BATCH_STANDARD};
        }
        if (sp == "batch_insert_edge") {
            return {storm::benchmark::sizes::BATCH_INSERT_EDGE};
        }
        if (sp == "batch_update_edge") {
            return {storm::benchmark::sizes::BATCH_UPDATE_EDGE};
        }
        return {};
    }

    // Per-(Fixture, Model, test) trampoline — one set of free functions and
    // one function-local static fixture instance per (Fixture, Model, test)
    // tuple, generated by the template. Pointer addresses are stable for the
    // program's lifetime, which is what gbench wants from `void(*)()`.
    // Both fixtures are move-only (QuerySet holds a unique_ptr to its
    // statement cache), so re-binding setup() goes through std::optional's
    // emplace — destroy + in-place construct, no assignment required.
    // Shared trampoline plumbing keyed on the concrete fixture TYPE: a
    // function-local-static optional<Fixture> (stable address for gbench's
    // void(*)()) plus the one-line run() that drives it. Each family supplies
    // its own setup() since the construct/prepare signatures differ
    // (BenchmarkTest fixtures take N; the m2m fixture is N-free).
    template <typename Fixture> struct TrampolineBase {
        static auto storage() -> std::optional<Fixture>& {
            static std::optional<Fixture> opt;
            return opt;
        }
        static auto run() -> void {
            storage()->run_once();
        }
    };

    template <template <typename, auto const&> class Fixture, typename Model, auto const& test>
    struct Trampoline : TrampolineBase<Fixture<Model, test>> {
        using Base = TrampolineBase<Fixture<Model, test>>;
        static auto setup(std::int64_t n) -> void {
            auto& opt = Base::storage();
            opt.emplace(static_cast<int>(n));
            opt->prepare(static_cast<int>(n));
        }
    };

    template <template <typename, auto const&> class Fixture, typename Model, auto const& test>
    auto register_one() -> void {
        constexpr auto rng  = range_for(test);
        auto           args = args_for(test);
        std::string    name = std::format(
                "Storm/{}/{}", std::string_view(test.test_category.view()), std::string_view(test.test_name.view())
        );
        std::vector<std::int64_t> args_vec;
        args_vec.reserve(args.size());
        for (auto n : args) {
            args_vec.push_back(n);
        }

        storm::benchmark::registered_benchmarks().push_back(
                RegisteredBenchmark{
                        .name             = std::move(name),
                        .setup            = &Trampoline<Fixture, Model, test>::setup,
                        .run              = &Trampoline<Fixture, Model, test>::run,
                        .sized            = rng.sized,
                        .range_lo         = rng.lo,
                        .range_hi         = rng.hi,
                        .range_multiplier = rng.multiplier,
                        .args             = std::move(args_vec),
                }
        );
    }

    // ---- Many-to-many eager-load benchmark (#391) ------------------------
    // Defined here in register.cpp's purview (not a .cppm) because the m2m
    // reflecting instantiation — `join<m2m::fields::BStudent.courses>().select()` on the
    // annotated models — segfaults the BMI serializer when it lives in a module
    // purview (clang-p2996 #262 family). The existing QueryBenchmark gets away
    // with a .cppm only because its splice instantiation is deferred to here.

    using storm::benchmark::m2m::BCourse;
    using storm::benchmark::m2m::BStudent;

    // Fixed base-entity count. #391's raw experiment used N=100; the win is
    // scale-invariant in fan-out, so a single base size isolates the fan-out
    // effect the rewrite targets.
    inline constexpr int kBaseStudents = 100;

    // Fan-out (courses per student) is the swept dimension, baked into the
    // fixture's template NTTP K. setup() seeds N students, K courses, N*K
    // junction rows; run() executes the eager m2m join+select once.
    template <int K> class M2MBenchmark {
        storm::QuerySet<BStudent> sqs_;
        storm::QuerySet<BCourse>  cqs_;

      public:
        auto prepare() -> void {
            sqlite3* db = storm::db::DefaultConnection<>::get()->get();
            sqlite3_exec(db, "DELETE FROM BStudent_BCourse", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "DELETE FROM BStudent", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "DELETE FROM BCourse", nullptr, nullptr, nullptr);

            std::vector<BCourse> courses;
            courses.reserve(K);
            for (int c = 0; c < K; ++c) {
                courses.push_back(BCourse{.id = 0, .title = std::format("Course{}", c)});
            }
            if (!cqs_.insert(courses).execute().has_value()) {
                std::cerr << "m2m bench: course insert failed\n";
                return;
            }

            std::vector<BStudent> students;
            students.reserve(kBaseStudents);
            for (int s = 0; s < kBaseStudents; ++s) {
                students.push_back(BStudent{.id = 0, .name = std::format("Student{}", s), .age = 20 + (s % 50)});
            }
            if (!sqs_.insert(students).execute().has_value()) {
                std::cerr << "m2m bench: student insert failed\n";
                return;
            }

            // Junction: every student linked to all K courses (uniform fan-out).
            std::string sql   = "INSERT INTO BStudent_BCourse (BStudent_id, BCourse_id) VALUES ";
            bool        first = true;
            for (int s = 1; s <= kBaseStudents; ++s) {
                for (int c = 1; c <= K; ++c) {
                    if (!first) {
                        sql += ", ";
                    }
                    sql += std::format("({}, {})", s, c);
                    first = false;
                }
            }
            if (!storm::db::DefaultConnection<>::get()->execute(sql).has_value()) {
                std::cerr << "m2m bench: junction insert failed\n";
            }
        }

        auto run_once() -> void {
            (void)sqs_.template join<storm::benchmark::m2m::fields::BStudent.courses>().select().execute();
        }
    };

    // Shares storage()/run() with TrampolineBase; only setup() differs (the m2m
    // fixture is N-free, constructed and prepared with no args).
    template <int K> struct M2MTrampoline : TrampolineBase<M2MBenchmark<K>> {
        using Base = TrampolineBase<M2MBenchmark<K>>;
        static auto setup(std::int64_t /*n*/) -> void {
            Base::storage().emplace();
            Base::storage()->prepare();
        }
    };

    template <int K> auto register_m2m_fanout() -> void {
        storm::benchmark::registered_benchmarks().push_back(
                RegisteredBenchmark{
                        .name             = std::format("Storm/M2M/join_select/fanout{}", K),
                        .setup            = &M2MTrampoline<K>::setup,
                        .run              = &M2MTrampoline<K>::run,
                        .sized            = false,
                        .range_lo         = K,
                        .range_hi         = K,
                        .range_multiplier = 1,
                        .args             = {},
                }
        );
    }

    auto register_m2m() -> void {
        register_m2m_fanout<1>();
        register_m2m_fanout<10>();
        register_m2m_fanout<50>();
        register_m2m_fanout<200>();
    }

    template <std::size_t I = 0> auto register_all() -> void {
        if constexpr (I < storm::benchmark::BENCHMARK_TESTS.size()) {
            constexpr auto const& t = storm::benchmark::BENCHMARK_TESTS[I];
            if constexpr (is_crud(t)) {
                storm::benchmark::registry::with_base_model<t>([]<typename M>() {
                    register_one<CrudBenchmark, M, t>();
                });
            } else {
                storm::benchmark::registry::with_base_model<t>([]<typename M>() {
                    register_one<QueryBenchmark, M, t>();
                });
            }
            register_all<I + 1>();
        }
    }

} // namespace

namespace storm::benchmark {

    auto build_registration_table() -> void {
        registered_benchmarks().clear();
        register_all();
        register_m2m();
    }

} // namespace storm::benchmark
