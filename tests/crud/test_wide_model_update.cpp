#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm;
import std;

#include "../../shared/models/wide_record.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::QuerySet;

// NOLINTBEGIN(readability-implicit-bool-conversion)

// The UPDATE / upsert SET clauses of a wide model outgrow the fixed 1024-byte buffer the
// grammar builders used to write into; the buffer cut the clause off silently, so the
// statement failed to prepare. Each test runs one statement shape over a model whose SET
// clause is longer than that buffer, then reads every column back.

namespace {
    // The SET targets of the conditional UPDATE / upsert, spelled once.
    template <auto... S> struct Cols {};

    using AllWideColumns =
            Cols<fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_00,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_01,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_02,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_03,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_04,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_05,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_06,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_07,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_08,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_09,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_10,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_11,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_12,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_13,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_14,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_15,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_16,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_17,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_18,
                 fields::WideRecord.wide_column_padded_to_fill_the_set_clause_buffer_19>;

    constexpr auto kWideMembers = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^WideRecord, std::meta::access_context::unchecked())
    );

    // Give every non-key column the value base + its position.
    auto filled(int key, int base) -> WideRecord {
        WideRecord record{.id = key};
        int        position = 0;
        template for (constexpr auto member : kWideMembers) {
            if constexpr (member != ^^WideRecord::id) {
                record.[:member:] = base + position++;
            }
        }
        return record;
    }

    void expect_filled(const WideRecord& record, int base) {
        int position = 0;
        template for (constexpr auto member : kWideMembers) {
            if constexpr (member != ^^WideRecord::id) {
                EXPECT_EQ(record.[:member:], base + position++) << std::meta::identifier_of(member);
            }
        }
    }

    template <typename Conn, auto... S>
    auto conditional_update(QuerySet<WideRecord, Conn>& qs, const WideRecord& proto, Cols<S...>) {
        return qs.where(fields::WideRecord.id == proto.id).template update<S...>(proto).execute();
    }

    template <typename Conn, auto... S>
    auto upsert_update(QuerySet<WideRecord, Conn>& qs, const WideRecord& row, Cols<S...>) {
        return qs.insert(row).template on_conflict<fields::WideRecord.id>().template update<S...>().execute();
    }
} // namespace

template <typename ConnType> class WideModelUpdateTest : public StormTestFixture<WideRecord, ConnType> {
  public:
    static auto fetch(QuerySet<WideRecord, ConnType>& qs) -> WideRecord {
        auto rows = qs.where(fields::WideRecord.id == 1).select().execute();
        EXPECT_TRUE(rows.has_value());
        EXPECT_EQ(rows.value().size(), 1U);
        return *rows.value().begin();
    }
};

TYPED_TEST_SUITE(WideModelUpdateTest, DatabaseTypes);

TYPED_TEST(WideModelUpdateTest, UpdateWritesEveryColumn) {
    QuerySet<WideRecord, TypeParam> qs;
    ASSERT_TRUE(qs.insert(filled(1, 100)).execute().has_value());

    ASSERT_TRUE(qs.update(filled(1, 1000)).execute().has_value()) << "wide UPDATE must prepare and run";

    expect_filled(this->fetch(qs), 1000);
}

TYPED_TEST(WideModelUpdateTest, ConditionalUpdateWritesEveryColumn) {
    QuerySet<WideRecord, TypeParam> qs;
    ASSERT_TRUE(qs.insert(filled(1, 100)).execute().has_value());

    ASSERT_TRUE(conditional_update(qs, filled(1, 2000), AllWideColumns{}).has_value())
            << "wide conditional UPDATE must prepare and run";

    expect_filled(this->fetch(qs), 2000);
}

TYPED_TEST(WideModelUpdateTest, UpsertDoUpdateWritesEveryColumn) {
    QuerySet<WideRecord, TypeParam> qs;
    ASSERT_TRUE(qs.insert(filled(1, 100)).execute().has_value());

    ASSERT_TRUE(upsert_update(qs, filled(1, 3000), AllWideColumns{}).has_value()) << "wide upsert must prepare and run";

    expect_filled(this->fetch(qs), 3000);
}

// NOLINTEND(readability-implicit-bool-conversion)
