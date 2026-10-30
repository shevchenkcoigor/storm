// Textual fragment of module storm_orm_statements_aggregate (#671) — NOT a header.
// #included once, at namespace scope in aggregate.cppm's module purview right after
// its imports; it compiles into that module's BMI. A file split rather
// than a module split: a new module costs ~0.09 s in every TU that imports storm.
//
// Compile-time aggregate SQL grammar: the connection-free pieces of the aggregate
// statements — the numeric-target concepts, the AggregateType/AggregateOp
// descriptors, the GROUP BY field holders, the consteval SUM/COUNT/... operand and
// GROUP BY column-list writers, and the per-op result type. Nothing here needs
// BaseStatement or a connection type.

#ifndef STORM_AGGREGATE_FRAGMENTS
#error "aggregate_grammar.hpp is a textual fragment of storm_orm_statements_aggregate; #include it only from aggregate.cppm"
#endif

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    // True for an arithmetic-but-not-bool scalar. SUM/AVG/MIN/MAX always yield a
    // numeric result (int64_t / double / std::optional<double>), so a text, BLOB,
    // enum, or struct target would silently coerce rather than fail. bool is
    // excluded — aggregating a boolean column is a modelling mistake.
    template <typename T> constexpr bool is_numeric_scalar_v = std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

    // A field type valid as the target of a numeric aggregate. Unwraps one level
    // of std::optional (a nullable numeric column, e.g. std::optional<int>, is a
    // legitimate SUM/AVG target — NULLs are simply skipped by SQL). Routed through
    // a constexpr bool because a concept body cannot use ?: / unwrap directly.
    template <typename T>
    constexpr bool is_numeric_aggregateable_v =
            utilities::is_optional_v<T> ? is_numeric_scalar_v<utilities::optional_inner_type_t<T>>
                                        : is_numeric_scalar_v<T>;

    template <typename T>
    concept NumericAggregateable = is_numeric_aggregateable_v<T>;

    // Every field in the pack is a NumericAggregateable column. Used as the
    // requires-clause on the numeric aggregate methods, which take the target
    // fields as a std::meta::info pack (SUM(a + b) over several fields).
    template <std::meta::info... FieldInfos>
    concept AllNumericAggregateable =
            (NumericAggregateable<std::remove_cvref_t<typename[:std::meta::type_of(FieldInfos):]>> && ...);

    // A COUNT(DISTINCT) or COUNT(field) target must name exactly ONE column (#570, #611);
    // bare COUNT(*) is unaffected — it names no column. Gate is storm::meta::SingleColumnSelector
    // (fields.cppm, #613) — see its definition for the SQLite-vs-PostgreSQL rationale.

    // Aggregate function types
    enum class AggregateType : std::uint8_t { SUM, COUNT, AVG, MIN, MAX, COUNT_DISTINCT };

    // LCOV_EXCL_START - compile-time only
    constexpr auto get_agg_function_name(AggregateType type) -> std::string_view {
        using enum AggregateType;
        switch (type) {
        case SUM:
            return "SUM";
        case COUNT:
            return "COUNT";
        case AVG:
            return "AVG";
        case MIN:
            return "MIN";
        case MAX:
            return "MAX";
        case COUNT_DISTINCT:
            return "COUNT";
        }
        return "";
    }
    // LCOV_EXCL_STOP

    // Aggregate operation descriptor
    template <AggregateType Type, std::meta::info... FieldInfos> struct AggregateOp {
        static constexpr AggregateType agg_type    = Type;
        static constexpr std::size_t   field_count = sizeof...(FieldInfos);
        // LCOV_EXCL_START - compile-time only
        static constexpr auto get_field_infos() -> std::array<std::meta::info, sizeof...(FieldInfos)> {
            if constexpr (sizeof...(FieldInfos) > 0) {
                return std::array{FieldInfos...};
            } else {
                return std::array<std::meta::info, 0>{};
            }
        }
        // LCOV_EXCL_STOP
        static constexpr auto field_infos = get_field_infos();
    };

    // Empty marker for no GROUP BY
    struct NoGroupBy {
        static constexpr std::size_t size = 0;
    };

    // GROUP BY fields holder
    template <std::meta::info... Infos> struct GroupByFields {
        static constexpr std::size_t                                   size = sizeof...(Infos);
        static constexpr std::array<std::meta::info, sizeof...(Infos)> values{Infos...};

        // LCOV_EXCL_START - compile-time only
        template <std::size_t I> static consteval auto at() -> std::meta::info {
            static_assert(I < size, "Index out of bounds");
            return values[I];
        }
        // LCOV_EXCL_STOP
    };

    // ============================================================================
    // Compile-time SQL Building Utilities
    // ============================================================================

    // LCOV_EXCL_START - compile-time only
    template <typename Op> consteval auto build_single_op_sql() {
        constexpr auto agg_name = get_agg_function_name(Op::agg_type);

        ConstexprString<utilities::buffer_size::SQL_SMALL> result;
        result.append(agg_name);
        result.append("(");

        // #570: aggregate operands name COLUMNS, so they route through the canonical
        // column-name writer (#422) — an FK member `sender` is the column `sender_id`.
        // COUNT(DISTINCT <fk>) is the reachable case; the SUM/AVG/MIN/MAX operand loop
        // below is corrected for the same structural reason even though
        // NumericAggregateable (#475) currently rejects an FK target.
        if constexpr (Op::agg_type == AggregateType::COUNT_DISTINCT) {
            result.append("DISTINCT ");
            static_assert(Op::field_count == 1, "COUNT(DISTINCT) requires exactly one field");
            storm::meta::append_column_name(result, Op::field_infos[0]);
        } else if constexpr (Op::field_count == 0) {
            result.append("*");
        } else {
            for (std::size_t i = 0; i < Op::field_count; ++i) {
                if (i > 0) {
                    result.append(" + ");
                }
                storm::meta::append_column_name(result, Op::field_infos[i]);
            }
        }

        result.append(")");
        return result;
    }

    template <typename... Ops, std::size_t... Is> consteval auto build_ops_list(std::index_sequence<Is...> /*unused*/) {
        ConstexprString<utilities::buffer_size::SQL_MEDIUM> result;
        (([&result]() {
             if constexpr (Is > 0) {
                 result.append(", ");
             }
             constexpr auto op_sql = build_single_op_sql<std::tuple_element_t<Is, std::tuple<Ops...>>>();
             result.append(op_sql);
         }()),
         ...);
        return result;
    }

    template <typename GroupFields, std::size_t... Is>
    consteval auto build_group_fields_sql(std::index_sequence<Is...> /*unused*/) {
        ConstexprString<utilities::buffer_size::SQL_SMALL> result;
        (([&result]() {
             if constexpr (Is > 0) {
                 result.append(", ");
             }
             // #570/#611: corrected for the same reason as the operand writer above.
             // A composite-PK FK key is rejected at the QuerySet::group_by<>() call
             // site (SingleColumnSelector, #611) — .sql()/.to_sql() reach this writer
             // via AggregateStatement's eagerly-evaluated static constexpr members
             // (aggregate.cppm) without ever needing extract_column_value<Related>
             // to exist, so the gate has to sit at the call site rather than rely
             // on that extractor staying incomplete. This list is emitted TWICE
             // per query (SELECT prefix and GROUP BY tail).
             storm::meta::append_column_name(result, GroupFields::template at<Is>());
         }()),
         ...);
        return result;
    }
    // LCOV_EXCL_STOP

    // True when any of the operation's summed fields is stored as a floating-point
    // type (double/float). SUM(a + b) promotes to double if ANY operand is floating.
    // LCOV_EXCL_START - compile-time only
    template <typename Op> consteval auto op_has_floating_field() -> bool {
        for (std::size_t i = 0; i < Op::field_count; ++i) {
            const auto field_type = std::meta::dealias(std::meta::type_of(Op::field_infos[i]));
            if (field_type == ^^double || field_type == ^^float) {
                return true;
            }
        }
        return false;
    }
    // LCOV_EXCL_STOP

    // Result type:
    //   COUNT / COUNT(DISTINCT) -> int64_t (never NULL)
    //   SUM (integer)           -> int64_t (#420; 0 over an empty set)
    //   SUM (floating)          -> double  (#420, no truncation; 0.0 over an empty set)
    //   AVG/MIN/MAX             -> std::optional<double> (#416)
    // MIN/MAX/AVG over an empty (or fully-filtered-out) set, or over an all-NULL
    // column within a GROUP BY group, return SQL NULL — surfaced as std::nullopt
    // so "no value" is distinguishable from a genuine 0. SUM keeps the 0-on-empty
    // convention, so it stays a plain (non-optional) numeric.
    template <typename Op>
    using OpResult = std::conditional_t<
            Op::agg_type == AggregateType::COUNT || Op::agg_type == AggregateType::COUNT_DISTINCT ||
                    (Op::agg_type == AggregateType::SUM && !op_has_floating_field<Op>()),
            std::int64_t,
            std::conditional_t<Op::agg_type == AggregateType::SUM, double, std::optional<double>>>;

} // namespace storm::orm::statements
