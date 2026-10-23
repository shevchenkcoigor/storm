// storm_benchmark_schema
//
// Compile-time C++ schema mirroring the YAML/JSON benchmark test descriptors.
// Provides BenchmarkTest plus its nested sub-specs (WhereSpec, OrderBySpec,
// HavingSpec, GroupBySpec, DistinctSpec, LimitSpec, AggregateSpec, JoinSpec,
// SetOpSpec) and the TypedValue tagged-union for sniffed JSON leaf values.
//
// Was: benchmarks/schema.hpp (textual header that re-defined a local
// `ConstexprString<N>`). Issue #221 — Phase 3 of the benchmark module
// conversion.
//
// DRY win
// -------
// The local ConstexprString is gone — we re-export
// `storm::orm::utilities::ConstexprString` into the `storm::benchmark`
// namespace, removing the duplication noted in Issue #204. The ORM template
// is API-compatible (ctor, view, c_str, empty, operator==, public data/len)
// and uses `constexpr` rather than `consteval`, which is strictly more
// permissive — every consteval call site continues to work.

module;

export module storm_benchmark_schema;

import std;

// Pull ConstexprString via the umbrella `storm` module rather than reaching
// directly into `storm_orm_utilities`. Importing the leaf submodule alone
// produced a `clang-scan-deps` crash inside ModuleMap::addHeader (PCM-cache
// hash divergence vs the path main.cpp uses to see the same symbols). The
// raw/base/registry modules that already work all use `import storm;`.
// `storm` no longer re-exports utilities, so the leaf is imported too —
// alongside the umbrella, not instead of it.
import storm;
import storm_orm_utilities; // ConstexprString — internal, not re-exported by `storm`

export namespace storm::benchmark {

    // Re-use the ORM-side ConstexprString — same layout, same API surface.
    using storm::orm::utilities::ConstexprString;

    // ========================================================================
    // TypedValue — tagged union of supported leaf value types
    // ========================================================================
    struct TypedValue {
        enum class Kind : unsigned char { None, Int, Double, Bool, String };

        Kind                kind      = Kind::None;
        int                 as_int    = 0;
        double              as_double = 0.0;
        bool                as_bool   = false;
        ConstexprString<64> as_string;

        constexpr auto is_set() const -> bool {
            return kind != Kind::None;
        }
    };

    // Extract numeric value with correct type (int or double).
    // TypedValue must be passed as NTTP so if constexpr can dispatch on kind.
    template <TypedValue tv> consteval auto numeric_value() {
        if constexpr (tv.kind == TypedValue::Kind::Double) {
            return tv.as_double;
        } else {
            return tv.as_int;
        }
    }

    // ========================================================================
    // WhereCondition — a single WHERE clause term (field + operator + values)
    //
    // values[] holds all operand values:
    //   Comparison (>, <, ==, ...): values[0]        (value_count=1)
    //   LIKE:                       values[0] string  (value_count=1)
    //   BETWEEN:                    values[0], [1]    (value_count=2)
    //   IN:                         values[0..N]      (value_count=N)
    //   IS NULL / IS NOT NULL:      (value_count=0)
    // ========================================================================
    struct WhereCondition {
        ConstexprString<32> field;
        ConstexprString<16> op;

        static constexpr std::size_t       MAX_VALUES = 10;
        std::array<TypedValue, MAX_VALUES> values{};
        std::size_t                        value_count = 0;
    };

    // ========================================================================
    // WhereSpec — one or more WHERE conditions, optionally combined with AND/OR
    //
    // Single condition: conditions[0] holds the clause.
    // Combined (AND/OR): conditions[0..1] hold two clauses, combine_and/combine_or set.
    // ========================================================================
    struct WhereSpec {
        bool enabled = false;

        static constexpr std::size_t               MAX_CONDITIONS = 2;
        std::array<WhereCondition, MAX_CONDITIONS> conditions{};
        std::size_t                                condition_count = 0;

        bool combine_and = false;
        bool combine_or  = false;
    };

    // ========================================================================
    // OrderBySpec — one ORDER BY term. Tests use an array of up to 2 terms.
    // ========================================================================
    struct OrderBySpec {
        bool                enabled = false;
        ConstexprString<32> field;
        ConstexprString<8>  direction; // "ASC" / "DESC" (empty = ASC)
        ConstexprString<16> collate;   // "NOCASE" / "BINARY" / "RTRIM" (empty = none)
    };

    // ========================================================================
    // HavingSpec — HAVING clause, only valid nested inside GroupBySpec.
    // ========================================================================
    struct HavingSpec {
        bool                enabled = false;
        ConstexprString<32> field;
        ConstexprString<16> op;
        TypedValue          value;
    };

    // ========================================================================
    // GroupBySpec — one or more GROUP BY fields, optional HAVING.
    // ========================================================================
    struct GroupBySpec {
        bool                                        enabled    = false;
        static constexpr std::size_t                MAX_FIELDS = 2;
        std::array<ConstexprString<32>, MAX_FIELDS> fields{};
        HavingSpec                                  having;

        consteval auto field_count() const -> std::size_t {
            std::size_t n = 0;
            for (std::size_t i = 0; i < MAX_FIELDS; ++i) {
                if (!fields[i].empty()) {
                    ++n;
                }
            }
            return n;
        }
    };

    // ========================================================================
    // DistinctSpec — SELECT DISTINCT over 1..3 fields.
    // ========================================================================
    struct DistinctSpec {
        bool                                        enabled    = false;
        static constexpr std::size_t                MAX_FIELDS = 3;
        std::array<ConstexprString<32>, MAX_FIELDS> fields{};

        consteval auto field_count() const -> std::size_t {
            std::size_t n = 0;
            for (std::size_t i = 0; i < MAX_FIELDS; ++i) {
                if (!fields[i].empty()) {
                    ++n;
                }
            }
            return n;
        }
    };

    // ========================================================================
    // LimitSpec — LIMIT and/or OFFSET. `enabled` is true if either is set.
    // ========================================================================
    struct LimitSpec {
        bool enabled = false;
        int  value   = 0; // 0 = no LIMIT
        int  offset  = 0; // 0 = no OFFSET
    };

    // ========================================================================
    // AggregateSpec — aggregate function (count/sum/avg/min/max/count_distinct)
    // over an optional field. `count` with empty field means COUNT(*).
    // ========================================================================
    struct AggregateSpec {
        bool                enabled = false;
        ConstexprString<16> func;  // "count" / "sum" / "avg" / "min" / "max" / "count_distinct"
        ConstexprString<32> field; // optional — empty means COUNT(*)
    };

    // ========================================================================
    // JoinSpec — JOIN configuration. Reserved for PR2 (schema-driven JOINs).
    // In PR1 this is parsed-but-unused; existing run_*_operation methods
    // continue to hardcode JOIN types until PR2.
    // ========================================================================
    struct JoinSpec {
        bool                enabled = false;
        ConstexprString<8>  type;    // "inner" / "left"
        ConstexprString<32> related; // related model name ("User")
        ConstexprString<32> fk;      // FK field name ("sender")
        // Multi-FK support (up to 2 for now)
        static constexpr std::size_t             MAX_FKS = 2;
        std::array<ConstexprString<32>, MAX_FKS> fks{};
        std::size_t                              fk_count = 0;
    };

    // ========================================================================
    // SetOpSpec — set operation type (union/union_all/except/intersect)
    // ========================================================================
    struct SetOpSpec {
        bool                enabled = false;
        ConstexprString<16> type; // "union" / "union_all" / "except" / "intersect"
    };

    // ========================================================================
    // BenchmarkTest — nested top-level test definition.
    // ========================================================================
    struct BenchmarkTest { // NOSONAR(cpp:S1820)
        ConstexprString<64>  test_name;
        ConstexprString<32>  test_category;
        ConstexprString<128> description;
        ConstexprString<32>  model;
        ConstexprString<32>  operation;

        // Nested specs
        WhereSpec                             where;
        static constexpr std::size_t          MAX_ORDER_BY = 2;
        std::array<OrderBySpec, MAX_ORDER_BY> order_by{};
        std::size_t                           order_by_count = 0;
        GroupBySpec                           group_by;
        DistinctSpec                          distinct;
        LimitSpec                             limit;
        AggregateSpec                         aggregate;
        JoinSpec                              join;
        SetOpSpec                             setop;

        // Scalar test parameters
        int iterations   = 1000;
        int dataset_size = 10000;
        int batch_size   = 1;

        // Size profile for automatic iteration over sizes
        // Values: "none", "batch_standard", "batch_insert_edge", "batch_update_edge",
        //         "dataset_standard", "dataset_small"
        ConstexprString<32> size_profile;

        consteval BenchmarkTest() = default;
    };

    // `load_benchmark_tests()` lives in the storm_benchmark_parser module —
    // import it directly where the JSON tests array is needed.

} // namespace storm::benchmark
