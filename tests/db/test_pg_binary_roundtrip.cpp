#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm;
import std;

#include "test_fixture.h"          // NOSONAR cpp:S954 — StormTestFixture
#include "test_pg_binary_models.h" // NOSONAR cpp:S954 — models + compile-time classification gates
using storm::QuerySet;
using storm::UUID;

// ============================================================================
// #600 Phase 1 — binary result format on PostgreSQL: the ORM ROUND-TRIPS
//
// The companion to test_pg_binary_format.cpp's decoder suite. Where that TU
// proves the decoders read bytes correctly, this one proves the CLASSIFICATION
// routes each statement to the right decoder: a model made entirely of safe
// columns goes binary, a model with one unsafe column drags the whole statement
// back to text, and the four deliberately-excluded groups keep round-tripping.
//
// Each execution path gets its own case, because each one opts in at its own
// call site: execute(), the first()/get() fast paths, values()/distinct(),
// set operations, the m2m Q1 leg, and the rows() coroutine.
//
// PostgreSQL only. Auto-skips when STORM_PG_CONNSTR is unset or unreachable.
// ============================================================================

// NOLINTBEGIN(readability-implicit-bool-conversion,readability-identifier-length)

namespace {

    using storm::test::binfmt::PgConn;

    // Select the whole table and hand back its single row. ADD_FAILURE + a default
    // row (rather than ASSERT_) keeps this usable from a non-void helper.
    template <typename Model> auto select_single(QuerySet<Model, PgConn>& qs) -> Model {
        auto rows = qs.select().execute();
        if (!rows.has_value()) {
            ADD_FAILURE() << "select failed: " << rows.error().message();
            return Model{};
        }
        if (rows->size() != 1U) {
            ADD_FAILURE() << "expected exactly one row, got " << rows->size();
            return Model{};
        }
        return *rows->begin();
    }

    // ── All-safe model: the binary path ─────────────────────────────────────

    class PgBinarySafeRowTest : public StormTestFixture<BinSafeRow, PgConn> {};

    TEST_F(PgBinarySafeRowTest, EveryBinarySafeTypeRoundTrips) {
        QuerySet<BinSafeRow, PgConn> qs;

        const BinSafeRow row{
                .big        = 9'876'543'210LL,
                .dbl        = 3.5,
                .flt        = 1.25F,
                .flag       = true,
                .label      = "hello",
                .file_path  = std::filesystem::path{"/tmp/storm/x.txt"},
                .payload    = {0x00, 0xFF, 0x10, 0x7F},
                .opt_int    = 7,
                .opt_dbl    = -2.5,
                .opt_text   = "opt",
                .signed_u64 = 4'000'000'000ULL,
        };
        ASSERT_TRUE(qs.insert(row).execute().has_value());

        const BinSafeRow got = select_single(qs);
        EXPECT_EQ(got.big, 9'876'543'210LL);
        EXPECT_DOUBLE_EQ(got.dbl, 3.5);
        EXPECT_FLOAT_EQ(got.flt, 1.25F);
        EXPECT_TRUE(got.flag);
        EXPECT_EQ(got.label, "hello");
        EXPECT_EQ(got.file_path, std::filesystem::path{"/tmp/storm/x.txt"});
        ASSERT_EQ(got.payload.size(), 4U);
        EXPECT_EQ(got.payload[0], 0x00);
        EXPECT_EQ(got.payload[1], 0xFF);
        EXPECT_EQ(got.payload[2], 0x10);
        EXPECT_EQ(got.payload[3], 0x7F);
        EXPECT_EQ(got.opt_int, 7);
        EXPECT_DOUBLE_EQ(*got.opt_dbl, -2.5);
        EXPECT_EQ(*got.opt_text, "opt");
        EXPECT_EQ(got.signed_u64, 4'000'000'000ULL);
    }

    TEST_F(PgBinarySafeRowTest, NullsRoundTripForEveryNullableColumn) {
        QuerySet<BinSafeRow, PgConn> qs;
        ASSERT_TRUE(qs.insert(BinSafeRow{.label = "nulls"}).execute().has_value());

        const BinSafeRow got = select_single(qs);
        EXPECT_FALSE(got.opt_int.has_value());
        EXPECT_FALSE(got.opt_dbl.has_value());
        EXPECT_FALSE(got.opt_text.has_value());
        EXPECT_TRUE(got.payload.empty()) << "an empty BLOB stays empty, it does not become garbage";
        EXPECT_EQ(got.label, "nulls");
    }

    TEST_F(PgBinarySafeRowTest, BoundaryValuesSurviveTheDecode) {
        QuerySet<BinSafeRow, PgConn> qs;

        const std::vector<BinSafeRow> batch{
                {.big   = std::numeric_limits<std::int64_t>::max(),
                 .dbl   = std::numeric_limits<double>::max(),
                 .flt   = std::numeric_limits<float>::max(),
                 .label = "max"},
                {.big   = std::numeric_limits<std::int64_t>::min(),
                 .dbl   = std::numeric_limits<double>::lowest(),
                 .flt   = std::numeric_limits<float>::lowest(),
                 .label = "min"},
                {.big = 0, .dbl = -0.0, .flt = -0.0F, .label = "negzero"},
        };
        ASSERT_TRUE(qs.insert(std::span<const BinSafeRow>(batch)).execute().has_value());

        auto rows = qs.select().execute();
        ASSERT_TRUE(rows.has_value()) << rows.error().message();
        ASSERT_EQ(rows->size(), 3U);

        for (const BinSafeRow& got : *rows) {
            if (got.label == "max") {
                EXPECT_EQ(got.big, std::numeric_limits<std::int64_t>::max());
                EXPECT_DOUBLE_EQ(got.dbl, std::numeric_limits<double>::max());
                EXPECT_FLOAT_EQ(got.flt, std::numeric_limits<float>::max());
            } else if (got.label == "min") {
                EXPECT_EQ(got.big, std::numeric_limits<std::int64_t>::min());
                EXPECT_DOUBLE_EQ(got.dbl, std::numeric_limits<double>::lowest());
                EXPECT_FLOAT_EQ(got.flt, std::numeric_limits<float>::lowest());
            } else {
                EXPECT_TRUE(std::signbit(got.dbl)) << "negative zero must keep its sign bit through the decode";
                EXPECT_TRUE(std::signbit(got.flt));
            }
        }
    }

    TEST_F(PgBinarySafeRowTest, EmptyStringsAndEmptyBlobsAreNotNull) {
        QuerySet<BinSafeRow, PgConn> qs;
        ASSERT_TRUE(qs.insert(BinSafeRow{.label = "", .payload = {}, .opt_text = std::string{}}).execute().has_value());

        const BinSafeRow got = select_single(qs);
        EXPECT_EQ(got.label, "");
        EXPECT_TRUE(got.payload.empty());
        ASSERT_TRUE(got.opt_text.has_value()) << "an empty string is a value, not a NULL";
        EXPECT_EQ(*got.opt_text, "");
    }

    // Projections and set operations read real model columns through their own
    // call sites, each opting in separately.
    TEST_F(PgBinarySafeRowTest, ProjectionsAndSetOpsRoundTrip) {
        QuerySet<BinSafeRow, PgConn>  qs;
        const std::vector<BinSafeRow> batch{
                {.big = 1, .dbl = 1.5, .flt = 1.0F, .label = "a"},
                {.big = 2, .dbl = 2.5, .flt = 2.0F, .label = "b"},
        };
        ASSERT_TRUE(qs.insert(std::span<const BinSafeRow>(batch)).execute().has_value());

        auto labels = qs.values<fields::BinSafeRow.label>().execute();
        ASSERT_TRUE(labels.has_value()) << labels.error().message();
        EXPECT_EQ(labels->size(), 2U);

        auto pairs = qs.values<fields::BinSafeRow.big, fields::BinSafeRow.dbl>().execute();
        ASSERT_TRUE(pairs.has_value()) << pairs.error().message();
        EXPECT_EQ(pairs->size(), 2U);

        auto distinct_flags = qs.distinct<fields::BinSafeRow.flag>().execute();
        ASSERT_TRUE(distinct_flags.has_value()) << distinct_flags.error().message();
        EXPECT_EQ(distinct_flags->size(), 1U);

        auto combined = qs.where(fields::BinSafeRow.big == std::int64_t{1})
                                .union_(qs.where(fields::BinSafeRow.big == std::int64_t{2}))
                                .execute();
        ASSERT_TRUE(combined.has_value()) << combined.error().message();
        EXPECT_EQ(combined->size(), 2U);
    }

    // first()/get() take the zero-parameter fast paths, which prepare their own
    // cached statement — separate call sites from execute()'s.
    TEST_F(PgBinarySafeRowTest, FirstAndGetFastPathsRoundTrip) {
        QuerySet<BinSafeRow, PgConn> qs;
        ASSERT_TRUE(qs.insert(BinSafeRow{.big = 5, .dbl = 0.25, .flt = 0.5F, .label = "solo"}).execute().has_value());

        auto first = qs.first().execute();
        ASSERT_TRUE(first.has_value()) << first.error().message();
        ASSERT_TRUE(first->has_value());
        EXPECT_EQ((*first)->big, 5);
        EXPECT_DOUBLE_EQ((*first)->dbl, 0.25);

        auto only = qs.get().execute();
        ASSERT_TRUE(only.has_value()) << only.error().message();
        EXPECT_EQ(only->label, "solo");
        EXPECT_FLOAT_EQ(only->flt, 0.5F);
    }

    // The rows() streaming path uses a DEDICATED statement (prepare(), not the
    // cache) that is moved into the coroutine frame. Statement's move is
    // swap-based, so the format flag survives only because swap() carries it —
    // drop it from swap and this decodes binary bytes with the text parsers.
    TEST_F(PgBinarySafeRowTest, RowsGeneratorStreamsOverTheBinaryPath) {
        QuerySet<BinSafeRow, PgConn>  qs;
        const std::vector<BinSafeRow> batch{
                {.big = 11, .dbl = 1.5, .flt = 1.25F, .flag = true, .label = "r1"},
                {.big = 22, .dbl = 2.5, .flt = 2.25F, .flag = false, .label = "r2"},
        };
        ASSERT_TRUE(qs.insert(std::span<const BinSafeRow>(batch)).execute().has_value());

        std::vector<std::int64_t> bigs;
        std::vector<std::string>  labels;
        for (auto&& row : qs.rows()) {
            ASSERT_TRUE(row.has_value()) << row.error().message();
            bigs.push_back(row->big);
            labels.push_back(row->label);
            EXPECT_TRUE(row->dbl == 1.5 || row->dbl == 2.5) << "double decoded as " << row->dbl;
        }
        std::ranges::sort(bigs);
        std::ranges::sort(labels);
        EXPECT_EQ(bigs, (std::vector<std::int64_t>{11, 22}));
        EXPECT_EQ(labels, (std::vector<std::string>{"r1", "r2"}));
    }

    // ── m2m: the Q1 leg ─────────────────────────────────────────────────────

    class PgBinaryM2MTest : public StormTestFixture<BinDoc, PgConn, BinTag> {};

    // The m2m Q1 leg (#391) is the only site where a model carrying a relation
    // member takes the binary path — relation members are filtered out of the
    // classification before it runs. Q2 (the related rows) stays on text.
    TEST_F(PgBinaryM2MTest, EagerLoadQ1DecodesOverTheBinaryPath) {
        QuerySet<BinTag, PgConn>  tag_qs;
        const std::vector<BinTag> tags{{.label = "alpha"}, {.label = "beta"}};
        ASSERT_TRUE(tag_qs.insert(std::span<const BinTag>(tags)).execute().has_value());

        QuerySet<BinDoc, PgConn> doc_qs;
        ASSERT_TRUE(doc_qs.insert(BinDoc{.title = "Doc", .rank = 4242}).execute().has_value());

        // Plain select first: Q1's column shape, without the eager load.
        const BinDoc plain = select_single(doc_qs);
        EXPECT_EQ(plain.title, "Doc");
        EXPECT_EQ(plain.rank, 4242) << "the owner's scalar columns must survive the binary decode";

        // LEFT keeps an owner with no related rows, so this exercises Q1 + the
        // stitch with the entity surviving to the assertions.
        auto left = doc_qs.left_join<fields::BinDoc.tags>().select().execute();
        ASSERT_TRUE(left.has_value()) << left.error().message();
        ASSERT_EQ(left->size(), 1U) << "LEFT keeps an owner with no related rows";
        EXPECT_EQ(left->begin()->title, "Doc");
        EXPECT_EQ(left->begin()->rank, 4242) << "Q1 decoded over the binary path";
        EXPECT_TRUE(left->begin()->tags.empty());
    }

    // ── Mixed model: the whole-statement fallback ───────────────────────────

    class PgBinaryMixedRowTest : public StormTestFixture<BinMixedRow, PgConn> {};

    // One TIMESTAMP column keeps this SELECT on the text path, and BOTH the safe
    // and the unsafe columns must come back intact. If the classification leaked
    // (binary requested anyway), `stamp` would decode an int64 microsecond count
    // as a text timestamp and the value would collapse.
    TEST_F(PgBinaryMixedRowTest, OneUnsafeColumnKeepsTheWholeRowOnText) {
        QuerySet<BinMixedRow, PgConn> qs;

        const auto when = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        ASSERT_TRUE(qs.insert(BinMixedRow{.n = 4242, .label = "mixed", .stamp = when}).execute().has_value());

        const BinMixedRow got = select_single(qs);
        EXPECT_EQ(got.n, 4242);
        EXPECT_EQ(got.label, "mixed");
        EXPECT_EQ(std::chrono::floor<std::chrono::seconds>(got.stamp), when);
    }

    // ── Unsafe model: the text path, and per-projection divergence ──────────

    class PgBinaryUnsafeRowTest : public StormTestFixture<BinUnsafeRow, PgConn> {};

    // Seed one row and hand back what was written, so each case below asserts
    // against the same known values.
    struct UnsafeSeed {
        std::chrono::system_clock::time_point when;
        std::chrono::year_month_day           day;
        UUID                                  uid;
    };

    auto seed_unsafe_row(QuerySet<BinUnsafeRow, PgConn>& qs) -> UnsafeSeed {
        const UnsafeSeed seed{
                .when = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
                .day  = std::chrono::year{2024} / std::chrono::March / std::chrono::day{17},
                .uid  = UUID{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"},
        };
        EXPECT_TRUE(qs.insert(BinUnsafeRow{
                                      .huge  = std::numeric_limits<std::uint64_t>::max(),
                                      .uid   = seed.uid,
                                      .day   = seed.day,
                                      .stamp = seed.when,
                              })
                            .execute()
                            .has_value());
        return seed;
    }

    // Regression: the four groups Phase 1 deliberately does not decode. Each has a
    // PG binary wire form that is NOT its text form (NUMERIC base-10000 digits,
    // 16 raw UUID bytes, int32 days since 2000-01-01, int64 microseconds), so
    // wrongly classifying any of them as safe corrupts it silently.
    TEST_F(PgBinaryUnsafeRowTest, UuidDateTimestampAndFullUnsignedStillRoundTrip) {
        QuerySet<BinUnsafeRow, PgConn> qs;
        const UnsafeSeed               seed = seed_unsafe_row(qs);

        const BinUnsafeRow got = select_single(qs);
        EXPECT_EQ(got.huge, std::numeric_limits<std::uint64_t>::max())
                << "full_unsigned is NUMERIC(20,0) — its binary form is base-10000 digit groups, not decimal text";
        EXPECT_EQ(got.uid.value, seed.uid.value);
        EXPECT_EQ(got.day, seed.day);
        EXPECT_EQ(std::chrono::floor<std::chrono::seconds>(got.stamp), seed.when);
    }

    // The per-projection predicate diverging from the whole-row one, executed.
    // `id` projects out of an unsafe model over the BINARY path (asserted at
    // compile time in the header); the other four stay on text. Both verdicts
    // must produce correct values.
    TEST_F(PgBinaryUnsafeRowTest, ProjectionsDivergeFromTheWholeRowVerdict) {
        QuerySet<BinUnsafeRow, PgConn> qs;
        const UnsafeSeed               seed = seed_unsafe_row(qs);

        auto ids = qs.values<fields::BinUnsafeRow.id>().execute();
        ASSERT_TRUE(ids.has_value()) << ids.error().message();
        ASSERT_EQ(ids->size(), 1U);
        EXPECT_GT(*ids->begin(), 0) << "a safe column projected out of an unsafe model, decoded over binary";

        // full_unsigned is the load-bearing exclusion: it shares std::uint64_t with
        // signed_storage, so only the annotation keeps this off the binary path.
        // That verdict is pinned by the static_assert in test_pg_binary_models.h;
        // asserted here only as "the query runs and yields a row".
        //
        // Its VALUE is deliberately not asserted. Projecting a full_unsigned column
        // is broken independently of #600 and has been since the annotation landed:
        // ProjectionStatement::execute_query_loop calls extract_column_value<T>
        // directly, so it never reaches parse_full_unsigned (which lives only in
        // BaseStatement::extract_column_fast). The NUMERIC(20,0) text is handed to
        // strtoll, which saturates — UINT64_MAX comes back as INT64_MAX. A whole-row
        // select() of the same column is correct (see the round-trip above), which is
        // what makes this a projection-path defect and not a #600 regression. Reported
        // for a follow-up issue rather than fixed here: the same hole affects SQLite,
        // where the column is zero-padded TEXT, so the fix needs its own cross-backend
        // tests.
        auto huge = qs.values<fields::BinUnsafeRow.huge>().execute();
        ASSERT_TRUE(huge.has_value()) << huge.error().message();
        ASSERT_EQ(huge->size(), 1U);

        auto uids = qs.values<fields::BinUnsafeRow.uid>().execute();
        ASSERT_TRUE(uids.has_value()) << uids.error().message();
        ASSERT_EQ(uids->size(), 1U);
        EXPECT_EQ(uids->begin()->value, seed.uid.value);

        auto days = qs.values<fields::BinUnsafeRow.day>().execute();
        ASSERT_TRUE(days.has_value()) << days.error().message();
        ASSERT_EQ(days->size(), 1U);
        EXPECT_EQ(*days->begin(), seed.day);

        auto stamps = qs.values<fields::BinUnsafeRow.stamp>().execute();
        ASSERT_TRUE(stamps.has_value()) << stamps.error().message();
        ASSERT_EQ(stamps->size(), 1U);
        EXPECT_EQ(std::chrono::floor<std::chrono::seconds>(*stamps->begin()), seed.when);

        // A mixed projection: one unsafe column disqualifies the whole statement,
        // so the SAFE column beside it must come back over the text path too.
        // (std::get<1> — the full_unsigned half — is not asserted, for the
        // projection-path reason documented above.)
        auto mixed = qs.values<fields::BinUnsafeRow.id, fields::BinUnsafeRow.huge>().execute();
        ASSERT_TRUE(mixed.has_value()) << mixed.error().message();
        ASSERT_EQ(mixed->size(), 1U);
        EXPECT_EQ(std::get<0>(*mixed->begin()), *ids->begin())
                << "the safe column keeps its value when an unsafe one forces the text path";
    }

    // Set operations over an unsafe model must take the text path and still
    // round-trip — pinning the fallback rather than assuming it.
    TEST_F(PgBinaryUnsafeRowTest, SetOpOnAnUnsafeModelStaysOnText) {
        QuerySet<BinUnsafeRow, PgConn> qs;
        const UnsafeSeed               seed = seed_unsafe_row(qs);

        auto combined = qs.where(fields::BinUnsafeRow.id > 0).union_(qs.where(fields::BinUnsafeRow.id > 0)).execute();
        ASSERT_TRUE(combined.has_value()) << combined.error().message();
        ASSERT_EQ(combined->size(), 1U) << "UNION de-duplicates the identical operands";

        const BinUnsafeRow& got = *combined->begin();
        EXPECT_EQ(got.huge, std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(got.uid.value, seed.uid.value);
        EXPECT_EQ(got.day, seed.day);
        EXPECT_EQ(std::chrono::floor<std::chrono::seconds>(got.stamp), seed.when);
    }

} // namespace

// NOLINTEND(readability-implicit-bool-conversion,readability-identifier-length)
