// Operator vocabulary of the WHERE/HAVING expression system: the comparison and logical
// operator enums, their SQL spellings, and the one text join the expression nodes share.
// Split out of where.cppm purely to keep that file under its line budget (as
// where_operand.cppm was) — no dependency reason; where.cppm re-exports this module, so
// importers are unchanged and everything stays in the storm::orm::where namespace.

export module storm_orm_where_ops;

import std;

export namespace storm::orm::where {

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

    // "<name><suffix>" — e.g. with_suffix("age", " LIKE ?"). Appends rather than
    // std::format: one std::format call reachable from an importing TU makes it generate
    // the whole formatting stack (scripts/ci/check_no_std_format.py).
    [[nodiscard]] inline auto with_suffix(std::string_view name, std::string_view suffix) -> std::string {
        std::string sql{name};
        sql += suffix;
        return sql;
    }

    // "(<left><op><right>)" — the one spelling of a logical node's text, shared by the plain
    // and the JOIN-qualified visitors. Appends for the same reason as with_suffix.
    [[nodiscard]] inline auto parenthesised(std::string_view left, std::string_view op, std::string_view right)
            -> std::string {
        std::string sql;
        sql.reserve(left.size() + op.size() + right.size() + 2);
        sql += '(';
        sql += left;
        sql += op;
        sql += right;
        sql += ')';
        return sql;
    }

} // namespace storm::orm::where
