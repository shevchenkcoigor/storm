module;

#include <meta>

export module storm_orm_reflection_cache;

import std;

// Dependency-free leaf module (#694): memoisation for consteval reflection queries.
//
// A consteval call is re-evaluated every time it runs, and libc++'s <meta> queries are
// ordinary library code run by the constant evaluator — annotations_of builds a vector
// on every call (~0.4 ms). A variable template is instantiated once per argument list,
// so a predicate whose answer is stored in one is computed once per member per TU, and
// every later call reuses it.
//
// memoised<R> reaches such a variable template from arguments that are not constant
// expressions (a predicate's `std::meta::info member` parameter): substitute
// instantiates the specialisation on first use, extract reads its value.
//
// Memoise VALUES (bool, std::optional<info>, std::optional<std::size_t>, std::array),
// never define_static_array storage: a specialisation instantiated inside a module BMI
// is reused by every importer, and a span into define_static_array storage is not a
// constant expression there (an earlier draft of #694 broke storm_bench that way).
// Values cross BMIs like any static constexpr member does. Imported, not re-exported, by
// storm_orm_field_attr and storm_orm_relation_meta.
export namespace storm::meta {

    template <typename R>
    [[nodiscard]] consteval auto memoised(std::meta::info var_template, std::initializer_list<std::meta::info> args)
            -> R {
        return std::meta::extract<R>(std::meta::substitute(var_template, args));
    }

} // namespace storm::meta
