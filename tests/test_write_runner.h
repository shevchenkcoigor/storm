#pragma once

/**
 * @file test_write_runner.h
 * @brief InsertRunner, UpdateRunner, EraseRunner, ConditionalWriteRunner (#871) and UpsertRunner (#871)
 *        for UnifiedTestCase.
 *
 * Include AFTER `import storm;` — it pulls the helpers and models it needs itself.
 */

#include "test_record_helpers.h"
#include "test_seed_helpers.h"
#include "test_select_runner.h"

namespace storm::test {

// Row count of the table behind qs equals `expected` (the shared tail of the
// insert and erase runners).
inline auto expect_row_count(auto &qs, int expected) -> void {
    auto cnt = qs.count().execute();
    ASSERT_TRUE(cnt.has_value()) << cnt.error().message();
    EXPECT_EQ(static_cast<int>(cnt.value()), expected);
}

// ---------------------------------------------------------------------------
// InsertRunner -- insert_one / insert_batch
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class InsertRunner {
  public:
    template <const auto &Tc> static auto run() -> void {
        QuerySet<Model, ConnType, true> qs;
        if constexpr (Tc.query_type == "insert_one") {
            auto r = qs.insert(make_record<Model>(0)).execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            expect_row_count(qs, Tc.expected.count);

        } else if constexpr (Tc.query_type == "insert_batch") {
            std::vector<Model> batch;
            batch.reserve(static_cast<std::size_t>(Tc.insert_count));
            for (int i = 0; i < Tc.insert_count; ++i)
                batch.push_back(make_record<Model>(i));
            auto r = qs.insert(std::span<const Model>(batch)).execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            expect_row_count(qs, Tc.expected.count);
        }
    }
};

// ---------------------------------------------------------------------------
// UpdateRunner -- update_batch
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class UpdateRunner {
  public:
    template <const auto &Tc> static auto run() -> void {
        QuerySet<Model, ConnType, true> qs;
        seed_records<Tc, Model, ConnType>();

        // Re-fetch to get actual IDs
        auto sel = qs.select().execute();
        ASSERT_TRUE(sel.has_value()) << sel.error().message();

        // Update first update_count records
        std::vector<Model> to_update;
        to_update.reserve(static_cast<std::size_t>(Tc.update_count));
        std::size_t n = 0;
        for (const auto &r : sel.value()) {
            if (n++ >= static_cast<std::size_t>(Tc.update_count))
                break;
            to_update.push_back(make_updated_record<Model>(r));
        }
        if (!to_update.empty()) {
            auto upd = qs.update(std::span<const Model>(to_update)).execute();
            ASSERT_TRUE(upd.has_value()) << upd.error().message();
        }

        // Count records still with original prefix
        auto after = qs.select().execute();
        ASSERT_TRUE(after.has_value()) << after.error().message();
        int unchanged_count = 0;
        for (const auto &r : after.value())
            if (is_original_record<Model>(r))
                ++unchanged_count;
        EXPECT_EQ(unchanged_count, Tc.expected.unchanged);
    }
};

// ---------------------------------------------------------------------------
// EraseRunner -- erase_all / erase_batch (erase_where is ConditionalWriteRunner, #871)
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class EraseRunner {
  public:
    template <const auto &Tc> static auto run() -> void {
        QuerySet<Model, ConnType, true> qs;
        if constexpr (Tc.query_type == "erase_all") {
            seed_records<Tc, Model, ConnType>();

            auto rem = qs.erase_all().execute();
            ASSERT_TRUE(rem.has_value()) << rem.error().message();

            expect_row_count(qs, Tc.expected.remaining);

        } else if constexpr (Tc.query_type == "erase_batch") {
            seed_records<Tc, Model, ConnType>();

            // Re-fetch to get real IDs
            auto sel = qs.select().execute();
            ASSERT_TRUE(sel.has_value()) << sel.error().message();

            // Erase first erase_count records
            std::vector<Model> to_erase;
            to_erase.reserve(static_cast<std::size_t>(Tc.erase_count));
            std::size_t n = 0;
            for (const auto &r : sel.value()) {
                if (n++ >= static_cast<std::size_t>(Tc.erase_count))
                    break;
                to_erase.push_back(r);
            }
            auto rem = qs.erase(std::span<const Model>(to_erase)).execute();
            ASSERT_TRUE(rem.has_value()) << rem.error().message();

            expect_row_count(qs, Tc.expected.remaining);
        }
    }
};

// ---------------------------------------------------------------------------
// Conditional writes and upsert (#871): the write's effect, checked by counting rows
// ---------------------------------------------------------------------------

// The rows equal to the case's `set:` on every field it lists: a spec whose WHERE is those equalities,
// AND-joined, so the shared QueryBuilder builds it and value-checks it exactly like a case's own WHERE
// (`null` lowers to IS NULL, a text is compared as text, a value that does not fit its member fails
// SpecValuesFit). The spec type is deduced from the case, so this header needs no benchmark module.
// TargetOnly keeps the on_conflict.target entries alone: the row an upsert wrote is the one whose
// conflict columns equal their `set:` values, whatever DO UPDATE left in the others.
template <const auto &Tc, bool TargetOnly = false> consteval auto make_set_match_spec() {
    auto spec = std::remove_cvref_t<decltype(Tc.bench)>{};
    auto &where = spec.where;
    using Node = std::remove_cvref_t<decltype(where.nodes[0])>;
    where.begin_form();
    const std::size_t stored = std::min(Tc.set.count, Tc.set.fields.size());
    std::size_t leaves = 0;
    for (std::size_t i = 0; i < stored; ++i) {
        if (TargetOnly && !Tc.on_conflict.targets(Tc.set.fields[i].name.view()))
            continue;
        ++leaves;
        Node leaf;
        leaf.field = Tc.set.fields[i].name;
        leaf.op = decltype(leaf.op){"=="};
        leaf.first_value = where.value_count;
        leaf.value_count = 1;
        where.add_value(Tc.set.fields[i].value);
        where.add_node(leaf);
    }
    where.join_leaves(0, leaves, where.combine_of("and"));
    return spec;
}
template <const auto &Tc, bool TargetOnly = false>
inline constexpr auto set_match_spec = make_set_match_spec<Tc, TargetOnly>();

template <typename Model, const auto &Tc> consteval auto set_names_known() -> bool {
    for (std::size_t i = 0; i < std::min(Tc.set.count, Tc.set.fields.size()); ++i)
        if (!storm::orm::query_builder::has_field<Model>(Tc.set.fields[i].name.view()))
            return false;
    return true;
}

// Every `set:` entry names a member of Model and its value can be read, and compared, as that member's
// type (#844). A blob or a duration has no WHERE comparison (#853), so it cannot be set from YAML: the
// rows the write produced could not be counted.
template <typename Model, const auto &Tc>
concept SetSpecFits =
    set_names_known<Model, Tc>() && storm::orm::query_builder::SpecValuesFit<Model, set_match_spec<Tc>>;

// The QuerySet of the rows equal to `set:`.
template <const auto &Tc, typename Model, typename ConnType, bool TargetOnly = false>
auto set_match_query() -> QuerySet<Model, ConnType, true> {
    return storm::orm::query_builder::QueryBuilder<Model, set_match_spec<Tc, TargetOnly>, yaml_join_resolver<Model>,
                                                   ConnType>::build_qs();
}

// The model with each `set:` member assigned, in its own type; an unlisted member keeps its default.
template <const auto &Tc, typename Model>
    requires SetSpecFits<Model, Tc>
auto set_prototype() -> Model {
    Model proto{};
    template for (constexpr std::size_t I : std::views::iota(0uz, Tc.set.count)) {
        constexpr auto &entry = Tc.set.fields[I];
        constexpr auto member = dispatch_field<Model>(entry.name.view());
        using FieldType = typename[:std::meta::type_of(member):];
        if constexpr (entry.value.kind == decltype(entry.value.kind)::Null)
            proto.[:member:] = std::nullopt;
        else
            proto.[:member:] = storm::orm::query_builder::typed_value_as<FieldType, entry.value>();
    }
    return proto;
}

// The rows of qs; a failed COUNT is a test failure and reads as -1.
inline auto row_count_of(auto &&qs) -> std::int64_t {
    auto n = qs.count().execute();
    if (!n) {
        ADD_FAILURE() << n.error().message();
        return -1;
    }
    return n.value();
}

// The case's own rows for dataset `custom` (`init_dataset_size` make_record rows); every other
// dataset is seeded by the fixture.
template <const auto &Tc, typename Model, typename ConnType> auto seed_custom_dataset() -> void {
    if constexpr (Tc.dataset == "custom")
        seed_records<Tc, Model, ConnType>();
}

// ---------------------------------------------------------------------------
// ConditionalWriteRunner -- update_where / update_all / erase_where (#871)
//
// update_where: base_query().update<set: keys...>(prototype)   (the case's where)
// update_all:   update_all<set: keys...>(prototype)
// erase_where:  base_query().erase()
// Checked: before an update, no row equals `set:` (so `matching` counts exactly the rows written);
// after it, `expected.matching` rows equal `set:`; after an erase, no row matches the where; and
// `expected.remaining` rows are in the table. write_shape_error() (asserted by the fixture body) has
// already rejected a join / order_by / limit, which conditional writes ignore.
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class ConditionalWriteRunner {
    template <const auto &Tc> static auto write(const Model &proto) {
        QuerySet<Model, ConnType, true> table;
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            if constexpr (Tc.query_type == "update_all")
                return table
                    .template update_all<
                        storm::meta::selector_for<dispatch_field<Model>(Tc.set.fields[Is].name.view())>()...>(proto)
                    .execute();
            else
                return base_query<Tc, Model, ConnType>()
                    .template update<
                        storm::meta::selector_for<dispatch_field<Model>(Tc.set.fields[Is].name.view())>()...>(proto)
                    .execute();
        }(std::make_index_sequence<Tc.set.count>{});
    }

  public:
    template <const auto &Tc> static auto run() -> void {
        seed_custom_dataset<Tc, Model, ConnType>();
        QuerySet<Model, ConnType, true> table;
        if constexpr (Tc.query_type == "erase_where") {
            auto r = base_query<Tc, Model, ConnType>().erase().execute();
            ASSERT_TRUE(r.has_value()) << r.error().message();
            EXPECT_EQ(row_count_of(base_query<Tc, Model, ConnType>()), 0) << "rows still matching the erase's where";
        } else {
            static_assert(SetSpecFits<Model, Tc>,
                          "set: names a member the model lacks, or a value that does not fit it");
            const Model proto = set_prototype<Tc, Model>();
            ASSERT_EQ(row_count_of(set_match_query<Tc, Model, ConnType>()), 0)
                << "a seeded row already equals set:, so `matching` would not count the rows written; "
                   "pick values no seeded row holds";
            auto r = write<Tc>(proto);
            ASSERT_TRUE(r.has_value()) << r.error().message();
            EXPECT_EQ(row_count_of(set_match_query<Tc, Model, ConnType>()), Tc.expected.matching)
                << "rows equal to set: after the write";
        }
        EXPECT_EQ(row_count_of(table), Tc.expected.remaining) << "rows in the table after the write";
    }
};

// ---------------------------------------------------------------------------
// UpsertRunner -- upsert_update / upsert_nothing (#871)
//
// insert(prototype).on_conflict<target...>().update<update...>() / .nothing(). Checked: before, no
// row equals `set:`; after, `expected.matching` rows do and `expected.remaining` rows are in the
// table; a returned id is the key of the row whose on_conflict.target columns equal their `set:` values; and DO NOTHING
// reports an id exactly when it inserted. A model whose key is not DB-generated returns void (#572), so there is no id
// to check.
// ---------------------------------------------------------------------------
template <typename Model, typename ConnType> class UpsertRunner {
    // Entry I of on_conflict.target (Update = false) or on_conflict.update (Update = true), as a selector.
    template <const auto &Tc, bool Update, std::size_t I>
    static constexpr auto selector = storm::meta::selector_for<dispatch_field<Model>(
        (Update ? Tc.on_conflict.update : Tc.on_conflict.target).fields[I].view())>();

    template <const auto &Tc> static auto upsert(QuerySet<Model, ConnType, true> &table, const Model &row) {
        auto target = [&]<std::size_t... Ts>(std::index_sequence<Ts...>) {
            return table.insert(row).template on_conflict<selector<Tc, false, Ts>...>();
        }(std::make_index_sequence<Tc.on_conflict.target.declared_count>{});
        if constexpr (Tc.query_type == "upsert_update")
            return [&]<std::size_t... Us>(std::index_sequence<Us...>) {
                return std::move(target).template update<selector<Tc, true, Us>...>().execute();
            }(std::make_index_sequence<Tc.on_conflict.update.declared_count>{});
        else
            return std::move(target).nothing().execute();
    }

    // The returned id is the key of the row whose conflict columns equal their `set:` values.
    template <const auto &Tc> static auto expect_id_names_the_upserted_row(std::int64_t id) -> void {
        auto rows = set_match_query<Tc, Model, ConnType, true>().select().execute();
        ASSERT_TRUE(rows.has_value()) << rows.error().message();
        constexpr auto pk = storm::meta::find_primary_key<Model>();
        bool found = false;
        for (const Model &m : rows.value()) // a plain loop: a splice in a lambda makes any_of an immediate call
            found = found || static_cast<std::int64_t>(m.[:pk:]) == id;
        EXPECT_TRUE(found) << "the returned id " << id << " is not the key of the row on_conflict.target names";
    }

  public:
    template <const auto &Tc> static auto run() -> void {
        static_assert(SetSpecFits<Model, Tc>, "set: names a member the model lacks, or a value that does not fit it");
        seed_custom_dataset<Tc, Model, ConnType>();
        QuerySet<Model, ConnType, true> table;
        const Model row = set_prototype<Tc, Model>();
        ASSERT_EQ(row_count_of(set_match_query<Tc, Model, ConnType>()), 0)
            << "a seeded row already equals set:, so `matching` would not count the rows written; "
               "pick values no seeded row holds";
        const std::int64_t before = row_count_of(table);
        auto r = upsert<Tc>(table, row);
        ASSERT_TRUE(r.has_value()) << r.error().message();
        const std::int64_t after = row_count_of(table);
        using Value = typename decltype(r)::value_type;
        if constexpr (std::is_same_v<Value, std::int64_t>) {
            expect_id_names_the_upserted_row<Tc>(r.value());
        } else if constexpr (std::is_same_v<Value, std::optional<std::int64_t>>) {
            EXPECT_EQ(r.value().has_value(), after == before + 1)
                << "DO NOTHING reports an id exactly when it inserted";
            if (r.value().has_value())
                expect_id_names_the_upserted_row<Tc>(*r.value());
        }
        EXPECT_EQ(row_count_of(set_match_query<Tc, Model, ConnType>()), Tc.expected.matching)
            << "rows equal to set: after the upsert";
        EXPECT_EQ(after, Tc.expected.remaining) << "rows in the table after the upsert";
    }
};

} // namespace storm::test
