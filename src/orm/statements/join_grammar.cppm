module;

// Compile-time JOIN SQL grammar (#671): the consteval helpers that spell the shared pieces
// of the two-query eager-load SQL (m2m #391/#392, reverse-FK #398) — the PK part list and
// Q2 IN-subquery open, the related/owner column block and its exact-size counter, the
// junction and owner-FK column lists, and the reverse-FK Q2 SELECT head. Split out of
// join.cppm, which keeps the statement classes and the runtime, ConnType-dependent query
// building. Stateless and connection-free: each helper derives purely from reflection over
// the BaseStatement<T> metadata it is handed.

#include <meta>

export module storm_orm_statements_join_grammar;

import std;

import storm_orm_field_attr;
import storm_orm_model_concepts;

export namespace storm::orm::statements {

    // Append the comma-joined PK member identifiers "<a>, <b>, <c>" — the row-value
    // list used both inside the Q2 IN-subquery's SELECT and (by the caller) on the
    // outer comparison side. FK parts go through append_column_name (#422) so a
    // composite key over an FK part emits "<name>_id", matching the column DDL.
    template <typename Base> consteval auto append_pk_part_list(auto& result) -> void {
        bool first = true;
        for (const std::meta::info member : Base::primary_key_members_) {
            if (!first) {
                result.append(", ");
            }
            meta::append_column_name(result, member);
            first = false;
        }
    }

    // Byte size of what append_pk_part_list emits.
    template <typename Base> consteval auto pk_part_list_size() -> std::size_t {
        std::size_t total = 0;
        bool        first = true;
        for (const std::meta::info member : Base::primary_key_members_) {
            if (!first) {
                total += 2; // ", "
            }
            total += storm::meta::column_name_size(member);
            first = false;
        }
        return total;
    }

    // Append " IN (SELECT <base.pk> FROM <Base>" — the open of the Q2 IN-subquery,
    // shared by both Q2-prefix builders (the leading "<col>_id"/"<col>" WHERE clause
    // and trailing ")" + base clauses are appended by the caller). Base supplies the
    // pk + table name. ConstexprString size is per-instantiation, hence the `auto&`.
    //
    // WidenForCompositePk is supplied by the CALLER rather than re-derived from
    // Base::has_composite_pk_ here, so each Q2-prefix builder states the arity of
    // its OWN comparison LHS at the point it emits it. Both builders pass
    // Base::has_composite_pk_ today — reverse-FK's LHS is the owning table's real
    // N-column FK (Task 6/7), m2m's is the junction's N owner-key columns (Task 9,
    // which widened it from the single "<Base>_id" it used to be). Widening the RHS
    // against an LHS of a different arity is a runtime error, not a silent
    // miscompare ("sub-select returns 3 columns - expected 1"), which is exactly
    // why the choice stays visible at the call site. A composite
    // base PK emits a PLAIN comma-joined column list "<a>, <b>, ..."
    // (row-value-IN-subquery form: SQLite/PG match row arity by SELECT-list
    // column COUNT, not by literal tuple syntax — parenthesizing the list
    // here would make it one parenthesized scalar expression instead of N
    // columns, which SQLite evaluates as "sub-select returns 1 columns", a
    // real bug caught during this task's own testing, not a hypothetical).
    // The caller wraps the OUTER comparison side in "(...)" when its OWN LHS
    // is multi-column (matching #501's bulk-DELETE row-value-IN precedent
    // there); single-column stays the exact pre-#504 form.
    template <typename Base, bool WidenForCompositePk> consteval auto append_in_subquery_open(auto& result) -> void {
        result.append(" IN (SELECT ");
        if constexpr (WidenForCompositePk) {
            append_pk_part_list<Base>(result);
        } else {
            result.append(Base::pk_name_);
        }
        result.append(" FROM ");
        result.append(Base::table_name_);
    }

    // Byte size of what append_in_subquery_open emits: " IN (SELECT <pk> FROM <Base>"
    // (or " IN (SELECT <a>, <b>, ... FROM <Base>" when WidenForCompositePk).
    template <typename Base, bool WidenForCompositePk> consteval auto in_subquery_open_size() -> std::size_t {
        std::size_t pk_part = WidenForCompositePk ? pk_part_list_size<Base>() : Base::pk_name_.size();
        return 12 + pk_part + 6 + Base::table_name_.size();
    }

    // Append ", <sep_prefix><col>[_id]" for RelatedBase member index I into
    // `result` — one member's contribution to the Q2 related/owner column block.
    // A regular (non-FK, or single-column-target FK) member emits ONE column,
    // "_id" suffixed for an FK (#422); a COMPOSITE-FK member (#504 Task 8 — e.g.
    // Shipment::line, itself targeting a composite-PK model) emits N
    // separately-prefixed columns "<sep_prefix><fk>_<part1>, <sep_prefix><fk>_
    // <part2>, …" instead — a plain comma-joined list would alias only the first
    // part. N=1 is byte-identical to the pre-#504 single-column form. `result` is
    // any buffer with the ConstexprString/ClauseSizer `.append` interface — the
    // SAME templated body serves both the actual SQL writer and the exact-size counter
    // (relation_columns_size below), so the two can never drift (mirrors
    // schema.cppm's ClauseSizer technique). `I` is a template parameter (not a
    // runtime loop variable) because RelatedBase::all_members_[I] must bind to a
    // `constexpr` for the `is_fk_field`/FieldType branch below to be usable in
    // `if constexpr` — a plain `for` loop index cannot do that even inside a
    // consteval function.
    template <typename RelatedBase, std::size_t I>
    consteval auto append_one_relation_member_columns(auto& result, std::string_view sep_prefix) -> void {
        constexpr auto member = RelatedBase::all_members_[I];
        if constexpr (meta::is_fk_field(member)) {
            using FieldType                   = std::remove_cvref_t<typename[:std::meta::type_of(member):]>;
            constexpr std::size_t field_parts = RelatedBase::template fk_primary_key_count<FieldType>();
            if constexpr (field_parts == 1) {
                result.append(", ");
                result.append(sep_prefix);
                meta::append_column_name(result, member); // #422 — emits FK "_id"
            } else {
                constexpr auto target_pk = RelatedBase::template find_fk_primary_key_members<FieldType>();
                for (std::size_t p = 0; p < field_parts; ++p) {
                    result.append(", ");
                    result.append(sep_prefix);
                    storm::meta::append_fk_column_name_for_part(result, member, target_pk, p);
                }
            }
        } else {
            result.append(", ");
            result.append(sep_prefix);
            result.append(std::meta::identifier_of(member));
        }
    }

    template <typename RelatedBase, std::size_t... Is>
    consteval auto append_relation_columns_impl(
            auto& result, std::string_view sep_prefix, std::index_sequence<Is...> /*unused*/
    ) -> void {
        (append_one_relation_member_columns<RelatedBase, Is>(result, sep_prefix), ...);
    }

    // Append ", <sep_prefix><col>[_id]" for every member of RelatedBase into
    // `result` — the Q2 related/owner column block, leading-comma-separated.
    // sep_prefix is the alias dot-prefix, e.g. "t3." (m2m related) or "t2."
    // (reverse-FK owner).
    template <typename RelatedBase>
    consteval auto append_relation_columns(auto& result, std::string_view sep_prefix) -> void {
        append_relation_columns_impl<RelatedBase>(
                result, sep_prefix, std::make_index_sequence<RelatedBase::field_count_>{}
        );
    }

    // A length-counting sink mirroring the ConstexprString append interface, so
    // running append_relation_columns against it yields the EXACT rendered length
    // (mirrors schema.cppm's ClauseSizer) — the column-block buffer budget stays
    // in lock-step with what the writer actually emits, with no separate,
    // hand-maintained size calculation to keep in sync (the DRY violation the
    // former relation_columns_size/append_relation_columns pair had).
    struct RelationColumnsSizer {
        std::size_t    len = 0;
        consteval void append(std::string_view str) {
            len += str.size();
        }
    };

    // Exact byte size of what append_relation_columns<RelatedBase> emits for the
    // given sep_prefix.
    template <typename RelatedBase> consteval auto relation_columns_size(std::string_view sep_prefix) -> std::size_t {
        RelationColumnsSizer sizer;
        append_relation_columns<RelatedBase>(sizer, sep_prefix);
        return sizer.len;
    }

    // Which table owns the junction columns being named, and therefore which naming
    // rule spells them (#536). The two rules agree for a single-column PK, and for a
    // composite key whose parts are plain columns; they diverge the moment a PK part
    // is ITSELF an FK, which is why the distinction cannot be elided.
    enum class JunctionNaming : std::uint8_t {
        // Storm's synthetic auto-junction table. Storm names these columns itself, in
        // schema.cppm's junction DDL, and routes each part through append_column_name
        // (#422) — so an FK part's column is "<side>_<name>_id".
        Auto,
        // An explicit many_to_many_through<> model. The junction is a REAL user-declared
        // table whose columns are ordinary composite-FK columns, emitted by the regular
        // model DDL path (schema.cppm's append_composite_fk_part_column, whose consteval
        // twin is meta::append_fk_column_name_for_part). That rule spells the target
        // part's BARE identifier — "<member>_<part>", never "_id".
        Through,
    };

    // Append ONE junction column name for `SideBase`'s PK part index `part`, with no
    // alias prefix: "<side_col>_id" for a single-PK side (byte-identical to the
    // pre-#504 emission), "<side_col>_<part>" for a composite one (#504 Task 9).
    // `side_col` is the junction column's base name — the side's TABLE name for an
    // auto-junction, or the through model's FK MEMBER name for a through junction.
    //
    // `Naming` selects which table's rule spells a COMPOSITE part; the single-PK
    // branch is shared, since both rules produce exactly "<side_col>_id" there (that
    // is what keeps every pre-#536 junction's SQL byte-identical). Each branch must
    // stay byte-identical to the DDL that actually CREATES the columns it reads:
    // Auto → schema.cppm's detail::append_junction_side_column_name, Through → the
    // ordinary composite-FK column DDL for the through model's own table.
    template <typename SideBase, JunctionNaming Naming>
    consteval auto append_junction_side_col(auto& result, std::string_view side_col, std::size_t part) -> void {
        result.append(side_col);
        if constexpr (SideBase::has_composite_pk_) {
            result.append("_");
            if constexpr (Naming == JunctionNaming::Auto) {
                // #422 — an FK key part's column is "<name>_id", so the junction column is
                // "<side>_<name>_id"; naming the bare member here would not match the
                // junction DDL's own append_junction_side_column_name.
                meta::append_column_name(result, SideBase::primary_key_members_[part]);
            } else {
                // The through model's column is a plain composite-FK column: bare part
                // identifier, no "_id" even when the part is an FK. Routing this through
                // append_column_name is exactly the #536 bug — it asked for a column
                // ("<member>_<part>_id") the through table does not have.
                //
                // This is the tail of storm::meta::append_fk_column_name_for_part's
                // composite branch — that helper spells "<fk member>_<target part>", and
                // `side_col` is already the FK member's identifier in Through mode (see
                // M2MJoinStatement::owner_col_name/related_col_name in join.cppm), so the two agree by
                // construction. It is not called directly because it re-appends the member
                // name that `side_col` has already contributed above; the shared prefix
                // write is what the Auto branch needs too. If that helper's composite
                // spelling ever changes, THIS line must change with it.
                result.append(std::meta::identifier_of(SideBase::primary_key_members_[part]));
            }
        } else {
            result.append("_id");
        }
    }

    // Append a junction side's whole column list, each part individually aliased with
    // `prefix` (e.g. "t2."): "<prefix><side>_id" for a single-PK side, or
    // "<prefix><side>_<part1>, <prefix><side>_<part2>, …" for a composite one. A plain
    // comma-joined name list would alias only the first part.
    //
    // Separate from append_owner_fk_col_list (the reverse-FK sibling) because the two
    // derive their names from different sources: reverse-FK names an FK MEMBER of the
    // owning model (whose composite spelling comes from the TARGET's parts via
    // append_fk_column_name_for_part), the junction names a junction column directly.
    template <typename SideBase, JunctionNaming Naming>
    consteval auto append_junction_side_col_list(auto& result, std::string_view side_col, std::string_view prefix)
            -> void {
        for (std::size_t p = 0; p < SideBase::primary_key_column_count_; ++p) {
            if (p > 0) {
                result.append(", ");
            }
            result.append(prefix);
            append_junction_side_col<SideBase, Naming>(result, side_col, p);
        }
    }

    // Append "t2.<part1>, t2.<part2>, …" — the owner-side FK column list, each part
    // individually aliased (a plain comma-joined name list would alias only the
    // first part). fk_member/target_pk_members are the same pair the JOIN ON-clause
    // AND-chain uses (#504 Task 7). Shared by the Q2 SELECT head and the Q2 WHERE
    // clause's outer comparison side (#504 Task 8) so the two can never drift.
    // N=1 collapses to exactly "t2.<fk>_id".
    template <std::size_t N>
    consteval auto append_owner_fk_col_list(
            auto& result, std::meta::info fk_member, const std::array<std::meta::info, N>& target_pk_members
    ) -> void {
        for (std::size_t p = 0; p < N; ++p) {
            if (p > 0) {
                result.append(", ");
            }
            result.append("t2.");
            storm::meta::append_fk_column_name_for_part(result, fk_member, target_pk_members, p);
        }
    }

    // Byte size of what append_owner_fk_col_list emits.
    template <std::size_t N>
    consteval auto
    owner_fk_col_list_size(std::meta::info fk_member, const std::array<std::meta::info, N>& target_pk_members)
            -> std::size_t {
        std::size_t total = 0;
        for (std::size_t p = 0; p < N; ++p) {
            if (p > 0) {
                total += 2; // ", "
            }
            total += 3; // "t2."
            total += storm::meta::fk_column_name_size_for_part(fk_member, target_pk_members, p);
        }
        return total;
    }

    // The reverse-FK Q2 SELECT head: "SELECT " + the owning table's own FK column(s)
    // back at Base, then the RelatedBase column block. The FK columns are named from
    // Base's PK parts via Task 6/7's composite-FK naming, NOT from the junction rule
    // above — a reverse-FK load has no junction table at all. N=1 collapses to exactly
    // "t2.<fk>_id", byte-identical to the pre-#504 form.
    template <typename RelatedBase, std::size_t N>
    consteval auto append_q2_select_head_multi(
            auto&                                 result,
            std::meta::info                       fk_member,
            const std::array<std::meta::info, N>& target_pk_members,
            std::string_view                      col_prefix
    ) -> void {
        result.append("SELECT ");
        append_owner_fk_col_list<N>(result, fk_member, target_pk_members);
        append_relation_columns<RelatedBase>(result, col_prefix);
    }

    // Byte size of what append_q2_select_head_multi emits for its owner-key part
    // (excluding the RelatedBase column block, sized separately by relation_columns_size).
    template <std::size_t N>
    consteval auto
    q2_select_head_multi_owner_size(std::meta::info fk_member, const std::array<std::meta::info, N>& target_pk_members)
            -> std::size_t {
        return 7 + owner_fk_col_list_size<N>(fk_member, target_pk_members); // "SELECT " + list
    }

} // namespace storm::orm::statements
