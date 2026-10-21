// storm_benchmark_base
//
// CRTP base class for data-driven benchmarks (Insert / UpdateByPK / Delete /
// Select). Wraps the QuerySet / plf_hive / SQLite-binding plumbing shared by
// all operation benchmarks.
//
// Living in a module keeps `<plf_hive/plf_hive.h>` (which transitively pulls
// `<cassert>` → `<__config>`) inside this module's compile context. That
// preprocessor state never leaks into main.cpp, sidestepping the
// `_LIBCPP_HAS_THREAD_API_PTHREAD` macro-redefinition + PCM-corruption trap
// that fires when the same chain is textually included into a TU that also
// imports the std module.

module;

// sqlite3.h provides the `sqlite3` and `sqlite3_stmt` typedefs used in this
// module's purview signatures.
#include <sqlite3.h>
#include <plf_hive/plf_hive.h>

// std::meta::identifier_of is used in this module's purview; import std; does not
// export std::meta:: (issue #326 Finding A), so the reflection header must be
// textually included in the global module fragment.
#include <meta>

export module storm_benchmark_base;

import std;

import storm;

export namespace storm::benchmark {

    // ========================================================================
    // get_db: Helper to get raw sqlite3* from default connection
    // ========================================================================
    template <typename Model> auto get_db() -> sqlite3* {
        const auto& conn = storm::db::DefaultConnection<>::get();
        return conn->get();
    }

    // CRTP base class for data-driven benchmarks (Insert, UpdateByPK)
    // BatchSize is now a runtime parameter for fair comparison with Storm ORM
    template <typename Derived, typename Model> class DataBenchmarkBase {
      private:
        QuerySet<Model>    qs_;
        std::vector<Model> data_;
        int                batch_size_ = 1; // Runtime batch size

      protected:
        // Accessor methods for derived classes
        auto qs() -> QuerySet<Model>& {
            return qs_;
        }
        auto qs() const -> const QuerySet<Model>& {
            return qs_;
        }
        auto data() -> std::vector<Model>& {
            return data_;
        }
        auto data() const -> const std::vector<Model>& {
            return data_;
        }
        auto batch_size() const -> int {
            return batch_size_;
        }
        auto set_batch_size(int size) -> void {
            batch_size_ = size;
        }

        // Default model creation - derived classes can override via static method hiding
        // index parameter allows generating varied data (useful for SELECT WHERE benchmarks)
        static auto create_model(int index = 0) -> Model {
            return Model{
                    .name      = std::format("Person{}", index),
                    .age       = 20 + (index % 50),
                    .salary    = 30000.0 + (index * 1000.0),
                    .is_active = (index % 2 == 0)
            };
        }

      public:
        // Constructor with optional batch size
        explicit DataBenchmarkBase(int batch_size = 1) : batch_size_(batch_size) {}

        // Basic prepare: generates test data only (for INSERT benchmark)
        auto prepare(int iterations) -> void {
            data().clear();
            int count = (batch_size_ == 1) ? iterations : batch_size_;
            data().reserve(count);
            for (int i = 0; i < count; i++) {
                data().push_back(Derived::create_model(i));
            }
        }

        // Extended prepare: clears table, generates data, inserts and retrieves IDs
        // Used by UPDATE and DELETE benchmarks that need existing rows with valid PKs
        auto prepare_with_insert(int iterations) -> void {
            // 1. Clear table using raw SQLite
            if (sqlite3* db = get_db<Model>()) {
                auto delete_sql = std::format("DELETE FROM {}", std::meta::identifier_of(^^Model));
                sqlite3_exec(db, delete_sql.c_str(), nullptr, nullptr, nullptr);
            }

            // 2. Generate test data
            prepare(iterations);

            // 3. INSERT data
            auto insert_result = qs().insert(data()).execute();
            if (!insert_result.has_value()) {
                std::cerr << "Failed to insert test data for benchmark\n";
                return;
            }

            // 4. SELECT back to get the auto-generated IDs
            auto select_result = qs().select().execute();
            if (!select_result.has_value()) {
                std::cerr << "Failed to select test data for benchmark\n";
                return;
            }

            // 5. Store retrieved IDs back into data
            const auto& selected = select_result.value();
            std::size_t i        = 0;
            for (const auto& row : selected) {
                if (i >= data().size()) {
                    break;
                }
                data()[i].id = row.id;
                i++;
            }
        }
    };

} // namespace storm::benchmark
