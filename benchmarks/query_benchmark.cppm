// storm_benchmark_query
//
// SELECT-family benchmark fixture body. `QueryBenchmark<Model, test>` consumes
// a BenchmarkTest NTTP and builds the configured QuerySet + terminal once,
// then `run_terminal()` is called by the Google Benchmark loop body.
//
// Issue #235 — Phase 2.
//
// Was: benchmarks/operations/query_benchmark.hpp (textual header). The module
// conversion drops the raw-SQLite execution path — Storm-only is the new
// contract; raw SQLite anchors live in benchmarks/anchors_raw.cpp (Phase 4).

module;

// `sqlite3` typedef must be textually visible — `storm_benchmark_base` exports
// `get_db<Model>() -> sqlite3*` but the typedef is hidden behind its BMI.
#include <sqlite3.h>
#include <plf_hive/plf_hive.h>

// `<tuple>` is needed by models.hpp (Person::storm_indexes) before
// reflection-annotated structs are visible. Annotations are blind across BMI
// boundaries (clang-p2996 #262, see feedback_cpp26_module_reflection_annotations),
// so model types must be textually visible in the consumer's GMF for any
// reflection-touching instantiation (e.g. QuerySet<User>::insert).
#include <tuple>

#include "models.hpp"

// std::meta:: is used in this module's purview; import std; does not export it
// (issue #326 Finding A), so the reflection header must be textually included.
#include <meta>

export module storm_benchmark_query;

import std;

import storm;
import storm_benchmark_base;
import storm_benchmark_registry;
import storm_benchmark_schema;

// Must follow all imports — storm types must be in scope for the templates.
// TODO: replace with `import storm_orm_query_builder;` once clang-p2996 no
// longer crashes on splice-in-exported-template patterns. Issue #256.
#include "shared/query_builder.hpp" // NOLINT(misc-header-include-cycle)

export namespace storm::benchmark {

    // ========================================================================
    // QueryBenchmark — SELECT-family operations driven by a BenchmarkTest NTTP
    // ========================================================================
    template <typename Model, auto const& test>
    class QueryBenchmark : public DataBenchmarkBase<QueryBenchmark<Model, test>, Model> {
        using Base = DataBenchmarkBase<QueryBenchmark<Model, test>, Model>;

        // WHERE — pick the model the WHERE fields actually live on.
        // Use a local lambda (not the header's has_field<Model>) to avoid the
        // cross-BMI consteval reflection crash in ninja-debug coverage builds.
        static constexpr std::meta::info where_model_ = []() consteval {
            if constexpr (!test.where.enabled || !test.join.enabled) {
                return ^^Model;
            } else {
                for (std::size_t i = 0; i < test.where.stored_node_count(); ++i) {
                    if (!test.where.nodes[i].is_leaf()) {
                        continue; // an AND/OR join names no field
                    }
                    bool found = false;
                    for (std::meta::info const m :
                         std::meta::nonstatic_data_members_of(^^Model, std::meta::access_context::unchecked())) {
                        if (std::meta::identifier_of(m) == test.where.nodes[i].field.view()) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        return ^^registry::FKRelated;
                    }
                }
                return ^^Model;
            }
        }();

        using WhereModel = [:where_model_:];

        static constexpr auto fk_resolver = [](std::string_view name) consteval {
            return registry::resolve_fk_field(name);
        };

        using Builder = storm::orm::query_builder::QueryBuilder<Model, test, fk_resolver>;

        static auto build_qs() {
            if constexpr (std::is_same_v<WhereModel, Model>) {
                return Builder::build_qs();
            } else {
                return Builder::template build_qs<registry::FKRelated>();
            }
        }

        static auto build_terminal(auto& qs) {
            return Builder::build_terminal(qs);
        }

      public:
        explicit constexpr QueryBenchmark(int dataset_size = 1000) : Base(dataset_size) {}

        static auto create_model(int index = 0) -> Model {
            int i = index + 1;
            if constexpr (std::is_same_v<Model, Person>) {
                return Model{
                        .name      = std::format("Person{}", i),
                        .age       = 20 + (i % 50),
                        .salary    = 30000.0 + (i * 1000.0),
                        .is_active = (i % 2 == 0),
                        .score     = (i % 3 == 0) ? std::optional<int>(60 + (i % 40)) : std::nullopt
                };
            } else if constexpr (std::is_same_v<Model, User>) {
                return Model{.id = 0, .name = std::format("User{}", i), .age = 20 + (i % 50)};
            } else {
                return Model{};
            }
        }

        auto prepare(int iterations) -> void {
            if constexpr (test.join.enabled) {
                prepare_join_data();
            } else {
                Base::prepare_with_insert(iterations);
            }
            query_qs_ = build_qs();
            terminal_.emplace(build_terminal(query_qs_));
        }

      private:
        auto prepare_join_data() -> void {
            sqlite3* db = get_db<Model>();
            if (db == nullptr) {
                return;
            }

            int dataset_size = Base::batch_size();

            sqlite3_exec(db, "DELETE FROM FKMessage", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "DELETE FROM User", nullptr, nullptr, nullptr);

            std::vector<registry::FKRelated> users;
            users.reserve(dataset_size);
            for (int i = 0; i < dataset_size; i++) {
                users.push_back(
                        registry::FKRelated{.id = 0, .name = std::format("User{}", i + 1), .age = 20 + (i % 50)}
                );
            }

            if (auto result = related_qs_.insert(users).execute(); !result.has_value()) {
                std::cerr << "Failed to insert users for benchmark\n";
                return;
            }

            auto user_select = related_qs_.select().execute();
            if (!user_select.has_value()) {
                std::cerr << "Failed to select users for benchmark\n";
                return;
            }

            std::vector<std::int64_t> user_ids;
            user_ids.reserve(user_select.value().size());
            for (const auto& user : user_select.value()) {
                user_ids.push_back(user.id);
            }

            std::vector<Model> messages;
            messages.reserve(dataset_size);
            for (int i = 0; i < dataset_size; i++) {
                registry::FKRelated const sender{
                        .id = static_cast<int>(user_ids[i % user_ids.size()]), .name = "", .age = 0
                };
                registry::FKRelated const receiver{
                        .id = static_cast<int>(user_ids[(i + 1) % user_ids.size()]), .name = "", .age = 0
                };
                messages.push_back(
                        Model{.id = 0, .sender = sender, .receiver = receiver, .text = std::format("Message{}", i + 1)}
                );
            }

            auto msg_result = Base::qs().insert(messages).execute();
            if (!msg_result.has_value()) {
                std::cerr << "Failed to insert messages for benchmark\n";
            }
        }

      public:
        // Single-iteration runner — Google Benchmark's `for (auto _ : state)`
        // calls this once per iteration; data + terminal are built in prepare()
        // so the loop body stays tight.
        auto run_once() -> void {
            run_terminal(*terminal_);
        }

      private:
        static auto run_terminal(auto& stmt) -> void {
            (void)stmt.execute();
        }

        using QsType       = decltype(build_qs());
        using TerminalType = decltype(build_terminal(std::declval<QsType&>()));

        QsType                        query_qs_;
        std::optional<TerminalType>   terminal_;
        QuerySet<registry::FKRelated> related_qs_;
    };

} // namespace storm::benchmark
