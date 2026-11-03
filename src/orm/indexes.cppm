module;

export module storm_orm_indexes;

import std;
import storm_std_meta;

export namespace storm {

    // Composite index types — variadic on std::meta::info field reflections
    template <std::meta::info... Fields> struct Index {
        static constexpr auto fields = std::array{Fields...};
        static constexpr bool unique = false;
    };

    template <std::meta::info... Fields> struct UniqueIndex {
        static constexpr auto fields = std::array{Fields...};
        static constexpr bool unique = true;
    };

    // Default trait — no composite indexes. Users specialize for their models.
    template <typename T> struct Indexes {
        using type = std::tuple<>;
    };

    // Nested-typedef opt-in — models declare `using storm_indexes = std::tuple<...>;`
    // inside the struct instead of specializing Indexes<T>. Required when the model
    // header is textually included in multiple module TUs (see issue #464).
    template <typename T>
        requires requires { typename T::storm_indexes; }
    struct Indexes<T> {
        using type = typename T::storm_indexes;
    };

    template <typename T> using indexes_t = typename Indexes<T>::type;

} // namespace storm
