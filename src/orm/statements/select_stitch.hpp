// Textual fragment of module storm_orm_statements_select (#671) — NOT a header.
// #included once, at namespace scope in select.cppm's module purview right after its
// imports; it compiles into that module's BMI. A file split rather than a module
// split: a new module costs ~0.09 s in every TU that imports storm.
//
// m2m stitch-key helpers for SelectStatement: the static pieces of the two-query
// eager load (#391) — the stitch map's key type and its single-column narrowing
// (#504), the Q1-side owner key built from an entity's own primary key, the Q2-side
// owner-key read, the INNER-join drop and the first()/get() reduction. Every member
// derives purely from reflection over the model type T (via Base = BaseStatement<T>)
// and never touches statement instance state.

#ifndef STORM_SELECT_FRAGMENTS
#error "select_stitch.hpp is a textual fragment of storm_orm_statements_select; #include it only from select.cppm"
#endif

export namespace storm::orm::statements {

    template <typename T, storm::db::DatabaseConnection ConnType> struct SelectStitch {
        using Base      = BaseStatement<T>;
        using Error     = typename ConnType::Error;
        using Statement = typename ConnType::Statement;
        using PkKeyType = detail::pk_key_type_t<T>;

        // The stitch map's key type (#504 perf). Both the Q1 insert and the Q2 probe
        // build a StitchKey, but a SINGLE-column PK — the overwhelmingly common shape
        // — fills exactly one 8-byte word of it, so the map is keyed on that word
        // instead of the 33-byte class. That is not a micro-optimization: it restores
        // libc++'s identity std::hash<integral> (which compiles to nothing) in place
        // of hash<StitchKey>'s FNV loop, and an inlined one-instruction operator== in
        // place of an out-of-line word-compare call. Both run once per Q2 row, and
        // together they were the entire +3.95% the StitchKey migration cost the
        // single-PK path. Composite models keep the full key — correctness there is
        // never traded for the narrowing (see narrow_stitch_key). MUST agree with
        // narrow_owner_key<JS>() in join.cppm (the producer) — a mismatch is a null
        // fn-pointer call, asserted in extract_owner_key.
        static constexpr bool narrow_stitch_key_ = Base::primary_key_column_count_ == 1;

        using StitchMapKey = std::conditional_t<narrow_stitch_key_, std::uint64_t, storm::orm::utilities::StitchKey>;

        // Narrow a built StitchKey to the map's key type. For a composite PK this is
        // the identity — every part is retained, so distinct composite keys stay
        // distinct and can never mis-stitch. Only the provably single-part case takes
        // the word (StitchKey::first_word asserts that in debug).
        [[nodiscard]] static auto narrow_stitch_key(const storm::orm::utilities::StitchKey& key) noexcept
                -> StitchMapKey {
            if constexpr (narrow_stitch_key_) {
                return key.first_word();
            } else {
                return key;
            }
        }

        // Read a Q2 row's owner key through whichever extractor the descriptor carries.
        // make_relation_descriptor populates exactly one of the pair, keyed on the same
        // PK width narrow_stitch_key_ tests, so the branch taken here always matches the
        // one that was set — asserted rather than left implicit, since calling the null
        // one would be a null-fn-pointer jump.
        [[nodiscard]] static auto extract_owner_key(const M2MRelation& rel, Statement* stmt) noexcept -> StitchMapKey {
            if constexpr (narrow_stitch_key_) {
                assert(rel.extract_q2_owner_pk_word_fn != nullptr);
                return rel.extract_q2_owner_pk_word_fn(stmt);
            } else {
                assert(rel.extract_q2_owner_pk_fn != nullptr);
                return rel.extract_q2_owner_pk_fn(stmt);
            }
        }

        // Build the Q1 hash-map key for `obj`'s own primary key (#504 Task 8) — the
        // SAME logical key TwoQueryJoinBase::extract_q2_owner_pk builds from a Q2
        // row's owner-key columns, so a matching row stitches correctly. Each PK
        // part is dispatched to append_int64/append_string by ITS OWN stored type
        // (mirrors bind_one_pk_part in base.cppm and the statement-side extractor in
        // join.cppm); an FK part's stitch value is the referenced row's own key
        // (matching the "<name>_id" column both sides key on), not the FK member's
        // whole C++ object.
        [[nodiscard]] static auto build_owner_stitch_key(const T& obj) noexcept -> storm::orm::utilities::StitchKey {
            storm::orm::utilities::StitchKey key;
            build_owner_stitch_key_impl(key, obj, std::make_index_sequence<Base::primary_key_members_.size()>{});
            return key;
        }

        template <std::size_t... Is>
        static void build_owner_stitch_key_impl(
                storm::orm::utilities::StitchKey& key, const T& obj, std::index_sequence<Is...> /*unused*/
        ) noexcept {
            (append_obj_pk_part_to_key<Base::primary_key_members_[Is]>(key, obj), ...);
        }

        template <std::meta::info Member>
        static void append_obj_pk_part_to_key(storm::orm::utilities::StitchKey& key, const T& obj) noexcept {
            if constexpr (meta::is_fk_field(Member)) {
                using FKType         = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                using InnerFK        = utilities::optional_inner_type_t<FKType>;
                constexpr auto fk_pk = Base::template find_fk_primary_key<FKType>();
                using PartType       = std::remove_cvref_t<decltype(std::declval<InnerFK>().[:fk_pk:])>;
                append_obj_dispatched_part<PartType>(key, obj.[:Member:].[:fk_pk:]);
            } else {
                using PartType = std::remove_cvref_t<typename[:std::meta::type_of(Member):]>;
                append_obj_dispatched_part<PartType>(key, obj.[:Member:]);
            }
        }

        // Mirrors TwoQueryJoinBase::append_dispatched_part's arms exactly — the two
        // must agree byte-for-byte or a Q2 row never finds its Q1 owner. storm::UUID
        // (#507) belongs with the text types: it is stored as TEXT/UUID, and its
        // implicit operator std::string_view feeds append_string directly.
        template <typename PartType>
        static void append_obj_dispatched_part(storm::orm::utilities::StitchKey& key, const auto& value) noexcept {
            if constexpr (std::same_as<PartType, std::string> || std::same_as<PartType, std::string_view> ||
                          std::same_as<PartType, storm::orm::utilities::UUID>) {
                key.append_string(value);
            } else {
                key.append_int64(static_cast<std::int64_t>(value));
            }
        }

        // INNER-join semantics (#392): remove entities whose container stayed
        // empty in ANY inner relation; LEFT relations never drop. When every
        // relation is LEFT the predicate is constant-false and nothing drops.
        static auto drop_empty_relations(plf::hive<T>& results, const JoinStatementWrapper<PkKeyType>& wrapper) noexcept
                -> void {
            for (auto it = results.begin(); it != results.end();) {
                T&         obj  = *it;
                const bool drop = std::ranges::any_of(wrapper.m2m_relations, [&obj](const M2MRelation& rel) {
                    return !rel.is_left && rel.container_empty_fn(&obj);
                });
                if (drop) {
                    it = results.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Reduce a 2-query m2m hive (already LIMIT-bounded to 1 or 2 base entities)
        // to the first()/get() result. ExactOne=false → optional<T>; ExactOne=true →
        // T with 0-row / >1-row errors.
        template <bool ExactOne>
        [[nodiscard]] static auto m2m_one_from_hive(plf::hive<T>&& rows) noexcept
                -> std::expected<std::conditional_t<ExactOne, T, std::optional<T>>, Error> {
            if constexpr (ExactOne) {
                if (rows.empty()) {
                    return std::unexpected(Error{-1, "No row found matching query"});
                }
                if (rows.size() > 1) {
                    return std::unexpected(Error{-1, "Multiple rows found matching query"});
                }
                return std::move(*rows.begin());
            } else {
                if (rows.empty()) {
                    return std::optional<T>{std::nullopt};
                }
                return std::optional<T>{std::move(*rows.begin())};
            }
        }
    };

} // namespace storm::orm::statements
