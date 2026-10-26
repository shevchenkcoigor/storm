module;

#include <meta>
#include <cassert>

export module storm_orm_statements_join;

import std;

import storm_orm_field_attr;
import storm_orm_statements_base;
import storm_orm_statements_extract;
import storm_orm_statements_join_grammar;
import storm_orm_statements_orderby;
import storm_orm_utilities;
import storm_orm_where;
import storm_db_concept;

export namespace storm::orm::statements {

    using storm::orm::utilities::ConstexprString;

    enum class JoinType : std::uint8_t { Inner, Left };

    // Type alias for type-erased pointers used in polymorphic JOIN wrapper.
    // void* is intentional here: JoinStatementWrapper must work with any model type T
    // without knowing T at compile time. The actual T* conversion happens in the
    // function pointers stored in make_join_wrapper().
    using ErasedObjectPtr    = void*; // NOSONAR(cpp:S5008) - type erasure requires void*
    using ErasedStatementPtr = void*; // NOSONAR(cpp:S5008) - type erasure requires void*

    // Builds clause-parameterized SQL (Q1 base subquery / per-relation Q2) from
    // the QuerySet's WHERE / ORDER BY / LIMIT / OFFSET state.
    using M2MClauseSqlFn = auto (*)(
            const orm::where::ExpressionVariantPtr&,
            const std::optional<OrderByWrapper>&,
            const std::optional<int>&,
            const std::optional<int>&
    ) -> std::string;

    // Helper to resolve the primary key type for a model (#507).
    // Returns int64_t for integer PKs, UUID for UUID PKs.
    namespace detail {
        template <typename T>
        using pk_key_type_t =
                std::conditional_t<BaseStatement<T>::has_uuid_pk_(), storm::orm::utilities::UUID, std::int64_t>;
    } // namespace detail

    // One eager-loaded m2m relation (#392): its Q2 builder plus the stitch
    // fn-pointers. extract_q2_owner_pk_fn keys the stitch into the shared Q1
    // pk→entity map; append_related_q2_fn fills the entity's container;
    // container_empty_fn + is_left drive the per-relation INNER drop.
    // The stitch key is a StitchKey byte buffer (#504), not the owner's PK type
    // itself: a composite PK (#500) has N parts of possibly DIFFERENT types, and
    // this descriptor crosses the type-erased vtable (ErasedStatementPtr — no T in
    // scope), so the key cannot be a template parameter here. StitchKey subsumes
    // both key types #507 templated PkKeyType on: an integer PK appends one
    // append_int64 (the exact byte layout of the old bare std::int64_t key), and a
    // storm::UUID PK appends one append_string — see append_dispatched_part, which
    // dispatches each part by its OWN stored type.
    struct M2MRelation {
        M2MClauseSqlFn build_q2_sql_fn                                                         = nullptr;
        auto (*extract_q2_owner_pk_fn)(ErasedStatementPtr) -> storm::orm::utilities::StitchKey = nullptr;
        // Single-column-PK fast path (#504 perf). Exactly ONE of these two extractors
        // is non-null, chosen at descriptor-build time by the base model's PK width:
        // a one-column key is a bare word, so it skips StitchKey's sret return, its
        // 32-byte zero-init and the out-of-line append_int64 call — all three per Q2
        // row. Composite keys keep extract_q2_owner_pk_fn. The pair exists because
        // M2MRelation is deliberately NOT templated (JoinStatementWrapper holds a
        // std::vector of these and is referenced from five modules), so the key type
        // cannot be a template parameter here; SelectStatement<T> re-derives the same
        // choice from Base::primary_key_column_count_ and reads the matching one.
        auto (*extract_q2_owner_pk_word_fn)(ErasedStatementPtr) -> std::uint64_t  = nullptr;
        auto (*append_related_q2_fn)(ErasedStatementPtr, ErasedObjectPtr) -> void = nullptr;
        auto (*container_empty_fn)(ErasedObjectPtr) -> bool                       = nullptr;
        // LEFT keeps zero-relation entities; INNER drops them after the stitch.
        bool is_left = false;
    };

    template <typename PkKeyType> struct JoinStatementWrapper {
        auto (*get_complete_sql_fn)() -> const std::string&;
        // Per-row extractor for FK joins. nullptr for m2m wrappers (the two-query
        // m2m path extracts base rows via Base::extract_all_columns, never this).
        auto (*extract_row_fn)(ErasedStatementPtr, ErasedObjectPtr) -> void = nullptr;

        // Many-to-many two-query predicate-pushdown extension (#391, #392).
        // build_q1_sql_fn produces the base-entity SELECT — its text depends only
        // on the base model + clauses, so ONE Q1 serves every relation. Each
        // eager-loaded m2m relation contributes one M2MRelation descriptor; the
        // stitch loop runs each Q2 in turn against the shared pk→entity map.
        // Empty for plain FK joins.
        //
        // PkKeyType (#507) survives as the wrapper's identity parameter — it keeps
        // aggregate/distinct/queryset's JoinStatementWrapper<pk_key_type_t<T>>
        // spellings intact — but the stitch key itself is now the type-erased
        // StitchKey (#504), which subsumes both int64_t and UUID keys and, unlike a
        // single PkKeyType, also spans a composite PK's N differently-typed parts.
        M2MClauseSqlFn           build_q1_sql_fn = nullptr;
        std::vector<M2MRelation> m2m_relations;

        [[nodiscard]] auto is_m2m() const -> bool {
            return !m2m_relations.empty();
        }

        // Get complete pre-computed SELECT...JOIN SQL
        [[nodiscard]] auto get_complete_sql() const -> const std::string& {
            return get_complete_sql_fn();
        }

        auto extract_row(ErasedStatementPtr stmt, ErasedObjectPtr obj) const -> void {
            // FK-join invariant: extract_row is never called on an m2m wrapper, whose
            // extract_row_fn stays nullptr (the two-query path uses extract_all_columns).
            assert(extract_row_fn != nullptr);
            extract_row_fn(stmt, obj);
        }
    };

    // =========================================================================
    // SHARED TWO-QUERY HELPERS — used by both the m2m (#391/#392) and reverse-FK
    // (#398) eager-load classes. Both run Q1 (base) + per-relation Q2 stitched by
    // a pk→entity map; these are the pieces that don't depend on the junction.
    // =========================================================================

    // Append the base-entity clauses — WHERE / ORDER BY / LIMIT / OFFSET — to `sql`.
    // Shared by Q1 (the base subquery) and Q2 (its IN-subquery): in both, these
    // clauses select WHICH base entities load.
    template <typename Base, storm::db::DatabaseConnection ConnType>
    auto append_base_clauses(
            std::string&                            sql,
            const orm::where::ExpressionVariantPtr& where_expr,
            const std::optional<OrderByWrapper>&    order_by,
            const std::optional<int>&               limit,
            const std::optional<int>&               offset
    ) -> void {
        if (where_expr) {
            sql += " WHERE ";
            sql += orm::where::to_sql(*where_expr);
        }
        Base::template append_order_by<ConnType>(sql, order_by);
        Base::template append_limit_offset<ConnType>(sql, limit, offset);
    }

    // Append `value` into a relation container (push_back, or insert for plf::hive).
    template <typename C, typename V> auto relation_insert_into(C& container, V&& value) -> void {
        if constexpr (requires { container.push_back(std::forward<V>(value)); }) {
            container.push_back(std::forward<V>(value));
        } else {
            container.insert(std::forward<V>(value)); // plf::hive
        }
    }

    // Extract one part of a composite FK target's primary key from a single column
    // (#504 Task 8) — mirrors base.cppm's extract_one_fk_part, which is protected on
    // BaseStatement and so not reachable from this free function. That mirroring is
    // load-bearing: this is the RELATION-side twin (Q2 result rows for an m2m related
    // entity or a reverse-FK owner), so the two must resolve a part identically or the
    // same model extracts differently depending on which side of the join it is on.
    //
    // The FK-part branch is therefore the same one #536 added to extract_one_fk_part: a
    // part that is itself an FK holds the REFERENCED row's key, so extract that key's
    // own type into the part's referenced-object member. Reading the declared type asks
    // ColumnExtractor for a whole model struct — a hard error, which is why this shape
    // did not compile. `fk_obj` is value-initialized by every caller before the fold
    // (see extract_relation_fk_column below), so the untouched members of the
    // referenced object are the same pk-only reconstruction the single-column path
    // produces.
    template <std::meta::info Member, typename Statement>
    auto extract_relation_fk_part(Statement* stmt, auto& fk_obj, int col) noexcept -> void {
        using PartType = std::remove_cvref_t<decltype(fk_obj.[:Member:])>;
        if constexpr (meta::is_fk_field(Member)) {
            constexpr auto part_pk        = BaseStatement<PartType>::primary_key_;
            using PartPKType              = std::remove_cvref_t<decltype(std::declval<PartType>().[:part_pk:])>;
            fk_obj.[:Member:].[:part_pk:] = ColumnExtractor::extract_column_value<PartPKType>(stmt, col);
        } else {
            fk_obj.[:Member:] = ColumnExtractor::extract_column_value<PartType>(stmt, col);
        }
    }

    // Extract every part of a composite FK target's primary key from N consecutive
    // columns starting at col, in declaration order (#504 Task 8) — mirrors
    // base.cppm's extract_fk_parts_impl/extract_fk_parts (protected there).
    template <typename RelatedBase, typename FieldType, typename Statement, std::size_t... Is>
    auto extract_relation_fk_parts_impl(
            Statement* stmt, auto& fk_obj, int col, std::index_sequence<Is...> /*unused*/
    ) noexcept -> void {
        constexpr auto members = RelatedBase::template find_fk_primary_key_members<FieldType>();
        (extract_relation_fk_part<members[Is]>(stmt, fk_obj, col + static_cast<int>(Is)), ...);
    }

    template <typename RelatedBase, typename FieldType, typename Statement>
    auto extract_relation_fk_parts(Statement* stmt, auto& fk_obj, int col) noexcept -> void {
        extract_relation_fk_parts_impl<RelatedBase, FieldType>(
                stmt, fk_obj, col, std::make_index_sequence<RelatedBase::template fk_primary_key_count<FieldType>()>{}
        );
    }

    // Extract one FK column (or column BLOCK, for a composite target, #504 Task 8)
    // for a related/owner entity: pk-only FK object, NULL (in the first column) →
    // nullopt. RelatedBase = BaseStatement of the entity being extracted (provides
    // the composite-FK helpers); Related = that entity type; Member = its FK data
    // member. N=1 (single-column target) is the exact pre-#504 single-column path.
    template <typename RelatedBase, typename Related, typename FieldType, std::meta::info Member, typename Statement>
    auto extract_relation_fk_column(Statement* stmt, Related& rel, int col) noexcept -> void {
        using InnerFK = utilities::optional_inner_type_t<FieldType>;
        if constexpr (utilities::is_optional_v<FieldType>) {
            if (stmt->is_null(col)) {
                rel.[:Member:] = std::nullopt;
                return;
            }
        }
        InnerFK inner{};
        if constexpr (RelatedBase::template fk_primary_key_count<FieldType>() == 1) {
            constexpr auto fk_pk = RelatedBase::template find_fk_primary_key<FieldType>();
            using PKType         = std::remove_cvref_t<decltype(std::declval<InnerFK>().[:fk_pk:])>;
            inner.[:fk_pk:]      = ColumnExtractor::template extract_column_value<PKType>(stmt, col);
        } else {
            extract_relation_fk_parts<RelatedBase, FieldType>(stmt, inner, col);
        }
        rel.[:Member:] = std::move(inner);
    }

    // How many SQL columns member I of RelatedBase occupies: 1 for a regular
    // column, N for an FK whose target has an N-part key (#504 Task 8).
    template <typename RelatedBase, typename Related, std::size_t I> consteval auto relation_member_width() -> int {
        constexpr auto member = RelatedBase::all_members_[I];
        if constexpr (meta::is_fk_field(member)) {
            using FieldType = std::remove_cvref_t<decltype(std::declval<Related&>().[:member:])>;
            return static_cast<int>(RelatedBase::template fk_primary_key_count<FieldType>());
        } else {
            return 1;
        }
    }

    // SQL column position of member I: ColOffset plus the widths of members
    // 0..I-1. A composite-FK member makes position diverge from member index from
    // that member onward, but every width is a compile-time constant, so the
    // position is one too — it never needs a runtime accumulator.
    //
    // This is load-bearing, not tidiness. Threading a runtime `int& col_idx`
    // through this fold (the shape #504 Task 8 originally used) turned
    // extract_relation_member from a fully-inlined constant-offset read into an
    // out-of-line call taking a reference, once per column per Q2 row: perf diff
    // against the merge-base attributed +7.8% relative to exactly that symbol on
    // Storm/M2M/join_select/fanout200.
    template <typename RelatedBase, typename Related, int ColOffset, std::size_t... Is>
    consteval auto relation_column_offset_impl(std::index_sequence<Is...> /*unused*/) -> int {
        return ColOffset + (relation_member_width<RelatedBase, Related, Is>() + ... + 0);
    }

    template <typename RelatedBase, typename Related, int ColOffset, std::size_t I>
    consteval auto relation_column_offset() -> int {
        return relation_column_offset_impl<RelatedBase, Related, ColOffset>(std::make_index_sequence<I>{});
    }

    // Extract one related/owner member from its (compile-time) SQL column
    // position. Regular columns by value, FK columns as pk-only objects — a
    // composite-FK member consumes a BLOCK of columns starting there (mirrors
    // plain-SELECT per-member semantics).
    template <typename RelatedBase, typename Related, int ColOffset, std::size_t I, typename Statement>
    auto extract_relation_member(Statement* stmt, Related& rel) noexcept -> void {
        constexpr auto member = RelatedBase::all_members_[I];
        constexpr int  col    = relation_column_offset<RelatedBase, Related, ColOffset, I>();
        using FieldType       = std::remove_cvref_t<decltype(rel.[:member:])>;
        if constexpr (meta::is_fk_field(member)) {
            extract_relation_fk_column<RelatedBase, Related, FieldType, member>(stmt, rel, col);
        } else {
            rel.[:member:] = ColumnExtractor::template extract_column_value<FieldType>(stmt, col);
        }
    }

    // Extract a full related/owner entity from columns starting at ColOffset.
    // Each member's column position is computed at compile time (see
    // relation_column_offset), so this stays a flat sequence of constant-offset
    // reads even when a composite-FK member widens the row.
    template <typename RelatedBase, typename Related, int ColOffset, typename Statement, std::size_t... Is>
    auto extract_relation_entity(Statement* stmt, Related& rel, std::index_sequence<Is...> /*unused*/) noexcept
            -> void {
        ((extract_relation_member<RelatedBase, Related, ColOffset, Is>(stmt, rel)), ...);
    }

    // Q1 — the base entity subquery: SELECT <base cols> FROM <Base> [clauses]. Its
    // text matches the subquery embedded in Q2's IN clause, keeping the two in lockstep.
    template <typename Base, storm::db::DatabaseConnection ConnType>
    auto build_base_subquery_sql(
            const orm::where::ExpressionVariantPtr& where_expr,
            const std::optional<OrderByWrapper>&    order_by,
            const std::optional<int>&               limit,
            const std::optional<int>&               offset
    ) -> std::string {
        std::string sql = "SELECT ";
        sql += Base::field_names_array_.view();
        sql += " FROM ";
        sql += Base::table_name_;
        append_base_clauses<Base, ConnType>(sql, where_expr, order_by, limit, offset);
        return sql;
    }

    // The Q1 (base subquery) clause-builder fn-pointer for a two-query join statement
    // First. Q1 text depends only on the base model + clauses, so one builder serves
    // every relation. Shared by the m2m (#392) and reverse-FK (#398) wrapper factories.
    template <typename First> auto make_q1_sql_fn() -> M2MClauseSqlFn {
        return +[](const orm::where::ExpressionVariantPtr& where_expr,
                   const std::optional<OrderByWrapper>&    order_by,
                   const std::optional<int>&               limit,
                   const std::optional<int>&               offset) -> std::string {
            return First::build_base_subquery(where_expr, order_by, limit, offset);
        };
    }

    // Q2 — `prefix` (everything up to the open IN-subquery) + the base clauses + ")".
    // The clauses live inside the IN-subquery so they bound WHICH base entities load.
    template <typename Base, storm::db::DatabaseConnection ConnType>
    auto build_q2_sql_from_prefix(
            std::string_view                        prefix,
            const orm::where::ExpressionVariantPtr& where_expr,
            const std::optional<OrderByWrapper>&    order_by,
            const std::optional<int>&               limit,
            const std::optional<int>&               offset
    ) -> std::string {
        std::string sql{prefix};
        append_base_clauses<Base, ConnType>(sql, where_expr, order_by, limit, offset);
        sql += ")";
        return sql;
    }

    // Two-query base for the eager-load join statements (m2m #391/#392, reverse-FK
    // #398). It owns the parts whose text is identical across both: Q1 (the base
    // subquery), Q2 (the derived's q2 prefix + base clauses), the JOIN keyword, and
    // the Q2 owner-pk extractor (column 0). The derived class supplies the prefix
    // (q2_prefix_view()) and the relation-specific column/stitch logic.
    template <typename Derived, typename Base, storm::db::DatabaseConnection ConnType, JoinType Type>
    class TwoQueryJoinBase {
      protected:
        static consteval auto join_keyword() -> std::string_view {
            return Type == JoinType::Inner ? " INNER JOIN " : " LEFT JOIN ";
        }

      public:
        // Q1 — the base entity subquery.
        static auto build_base_subquery(
                const orm::where::ExpressionVariantPtr& where_expr,
                const std::optional<OrderByWrapper>&    order_by,
                const std::optional<int>&               limit,
                const std::optional<int>&               offset
        ) -> std::string {
            return build_base_subquery_sql<Base, ConnType>(where_expr, order_by, limit, offset);
        }

        // Q2 — the derived's prefix (up to the open IN-subquery) + base clauses + ")".
        static auto build_q2_sql(
                const orm::where::ExpressionVariantPtr& where_expr,
                const std::optional<OrderByWrapper>&    order_by,
                const std::optional<int>&               limit,
                const std::optional<int>&               offset
        ) -> std::string {
            return build_q2_sql_from_prefix<Base, ConnType>(
                    Derived::q2_prefix_arr_.view(), where_expr, order_by, limit, offset
            );
        }

        // Q2 row owner key (columns 0..N-1) — keys the stitch into the Q1 hash map.
        // N is Derived::owner_key_column_count_, which both derived classes define as
        // Base::primary_key_column_count_ since #504 Task 9 widened the m2m junction
        // to one owner column per PK part (reverse-FK already matched, Task 8: the
        // owning table carries a REAL N-column FK). It stays a Derived:: constant
        // rather than being read off Base here so the count remains ONE named value
        // per statement class, shared with that class's Q2 SELECT emission and
        // insert_related offset: reading a different count here than the SELECT emits
        // would read PAST the owner key into the RELATED entity's own columns — a
        // silent cross-entity misread (column_int64 on a TEXT column just returns 0),
        // not merely a stitch collision. Base::primary_key_members_ may mix types
        // (int, std::string, int64_t, …), so each part is dispatched to
        // append_int64/append_string by ITS OWN type at compile time (mirrors
        // bind_one_pk_part in base.cppm). N=1 (single-column PK, or any m2m relation)
        // reads exactly column 0 with one append_int64/append_string call — the same
        // byte layout StitchKey would have held for the old bare-int64_t stitch key,
        // so that case stays behaviorally identical.
        //
        // The index sequence is over Base::primary_key_members_ directly (a fold
        // expression, not a nested generic lambda) because a loop variable indexing a
        // constexpr std::meta::info array cannot be spliced into a template argument
        // inside a lambda body — the same restriction bind_pk_values_impl in
        // base.cppm works around with this exact shape.
        static auto extract_q2_owner_pk(typename ConnType::Statement* stmt) noexcept
                -> storm::orm::utilities::StitchKey {
            storm::orm::utilities::StitchKey key;
            extract_q2_owner_pk_impl(key, stmt, std::make_index_sequence<Derived::owner_key_column_count_>{});
            return key;
        }

        // Single-column-PK fast path (#504 perf): the owner key as one word, skipping
        // StitchKey entirely. The general extractor above is correct for this case
        // too, but crossing the M2MRelation fn-pointer boundary with a 33-byte return
        // costs three things per Q2 row that a bare word does not: an sret memory
        // round-trip, the 32-byte zero-init of StitchKey::bytes_, and an out-of-line
        // call to append_int64 (an exported module symbol, so not inlinable into the
        // erased thunk). Together those were the residual regression left after the
        // stitch map itself was narrowed. Reuses the SAME per-part type dispatch as
        // the general path, so the two cannot disagree about what column 0 holds.
        // Instantiated only when owner_key_column_count_ == 1 — the caller
        // (make_relation_descriptor) picks between the two at compile time, so a
        // composite key can never reach this.
        static auto extract_q2_owner_pk_word(typename ConnType::Statement* stmt) noexcept -> std::uint64_t
            requires(Derived::owner_key_column_count_ == 1)
        {
            return extract_pk_word_at<Base::primary_key_members_[0], 0>(stmt);
        }

      private:
        template <std::size_t... Is>
        static void extract_q2_owner_pk_impl(
                storm::orm::utilities::StitchKey& key,
                typename ConnType::Statement*     stmt,
                std::index_sequence<Is...> /*unused*/
        ) {
            (append_pk_part_to_key<Base::primary_key_members_[Is], static_cast<int>(Is)>(key, stmt), ...);
        }

        // Append one PK part (Member, read from column ColIdx) into `key`, dispatched
        // by the part's OWN stored type. An FK part's SQL column stores the referenced
        // row's key (the "<name>_id" column, #422), so the dispatch type is the
        // referenced key's type, not the FK member's C++ type (mirrors bind_one_pk_part).
        template <std::meta::info Member, int ColIdx>
        static void append_pk_part_to_key(storm::orm::utilities::StitchKey& key, typename ConnType::Statement* stmt) {
            if constexpr (meta::is_fk_field(Member)) {
                using FKType         = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                using InnerFK        = utilities::optional_inner_type_t<FKType>;
                constexpr auto fk_pk = Base::template find_fk_primary_key<FKType>();
                using PartType       = std::remove_cvref_t<decltype(std::declval<InnerFK>().[:fk_pk:])>;
                append_dispatched_part<PartType>(key, stmt, ColIdx);
            } else {
                using PartType = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                append_dispatched_part<PartType>(key, stmt, ColIdx);
            }
        }

        // storm::UUID is listed alongside the text types (#507): a UUID PK is stored
        // as TEXT/UUID, so reading its column as an integer would silently key the
        // whole stitch map on 0. It cannot be folded into the text arm by
        // convertibility — UUID converts implicitly to string_view, but PartType is
        // matched exactly here, so it needs its own name in the condition.
        template <typename PartType>
        static void
        append_dispatched_part(storm::orm::utilities::StitchKey& key, typename ConnType::Statement* stmt, int col) {
            if constexpr (std::same_as<PartType, std::string> || std::same_as<PartType, std::string_view> ||
                          std::same_as<PartType, storm::orm::utilities::UUID>) {
                key.append_string(ColumnExtractor::read_text_view(stmt, col));
            } else {
                key.append_int64(ColumnExtractor::extract_int_like<std::int64_t>(stmt, col));
            }
        }

        // The key WORD one PK part contributes — the single source of truth for
        // "what does column ColIdx contribute to the stitch key", shared by the
        // StitchKey path (via append_dispatched_part, whose two appenders write
        // exactly this) and the single-PK word fast path. Keeping the type dispatch
        // in one place is what guarantees the two extractors agree; a text part
        // contributes its hash on both sides, an integral part its int64 value.
        template <std::meta::info Member, int ColIdx>
        static auto extract_pk_word_at(typename ConnType::Statement* stmt) noexcept -> std::uint64_t {
            if constexpr (meta::is_fk_field(Member)) {
                using FKType         = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                using InnerFK        = utilities::optional_inner_type_t<FKType>;
                constexpr auto fk_pk = Base::template find_fk_primary_key<FKType>();
                using PartType       = std::remove_cvref_t<decltype(std::declval<InnerFK>().[:fk_pk:])>;
                return dispatched_pk_word<PartType>(stmt, ColIdx);
            } else {
                using PartType = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                return dispatched_pk_word<PartType>(stmt, ColIdx);
            }
        }

        // The arms here MUST match append_dispatched_part's exactly — a single-column
        // PK takes this path while the object side builds its key through
        // append_string/append_int64, so any type routed differently by the two keys
        // the map on a value the other never produces. storm::UUID (#507) is the case
        // that makes this load-bearing rather than theoretical: a UUID PK is always
        // single-column, so it always reaches THIS function, and it is stored as
        // TEXT/UUID — reading it as an integer would key every owner on 0.
        template <typename PartType>
        static auto dispatched_pk_word(typename ConnType::Statement* stmt, int col) noexcept -> std::uint64_t {
            if constexpr (std::same_as<PartType, std::string> || std::same_as<PartType, std::string_view> ||
                          std::same_as<PartType, storm::orm::utilities::UUID>) {
                return std::hash<std::string_view>{}(ColumnExtractor::read_text_view(stmt, col));
            } else {
                return static_cast<std::uint64_t>(ColumnExtractor::extract_int_like<std::int64_t>(stmt, col));
            }
        }
    };

    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info... FKFields>
        requires(sizeof...(FKFields) >= 1 && (FKFieldOf<T, FKFields> && ...))
    class JoinStatement : private BaseStatement<T> {
        friend class BaseStatement<T>;
        using Base      = BaseStatement<T>;
        using Error     = typename ConnType::Error;
        using Statement = typename ConnType::Statement;

        static constexpr std::size_t fk_count_ = sizeof...(FKFields);

        // C++26: Direct pack indexing
        // FK_type unwraps std::optional<FKModel> → FKModel so FKBase_at works correctly
        template <std::size_t Idx>
        using FK_type =
                utilities::optional_inner_type_t<std::remove_cvref_t<typename[:std::meta::type_of(FKFields...[Idx]):]>>;

        template <std::size_t Idx> using FKBase_at = BaseStatement<FK_type<Idx>>;

        static consteval auto count_non_fk_fields() -> std::size_t {
            std::size_t count = 0;
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                if (!Base::is_fk_field(Base::all_members_[i])) {
                    count++;
                }
            }
            return count;
        }

        static constexpr std::size_t non_fk_field_count_ = count_non_fk_fields();

        // LCOV_EXCL_START - compile-time only (initializes constexpr column_offsets_)
        // Constexpr storage for column offsets
        static constexpr auto calculate_column_offsets() {
            std::array<std::size_t, fk_count_> offsets{};
            std::size_t                        current_offset = non_fk_field_count_;
            std::size_t                        idx            = 0;

            [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                ((offsets[idx++] = current_offset, current_offset += FKBase_at<Is>::field_count_), ...);
            }(std::make_index_sequence<fk_count_>{});

            return offsets;
        }
        // LCOV_EXCL_STOP

        static constexpr auto column_offsets_ = calculate_column_offsets();

        // LCOV_EXCL_START - compile-time only (called from consteval functions)
        static constexpr auto get_join_keyword() -> std::string_view {
            if constexpr (Type == JoinType::Inner) {
                return " INNER JOIN ";
            } else {
                return " LEFT JOIN ";
            }
        }
        // LCOV_EXCL_STOP

        // Byte size of the AND-joined ON clause body for FK index Is:
        // "<part1> = t1.<fk>_<part1> AND <part2> = t1.<fk>_<part2> ..." (target-side
        // identifiers are the target's OWN PK column names — never suffixed; only the
        // local side goes through the composite naming convention, #504). N == 1
        // collapses to EXACTLY "<pk> = t1.<fk>_id" — byte-identical to the pre-#504
        // single-column clause.
        template <std::size_t Is> static consteval auto fk_on_clause_body_size() -> std::size_t {
            using utilities::numeric::digits_of;
            using utilities::sql_len::AND;
            constexpr auto target_pk = Base::template find_fk_primary_key_members<FK_type<Is>>();
            std::size_t    total     = 0;
            for (std::size_t p = 0; p < target_pk.size(); ++p) {
                if (p > 0) {
                    // append_fk_on_clause_body writes " AND t<alias>." here, not just
                    // " AND " — reserve the exact bytes: AND (" AND ") + "t" + the
                    // alias's digit width + ".".
                    total += AND + 1 + digits_of(Is + 2) + 1;
                }
                total += std::meta::identifier_of(target_pk[p]).size();
                total += 6; // " = t1."
                total += storm::meta::fk_column_name_size_for_part(FKFields...[Is], target_pk, p);
            }
            return total;
        }

        // Compile-time SQL generation with ConstexprString
        static consteval auto calculate_join_sql_size() -> std::size_t {
            using utilities::numeric::digits_of;
            using utilities::sql_len::SMALL_BUFFER;
            std::size_t total = 0;

            // Per FK: <keyword><table> t<alias> ON t<alias>.<on-clause-body>
            // The alias t<Is+2> appears twice — reserve its exact digit width both times.
            [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                ((total += get_join_keyword().size() + FKBase_at<Is>::table_name_.size() + 2 + digits_of(Is + 2) + 5 +
                           digits_of(Is + 2) + 1 + fk_on_clause_body_size<Is>()),
                 ...);
            }(std::make_index_sequence<fk_count_>{});

            return total + SMALL_BUFFER;
        }

        // Appends "<part1> = t1.<fk>_<part1> AND <part2> = t1.<fk>_<part2> ..." for FK
        // index Is — the target alias prefix ("t<alias>.") is already written by the
        // caller before the first part; each subsequent part re-writes its own
        // "t<alias>." prefix after the AND.
        template <std::size_t Is, typename Result> static consteval void append_fk_on_clause_body(Result& result) {
            constexpr auto target_pk = Base::template find_fk_primary_key_members<FK_type<Is>>();
            for (std::size_t p = 0; p < target_pk.size(); ++p) {
                if (p > 0) {
                    result.append(" AND t");
                    result.append_uint(Is + 2);
                    result.append(".");
                }
                result.append(std::meta::identifier_of(target_pk[p]));
                result.append(" = t1.");
                storm::meta::append_fk_column_name_for_part(result, FKFields...[Is], target_pk, p);
            }
        }

        static consteval auto build_join_sql_array() {
            constexpr std::size_t     sql_size = calculate_join_sql_size();
            ConstexprString<sql_size> result;

            [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                ((result.append(get_join_keyword()),
                  result.append(FKBase_at<Is>::table_name_),
                  result.append(" t"),
                  result.append_uint(Is + 2),
                  result.append(" ON t"),
                  result.append_uint(Is + 2),
                  result.append("."),
                  append_fk_on_clause_body<Is>(result)),
                 ...);
            }(std::make_index_sequence<fk_count_>{});

            return result;
        }

        // Iterate the FK indices [0, fk_count_) and invoke `body.template operator()<I>()` for each.
        // Used by the consteval SQL-builder helpers; encapsulates the index-sequence fold.
        template <typename Body> static consteval void for_each_fk_field(const Body& body) {
            [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
                (body.template operator()<Is>(), ...);
            }(std::make_index_sequence<fk_count_>{});
        }

        static consteval auto calculate_select_fields_size() -> std::size_t {
            std::size_t total = 0;

            // Base fields
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                auto member = Base::all_members_[i];
                if (!Base::is_fk_field(member)) {
                    total += (total > 0 ? 2 : 0) + 3 + std::meta::identifier_of(member).size();
                }
            }

            // FK fields — each emits "t<alias>.<field>"; reserve the alias's exact digit width.
            for_each_fk_field([&]<std::size_t I>() {
                total += 2; // ", "
                [&]<std::size_t... FieldIs>(std::index_sequence<FieldIs...> /*unused*/) {
                    ((total += (FieldIs > 0 ? 2 : 0) + 2 + utilities::numeric::digits_of(I + 2) +
                               std::meta::identifier_of(FKBase_at<I>::all_members_[FieldIs]).size()),
                     ...);
                }(std::make_index_sequence<FKBase_at<I>::field_count_>{});
            });

            return total + utilities::sql_len::SMALL_BUFFER;
        }

        static consteval auto build_select_fields_array() {
            constexpr std::size_t        fields_size = calculate_select_fields_size();
            ConstexprString<fields_size> result;

            // Base fields
            bool first = true;
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                auto member = Base::all_members_[i];
                if (!Base::is_fk_field(member)) {
                    if (!first) {
                        result.append(", ");
                    }
                    result.append("t1.");
                    result.append(std::meta::identifier_of(member));
                    first = false;
                }
            }

            // FK fields
            for_each_fk_field([&]<std::size_t I>() {
                result.append(", ");
                [&]<std::size_t... FieldIs>(std::index_sequence<FieldIs...> /*unused*/) {
                    bool first_in_table = true;
                    (((first_in_table ? (void)0 : result.append(", ")),
                      result.append("t"),
                      result.append_uint(I + 2),
                      result.append("."),
                      result.append(std::meta::identifier_of(FKBase_at<I>::all_members_[FieldIs])),
                      first_in_table = false),
                     ...);
                }(std::make_index_sequence<FKBase_at<I>::field_count_>{});
            });

            return result;
        }

        static constexpr auto join_sql_array      = build_join_sql_array();
        static constexpr auto select_fields_array = build_select_fields_array();

        // NEW: Compile-time complete SQL generation
        static consteval auto calculate_complete_sql_size() -> std::size_t {
            using utilities::sql_len::FROM;
            using utilities::sql_len::SELECT;
            using utilities::sql_len::SMALL_BUFFER;
            std::size_t total = 0;
            total += SELECT; // "SELECT "
            total += calculate_select_fields_size();
            total += FROM; // " FROM "
            total += Base::table_name_.size();
            total += 3; // " t1"
            total += calculate_join_sql_size();
            return total + SMALL_BUFFER;
        }

        static consteval auto build_complete_sql_array() {
            constexpr std::size_t     sql_size = calculate_complete_sql_size();
            ConstexprString<sql_size> result;

            result.append("SELECT ");
            result.append(select_fields_array);
            result.append(" FROM ");
            result.append(Base::table_name_);
            result.append(" t1");
            result.append(join_sql_array);

            return result;
        }

        static constexpr auto complete_sql_array = build_complete_sql_array();

      public:
        // Get complete pre-computed SELECT...JOIN SQL with lazy initialization
        static auto get_complete_sql() -> const std::string& {
            static const std::string str{complete_sql_array.data.data(), complete_sql_array.len};
            return str;
        }

      private:
        // =====================================================================
        // DATABASE-AGNOSTIC EXTRACTION - Uses Statement template methods
        // =====================================================================

        template <std::size_t... Is>
        __attribute__((always_inline)) static void
        extract_t_fields(Statement* stmt, T& obj, std::index_sequence<Is...> /*unused*/) noexcept {
            int col_idx = 0;
            ((extract_column_at<Is>(stmt, obj, col_idx)), ...);
        }

        template <std::size_t MemberIdx>
        __attribute__((always_inline)) static void extract_column_at(Statement* stmt, T& obj, int& col_idx) noexcept {
            if constexpr (MemberIdx < Base::field_count_) {
                constexpr auto member = Base::all_members_[MemberIdx];

                if constexpr (Base::is_fk_field(member)) {
                    return;
                } else {
                    using FieldType = std::remove_cvref_t<decltype(obj.[:member:])>;
                    obj.[:member:]  = ColumnExtractor::template extract_column_value<FieldType>(stmt, col_idx);
                    col_idx++;
                }
            }
        }

        template <std::size_t FKIdx, std::size_t... FieldIs>
        __attribute__((always_inline)) static void extract_fk_fields_impl(
                Statement*      stmt,
                FK_type<FKIdx>& fk_obj,
                std::size_t     col_offset,
                std::index_sequence<FieldIs...> /*unused*/
        ) noexcept {
            using FKBase = FKBase_at<FKIdx>;
            ((extract_fk_field_at<FKBase, FieldIs>(stmt, fk_obj, col_offset + FieldIs)), ...);
        }

        template <typename FKBase, std::size_t FieldIdx>
        __attribute__((always_inline)) static void
        extract_fk_field_at(Statement* stmt, auto& fk_obj, int col_idx) noexcept {
            if constexpr (FieldIdx < FKBase::field_count_) {
                constexpr auto member = FKBase::all_members_[FieldIdx];
                using FieldType       = std::remove_cvref_t<decltype(fk_obj.[:member:])>;
                fk_obj.[:member:]     = ColumnExtractor::template extract_column_value<FieldType>(stmt, col_idx);
            }
        }

        template <std::size_t Idx>
        __attribute__((always_inline)) static void extract_fk_at(Statement* stmt, T& obj) noexcept {
            constexpr auto FKField            = FKFields...[Idx]; // C++26 pack indexing
            using RawFKFieldType              = std::remove_cvref_t<decltype(obj.[:FKField:])>;
            constexpr std::size_t field_count = FKBase_at<Idx>::field_count_;

            if constexpr (utilities::is_optional_v<RawFKFieldType>) {
                // Optional FK: NULL first joined column means no match → set nullopt
                const auto first_col = static_cast<int>(column_offsets_[Idx]);
                if (stmt->is_null(first_col)) {
                    obj.[:FKField:] = std::nullopt;
                } else {
                    FK_type<Idx> fk_inner{};
                    extract_fk_fields_impl<Idx>(
                            stmt, fk_inner, column_offsets_[Idx], std::make_index_sequence<field_count>{}
                    );
                    obj.[:FKField:] = std::move(fk_inner);
                }
            } else {
                auto& fk_obj = obj.[:FKField:];
                extract_fk_fields_impl<Idx>(
                        stmt, fk_obj, column_offsets_[Idx], std::make_index_sequence<field_count>{}
                );
            }
        }

        template <std::size_t... Is>
        __attribute__((always_inline)) static void
        extract_all_fks(Statement* stmt, T& obj, std::index_sequence<Is...> /*unused*/) noexcept {
            (extract_fk_at<Is>(stmt, obj), ...);
        }

        // FIX: Initialize all FK fields to default values before extraction
        // This ensures non-JOINed FK fields have proper default values instead of garbage
        template <std::size_t MemberIdx> __attribute__((always_inline)) static void init_fk_field_at(T& obj) noexcept {
            if constexpr (MemberIdx < Base::field_count_) {
                constexpr auto member = Base::all_members_[MemberIdx];
                if constexpr (Base::is_fk_field(member)) {
                    using FieldType = std::remove_cvref_t<decltype(obj.[:member:])>;
                    obj.[:member:]  = FieldType{}; // Default-construct FK object
                }
            }
        }

        template <std::size_t... Is>
        __attribute__((always_inline)) static void
        init_all_fk_fields(T& obj, std::index_sequence<Is...> /*unused*/) noexcept {
            (init_fk_field_at<Is>(obj), ...);
        }

      public:
        // Database-agnostic API: Uses Statement template methods for extraction
        // Template methods enable cross-module inlining without LTO
        __attribute__((hot)) __attribute__((flatten)) static auto extract_joined_row(Statement* stmt, T& obj) noexcept
                -> void {
            // Initialize ALL FK fields to defaults first
            init_all_fk_fields(obj, typename Base::field_indices_t{});
            // Extract base fields and FK fields using Statement methods
            extract_t_fields(stmt, obj, typename Base::field_indices_t{});
            extract_all_fks(stmt, obj, std::make_index_sequence<fk_count_>{});
        }
    };

    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info... FKFields>
        requires(sizeof...(FKFields) >= 1 && (FKFieldOf<T, FKFields> && ...))
    [[nodiscard]] auto make_join_wrapper() -> JoinStatementWrapper<detail::pk_key_type_t<T>> {
        using JS        = JoinStatement<T, ConnType, Type, FKFields...>;
        using PkKeyType = detail::pk_key_type_t<T>;

        return JoinStatementWrapper<PkKeyType>{
                +[]() -> const std::string& { return JS::get_complete_sql(); },
                +[](ErasedStatementPtr stmt, ErasedObjectPtr obj) -> void {
                    JS::extract_joined_row(static_cast<typename ConnType::Statement*>(stmt), *static_cast<T*>(obj));
                }
        };
    }

    // =========================================================================
    // MANY-TO-MANY JOIN (#203 model + schema; #391 two-query execution).
    //
    // Eager load runs as TWO queries (SelectStatement::execute_m2m_2query),
    // stitched client-side by a pk→entity hash map:
    //
    //   Q1 (build_base_subquery): the base entities to load —
    //     SELECT <base cols> FROM <Base> [WHERE …] [ORDER BY …] [LIMIT/OFFSET]
    //
    //   Q2 (build_q2_sql): their related rows, filtered by the same subquery —
    //     SELECT t2.<owner>_id, t3.<related cols>
    //     FROM <junction> t2 INNER JOIN <Related> t3 ON t2.<related>_id = t3.<rpk>
    //     WHERE t2.<owner>_id IN (<the SAME base subquery>)
    //
    // WHERE/ORDER BY/LIMIT/OFFSET select WHICH base entities load — they live in
    // Q1 and inside Q2's IN-subquery so both pick the same set. No outer ORDER BY,
    // no pk-adjacency contract: the stitch is a hash map (#391). INNER drops
    // zero-relation entities after the stitch; LEFT keeps them.
    //
    // The modifier-free join SQL (aggregates count (base, related) tuples over
    // it) is assembled per-wrapper in make_m2m_join_wrapper from base_cols_ /
    // per-relation related cols / base_from_ / per-relation join fragments —
    // several relations chain with unique aliases (#392).
    //
    // Junction descriptor: auto mode (Through = void) uses <Base>_<Related> with
    // <Base>_id / <Related>_id columns; through mode uses the through model's
    // table with its FK field names (<field>_id).
    //
    // A composite-PK owner (T) is supported since #504 Task 9: the junction table
    // carries one owner-key column PER PK part ("<Base>_<part>"), so the Q2 owner
    // key is an N-column row value, matched against an N-column subquery. Before
    // that, the junction had exactly one physical "<Base>_id" column with no valid
    // composite counterpart, and the case was rejected at compile time
    // (M2MOwnerPkSupported, removed together with the junction widening).
    // =========================================================================
    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info M2MField>
        requires M2MFieldOf<T, M2MField> && (Type == JoinType::Inner || Type == JoinType::Left)
    class M2MJoinStatement
        : private BaseStatement<T>,
          public TwoQueryJoinBase<M2MJoinStatement<T, ConnType, Type, M2MField>, BaseStatement<T>, ConnType, Type> {
        friend class BaseStatement<T>;
        using Base      = BaseStatement<T>;
        using Statement = typename ConnType::Statement;
        using TwoQuery  = TwoQueryJoinBase<M2MJoinStatement, BaseStatement<T>, ConnType, Type>;
        friend TwoQuery;

      public:
        // The junction carries one owner-key column per PK part of Base — one
        // "<Base>_id" for a single-column PK, N "<Base>_<part>" columns for a
        // composite one (#504 Task 9's junction DDL widening). Three things must
        // agree on this count: the Q2 SELECT head's owner-key column emission
        // (append_junction_side_col_list, join_grammar.cppm), TwoQueryJoinBase::
        // extract_q2_owner_pk's column-reading loop (which reads exactly this many),
        // and insert_related's starting offset for the related entity's own columns.
        // Reading a different count here than the SELECT emits would read PAST the
        // owner key into the RELATED entity's data — a silent cross-entity misread
        // (SQLite's column_int64 on a TEXT column just returns 0), which is why this
        // is a single named constant rather than each site deriving its own.
        // Public because TwoQueryJoinBase reads it through Derived::.
        static constexpr std::size_t owner_key_column_count_ = Base::primary_key_column_count_;

      private:
        // Re-derive the member from data_members_of_v<T> by identifier — annotation reads on a
        // reflection that crossed a BMI boundary segfault clang-p2996 (#262).
        static constexpr auto m2m_member_ = []() consteval {
            for (auto m : storm::meta::data_members_of_v<T>) {
                if (std::meta::identifier_of(m) == std::meta::identifier_of(M2MField)) {
                    return m;
                }
            }
            std::unreachable(); // M2MFieldOf<T, M2MField> guarantees a match
        }();

        using ContainerType = std::remove_cvref_t<typename[:std::meta::type_of(m2m_member_):]>;
        using Related       = meta::m2m_related_t<ContainerType>;
        using Through       = meta::m2m_through_t<m2m_member_>;
        using RelatedBase   = BaseStatement<Related>;

        static_assert(!std::same_as<Related, T>, "self-referential many-to-many is not supported (#203)");

        // if constexpr keeps the concept from being checked with Through = void
        // (nonstatic_data_members_of(^^void) is not a constant expression).
        static consteval auto through_is_valid() -> bool {
            if constexpr (std::same_as<Through, void>) {
                return true;
            } else {
                return ThroughWithFKTo<Through, T> && ThroughWithFKTo<Through, Related>;
            }
        }
        static_assert(through_is_valid(), "through model must have exactly one fk<...> field for each side (#203)");

        // ---- Junction descriptor ---------------------------------------------
        template <typename Side> static consteval auto find_through_fk() -> std::meta::info {
            for (auto m : storm::meta::data_members_of_v<Through>) {
                if (meta::is_fk_field(m) && std::meta::dealias(std::meta::type_of(m)) == std::meta::dealias(^^Side)) {
                    return m;
                }
            }
            std::unreachable(); // ThroughWithFKTo guarantees a match
        }

        // Auto-junction table name "<Base>_<Related>" needs static storage.
        static constexpr auto junction_table_arr_ = []() consteval {
            ConstexprString<Base::table_name_.size() + RelatedBase::table_name_.size() + 2> name;
            name.append(Base::table_name_);
            name.append("_");
            name.append(RelatedBase::table_name_);
            return name;
        }();

        static consteval auto junction_table_name() -> std::string_view {
            if constexpr (std::same_as<Through, void>) {
                return junction_table_arr_.view();
            } else {
                // ^^Through names the local alias — dealias to the model's own name
                return std::meta::identifier_of(std::meta::dealias(^^Through));
            }
        }

        // Junction column base names (the "_id" suffix is appended in the builder).
        static consteval auto owner_col_name() -> std::string_view {
            if constexpr (std::same_as<Through, void>) {
                return Base::table_name_;
            } else {
                return std::meta::identifier_of(find_through_fk<T>());
            }
        }
        static consteval auto related_col_name() -> std::string_view {
            if constexpr (std::same_as<Through, void>) {
                return RelatedBase::table_name_;
            } else {
                return std::meta::identifier_of(find_through_fk<Related>());
            }
        }

        // Which rule spells this junction's COMPOSITE columns (#536). An auto-junction
        // is Storm's own table; a through model is the user's, and its columns are
        // ordinary composite-FK columns. Derived from the same `Through` alias that
        // picks the table and column base names above, so the three cannot disagree
        // about which junction is being addressed.
        static constexpr JunctionNaming junction_naming_ =
                std::same_as<Through, void> ? JunctionNaming::Auto : JunctionNaming::Through;

        // ---- Complete-SQL fragments (#392) ------------------------------------
        // The modifier-free join SQL (aggregates / DISTINCT / set-ops) is
        // assembled per-wrapper in make_m2m_join_wrapper from these fragments so
        // several relations can chain with unique aliases: relation i uses
        // junction alias 2+2i, related alias 3+2i (relation 0 keeps t2/t3, so the
        // single-relation text is unchanged).

        // base_cols_: "SELECT t1.<c1>, t1.<c2>[_id], …" — base columns only.
        static consteval auto calculate_base_cols_size() -> std::size_t {
            std::size_t total = 7; // "SELECT "
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                total += 2 + 3 + std::meta::identifier_of(Base::all_members_[i]).size() + 3;
            }
            return total + utilities::sql_len::SMALL_BUFFER;
        }

        static consteval auto build_base_cols() {
            ConstexprString<calculate_base_cols_size()> result;
            result.append("SELECT ");
            bool first = true;
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                if (!first) {
                    result.append(", ");
                }
                result.append("t1.");
                meta::append_column_name(result, Base::all_members_[i]); // #422 — emits FK "_id"
                first = false;
            }
            return result;
        }

        // base_from_: " FROM (SELECT <cols> FROM <Base>) t1"
        static consteval auto calculate_base_from_size() -> std::size_t {
            return 14 + Base::field_names_array_.len + 6 + Base::table_name_.size() + 4 +
                   utilities::sql_len::SMALL_BUFFER;
        }

        static consteval auto build_base_from() {
            ConstexprString<calculate_base_from_size()> result;
            result.append(" FROM (SELECT ");
            result.append(Base::field_names_array_);
            result.append(" FROM ");
            result.append(Base::table_name_);
            result.append(") t1");
            return result;
        }

        static constexpr auto base_cols_arr_ = build_base_cols();
        static constexpr auto base_from_arr_ = build_base_from();

        // Constexpr name snapshots for the runtime fragment appenders below
        // (the consteval name fns cannot be called at runtime).
        static constexpr std::string_view junction_name_v_ = junction_table_name();
        static constexpr std::string_view owner_col_v_     = owner_col_name();
        static constexpr std::string_view related_col_v_   = related_col_name();
        static constexpr std::string_view join_kw_v_       = TwoQuery::join_keyword();

        // ---- ON-clause column pairs for the aggregate path (#504 Task 9) ----------
        //
        // The complete-SQL join is a chain of two ON clauses — base⟷junction and
        // junction⟷related — each of which must AND-join ONE equality PER PK PART of
        // the side it keys on. Both sides need the same pair of names per part:
        //
        //   model_col    the part's own column in the side's own table
        //                ("region", or "warehouse_id" for an FK part — via
        //                append_column_name, #422; naming the bare member would emit
        //                a column that does not exist)
        //   junction_col the junction column carrying it ("<side>_id" single-PK,
        //                "<side>_<part>" composite — schema.cppm's junction DDL rule)
        //
        // Both names can be CONCATENATIONS, so neither exists as a stable
        // string_view on its own: they must be MATERIALIZED into static storage. One
        // newline-separated buffer per side holds them; the arrays below are
        // non-owning views into that buffer, valid for the program's lifetime since
        // the buffer is a constexpr static.
        //
        // Writer + counting sink, so the buffer budget is measured from the code that
        // fills it (append_side_join_names is run against BOTH) and the two cannot
        // drift — the ConstexprString overflow this guards against truncates SILENTLY.
        template <typename SideBase>
        static consteval auto append_side_join_names(auto& buf, std::string_view side_col) -> void {
            for (std::size_t p = 0; p < SideBase::primary_key_column_count_; ++p) {
                meta::append_column_name(buf, SideBase::primary_key_members_[p]); // #422 — FK part → "<name>_id"
                buf.append("\n");                                                 // separator — split back out below
                append_junction_side_col<SideBase, junction_naming_>(buf, side_col, p);
                buf.append("\n");
            }
        }

        template <typename SideBase>
        static consteval auto side_join_names_size(std::string_view side_col) -> std::size_t {
            RelationColumnsSizer sizer;
            append_side_join_names<SideBase>(sizer, side_col);
            return sizer.len;
        }

        struct JoinCol {
            std::string_view model_col;    // the side's own table column
            std::string_view junction_col; // the junction column carrying it
        };

        // Slice a side's newline-separated name buffer back into per-part pairs.
        template <typename SideBase>
        static consteval auto slice_side_join_cols(std::string_view all)
                -> std::array<JoinCol, SideBase::primary_key_column_count_> {
            std::array<JoinCol, SideBase::primary_key_column_count_> cols{};
            std::size_t                                              pos = 0;
            for (std::size_t i = 0; i < SideBase::primary_key_column_count_; ++i) {
                const std::size_t model_end = all.find('\n', pos);
                cols[i].model_col           = all.substr(pos, model_end - pos);
                pos                         = model_end + 1;
                const std::size_t junc_end  = all.find('\n', pos);
                cols[i].junction_col        = all.substr(pos, junc_end - pos);
                pos                         = junc_end + 1;
            }
            return cols;
        }

        static constexpr auto owner_join_names_ = []() consteval {
            ConstexprString<side_join_names_size<Base>(owner_col_name()) + 1> buf; // +1: NUL terminator
            append_side_join_names<Base>(buf, owner_col_name());
            return buf;
        }();
        static constexpr auto owner_join_cols_ = slice_side_join_cols<Base>(owner_join_names_.view());

        static constexpr auto related_join_names_ = []() consteval {
            ConstexprString<side_join_names_size<RelatedBase>(related_col_name()) + 1> buf;
            append_side_join_names<RelatedBase>(buf, related_col_name());
            return buf;
        }();
        static constexpr auto related_join_cols_ = slice_side_join_cols<RelatedBase>(related_join_names_.view());

        struct RelatedCol {
            std::string_view name;
            bool             is_fk;
        };
        static constexpr auto related_cols_ = []() consteval {
            std::array<RelatedCol, RelatedBase::field_count_> cols{};
            for (std::size_t i = 0; i < RelatedBase::field_count_; ++i) {
                cols[i] =
                        {std::meta::identifier_of(RelatedBase::all_members_[i]),
                         Base::is_fk_field(RelatedBase::all_members_[i])};
            }
            return cols;
        }();

      public:
        static constexpr auto base_cols_view() -> std::string_view {
            return base_cols_arr_.view();
        }
        static constexpr auto base_from_view() -> std::string_view {
            return base_from_arr_.view();
        }

        // Append ", t<A>.<col>[_id]" for every related member. Runs once per
        // wrapper type (the complete SQL is a function-local static in the
        // factory's get_complete_sql_fn). Plain += appends — std::format inside
        // the module purview mis-deduces the wchar_t overload when instantiated
        // from an import-std TU (clang-p2996).
        static auto append_complete_cols(std::string& sql, std::size_t related_alias) -> void {
            const std::string alias = std::to_string(related_alias);
            for (const auto& col : related_cols_) {
                sql += ", t";
                sql += alias;
                sql += ".";
                sql += col.name;
                if (col.is_fk) {
                    sql += "_id";
                }
            }
        }

        // Append one side's ON-clause body: one equality PER PK PART of that side,
        // AND-joined (#504 Task 9). Matching on only the first part would join rows
        // whose remaining parts differ — silently wrong, not an error. A single-column
        // PK emits exactly one equality, byte-identical to the pre-#504 form.
        //
        // `model_first` fixes the operand ORDER, which differs between the two clauses
        // in the chain and is part of the byte-identical guarantee: the base⟷junction
        // clause writes "t1.<model> = t<J>.<junction>", the junction⟷related clause
        // writes "t<J>.<junction> = t<R>.<model>".
        template <std::size_t N>
        static auto append_join_on_body(
                std::string&                  sql,
                const std::array<JoinCol, N>& cols,
                std::string_view              model_alias,
                std::string_view              junction_alias,
                bool                          model_first
        ) -> void {
            bool first_part = true;
            for (const auto& part : cols) {
                if (!first_part) {
                    sql += " AND ";
                }
                const std::string_view lhs_alias = model_first ? model_alias : junction_alias;
                const std::string_view lhs_col   = model_first ? part.model_col : part.junction_col;
                const std::string_view rhs_alias = model_first ? junction_alias : model_alias;
                const std::string_view rhs_col   = model_first ? part.junction_col : part.model_col;
                sql += lhs_alias;
                sql += ".";
                sql += lhs_col;
                sql += " = ";
                sql += rhs_alias;
                sql += ".";
                sql += rhs_col;
                first_part = false;
            }
        }

        // Append "<KW> <junction> t<J> ON t1.<pk parts> = t<J>.<owner cols>
        //         <KW> <Related> t<R> ON t<J>.<related cols> = t<R>.<rpk parts>"
        // with J = junction_alias, R = junction_alias + 1. BOTH ON clauses AND-join
        // every PK part of the side they key on: the owner side from owner_join_cols_,
        // the related side from related_join_cols_ (#504 Task 9). Each list pairs a
        // model column with the junction column carrying it, so a list can never fall
        // out of step with the junction DDL that produced those columns.
        static auto append_complete_join(std::string& sql, std::size_t junction_alias) -> void {
            const std::string junction_ali = "t" + std::to_string(junction_alias);
            const std::string related_ali  = "t" + std::to_string(junction_alias + 1);
            sql += join_kw_v_;
            sql += junction_name_v_;
            sql += " ";
            sql += junction_ali;
            sql += " ON ";
            append_join_on_body(sql, owner_join_cols_, "t1", junction_ali, /*model_first=*/true);
            sql += join_kw_v_;
            sql += RelatedBase::table_name_;
            sql += " ";
            sql += related_ali;
            sql += " ON ";
            append_join_on_body(sql, related_join_cols_, related_ali, junction_ali, /*model_first=*/false);
        }

        // =====================================================================
        // TWO-QUERY PREDICATE-PUSHDOWN PATH (#391)
        //
        // Q1: SELECT <base cols> FROM <Base> [WHERE …][ORDER BY …][LIMIT/OFFSET]
        //     — the entities to load (a plain base SELECT; no join, no sorter).
        // Q2: SELECT t2.<owner key cols>, t3.<related cols>
        //     FROM <junction> t2 INNER JOIN <Related> t3
        //          ON t2.<related key cols> = t3.<related pk parts>   (AND-joined)
        //     WHERE t2.<owner key cols> IN (<the SAME base subquery as Q1>)
        //     — related rows for exactly the loaded entities, no base columns
        //       duplicated, no ORDER BY.
        //
        // SelectStatement runs both inside a transaction, builds a pk→entity hash
        // map from Q1, and stitches each Q2 row by owner pk. INNER drops
        // zero-relation entities after the stitch; LEFT keeps them (Q1 already
        // yielded them, Q2 just leaves their container empty). Q2 is always an
        // INNER junction⋈related join — the INNER/LEFT distinction is purely a
        // post-stitch filter, never an SQL difference.
        // =====================================================================

        // Q1 (build_base_subquery), Q2 (build_q2_sql), and the owner-pk extractor are
        // inherited from TwoQueryJoinBase; this class only supplies the Q2 prefix below.

        // The consteval junction⟷related ON body: one equality PER RELATED PK PART,
        // AND-joined. The consteval twin of append_join_on_body above (which serves
        // the runtime-assembled aggregate path and cannot be reused here — its
        // JoinCol views are runtime views into a static buffer). Single-PK collapses
        // to exactly "t2.<related>_id = t3.<rpk>", the pre-#504 text.
        //
        // Widening this side is what #504 Task 9's junction DDL made MANDATORY: the
        // junction of a composite-PK related model has no "<Related>_id" column at
        // all, so the pre-widening single-column form named a column that does not
        // exist ("no such column: t2.CatalogEntry_id"). Even with the column present,
        // matching only RelatedBase::pk_name_ (the FIRST part) would join related rows
        // that share their first part but differ in a later one.
        static consteval auto append_q2_related_on(auto& result) -> void {
            for (std::size_t p = 0; p < RelatedBase::primary_key_column_count_; ++p) {
                if (p > 0) {
                    result.append(" AND ");
                }
                result.append("t2.");
                append_junction_side_col<RelatedBase, junction_naming_>(result, related_col_name(), p);
                result.append(" = t3.");
                meta::append_column_name(result, RelatedBase::primary_key_members_[p]); // #422
            }
        }

        // q2_prefix_: "SELECT t2.<owner cols>, t3.<r…> FROM <junction> t2
        //              INNER JOIN <Related> t3 ON t2.<related cols> = t3.<rpk parts>
        //              WHERE t2.<owner cols> IN (SELECT <base.pk parts> FROM <Base>"
        // Writes the Q2 prefix into any sink with .append(string_view) — the size
        // companion below runs THIS function into the counting sink, so the buffer
        // budget is measured from the code that fills it and the two cannot drift.
        //
        // BOTH junction sides are N columns wide since #504 Task 9 widened the
        // junction DDL to one column per PK part on each side: the owner key is
        // owner_key_column_count_ columns ("<owner>_id" single-PK, N "<owner>_<part>"
        // composite), the related key likewise. For an owner N > 1 the WHERE
        // comparison side is wrapped in parens as a row value — "(a, b) IN (SELECT a,
        // b FROM …)" — while the subquery's own SELECT list stays UNparenthesized: a
        // parenthesized comma list inside a SELECT list parses as ONE scalar
        // expression, not N columns, which SQLite rejects with "sub-select returns 1
        // columns - expected 2". The LHS arity and the subquery arity are both
        // driven by Base::has_composite_pk_ here so they always agree. The related
        // side never reaches the IN-subquery — it is an ordinary equi-join — so its
        // arity is independent of the owner's.
        static consteval auto append_q2_prefix(auto& result) -> void {
            result.append("SELECT ");
            append_junction_side_col_list<Base, junction_naming_>(result, owner_col_name(), "t2.");
            append_relation_columns<RelatedBase>(result, "t3.");
            result.append(" FROM ");
            result.append(junction_table_name());
            result.append(" t2 INNER JOIN ");
            result.append(RelatedBase::table_name_);
            result.append(" t3 ON ");
            append_q2_related_on(result);
            result.append(" WHERE ");
            if constexpr (Base::has_composite_pk_) {
                result.append("(");
                append_junction_side_col_list<Base, junction_naming_>(result, owner_col_name(), "t2.");
                result.append(")");
            } else {
                append_junction_side_col_list<Base, junction_naming_>(result, owner_col_name(), "t2.");
            }
            append_in_subquery_open<Base, Base::has_composite_pk_>(result);
        }

        static consteval auto calculate_q2_prefix_size() -> std::size_t {
            RelationColumnsSizer sizer;
            append_q2_prefix(sizer);
            return sizer.len + utilities::sql_len::SMALL_BUFFER;
        }

        static consteval auto build_q2_prefix() {
            ConstexprString<calculate_q2_prefix_size()> result;
            append_q2_prefix(result);
            return result;
        }

        // Q2 prefix (everything up to the open IN-subquery) — TwoQueryJoinBase reads
        // it directly to assemble build_q2_sql (base clauses + ")" appended there).
        static constexpr auto q2_prefix_arr_ = build_q2_prefix();

        // Append the Q2 row's related object into obj's container. Q2 related
        // columns start after the owner key, which is owner_key_column_count_
        // columns wide (1 for a single-PK owner — the pre-#504 value — N for a
        // composite one, #504 Task 9). Always present — Q2 is an INNER join, so
        // there is never a NULL related row to skip.
        static auto append_related_q2(Statement* stmt, T& obj) noexcept -> void {
            insert_related<static_cast<int>(owner_key_column_count_)>(stmt, obj);
        }

        // True when this entity has at least one related row — drives the
        // INNER-join post-stitch drop of zero-relation entities.
        static auto container_empty(const T& obj) noexcept -> bool {
            return obj.[:m2m_member_:].empty();
        }

      private:
        // Extract one Related from the row (columns start at RelColOffset) and append
        // it to obj's container, wrapping in shared_ptr if the container holds one.
        // Extraction itself is the shared extract_relation_entity free helper.
        template <int RelColOffset> static auto insert_related(Statement* stmt, T& obj) noexcept -> void {
            Related rel{};
            extract_relation_entity<RelatedBase, Related, RelColOffset>(
                    stmt, rel, std::make_index_sequence<RelatedBase::field_count_>{}
            );
            using Elem = typename ContainerType::value_type;
            if constexpr (meta::is_shared_ptr_v<Elem>) {
                relation_insert_into(obj.[:m2m_member_:], std::make_shared<Related>(std::move(rel)));
            } else {
                relation_insert_into(obj.[:m2m_member_:], std::move(rel));
            }
        }
    };

    // =========================================================================
    // REVERSE-FK JOIN (#398) — "all Persons, each with the Tasks that point at them".
    //
    // The SQL identity Task RIGHT JOIN Person ≡ Person LEFT JOIN Task is expressed
    // directly with the base model (Person) on the correct side. The selector is an
    // FK field of the OWNING model (^^Task::assignee); the join key is t2.<fk>_id.
    //
    // Eager load reuses the m2m two-query predicate-pushdown path (#391) verbatim —
    // SelectStatement::execute_m2m_2query stitches by a pk→entity map — but Q2 hits
    // the owning table directly (no junction):
    //
    //   Q1 (build_base_subquery): the base entities to load —
    //     SELECT <base cols> FROM <Base> [WHERE …][ORDER BY …][LIMIT/OFFSET]
    //
    //   Q2 (build_q2_sql): the owning rows that point at them —
    //     SELECT t2.<fk>_id, t2.<owner cols>
    //     FROM <Owner> t2
    //     WHERE t2.<fk>_id IN (<the SAME base subquery as Q1>)
    //
    // WHERE/ORDER BY/LIMIT/OFFSET select WHICH base entities load (they live in Q1
    // and Q2's IN-subquery). INNER drops zero-relation entities after the stitch;
    // LEFT keeps them. The modifier-free complete SQL (for aggregates / anti-join /
    // zero-group COUNT) is Person t1 <KW> Owner t2 ON t2.<fk>_id = t1.<pk>.
    //
    // FkField names an FK member of Owner whose (optional-unwrapped) type is T.
    // =========================================================================
    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info FkField>
        requires ReverseFKSelector<T, FkField> && (Type == JoinType::Inner || Type == JoinType::Left)
    class ReverseFKJoinStatement : private BaseStatement<T>,
                                   public TwoQueryJoinBase<
                                           ReverseFKJoinStatement<T, ConnType, Type, FkField>,
                                           BaseStatement<T>,
                                           ConnType,
                                           Type> {
        friend class BaseStatement<T>;
        using Base      = BaseStatement<T>;
        using Statement = typename ConnType::Statement;
        using TwoQuery  = TwoQueryJoinBase<ReverseFKJoinStatement, BaseStatement<T>, ConnType, Type>;
        friend TwoQuery;

      public:
        // The reverse-FK owning table carries a REAL N-column FK back to Base (#504
        // Task 6/7's widened FK naming), so N here is Base::primary_key_column_count_.
        // TwoQueryJoinBase::extract_q2_owner_pk reads this many columns from the Q2
        // row's owner key, in lockstep with append_q2_select_head_multi's N-column
        // emission below, and append_related_q2 starts the owning entity's own
        // columns right after them. Public so tests
        // can assert this compile-time invariant directly (a regression guard for
        // the cross-entity misread this exact offset arithmetic was found to have
        // during this task's own self-review).
        static constexpr std::size_t owner_key_column_count_ = Base::primary_key_column_count_;

      private:
        // Owning model carrying the FK — Owner = parent_of(FkField) (e.g. Task).
        using Owner     = typename[:std::meta::parent_of(FkField):];
        using OwnerBase = BaseStatement<Owner>;

        static_assert(!std::same_as<Owner, T>, "reverse_fk must point from a different model (#398)");

        // Target-pk-member list for the composite-FK naming helpers (#504 Task 8) —
        // Base (T) IS FkField's target, so Base::primary_key_members_ is exactly
        // what fk_column_name_size_for_part/append_fk_column_name_for_part need to
        // name the owning table's FK column(s): "<fk>_id" for a single-column Base
        // PK (byte-identical to the pre-#504 form), "<fk>_<part1>, <fk>_<part2>, …"
        // (per-part, individually aliased) for a composite one.
        static constexpr auto fk_target_pk_ = Base::primary_key_members_;

        // The base member receiving the related rows on select() — the container
        // annotated reverse_fk<FkField>. Re-derived from data_members_of_v<T> by matching the carried
        // FK target (annotation reads on a BMI-crossing reflection segfault, #262).
        // Only the select path uses it; aggregate/filter chains never reach append_*.
        static consteval auto find_dest_member() -> std::optional<std::meta::info> {
            for (auto m : storm::meta::data_members_of_v<T>) {
                // Resolve each reverse_fk member's target (owner type or FK field) to its
                // concrete FK field and match against the one this statement keys on.
                if (meta::is_reverse_fk_field(m) &&
                    meta::resolve_reverse_fk_target(meta::reverse_fk_target_of(m), ^^T) == FkField) {
                    return m;
                }
            }
            return std::nullopt;
        }

      public:
        // True when a reverse_fk container destination for THIS FkField exists on T —
        // gates select() (needs a destination) vs aggregate/filter chains (do not).
        static constexpr bool has_destination_ = find_dest_member().has_value();

        // Byte size of the AND-joined ON-clause body "t2.<part1> = t1.<pk_part1> AND
        // t2.<part2> = t1.<pk_part2> …" — N=1 collapses to exactly "t2.<fk>_id =
        // t1.<pk>" (byte-identical to the pre-#504 single-column clause). Mirrors
        // JoinStatement::fk_on_clause_body_size (#504 Task 7), sides swapped: here
        // the OWNER table (t2) carries the FK column(s), the BASE table (t1) carries
        // its own bare PK part identifiers (never suffixed — Base's own PK is never
        // itself an FK... unless a PK part IS an FK member, in which case
        // append_column_name already emits its "_id" suffix on the base side too).
        static consteval auto fk_on_clause_body_size() -> std::size_t {
            std::size_t total = 0;
            for (std::size_t p = 0; p < fk_target_pk_.size(); ++p) {
                if (p > 0) {
                    total += 5; // " AND "
                }
                total += 3; // "t2."
                total += storm::meta::fk_column_name_size_for_part(FkField, fk_target_pk_, p);
                total += 6; // " = t1."
                total += storm::meta::column_name_size(fk_target_pk_[p]);
            }
            return total;
        }

        template <typename Result> static consteval void append_fk_on_clause_body(Result& result) {
            for (std::size_t p = 0; p < fk_target_pk_.size(); ++p) {
                if (p > 0) {
                    result.append(" AND ");
                }
                result.append("t2.");
                storm::meta::append_fk_column_name_for_part(result, FkField, fk_target_pk_, p);
                result.append(" = t1.");
                storm::meta::append_column_name(result, fk_target_pk_[p]);
            }
        }

        // ---- Modifier-free complete SQL (aggregates / anti-join / zero-group COUNT) ----
        // SELECT t1.<base cols> FROM <Base> t1 <KW> <Owner> t2 ON t2.<fk>_id = t1.<pk>
        // (AND-chained per part for a composite Base PK, #504 Task 8).
        static consteval auto calculate_complete_sql_size() -> std::size_t {
            std::size_t total = 7; // "SELECT "
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                total += 2 + 3 + std::meta::identifier_of(Base::all_members_[i]).size() + 3;
            }
            total += 6 + Base::table_name_.size() + 3;                                    // " FROM <Base> t1"
            total += TwoQuery::join_keyword().size() + OwnerBase::table_name_.size() + 3; // "<KW><Owner> t2"
            total += 4 + fk_on_clause_body_size();                                        // " ON " + body
            return total + utilities::sql_len::SMALL_BUFFER;
        }

        static consteval auto build_complete_sql() {
            ConstexprString<calculate_complete_sql_size()> sql;
            sql.append("SELECT ");
            bool first = true;
            for (std::size_t i = 0; i < Base::field_count_; ++i) {
                if (!first) {
                    sql.append(", ");
                }
                sql.append("t1.");
                meta::append_column_name(sql, Base::all_members_[i]); // #422 — emits FK "_id"
                first = false;
            }
            sql.append(" FROM ");
            sql.append(Base::table_name_);
            sql.append(" t1");
            sql.append(TwoQuery::join_keyword());
            sql.append(OwnerBase::table_name_);
            sql.append(" t2 ON ");
            append_fk_on_clause_body(sql);
            return sql;
        }

        static constexpr auto complete_sql_arr_ = build_complete_sql();

        static auto get_complete_sql() -> const std::string& {
            static const std::string str{complete_sql_arr_.data.data(), complete_sql_arr_.len};
            return str;
        }

        // Q1 (build_base_subquery), Q2 (build_q2_sql), and the owner-pk extractor are
        // inherited from TwoQueryJoinBase; this class only supplies the Q2 prefix below.

        // ---- Q2 prefix: "SELECT t2.<fk>_id, t2.<owner cols> FROM <Owner> t2
        //                  WHERE t2.<fk>_id IN (SELECT <base.pk> FROM <Base>" ----
        // A composite Base PK widens BOTH sides of the WHERE to the row-value form
        // (#504 Task 8, matching #501's bulk-DELETE row-value-IN precedent): the
        // outer comparison becomes "(t2.<part1>, t2.<part2>) IN (SELECT <a>, <b>
        // FROM <Base> …)" — the inner SELECT list is a PLAIN, unparenthesized
        // comma list (SQLite/PG match subquery row arity by SELECT-list column
        // COUNT, not by literal tuple syntax; parenthesizing it would make it one
        // scalar expression instead of N columns — a real bug this task's own
        // testing caught). The per-column form "t2.a IN (…) AND t2.b IN (…)"
        // would match the cross product of parts, not the intended row pairs
        // (#501's exact bug class). N=1 collapses to the pre-#504 bare
        // "t2.<fk>_id IN (SELECT <pk> FROM <Base> …)".
        static consteval auto calculate_q2_prefix_size() -> std::size_t {
            std::size_t total = q2_select_head_multi_owner_size(FkField, fk_target_pk_);
            total += relation_columns_size<OwnerBase>("t2.");
            total += 6 + OwnerBase::table_name_.size() + 3; // " FROM <Owner> t2"
            total += 7;                                     // " WHERE "
            if constexpr (Base::has_composite_pk_) {
                total += 2 + owner_fk_col_list_size(FkField, fk_target_pk_); // "(" + list + ")"
            } else {
                total += owner_fk_col_list_size(FkField, fk_target_pk_); // "t2.<fk>_id"
            }
            // true: this site's LHS is already widened to Base's real PK arity
            // above (the row-value form when Base::has_composite_pk_), so the
            // RHS subquery must match it column-for-column — pass the SAME flag
            // through explicitly rather than letting the helper re-derive it.
            total += in_subquery_open_size<Base, Base::has_composite_pk_>();
            return total + utilities::sql_len::SMALL_BUFFER;
        }

        static consteval auto build_q2_prefix() {
            ConstexprString<calculate_q2_prefix_size()> result;
            append_q2_select_head_multi<OwnerBase>(result, FkField, fk_target_pk_, "t2.");
            result.append(" FROM ");
            result.append(OwnerBase::table_name_);
            result.append(" t2 WHERE ");
            if constexpr (Base::has_composite_pk_) {
                result.append("(");
                append_owner_fk_col_list(result, FkField, fk_target_pk_);
                result.append(")");
            } else {
                append_owner_fk_col_list(result, FkField, fk_target_pk_);
            }
            append_in_subquery_open<Base, Base::has_composite_pk_>(result); // LHS/RHS in lockstep — see size fn
            return result;
        }

        static constexpr auto q2_prefix_arr_ = build_q2_prefix(); // TwoQueryJoinBase reads this for build_q2_sql

        // Append the Q2 row's Owner object into the base entity's container. Owner
        // columns start at index fk_target_pk_.size() (after the FK owner key —
        // N=1 for a single-column Base PK, matching the pre-#504 offset of 1; N
        // columns for a composite one, #504 Task 8). Only ever called when
        // has_destination_ — guarded in the wrapper factory.
        static auto append_related_q2(Statement* stmt, T& obj) noexcept -> void {
            insert_owner<static_cast<int>(fk_target_pk_.size())>(stmt, obj);
        }

        static auto container_empty(const T& obj) noexcept -> bool {
            return obj.[:dest_member_:].empty();
        }

      private:
        // The destination container member (select path). When absent (a pure
        // cross-model selector used only for aggregates/filters), fall back to the base
        // pk so type_of/the splice below stay well-formed; the select-only methods that
        // use it are never called for such instantiations (gated by has_destination_).
        static constexpr auto dest_member_ = find_dest_member().value_or(Base::primary_key_);

        using ContainerType = std::remove_cvref_t<typename[:std::meta::type_of(dest_member_):]>;

        // Extract the Owner entity from Q2 columns (starting at OwnerColOffset, after
        // the FK owner key) via the shared extract_relation_entity helper and append
        // to the base entity's reverse_fk container — wrapping in shared_ptr if it
        // holds one.
        template <int OwnerColOffset> static auto insert_owner(Statement* stmt, T& obj) noexcept -> void {
            Owner rel{};
            extract_relation_entity<OwnerBase, Owner, OwnerColOffset>(
                    stmt, rel, std::make_index_sequence<OwnerBase::field_count_>{}
            );
            using Elem = typename ContainerType::value_type;
            if constexpr (meta::is_shared_ptr_v<Elem>) {
                relation_insert_into(obj.[:dest_member_:], std::make_shared<Owner>(std::move(rel)));
            } else {
                relation_insert_into(obj.[:dest_member_:], std::move(rel));
            }
        }
    };

    // Whether JS's owner key is a single column, and so takes the bare-word stitch
    // fast path (#504 perf) rather than a StitchKey. Named once here because both
    // descriptor fields below branch on it and SelectStatement re-derives the same
    // condition; they must never disagree about which extractor is populated.
    template <typename JS> consteval auto narrow_owner_key() -> bool {
        return JS::owner_key_column_count_ == 1;
    }

    // The word extractor for a single-column owner key, or nullptr for a composite
    // one. The if constexpr is load-bearing: JS::extract_q2_owner_pk_word is
    // constrained on a one-column key, so naming it for a composite JS would be a
    // hard error rather than a quietly-unused branch.
    template <storm::db::DatabaseConnection ConnType, typename JS>
    [[nodiscard]] consteval auto make_owner_key_word_fn() -> auto (*)(ErasedStatementPtr) -> std::uint64_t {
        if constexpr (narrow_owner_key<JS>()) {
            return +[](ErasedStatementPtr stmt) -> std::uint64_t {
                return JS::extract_q2_owner_pk_word(static_cast<typename ConnType::Statement*>(stmt));
            };
        } else {
            return nullptr;
        }
    }

    // The StitchKey extractor for a composite owner key, or nullptr when the word
    // fast path above is taken. Leaving the unused one null rather than populating
    // both keeps "exactly one is set" an invariant the stitch loop can assert on.
    template <storm::db::DatabaseConnection ConnType, typename JS>
    [[nodiscard]] consteval auto make_owner_key_fn() -> auto (*)(ErasedStatementPtr)
            -> storm::orm::utilities::StitchKey {
        if constexpr (narrow_owner_key<JS>()) {
            return nullptr;
        } else {
            return +[](ErasedStatementPtr stmt) -> storm::orm::utilities::StitchKey {
                return JS::extract_q2_owner_pk(static_cast<typename ConnType::Statement*>(stmt));
            };
        }
    }

    // Build the M2MRelation descriptor (Q2 builder + stitch fns) from any two-query
    // join statement JS exposing build_q2_sql / extract_q2_owner_pk / append_related_q2
    // / container_empty. Shared by the m2m (#392) and reverse-FK (#398) factories.
    template <typename T, storm::db::DatabaseConnection ConnType, typename JS>
    [[nodiscard]] auto make_relation_descriptor(bool is_left) -> M2MRelation {
        return M2MRelation{
                .build_q2_sql_fn = +[](const orm::where::ExpressionVariantPtr& where_expr,
                                       const std::optional<OrderByWrapper>&    order_by,
                                       const std::optional<int>&               limit,
                                       const std::optional<int>&               offset) -> std::string {
                    return JS::build_q2_sql(where_expr, order_by, limit, offset);
                },
                // Exactly one of the two owner-key extractors is populated, by PK width
                // (#504 perf). SelectStatement<T> re-derives the same condition from
                // Base::primary_key_column_count_, so it always reads the one that is set.
                .extract_q2_owner_pk_fn      = make_owner_key_fn<ConnType, JS>(),
                .extract_q2_owner_pk_word_fn = make_owner_key_word_fn<ConnType, JS>(),
                .append_related_q2_fn        = +[](ErasedStatementPtr stmt, ErasedObjectPtr obj) -> void {
                    JS::append_related_q2(static_cast<typename ConnType::Statement*>(stmt), *static_cast<T*>(obj));
                },
                .container_empty_fn =
                        +[](ErasedObjectPtr obj) -> bool { return JS::container_empty(*static_cast<T*>(obj)); },
                .is_left = is_left
        };
    }

    // One M2MRelation descriptor for a reverse-FK relation (#398). Reuses the m2m
    // two-query stitch machinery: the Q2 builder hits the owning table directly and
    // the stitch fns append a full Owner entity. Requires a reverse_fk destination
    // container on T (select path); aggregate/filter chains never build a wrapper.
    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info FkField>
        requires ReverseFKSelector<T, FkField>
    [[nodiscard]] auto make_reverse_fk_relation() -> M2MRelation {
        using JS = ReverseFKJoinStatement<T, ConnType, Type, FkField>;
        static_assert(JS::has_destination_, "reverse-FK select() needs a reverse_fk<...> container on the base model");
        return make_relation_descriptor<T, ConnType, JS>(Type == JoinType::Left);
    }

    // Rejects join<^^T::courses, ^^T::courses>() — a duplicated m2m field would
    // run its Q2 twice and silently double-fill the same container (#392).
    template <std::meta::info... Fields> consteval auto m2m_fields_distinct() -> bool {
        const std::array<std::string_view, sizeof...(Fields)> names{std::meta::identifier_of(Fields)...};
        for (std::size_t i = 0; i < names.size(); ++i) {
            for (std::size_t j = i + 1; j < names.size(); ++j) {
                if (names[i] == names[j]) {
                    return false;
                }
            }
        }
        return true;
    }

    // Reverse-FK selector resolution (#398): the selector the join machinery keys on
    // is always the cross-model FK field (^^Task::assignee). When the user passes an
    // annotated reverse_fk container (^^Person::tasks), resolve it to that FK; when
    // they pass the FK directly (aggregate/filter chains), use it as-is.
    template <typename T, std::meta::info Field> consteval auto resolve_reverse_fk_selector() -> std::meta::info {
        if constexpr (ReverseFKFieldOf<T, Field>) {
            // Re-derive the member from ^^T, read its reverse_fk target (owner type or
            // FK field), and resolve it to the concrete FK field pointing back at T.
            for (auto m : storm::meta::data_members_of_v<T>) {
                if (std::meta::identifier_of(m) == std::meta::identifier_of(Field)) {
                    return meta::resolve_reverse_fk_target(meta::reverse_fk_target_of(m), ^^T);
                }
            }
            std::unreachable();
        } else {
            return Field; // already a cross-model FK selector
        }
    }

    // A field usable as a reverse-FK join selector: either an annotated reverse_fk
    // container on T (#398, has a select() destination) or a cross-model FK pointing
    // back at T (aggregate/filter chains).
    template <typename T, std::meta::info Field>
    concept ReverseFKJoinable = ReverseFKFieldOf<T, Field> || ReverseFKSelector<T, Field>;

    // Field pack accepted by QuerySet::join / left_join: all-FK, all-m2m, or
    // all-reverse-FK — each kind homogeneous, no duplicates. Mixed kinds in one call
    // are rejected (out of scope, #392/#398).
    template <typename T, std::meta::info... Fields>
    concept JoinableFields =
            sizeof...(Fields) >= 1 &&
            ((FKFieldOf<T, Fields> && ...) || ((M2MFieldOf<T, Fields> && ...) && m2m_fields_distinct<Fields...>()) ||
             ((ReverseFKJoinable<T, Fields> && ...) && m2m_fields_distinct<Fields...>()));

    // One M2MRelation descriptor (#392) — Q2 builder + stitch fns for one field.
    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info M2MField>
        requires M2MFieldOf<T, M2MField>
    [[nodiscard]] auto make_m2m_relation() -> M2MRelation {
        using JS = M2MJoinStatement<T, ConnType, Type, M2MField>;
        return make_relation_descriptor<T, ConnType, JS>(Type == JoinType::Left);
    }

    // Modifier-free chained join SQL over every relation — relation i uses
    // junction alias 2+2i / related alias 3+2i, so the single-relation text is
    // byte-identical to the pre-#392 monolithic builder. Consumed by
    // aggregates / DISTINCT / set-ops; a COUNT over it counts cartesian TUPLES
    // (the N-relation extension of the documented "(base, related) pairs").
    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info... M2MFields>
    [[nodiscard]] auto build_m2m_complete_sql() -> std::string {
        std::string sql{M2MJoinStatement<T, ConnType, Type, M2MFields...[0]>::base_cols_view()};
        [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
            (M2MJoinStatement<T, ConnType, Type, M2MFields...[Is]>::append_complete_cols(sql, 3 + (2 * Is)), ...);
        }(std::make_index_sequence<sizeof...(M2MFields)>{});
        sql += M2MJoinStatement<T, ConnType, Type, M2MFields...[0]>::base_from_view();
        [&]<std::size_t... Is>(std::index_sequence<Is...> /*unused*/) {
            (M2MJoinStatement<T, ConnType, Type, M2MFields...[Is]>::append_complete_join(sql, 2 + (2 * Is)), ...);
        }(std::make_index_sequence<sizeof...(M2MFields)>{});
        return sql;
    }

    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info... M2MFields>
        requires(sizeof...(M2MFields) >= 1 && (M2MFieldOf<T, M2MFields> && ...))
    [[nodiscard]] auto make_m2m_join_wrapper() -> JoinStatementWrapper<detail::pk_key_type_t<T>> {
        using First = M2MJoinStatement<T, ConnType, Type, M2MFields...[0]>;
        JoinStatementWrapper<detail::pk_key_type_t<T>> wrapper{
                .get_complete_sql_fn = +[]() -> const std::string& {
                    static const std::string str = build_m2m_complete_sql<T, ConnType, Type, M2MFields...>();
                    return str;
                },
                // Q1 text depends only on the base model + clauses — any relation
                // yields the same string; take it from the first.
                .build_q1_sql_fn = make_q1_sql_fn<First>()
        };
        wrapper.m2m_relations.reserve(sizeof...(M2MFields));
        (wrapper.m2m_relations.push_back(make_m2m_relation<T, ConnType, Type, M2MFields>()), ...);
        return wrapper;
    }

    // Reverse-FK join wrapper (#398). Selectors are resolved to cross-model FK fields
    // (annotated containers → their reverse_fk target). The wrapper carries:
    //   - get_complete_sql_fn: Person t1 <KW> Owner t2 ON … (single relation) — used by
    //     aggregates / anti-join / zero-group COUNT. Multi-relation aggregates are out
    //     of scope; the wrapper is built per-relation by the select path below.
    //   - build_q1_sql_fn + m2m_relations: the two-query eager-load (select), one Q2 per
    //     relation, reusing the m2m stitch. Relations are added only when a reverse_fk
    //     destination exists; a pure cross-model selector (aggregate chain) has none.
    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info... Selectors>
        requires(sizeof...(Selectors) >= 1 && (ReverseFKJoinable<T, Selectors> && ...))
    [[nodiscard]] auto make_reverse_fk_join_wrapper() -> JoinStatementWrapper<detail::pk_key_type_t<T>> {
        // The FK fields the join keys on (annotated containers resolved to their target).
        using First = ReverseFKJoinStatement<T, ConnType, Type, resolve_reverse_fk_selector<T, Selectors...[0]>()>;
        JoinStatementWrapper<detail::pk_key_type_t<T>> wrapper{
                .get_complete_sql_fn = +[]() -> const std::string& { return First::get_complete_sql(); },
                .build_q1_sql_fn     = make_q1_sql_fn<First>()
        };
        // One relation per selector that has a destination container (the select path).
        // Cross-model selectors used only for aggregates/filters have none — the
        // wrapper then stays relation-free and routes through get_complete_sql().
        (add_reverse_fk_relation_if_destination<T, ConnType, Type, resolve_reverse_fk_selector<T, Selectors>()>(
                 wrapper
         ),
         ...);
        return wrapper;
    }

    // Push a reverse-FK M2MRelation onto the wrapper iff a destination container exists
    // for FkField on T (select path). Compile-time gate via has_destination_.
    template <typename T, storm::db::DatabaseConnection ConnType, JoinType Type, std::meta::info FkField>
    auto add_reverse_fk_relation_if_destination(JoinStatementWrapper<detail::pk_key_type_t<T>>& wrapper) -> void {
        if constexpr (ReverseFKJoinStatement<T, ConnType, Type, FkField>::has_destination_) {
            wrapper.m2m_relations.push_back(make_reverse_fk_relation<T, ConnType, Type, FkField>());
        }
    }

} // namespace storm::orm::statements
