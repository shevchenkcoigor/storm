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
    //
    // The kind records what was WRITTEN, never what a field can hold (#844): `Null` is a JSON
    // null, `Int` an integer that fits 32 bits, `Int64` one that does not. A UUID, date, time,
    // enumerator name or blob is a `String`; the field's type decides how it reads.
    struct TypedValue {
        enum class Kind : unsigned char { None, Int, Double, Bool, String, Null, Int64 };

        Kind                kind      = Kind::None;
        int                 as_int    = 0;
        std::int64_t        as_int64  = 0;
        double              as_double = 0.0;
        bool                as_bool   = false;
        ConstexprString<64> as_string;

        constexpr auto is_set() const -> bool {
            return kind != Kind::None;
        }

        static constexpr auto null_value() -> TypedValue {
            TypedValue tv;
            tv.kind = Kind::Null;
            return tv;
        }

        // The one rule for an integer literal: 32 bits stay `Int`, anything wider is `Int64`.
        static constexpr auto of_integer(std::int64_t value) -> TypedValue {
            TypedValue tv;
            if (value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max()) {
                tv.kind   = Kind::Int;
                tv.as_int = static_cast<int>(value);
            } else {
                tv.kind = Kind::Int64;
            }
            tv.as_int64 = value;
            return tv;
        }
    };

    // The one rule for every capped spec array (#843): count the entry always, store it only
    // while there is room. A count past the array's size is what within_limits() rejects.
    template <typename Array, typename Value>
    constexpr void add_capped(Array& array, std::size_t& count, const Value& value) {
        if (count < array.size()) {
            array[count] = value;
        }
        ++count;
    }

    // ========================================================================
    // WhereNode — one node of the WHERE tree (#843)
    //
    // A LEAF (left < 0) is one comparison: `field`, `op` and `value_count` operands, which are
    // WhereSpec::values[first_value .. first_value + value_count):
    //   Comparison (>, <, ==, ...): 1 value      LIKE: 1 string
    //   BETWEEN: 2 values                        IN: N values
    //   a value of `null` (== null / != null): 1 value, lowered to IS [NOT] NULL (#844)
    // An INNER node has op "AND" or "OR" and the indices of its two children.
    // ========================================================================
    struct WhereNode {
        ConstexprString<32> field;
        ConstexprString<16> op;
        // "NOCASE" / "BINARY" / "RTRIM" (empty = none): the leaf compares through field.collate(...).
        // SQLite only: PostgreSQL has no such collations, so a case using it is `backends: [sqlite]`.
        ConstexprString<16> collate;
        std::size_t         first_value = 0;
        std::size_t         value_count = 0;
        int                 left        = -1;
        int                 right       = -1;

        [[nodiscard]] constexpr auto is_leaf() const -> bool {
            return left < 0 && right < 0;
        }

        // A leaf must carry the operands its operator reads. All leaves share one value pool, so a
        // leaf short of values would read the NEXT leaf's operand and one with extra values would
        // drop them silently. IN takes any number (an empty list is valid, #835).
        [[nodiscard]] constexpr auto operands_match_op() const -> bool {
            if (op == "IN") {
                return true;
            }
            if (op == "BETWEEN") {
                return value_count == 2;
            }
            return value_count == 1;
        }
    };

    // ========================================================================
    // WhereSpec — the WHERE clause as one tree of WhereNodes (#843)
    //
    // Every WHERE form builds this tree: `where:`/`conditions` is its leaves joined left to
    // right by `combine` (AND when absent), and the test-only `where_expr:` is written as the
    // tree itself. The operands of all leaves live in one pool, so the spec stays small however
    // many leaves there are. `node_count` and `value_count` count past the caps, so an overflow
    // is rejected (within_limits) instead of dropping entries silently.
    // ========================================================================
    // How a `where:` block joins its conditions (#847). `Unknown` is any spelling other than the
    // two accepted, which is_well_formed() rejects instead of reading it as AND.
    enum class WhereCombine : unsigned char { And, Or, Unknown };

    struct WhereSpec {
        bool enabled = false;

        // Sized to the corpus (max 5 nodes, 3 values) with headroom: 4 leaves + 3 joins.
        static constexpr std::size_t       MAX_NODES = 7;
        std::array<WhereNode, MAX_NODES>   nodes{};
        std::size_t                        node_count = 0;
        int                                root       = 0;
        static constexpr std::size_t       MAX_VALUES = 10;
        std::array<TypedValue, MAX_VALUES> values{};
        std::size_t                        value_count = 0;
        // How many WHERE forms the case wrote (`where:` / `where_expr:`); more than one is rejected,
        // because two trees cannot share one set of nodes.
        std::size_t forms = 0;
        // A `combine` that was neither "and" nor "or" (#847).
        bool unknown_combine = false;

        // The accepted `combine` spellings, exactly and case-sensitively: "and", "or" (#847).
        [[nodiscard]] static constexpr auto combine_of(std::string_view spelling) -> WhereCombine {
            if (spelling == "and") {
                return WhereCombine::And;
            }
            if (spelling == "or") {
                return WhereCombine::Or;
            }
            return WhereCombine::Unknown;
        }

        constexpr void begin_form() {
            enabled = true;
            ++forms;
        }

        // Appends a node and returns its index (past MAX_NODES the node is counted, not stored).
        constexpr auto add_node(const WhereNode& node) -> std::size_t {
            const std::size_t index = node_count;
            add_capped(nodes, node_count, node);
            return index;
        }

        constexpr void add_value(const TypedValue& value) {
            add_capped(values, value_count, value);
        }

        // Fills a node reserved earlier by add_node, once its fields are known. No-op past the cap.
        constexpr void put_node(std::size_t index, const WhereNode& node) {
            if (index < MAX_NODES) {
                nodes[index] = node;
            }
        }

        constexpr void set_children(std::size_t index, int left, int right) {
            if (index < MAX_NODES) {
                nodes[index].left  = left;
                nodes[index].right = right;
            }
        }

        // Joins the `leaf_count` leaves added from `first_leaf` on, left to right: ((l0 op l1) op l2) ...
        // An unknown `combine` is recorded even for one leaf, where there is nothing to join.
        constexpr void join_leaves(std::size_t first_leaf, std::size_t leaf_count, WhereCombine combine) {
            unknown_combine   = unknown_combine || combine == WhereCombine::Unknown;
            const bool use_or = combine == WhereCombine::Or;
            if (leaf_count == 0) {
                return;
            }
            std::size_t joined = first_leaf;
            for (std::size_t i = 1; i < leaf_count; ++i) {
                WhereNode join;
                join.op    = use_or ? ConstexprString<16>{"OR"} : ConstexprString<16>{"AND"};
                join.left  = static_cast<int>(joined);
                join.right = static_cast<int>(first_leaf + i);
                joined     = add_node(join);
            }
            root = static_cast<int>(joined);
        }

        [[nodiscard]] constexpr auto within_limits() const -> bool {
            return node_count <= MAX_NODES && value_count <= MAX_VALUES && forms <= 1;
        }

        // The tree can be built: a known `combine` (#847), a root and children that exist, AND/OR joins with both
        // children and no operands of their own, every leaf with the operands its operator reads, and no node stored
        // that the root cannot reach (a dropped subtree would be silently ignored). A QueryBuilder constraint, separate
        // from the caps above.
        [[nodiscard]] constexpr auto is_well_formed() const -> bool {
            if (!enabled) {
                return true;
            }
            if (unknown_combine) {
                return false;
            }
            const std::size_t stored = stored_node_count();
            if (root < 0 || static_cast<std::size_t>(root) >= stored) {
                return false;
            }
            for (std::size_t i = 0; i < stored; ++i) {
                if (!node_is_well_formed(nodes[i], stored)) {
                    return false;
                }
            }
            return reachable_from_root(stored) == stored;
        }

        // Does any stored leaf compare through a collation? Such a case runs on SQLite only.
        [[nodiscard]] constexpr auto any_collate() const -> bool {
            for (std::size_t i = 0; i < stored_node_count(); ++i) {
                if (!nodes[i].collate.empty()) {
                    return true;
                }
            }
            return false;
        }

        // The nodes actually stored, for a reader that runs before the limits are checked.
        [[nodiscard]] constexpr auto stored_node_count() const -> std::size_t {
            return node_count < MAX_NODES ? node_count : MAX_NODES;
        }

      private:
        [[nodiscard]] static constexpr auto node_is_well_formed(const WhereNode& node, std::size_t stored) -> bool {
            const bool is_join = node.op == "AND" || node.op == "OR";
            if (node.is_leaf()) {
                return !is_join && node.operands_match_op();
            }
            if (!is_join || node.left < 0 || node.right < 0 || !node.field.view().empty() || node.value_count != 0 ||
                !node.collate.empty()) {
                return false;
            }
            return static_cast<std::size_t>(node.left) < stored && static_cast<std::size_t>(node.right) < stored;
        }

        // Nodes visited walking down from the root; a shared child or a cycle is counted per visit, so
        // such a tree never equals the stored count. The walk is bounded by the stored count.
        [[nodiscard]] constexpr auto reachable_from_root(std::size_t stored) const -> std::size_t {
            std::array<int, MAX_NODES> pending{};
            std::size_t                depth   = 0;
            std::size_t                visited = 0;
            pending[depth++]                   = root;
            while (depth > 0 && visited <= stored) {
                const WhereNode& node = nodes[static_cast<std::size_t>(pending[--depth])];
                ++visited;
                if (!node.is_leaf() && depth + 2 <= MAX_NODES) {
                    pending[depth++] = node.left;
                    pending[depth++] = node.right;
                }
            }
            return visited;
        }
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

    // HavingSpec -- the HAVING clause is the same tree of conditions as WHERE (#868), nested in GroupBySpec.
    using HavingSpec = WhereSpec;

    // ========================================================================
    // CappedFields<N> — up to N field names, counting past N (#843) so an overflow is detectable.
    // The one list behind GROUP BY, DISTINCT and values; each parser adds through add_field.
    // ========================================================================
    template <std::size_t N> struct CappedFields {
        bool                               enabled    = false;
        static constexpr std::size_t       MAX_FIELDS = N;
        std::array<ConstexprString<32>, N> fields{};
        std::size_t                        declared_count = 0;

        constexpr void add_field(const ConstexprString<32>& name) {
            add_capped(fields, declared_count, name);
        }

        [[nodiscard]] constexpr auto within_limits() const -> bool {
            return declared_count <= MAX_FIELDS;
        }

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

    // GroupBySpec — one or more GROUP BY fields (max 4), optional HAVING tree.
    struct GroupBySpec : CappedFields<4> {
        HavingSpec having;

        [[nodiscard]] constexpr auto within_limits() const -> bool {
            return CappedFields<4>::within_limits() && having.within_limits();
        }

        // Every HAVING leaf names a GROUP BY field: PostgreSQL rejects any other bare column there, and
        // SQLite would run it, so the case would pass on one backend only (#868).
        [[nodiscard]] constexpr auto having_fields_are_grouped() const -> bool {
            for (std::size_t i = 0; i < having.stored_node_count(); ++i) {
                if (having.nodes[i].is_leaf() && !groups_field(having.nodes[i].field.view())) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] constexpr auto groups_field(std::string_view name) const -> bool {
            for (std::size_t i = 0; i < MAX_FIELDS; ++i) {
                if (fields[i].view() == name) {
                    return true;
                }
            }
            return false;
        }
    };

    // DistinctSpec — SELECT DISTINCT over 1..3 fields.
    using DistinctSpec = CappedFields<3>;

    // ValuesSpec — column projection `values<f1, ...>()` over 1..4 fields (#867).
    using ValuesSpec = CappedFields<4>;

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
        std::size_t                              fk_count = 0; // counts past MAX_FKS (#843)

        constexpr void add_fk(const ConstexprString<32>& name) {
            add_capped(fks, fk_count, name);
        }

        [[nodiscard]] constexpr auto within_limits() const -> bool {
            return fk_count <= MAX_FKS;
        }
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
        std::size_t                           order_by_count = 0; // counts past MAX_ORDER_BY (#843)
        GroupBySpec                           group_by;
        DistinctSpec                          distinct;
        ValuesSpec                            values;
        LimitSpec                             limit;
        AggregateSpec                         aggregate;
        JoinSpec                              join;
        SetOpSpec                             setop;

        // Scalar test parameters
        int iterations   = 1000;
        int dataset_size = 10000;
        int batch_size   = 1;

        // Strings longer than their buffer (#845): kept truncated, counted, and rejected below.
        std::size_t truncated_strings = 0;

        // Size profile for automatic iteration over sizes
        // Values: "none", "batch_standard", "batch_insert_edge", "batch_update_edge",
        //         "dataset_standard", "dataset_small"
        ConstexprString<32> size_profile;

        consteval BenchmarkTest() = default;

        constexpr void add_order_by(const OrderBySpec& entry) {
            add_capped(order_by, order_by_count, entry);
        }

        // #867: build_terminal picks ONE result shape (the first match in a fixed order), so a case
        // naming two would silently run the earlier one and ignore the other. An aggregate with a
        // GROUP BY is one shape: the group is how the aggregate chain is applied. The `first` / `get`
        // operations are shapes too: they are tested after the projections.
        [[nodiscard]] constexpr auto result_shape_count() const -> std::size_t {
            const bool single_row_op = operation == "first" || operation == "first_where" || operation == "get_where";
            return std::size_t{setop.enabled} + std::size_t{aggregate.enabled || group_by.enabled} +
                   std::size_t{distinct.enabled} + std::size_t{values.enabled} + std::size_t{single_row_op};
        }

        // #843: false when a case listed more entries than a capped array holds, or names two result shapes (#867), or
        // (#845) wrote a string longer than its buffer. The parser keeps counting past a cap instead of dropping the
        // extra entries silently; the consumer (QueryBuilder) rejects the case at compile time.
        [[nodiscard]] constexpr auto within_limits() const -> bool {
            return where.within_limits() && order_by_count <= MAX_ORDER_BY && group_by.within_limits() &&
                   distinct.within_limits() && values.within_limits() && join.within_limits() &&
                   result_shape_count() <= 1 && truncated_strings == 0;
        }
    };

    // `load_benchmark_tests()` lives in the storm_benchmark_parser module —
    // import it directly where the JSON tests array is needed.

} // namespace storm::benchmark
