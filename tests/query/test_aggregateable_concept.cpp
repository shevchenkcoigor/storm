#include <gtest/gtest.h>
#include <plf_hive/plf_hive.h>
#include "test_db_helpers.h"

import storm;
import storm_orm_utilities; // storm::orm::utilities::UUID (canonical name of storm::UUID) — not re-exported by `storm`
import storm_orm_statements_aggregate;
import std;

#include "../../shared/models/color.h" // NOSONAR cpp:S954
#include "../../shared/models/person.h"

// Compile-time-only verification of the NumericAggregateable concept (#475).
// Every static_assert below is checked at TU compile time; the runtime TEST body
// only exists so the file registers with GoogleTest and the assertions compile.

using storm::orm::statements::NumericAggregateable;

// ---- Positive: arithmetic (non-bool) field types are numeric-aggregateable ---

static_assert(NumericAggregateable<int>);
static_assert(NumericAggregateable<short>);
static_assert(NumericAggregateable<unsigned int>);
static_assert(NumericAggregateable<std::int64_t>);
static_assert(NumericAggregateable<long>);
static_assert(NumericAggregateable<long long>);
static_assert(NumericAggregateable<std::uint64_t>);
static_assert(NumericAggregateable<double>);
static_assert(NumericAggregateable<float>);
static_assert(NumericAggregateable<signed char>);
static_assert(NumericAggregateable<char>);

// A nullable numeric column (std::optional<Numeric>) is a legitimate target —
// SQL simply skips NULLs. One level of optional is unwrapped.
static_assert(NumericAggregateable<std::optional<int>>);
static_assert(NumericAggregateable<std::optional<double>>);
static_assert(NumericAggregateable<std::optional<std::int64_t>>);

// ---- Negative: bool, text, BLOB, enum, and unrelated types are rejected ------

// bool is arithmetic in C++, but SUM/AVG/MIN/MAX over a boolean column is a
// modelling mistake, not a real query — deliberately excluded.
static_assert(!NumericAggregateable<bool>);
static_assert(!NumericAggregateable<std::string>);
static_assert(!NumericAggregateable<std::string_view>);
static_assert(!NumericAggregateable<std::vector<std::uint8_t>>);
static_assert(!NumericAggregateable<Color>); // enum, from shared/models/color.h
static_assert(!NumericAggregateable<void*>);
static_assert(!NumericAggregateable<std::optional<std::string>>); // optional of non-numeric
static_assert(!NumericAggregateable<std::optional<bool>>);        // optional of bool

// Temporal / UUID columns are storable & WHERE-filterable but NOT numeric —
// SUM/AVG/MIN/MAX over them is meaningless, so they are rejected (incl. optional).
static_assert(!NumericAggregateable<storm::orm::utilities::UUID>);
static_assert(!NumericAggregateable<std::chrono::system_clock::time_point>);
static_assert(!NumericAggregateable<std::chrono::year_month_day>);
static_assert(!NumericAggregateable<std::optional<storm::orm::utilities::UUID>>);
static_assert(!NumericAggregateable<std::optional<std::chrono::system_clock::time_point>>);

// Iterable / container field types (BLOBs and m2m/reverse-fk relation members
// such as std::vector<Model> / plf::hive<Model>) are never a numeric aggregate
// target — a container is not arithmetic, and the optional unwrap does not reach
// into element types. Rejected regardless of element type.
static_assert(!NumericAggregateable<std::vector<int>>);
static_assert(!NumericAggregateable<std::vector<double>>);
static_assert(!NumericAggregateable<plf::hive<int>>);
static_assert(!NumericAggregateable<std::array<int, 3>>);

// ---- Method-level rejection ---------------------------------------------------
// A numeric aggregate over a non-numeric field must NOT be instantiable. The
// variable template makes the requires-clause failure SFINAE-soft rather than a
// hard error (the #472 wrapper trick).

template <typename QS>
concept min_name_ok = requires(QS qs) { qs.template min<fields::Person.name>(); };
template <typename QS>
concept min_age_ok = requires(QS qs) { qs.template min<fields::Person.age>(); };
template <typename QS>
concept sum_avatar_ok = requires(QS qs) { qs.template sum<fields::Person.avatar>(); };
template <typename QS>
concept sum_salary_ok = requires(QS qs) { qs.template sum<fields::Person.salary>(); };
template <typename QS>
concept max_active_ok = requires(QS qs) { qs.template max<fields::Person.is_active>(); };

using PersonQS = storm::QuerySet<Person>;

// Numeric targets are accepted.
static_assert(min_age_ok<PersonQS>);
static_assert(sum_salary_ok<PersonQS>);

// Non-numeric targets are rejected.
static_assert(!min_name_ok<PersonQS>);   // std::string
static_assert(!sum_avatar_ok<PersonQS>); // std::vector<uint8_t> (BLOB)
static_assert(!max_active_ok<PersonQS>); // bool

// count / count_distinct stay unconstrained — any field type is countable.
template <typename QS>
concept count_distinct_name_ok = requires(QS qs) { qs.template count_distinct<fields::Person.name>(); };
static_assert(count_distinct_name_ok<PersonQS>);

TEST(AggregateableConceptTest, CompileTimeOnly) {
    // The verification is entirely in the static_asserts above; this body just
    // gives GoogleTest something to run.
    SUCCEED();
}
