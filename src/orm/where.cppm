module;

// Single cohesive class template; thresholds intentionally relaxed (see #264 finding).
// `duplicate` removed in #277 Phase 3 (BindParamsVisitor casts the type-erased pointer once and calls each Expr's
// bind_impl; make_null_check_expr shared by CollatedField/Field).

#include <meta>

export module storm_orm_where;

import std;

import storm_orm_utilities;     // For ConstexprString
import storm_orm_field_attr;    // ValidFieldInfo — gate the field-selector NTTP (#478)
import storm_orm_relation_meta; // is_relation_field — reject m2m/reverse_fk members in Field<M> (#408)
import storm_orm_where_operand; // normalize_operand/normalize_column_operand/ColumnIsUuid (#622)

export namespace storm::orm::where {

    // VARIANT-BASED EXPRESSION SYSTEM (eliminates heap allocation and vtable overhead)
    // Instead of virtual inheritance, we use std::variant for compile-time polymorphism
    //
    // Forward declare the variant type
    struct ExpressionVariant;

    // Recursive variant using std::shared_ptr for LogicalExpr children
    // (LogicalExpr needs to hold other expressions, creating recursion)
    using ExpressionVariantPtr = std::shared_ptr<ExpressionVariant>;

    // Type-erased statement pointer for parameter binding.
    // This is intentionally type-erased (void*) to allow the WHERE expression system
    // to bind parameters to any database statement type without knowing the concrete
    // type at compile time. The actual Statement* conversion happens in bind_params_direct().
    using ErasedStatementPtr = void*; // NOSONAR(cpp:S5008) - type erasure requires void*

    // Comparison operators
    enum class CompOp : std::uint8_t { Equal, NotEqual, Greater, GreaterEqual, Less, LessEqual };

    constexpr auto comp_op_to_sql(CompOp op) noexcept -> std::string_view {
        using enum CompOp;
        switch (op) {
        case Equal:
            return " = ";
        case NotEqual:
            return " != ";
        case Greater:
            return " > ";
        case GreaterEqual:
            return " >= ";
        case Less:
            return " < ";
        case LessEqual:
            return " <= ";
        }
        return " = "; // LCOV_EXCL_LINE - unreachable: all enum values handled above
    }

    // Logical operators
    enum class LogicalOp : std::uint8_t { And, Or };

    constexpr auto logical_op_to_sql(LogicalOp op) noexcept -> std::string_view {
        switch (op) {
        case LogicalOp::And:
            return " AND ";
        case LogicalOp::Or:
            return " OR ";
        }
        return " AND "; // LCOV_EXCL_LINE - unreachable: all enum values handled above
    }

    // VALUE-TYPE Comparison expression: field > value (NO VIRTUAL FUNCTIONS!)
    template <typename ValueType> struct ComparisonExpr {
        std::string field_name_;
        CompOp      op_;
        ValueType   value_;

        [[nodiscard]] __attribute__((always_inline)) auto to_sql() const -> std::string {
            return std::format("{}{}?", field_name_, comp_op_to_sql(op_));
        }

        template <typename StmtType, typename ErrorType>
        [[nodiscard]] __attribute__((always_inline)) auto bind_impl(StmtType* stmt, int& param_index) const
                -> std::expected<void, ErrorType> {
            return utilities::bind_filter_value<StmtType, ErrorType>(*stmt, param_index++, value_);
        }
    };

    // NULL check expression: field IS NULL / field IS NOT NULL
    struct NullCheckExpr {
        std::string field_name_;
        bool        is_null_; // true = IS NULL, false = IS NOT NULL

        [[nodiscard]] __attribute__((always_inline)) auto to_sql() const -> std::string {
            return is_null_ ? std::format("{} IS NULL", field_name_) : std::format("{} IS NOT NULL", field_name_);
        }

        template <typename StmtType, typename ErrorType>
        [[nodiscard]] __attribute__((always_inline)) auto bind_impl(StmtType* /*stmt*/, int& /*param_index*/) const
                -> std::expected<void, ErrorType> {
            return {};
        }
    };

    // LIKE expression: field.like("pattern%")
    struct LikeExpr {
        std::string field_name_;
        std::string pattern_;

        [[nodiscard]] __attribute__((always_inline)) auto to_sql() const -> std::string {
            return std::format("{} LIKE ?", field_name_);
        }

        template <typename StmtType, typename ErrorType>
        [[nodiscard]] __attribute__((always_inline)) auto bind_impl(StmtType* stmt, int& param_index) const
                -> std::expected<void, ErrorType> {
            return stmt->bind_text(param_index++, pattern_);
        }
    };

    // BETWEEN expression: field.between(a, b)
    template <typename ValueType> struct BetweenExpr {
        std::string field_name_;
        ValueType   min_val_;
        ValueType   max_val_;

        [[nodiscard]] __attribute__((always_inline)) auto to_sql() const -> std::string {
            return std::format("{} BETWEEN ? AND ?", field_name_);
        }

        template <typename StmtType, typename ErrorType>
        [[nodiscard]] __attribute__((hot)) auto bind_impl(StmtType* stmt, int& param_index) const
                -> std::expected<void, ErrorType> {
            if (auto result = utilities::bind_filter_value<StmtType, ErrorType>(*stmt, param_index++, min_val_);
                !result) {
                return result;
            }
            return utilities::bind_filter_value<StmtType, ErrorType>(*stmt, param_index++, max_val_);
        }
    };

    // IN expression (runtime): field IN (val1, val2, ..., valN)
    template <typename ValueType> struct InExpression {
        std::string            field_name_;
        std::vector<ValueType> values_;

        [[nodiscard]] __attribute__((always_inline)) auto to_sql() const -> std::string {
            if (values_.empty()) {
                return "1 = 0"; // SQL that always evaluates to false
            }
            auto placeholders = std::views::repeat(std::string_view("?"), values_.size()) |
                                std::views::join_with(std::string_view(", "));
            return std::format("{} IN ({})", field_name_, std::string(placeholders.begin(), placeholders.end()));
        }

        template <typename StmtType, typename ErrorType>
        [[nodiscard]] __attribute__((hot)) auto bind_impl(StmtType* stmt, int& param_index) const
                -> std::expected<void, ErrorType> {
            // `const ValueType&`, not `const auto&`: for bool, values_ is the std::vector<bool>
            // bitset specialization, whose __bit_const_reference proxy is not a BindableType.
            // Naming the type converts it once into a lifetime-extended bool; every other
            // ValueType binds the existing reference unchanged, so this hot loop is untouched.
            for (const ValueType& value : values_) {
                if (auto result = utilities::bind_filter_value<StmtType, ErrorType>(*stmt, param_index++, value);
                    !result) {
                    return result;
                }
            }
            return {};
        }
    };

    // Logical expression: expr1 AND expr2 / expr1 OR expr2
    struct LogicalExpr {
        ExpressionVariantPtr left; // Recursive: holds another expression
        LogicalOp            op;
        ExpressionVariantPtr right; // Recursive: holds another expression

        LogicalExpr(ExpressionVariantPtr l, LogicalOp operation, ExpressionVariantPtr r)
            : left(std::move(l)), op(operation), right(std::move(r)) {}

        // Note: to_sql() and bind_params_direct() will be implemented via std::visit
        // (see visitor functions below)
    };

    // ============================================================================
    // VARIANT DEFINITION: All expression types in one variant (NO HEAP ALLOCATION!)
    // ============================================================================
    //
    // std::variant provides compile-time polymorphism without vtable overhead
    // Common types (int, std::string) are stored inline, eliminating heap allocation
    struct ExpressionVariant : std::variant<
                                       ComparisonExpr<int>,
                                       ComparisonExpr<std::int64_t>,
                                       ComparisonExpr<double>,
                                       ComparisonExpr<float>,
                                       ComparisonExpr<std::string>,
                                       ComparisonExpr<bool>,
                                       ComparisonExpr<std::chrono::year_month_day>,
                                       ComparisonExpr<std::chrono::system_clock::time_point>,
                                       ComparisonExpr<utilities::UUID>,
                                       NullCheckExpr,
                                       LikeExpr,
                                       BetweenExpr<int>,
                                       BetweenExpr<std::int64_t>,
                                       BetweenExpr<double>,
                                       BetweenExpr<float>,
                                       BetweenExpr<std::string>,
                                       BetweenExpr<std::chrono::year_month_day>,
                                       BetweenExpr<std::chrono::system_clock::time_point>,
                                       InExpression<int>,
                                       InExpression<std::int64_t>,
                                       InExpression<bool>,
                                       InExpression<double>,
                                       InExpression<float>,
                                       InExpression<std::string>,
                                       InExpression<std::chrono::year_month_day>,
                                       InExpression<std::chrono::system_clock::time_point>,
                                       InExpression<utilities::UUID>,
                                       LogicalExpr> {
        // Inherit constructors from variant
        using variant::variant;
    };

    // Recursive variant using std::shared_ptr for LogicalExpr children
    // (LogicalExpr needs to hold other expressions, creating recursion)
    using ExpressionVariantPtr = std::shared_ptr<ExpressionVariant>;

    // ============================================================================
    // VISITOR FUNCTIONS: Replace virtual dispatch with std::visit
    // ============================================================================

    // Forward declare visitor functions
    [[nodiscard]] auto to_sql(const ExpressionVariant& expr) -> std::string;

    template <typename StmtType, typename ErrorType>
    [[nodiscard]] auto bind_params_direct(const ExpressionVariant& expr, ErasedStatementPtr stmt_ptr, int& param_index)
            -> std::expected<void, ErrorType>;

    // Visitor for to_sql() - called via std::visit
    struct ToSqlVisitor {
        [[nodiscard]] auto operator()(const LogicalExpr& expr) const -> std::string {
            return std::format("({}{}{})", to_sql(*expr.left), logical_op_to_sql(expr.op), to_sql(*expr.right));
        }

        // All other expression types have their own to_sql() method
        template <typename T> [[nodiscard]] auto operator()(const T& expr) const -> std::string {
            return expr.to_sql();
        }
    };

    // Main visitor entry points (called by users and recursively by LogicalExpr)
    [[nodiscard]] inline auto to_sql(const ExpressionVariant& expr) -> std::string {
        return std::visit(ToSqlVisitor{}, expr);
    }

    // bind_params_direct uses a lambda visitor that captures param_index by reference,
    // so no raw pointer/reference data members are required.
    // Every Expr type exposes a small `bind_impl(StmtType*, int&)`. The type-erased
    // pointer cast used to be duplicated in each Expr's own `bind_params_direct` — that's
    // the duplicate this visitor now centralises.
    template <typename StmtType, typename ErrorType>
    [[nodiscard]] inline auto
    bind_params_direct(const ExpressionVariant& expr, ErasedStatementPtr stmt_ptr, int& param_index)
            -> std::expected<void, ErrorType> {
        return std::visit(
                [stmt_ptr, &param_index](const auto& node) -> std::expected<void, ErrorType> {
                    using NodeT = std::remove_cvref_t<decltype(node)>;
                    if constexpr (std::is_same_v<NodeT, LogicalExpr>) {
                        if (auto result = bind_params_direct<StmtType, ErrorType>(*node.left, stmt_ptr, param_index);
                            !result) {
                            return result;
                        }
                        return bind_params_direct<StmtType, ErrorType>(*node.right, stmt_ptr, param_index);
                    } else {
                        return node
                                .template bind_impl<StmtType, ErrorType>(static_cast<StmtType*>(stmt_ptr), param_index);
                    }
                },
                expr
        );
    }

    // Expression wrapper to enable natural && and || operators without ambiguity
    // Solves the problem of shared_ptr's implicit bool conversion conflicting with built-in operators
    class Expr {
      public:
        // Constructor from ExpressionVariantPtr
        explicit Expr(ExpressionVariantPtr expr) noexcept : expr_(std::move(expr)) {}

        explicit Expr(ExpressionVariant&& expr) : expr_(std::make_shared<ExpressionVariant>(std::move(expr))) {}

        // Implicit conversion to ExpressionVariantPtr for where() calls
        operator ExpressionVariantPtr() const noexcept {
            return expr_;
        }

        // Logical AND operator (also accessible via 'and' keyword)
        auto operator&&(const Expr& other) const -> Expr {
            return Expr(std::make_shared<ExpressionVariant>(LogicalExpr{expr_, LogicalOp::And, other.expr_}));
        }

        // Logical OR operator (also accessible via 'or' keyword)
        auto operator||(const Expr& other) const -> Expr {
            return Expr(std::make_shared<ExpressionVariant>(LogicalExpr{expr_, LogicalOp::Or, other.expr_}));
        }

        // Access the underlying expression
        [[nodiscard]] auto get() const noexcept -> const ExpressionVariantPtr& {
            return expr_;
        }

      private:
        ExpressionVariantPtr expr_; // VARIANT-based, not virtual!
    };

    // Concept: field type is std::optional<T> (nullable in the database). Routes through
    // utilities::is_optional_v (#613) instead of duck-typing on `::value_type`, which admits
    // std::string/std::vector<uint8_t> too. Also gates query_builder.hpp's ValidOperator
    // (YAML/JSON-driven IS NULL / IS NOT NULL) — narrows there too, not just here.
    template <typename T>
    concept NullableField = utilities::is_optional_v<T>;

    // Free helpers for null-check expression construction. Both CollatedField
    // and Field used to spell out the four nullable methods (is_null, is_not_null,
    // operator==(nullopt_t), operator!=(nullopt_t)) body-for-body identical.
    // Now both classes route through these one-line helpers.
    [[nodiscard]] inline auto make_null_check_expr(std::string field_name, bool is_null) -> Expr {
        return Expr(
                std::make_shared<ExpressionVariant>(
                        NullCheckExpr{.field_name_ = std::move(field_name), .is_null_ = is_null}
                )
        );
    }

    // normalize_operand/normalize_column_operand now live in storm_orm_where_operand (#622) —
    // split out to keep this module under its line budget. Same storm::orm::where namespace, so
    // callers below reach them unqualified.

    // Build a value-owning ComparisonExpr, normalized against the column's own type (see
    // normalize_column_operand — #622). ColumnType is the field's declared type, passed
    // explicitly by Field/CollatedField below, which have it as their own FieldType alias.
    template <typename ColumnType, typename V>
    [[nodiscard]] auto make_comparison_expr(const std::string& field_name, CompOp op, V&& value) -> Expr {
        auto stored = normalize_column_operand<ColumnType>(std::forward<V>(value));
        return Expr(
                std::make_shared<ExpressionVariant>(ComparisonExpr<decltype(stored)>{
                        .field_name_ = std::move(field_name), .op_ = op, .value_ = std::move(stored)
                })
        );
    }

    // Build a LikeExpr for `field_name LIKE pattern`. Shared by Field::like and
    // CollatedField::like so the two bodies cannot drift (same rationale as make_in_expr's
    // #578 note).
    [[nodiscard]] inline auto make_like_expr(std::string field_name, std::string_view pattern) -> Expr {
        return Expr(
                std::make_shared<ExpressionVariant>(
                        LikeExpr{.field_name_ = std::move(field_name), .pattern_ = std::string(pattern)}
                )
        );
    }

    // Build a value-owning BetweenExpr from its bounds. Both bounds go through normalize_operand
    // (#406): text operands are copied into an owning std::string (closes the deferred-bind dangle
    // #352 fixed for comparisons) and enums fold to int — the same rules as make_comparison_expr.
    template <typename V>
    [[nodiscard]] auto make_between_expr(const std::string& field_name, V&& min_val, V&& max_val) -> Expr {
        auto stored_min = normalize_operand(std::forward<V>(min_val));
        auto stored_max = normalize_operand(std::forward<V>(max_val));
        return Expr(
                std::make_shared<ExpressionVariant>(BetweenExpr<decltype(stored_min)>{
                        .field_name_ = std::move(field_name),
                        .min_val_    = std::move(stored_min),
                        .max_val_    = std::move(stored_max)
                })
        );
    }

    // Build a value-owning InExpression from a value pack constructed to TargetType, each element
    // routed through normalize_operand so the stored element type matches an existing variant arm
    // for folding categories — enums and narrow/unsigned ints fold to int/int64_t, text to
    // std::string (#407, same rules as make_comparison_expr). Shared by Field::in and
    // CollatedField::in so the two bodies cannot drift the way they did for #578: CollatedField::in
    // used to construct InExpression<FieldType> directly, naming a variant arm that does not exist
    // for a folding FieldType.
    //
    // static_cast, not TargetType{...} (#610): the requires-clause checks constructible_from,
    // which is direct-init semantics, but a braced TargetType{value} is LIST-init and hard-errors
    // on a narrowing conversion the constraint already accepted — e.g. an int64_t insert() id
    // passed to .in() on an int-PK FK member. static_cast<TargetType>(value) is direct-init too,
    // so the body now matches what the requires-clause actually checked — same truncate-on-
    // overflow tradeoff as a plain `int x = some_int64;` assignment elsewhere in the codebase,
    // not a new risk class.
    template <typename TargetType, typename... Values>
        requires(std::constructible_from<TargetType, Values> && ...)
    [[nodiscard]] auto make_in_expr(const std::string& field_name, Values&&... values) -> Expr {
        using StoredType = decltype(normalize_operand(std::declval<TargetType>()));
        // Also checked HERE, not only in in()'s requires-clause: this function is exported, so a
        // direct make_in_expr<std::filesystem::path>(...) would otherwise still hard-error below.
        static_assert(
                std::is_constructible_v<ExpressionVariant, InExpression<StoredType>>,
                "no InExpression arm exists for this target type — see InStorableTarget"
        );
        return Expr(
                std::make_shared<ExpressionVariant>(InExpression<StoredType>{
                        .field_name_ = std::move(field_name),
                        .values_     = {normalize_operand(static_cast<TargetType>(std::forward<Values>(values)))...}
                })
        );
    }

    // in_target_pk_info/HasInTargetType/InTargetType/InOperandFor (#610/#625) and ComparisonColumnType/ColumnIsUuid
    // (#622) now live in storm_orm_where_operand — split out to keep this module under its line
    // budget. Same storm::orm::where namespace, so callers below reach them unqualified.

    // The normalized IN target must name an InExpression arm that EXISTS in ExpressionVariant —
    // the invariant #578, #610 and #625 each violated differently (a Field/CollatedField body
    // drift, an FK member whose FieldType is the related struct, a nullable column typed
    // std::optional<T>). Naming it makes that failure mode structurally impossible: in() fails
    // at its requires-clause, not inside make_shared<ExpressionVariant>. Declared here rather
    // than in the where_operand leaf, which cannot see ExpressionVariant; a requires-expression
    // rather than a bare decltype so a non-BindableType target substitutes to false instead of
    // hard-erroring on normalize_operand's constraint. Widening the arm set needs no edit here
    // or at either call site.
    template <std::meta::info MemberInfo>
    concept InStorableTarget = HasInTargetType<MemberInfo> && requires {
        ExpressionVariant{InExpression<decltype(normalize_operand(std::declval<InTargetType<MemberInfo>>()))>{}};
    };

    // Shared gate for WHERE's value-comparing operators: one column + an operand normalize_operand
    // can bind. std::optional operands are rejected here (#622): BindableType recurses through
    // is_optional_v, but ExpressionVariant has no ComparisonExpr<std::optional<T>> arm for any T,
    // so one used to pass this constraint and then hard-error deep inside the variant construction
    // in make_comparison_expr instead of failing cleanly at the call site. A UUID column also
    // rejects any operand that can't become a storm::UUID (#622) — e.g. `uuid_col == 5` used to
    // silently bind an int against a UUID/TEXT column instead of failing at the constraint.
    template <std::meta::info MemberInfo, typename V>
    concept ComparableOperand =
            storm::meta::SingleColumnMember<MemberInfo> && utilities::BindableType<std::decay_t<V>> &&
            !utilities::is_optional_v<std::decay_t<V>> &&
            (!ColumnIsUuid<MemberInfo> || std::is_same_v<std::decay_t<V>, utilities::UUID> ||
             std::is_constructible_v<utilities::UUID, V>);
    // Narrows ComparableOperand: storm::UUID is equality/IN only, no ordering (#609/#407). A
    // UUID-typed COLUMN rejects every operand, even one ComparableOperand accepts (#622): before
    // this, a string-spelled `.between("a", "b")` against a UUID column silently worked as a
    // plain lexicographic TEXT compare (BetweenExpr<std::string> exists) — inconsistent with the
    // documented equality/IN-only contract. Deliberate, breaking tightening for consistency, not
    // a crash fix.
    template <std::meta::info MemberInfo, typename V>
    concept OrderableOperand = ComparableOperand<MemberInfo, V> && !std::is_same_v<std::decay_t<V>, utilities::UUID> &&
                               !ColumnIsUuid<MemberInfo>;

    // CollatedField proxy - wraps field name with COLLATE clause
    // Created via fields::Person.name.collate(Collate::NoCase)
    // All comparison operators produce SQL like: "name COLLATE NOCASE = ?"
    // Class-level gated on SingleColumnMember (#613), safe now Field::collate() below has a
    // DEDUCED return type: naming CollatedField<MemberInfo> defers to collate()'s body,
    // instantiated only on a call collate() already restricts — not to Field<MemberInfo>'s
    // instantiation, where it hard-errored before (#575). Compile-verified w/ Shipment::line.
    template <std::meta::info MemberInfo>
        requires(
                storm::meta::ValidFieldInfo<MemberInfo> && !storm::meta::is_relation_field(MemberInfo) &&
                storm::meta::SingleColumnMember<MemberInfo>
        )
    class CollatedField {
      public:
        using FieldType = typename[:std::meta::type_of(MemberInfo):];

        explicit CollatedField(utilities::Collate col) : collated_name_(make_collated_name(col)) {}

        // in() routes the operand through the FK TARGET's primary-key type (InTargetType)
        // instead of FieldType — FieldType for an FK member is the RELATED MODEL TYPE, and
        // normalize_operand can never bind a struct. InStorableTarget still guards a
        // PK-less FK target (SingleColumnMember is the class-level gate now).
        template <typename... Values>
            requires InStorableTarget<MemberInfo> && (InOperandFor<MemberInfo, Values> && ...)
        auto in(Values&&... values) const {
            return where::make_in_expr<InTargetType<MemberInfo>>(collated_name_, std::forward<Values>(values)...);
        }

        // Comparison/pattern operators, gated on ComparableOperand (ordering ops and between()
        // narrow to OrderableOperand, which additionally excludes storm::UUID — #609/#407): a
        // non-bindable operand (e.g. a model struct) has nothing for normalize_operand to bind.
        // One line per method (clang-format off) — the OwnLine multi-line requires-clause form
        // the formatter otherwise produces duplicates near-identically across this class's twin below.
        // clang-format off
        template <typename V> auto operator==(V&& value) const -> Expr requires ComparableOperand<MemberInfo, V> { return make_comparison(CompOp::Equal, std::forward<V>(value)); }
        template <typename V> auto operator!=(V&& value) const -> Expr requires ComparableOperand<MemberInfo, V> { return make_comparison(CompOp::NotEqual, std::forward<V>(value)); }
        template <typename V> auto operator>(V&& value)  const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comparison(CompOp::Greater, std::forward<V>(value)); }
        template <typename V> auto operator>=(V&& value) const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comparison(CompOp::GreaterEqual, std::forward<V>(value)); }
        template <typename V> auto operator<(V&& value)  const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comparison(CompOp::Less, std::forward<V>(value)); }
        template <typename V> auto operator<=(V&& value) const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comparison(CompOp::LessEqual, std::forward<V>(value)); }
        [[nodiscard]] auto is_null()     const -> Expr requires NullableField<FieldType> { return make_null_check_expr(collated_name_, true); }
        [[nodiscard]] auto is_not_null() const -> Expr requires NullableField<FieldType> { return make_null_check_expr(collated_name_, false); }
        auto operator==(std::nullopt_t /*unused*/)  const -> Expr requires NullableField<FieldType> { return is_null(); }
        auto operator!=(std::nullopt_t /*unused*/)  const -> Expr requires NullableField<FieldType> { return is_not_null(); }
        [[nodiscard]] auto like(std::string_view pattern) const -> Expr { return where::make_like_expr(collated_name_, pattern); }
        template <typename V> auto between(V&& min_val, V&& max_val) const -> Expr requires OrderableOperand<MemberInfo, V> { return where::make_between_expr(collated_name_, std::forward<V>(min_val), std::forward<V>(max_val)); }
        // clang-format on

      private:
        std::string collated_name_;

        static auto make_collated_name(utilities::Collate col) -> std::string {
            return std::format("{}{}", storm::meta::column_name_view<MemberInfo>, utilities::collate_to_sql(col));
        }

        template <typename V> auto make_comparison(CompOp op, V&& value) const -> Expr {
            return where::make_comparison_expr<ComparisonColumnType<MemberInfo>>(
                    collated_name_, op, std::forward<V>(value)
            );
        }
    };

    // Field proxy - stores reflection info as template parameter
    // Provides compile-time IN expression and runtime comparison/special methods
    template <std::meta::info MemberInfo>
        requires(storm::meta::ValidFieldInfo<MemberInfo> && !storm::meta::is_relation_field(MemberInfo))
    class Field {
      public:
        // The SQL column name (#575): a plain member's identifier, or an FK member's
        // "<member>_id" — the same canonical writer ORDER BY/COUNT(DISTINCT) route
        // through (#570). Byte-identical to std::meta::identifier_of(MemberInfo) for a
        // non-FK member. CAUTION: for an FK to a COMPOSITE-PK target this still evaluates
        // (Field<M> must instantiate for every member — field_specs_for emits a FieldRef<M>
        // unconditionally) but yields a column that does NOT exist ("line_id" instead of
        // "line_order_id"/"line_product_id"), because column_name_view always assumes a
        // single "_id" suffix. Every method below gates on SingleColumnMember<MemberInfo>
        // before reading this; a new method that reads field_name_sv MUST gate the same way.
        static constexpr auto field_name_sv = storm::meta::column_name_view<MemberInfo>;
        using FieldType                     = typename[:std::meta::type_of(MemberInfo):];

        // Return a CollatedField proxy. Deduced return type, deliberately not
        // `-> CollatedField<MemberInfo>` (#613) — see CollatedField's class comment.
        [[nodiscard]] auto collate(utilities::Collate col) const
            requires storm::meta::SingleColumnMember<MemberInfo>
        {
            return CollatedField<MemberInfo>(col);
        }

        // IN: Returns Expr wrapping VARIANT (no heap allocation for expression itself!)
        // Usage: fields::Person.id.in(100, 200, 300)
        // Each value is constructed to InTargetType (the free function above), then routed
        // through normalize_operand so the stored element type matches the variant arm —
        // enums/narrow ints fold to int/int64_t, text to std::string, temporal/UUID keep their
        // own arm (#407, same rules as comparisons). For a single-column FK member,
        // InTargetType is the FK TARGET's primary-key type, not FieldType (#610). A composite
        // (or PK-less) FK target stays rejected — see CollatedField::in's twin comment above.
        template <typename... Values>
            requires storm::meta::SingleColumnMember<MemberInfo> && InStorableTarget<MemberInfo> &&
                     (InOperandFor<MemberInfo, Values> && ...)
        auto in(Values&&... values) const {
            return where::make_in_expr<InTargetType<MemberInfo>>(
                    std::string(field_name_sv), std::forward<Values>(values)...
            );
        }

        // Comparison operators — enum values are auto-converted to underlying int. Gated on
        // ComparableOperand (#575/#610): an FK to a composite-PK target spreads over
        // "<member>_<part>" columns with no single name to compare against, and a non-bindable
        // operand has nothing for normalize_operand to bind. Ordering ops/between() narrow to
        // OrderableOperand, which also excludes storm::UUID (#609/#407). WHERE (a, b) = (?, ?)
        // row-value syntax exists on both backends but isn't wired: well-defined only for ==/!=.
        // One line per method (clang-format off) — see CollatedField's twin block above.
        // clang-format off
        template <typename V> auto operator==(V&& value) const -> Expr requires ComparableOperand<MemberInfo, V> { return make_comp(CompOp::Equal, std::forward<V>(value)); }
        template <typename V> auto operator!=(V&& value) const -> Expr requires ComparableOperand<MemberInfo, V> { return make_comp(CompOp::NotEqual, std::forward<V>(value)); }
        template <typename V> auto operator>(V&& value)  const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comp(CompOp::Greater, std::forward<V>(value)); }
        template <typename V> auto operator>=(V&& value) const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comp(CompOp::GreaterEqual, std::forward<V>(value)); }
        template <typename V> auto operator<(V&& value)  const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comp(CompOp::Less, std::forward<V>(value)); }
        template <typename V> auto operator<=(V&& value) const -> Expr requires OrderableOperand<MemberInfo, V> { return make_comp(CompOp::LessEqual, std::forward<V>(value)); }
        // Deleted, not absent (#613, RelationRef's pattern): <=> covers !=/>/>=/< via rewrite.
        template <typename V> auto operator==(const V&) const -> bool requires (!storm::meta::SingleColumnMember<MemberInfo>) = delete ("this member is a foreign key to a composite-PK target: it spreads over multiple \"<member>_<part>\" columns, so there is no single column to compare in where().");
        template <typename V> auto operator<=>(const V&) const -> std::partial_ordering requires (!storm::meta::SingleColumnMember<MemberInfo>) = delete ("this member is a foreign key to a composite-PK target: it spreads over multiple \"<member>_<part>\" columns, so there is no single column to compare in where().");
        [[nodiscard]] auto is_null()     const -> Expr requires (NullableField<FieldType> && storm::meta::SingleColumnMember<MemberInfo>) { return make_null_check_expr(std::string(field_name_sv), true); }
        [[nodiscard]] auto is_not_null() const -> Expr requires (NullableField<FieldType> && storm::meta::SingleColumnMember<MemberInfo>) { return make_null_check_expr(std::string(field_name_sv), false); }
        auto operator==(std::nullopt_t /*unused*/)  const -> Expr requires (NullableField<FieldType> && storm::meta::SingleColumnMember<MemberInfo>) { return is_null(); }
        auto operator!=(std::nullopt_t /*unused*/)  const -> Expr requires (NullableField<FieldType> && storm::meta::SingleColumnMember<MemberInfo>) { return is_not_null(); }
        [[nodiscard]] auto like(std::string_view pattern) const -> Expr requires storm::meta::SingleColumnMember<MemberInfo> { return where::make_like_expr(std::string(field_name_sv), pattern); }
        // clang-format on

        template <typename V>
        auto between(V&& min_val, V&& max_val) const -> Expr
            requires OrderableOperand<MemberInfo, V>
        {
            return where::make_between_expr(
                    std::string(field_name_sv), std::forward<V>(min_val), std::forward<V>(max_val)
            );
        }

      private:
        // Helper: create comparison expression, converting enum values to int
        template <typename V> auto make_comp(CompOp op, V&& value) const -> Expr {
            return where::make_comparison_expr<ComparisonColumnType<MemberInfo>>(
                    std::string(field_name_sv), op, std::forward<V>(value)
            );
        }
    };

    // Combine two optional expressions with `op`; an empty side yields the other.
    [[nodiscard]] inline auto combine(ExpressionVariantPtr lhs, LogicalOp op, ExpressionVariantPtr rhs)
            -> ExpressionVariantPtr {
        if (!lhs) {
            return rhs;
        }
        if (!rhs) {
            return lhs;
        }
        return std::make_shared<ExpressionVariant>(LogicalExpr{std::move(lhs), op, std::move(rhs)});
    }

    [[nodiscard]] inline auto and_combine(ExpressionVariantPtr lhs, ExpressionVariantPtr rhs) -> ExpressionVariantPtr {
        return combine(std::move(lhs), LogicalOp::And, std::move(rhs));
    }

} // namespace storm::orm::where
