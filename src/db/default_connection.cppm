module;

#include <cstdio>  // stderr
#include <cstdlib> // std::abort (also not a macro, but harmless to include)

export module storm_db_default_connection;

import std;
import storm_db_concept;
import storm_db_pool;

export namespace storm::db {

    template <DatabaseConnection ConnType> class BasicDefaultConnection {
      public:
        using Error = typename ConnType::Error;
        using Pool  = ConnectionPool<ConnType>;

        BasicDefaultConnection() = delete; // static-only facade

        // Open a new connection on the calling thread and make it the default.
        [[nodiscard]] static auto set(std::string_view db_path) -> std::expected<void, Error> {
            auto r = ConnType::open(db_path);
            if (!r) {
                return std::unexpected(r.error());
            }
            auto p = std::make_shared<ConnType>(std::move(*r));
            if constexpr (requires { p->prepare_common_statements(); }) {
                p->prepare_common_statements();
            }
            slot() = std::move(p);
            return {};
        }

        // Check out a connection from the pool into the calling thread's slot.
        [[nodiscard]] static auto set(Pool& pool) -> std::expected<void, Error> {
            auto c = pool.checkout();
            if (!c) {
                return std::unexpected(c.error());
            }
            slot() = std::move(*c);
            return {};
        }

        // Returns a COPY (#357): a held pointer survives a later clear()/set().
        // Aborts in every build mode if no default is available (a null connection
        // would otherwise crash later, far from the cause).
        [[nodiscard]] static auto get() -> std::shared_ptr<ConnType> {
            auto& s = slot();
            // LCOV_EXCL_START — aborts the process; not reachable from a test
            if (!s) [[unlikely]] {
                std::println(stderr, "storm: default connection not set on this thread");
                std::abort();
            }
            // LCOV_EXCL_STOP
            return s;
        }

        // Non-aborting variant: may return null.
        [[nodiscard]] static auto try_get() noexcept -> std::shared_ptr<ConnType> {
            return slot();
        }

        [[nodiscard]] static auto has() noexcept -> bool {
            return static_cast<bool>(slot());
        }

        static auto clear() noexcept -> void {
            slot().reset();
        }

      private:
        static auto slot() noexcept -> std::shared_ptr<ConnType>& {
            static thread_local std::shared_ptr<ConnType> conn_;
            return conn_;
        }
    };

} // namespace storm::db
