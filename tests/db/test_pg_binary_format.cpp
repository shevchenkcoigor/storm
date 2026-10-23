#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

#include "test_pg_binary_models.h" // NOSONAR cpp:S954 — models + compile-time classification gates

// ============================================================================
// #600 Phase 1 — binary result format on PostgreSQL: the DECODERS
//
// This TU drives Statement::set_result_binary directly against real server
// types. Wrong width or wrong byte order silently corrupts values that still
// "look fine" (a byte-swapped 42 is still an int), so the assertions here lean
// on boundaries and on widths Storm's own DDL never emits.
//
// The classification half — which statements are ALLOWED to ask for binary —
// lives in test_pg_binary_models.h (compile-time) and
// test_pg_binary_roundtrip.cpp (live ORM round-trips).
//
// PostgreSQL only: SQLite has no equivalent switch (sqlite3_column_* is already
// native binary). Auto-skips when STORM_PG_CONNSTR is unset or unreachable.
// ============================================================================

// NOLINTBEGIN(readability-implicit-bool-conversion,readability-identifier-length)

namespace {

    using storm::test::binfmt::PgConn;

    class PgBinaryDecoderTest : public ::testing::Test {
      public:
        auto SetUp() -> void override {
            if (!storm::test::backend_available<PgConn>()) {
                GTEST_SKIP() << "PostgreSQL not available (STORM_PG_CONNSTR unset / unreachable)";
            }
            auto opened = PgConn::open(storm::test::get_connection_string<PgConn>());
            ASSERT_TRUE(opened.has_value()) << "could not open PostgreSQL connection";
            conn_ = std::make_unique<PgConn>(std::move(opened.value()));
        }

        // Prepare `sql`, ask for binary results, and step to the single row.
        auto binary_row(std::string_view sql) -> PgConn::Statement {
            auto stmt = conn_->prepare(sql);
            EXPECT_TRUE(stmt.has_value());
            stmt->set_result_binary(true);
            auto step = stmt->step();
            EXPECT_TRUE(step.has_value());
            EXPECT_TRUE(step.value());
            return std::move(stmt.value());
        }

        std::unique_ptr<PgConn> conn_;
    };

    // Widths are read from the SERVER, not assumed from the caller's C++ type.
    // That is load-bearing: PG's width-matched integer mapping (#603) still puts
    // an `unsigned int` field on BIGINT (a 32-bit unsigned value needs 8 bytes of
    // signed range), so extract_int() — the extractor for 32-bit-and-smaller C++
    // types — routinely faces an 8-byte int8. A fixed 4-byte read would return
    // the high half (0 for every small positive number) and look entirely
    // plausible.
    TEST_F(PgBinaryDecoderTest, IntegerWidthsDecodeByServerWidth) {
        auto stmt = binary_row("SELECT 42::int8, 42::int4, 42::int2, (-42)::int8, (-42)::int4, (-42)::int2");
        EXPECT_EQ(stmt.extract_int64(0), 42);
        EXPECT_EQ(stmt.extract_int64(1), 42);
        EXPECT_EQ(stmt.extract_int64(2), 42);
        EXPECT_EQ(stmt.extract_int(0), 42) << "extract_int must cope with a BIGINT column — e.g. Storm's PG DDL "
                                              "gives an `unsigned int` field BIGINT";
        EXPECT_EQ(stmt.extract_int(1), 42);
        EXPECT_EQ(stmt.extract_int(2), 42);
        EXPECT_EQ(stmt.extract_int64(3), -42) << "sign must survive the width widening";
        EXPECT_EQ(stmt.extract_int64(4), -42);
        EXPECT_EQ(stmt.extract_int64(5), -42);
    }

    TEST_F(PgBinaryDecoderTest, IntegerBoundariesSurviveByteOrder) {
        // A byte-swapped value is still a plausible-looking integer, so the
        // boundaries are the assertion that actually pins the endian transform.
        auto stmt = binary_row(
                "SELECT 9223372036854775807::int8, (-9223372036854775808)::int8, "
                "2147483647::int4, (-2147483648)::int4"
        );
        EXPECT_EQ(stmt.extract_int64(0), std::numeric_limits<std::int64_t>::max());
        EXPECT_EQ(stmt.extract_int64(1), std::numeric_limits<std::int64_t>::min());
        EXPECT_EQ(stmt.extract_int64(2), std::numeric_limits<std::int32_t>::max());
        EXPECT_EQ(stmt.extract_int64(3), std::numeric_limits<std::int32_t>::min());
    }

    TEST_F(PgBinaryDecoderTest, FloatingWidthsDecodeByServerWidth) {
        // '-0' must be quoted: PostgreSQL parses a bare -0.0 as unary minus over a
        // NUMERIC literal, and numeric has no signed zero, so (-0.0)::float8 is
        // POSITIVE zero and the sign-bit assertion below would pass vacuously.
        // float8's input function is what preserves the sign.
        auto stmt = binary_row("SELECT 1.5::float8, 2.5::float4, '-0'::float8, 'NaN'::float8, 'Infinity'::float8");
        EXPECT_DOUBLE_EQ(stmt.extract_double(0), 1.5);
        EXPECT_FLOAT_EQ(stmt.extract_float(1), 2.5F);
        EXPECT_DOUBLE_EQ(stmt.extract_double(1), 2.5) << "a REAL column read as double must widen, not misread";
        EXPECT_FLOAT_EQ(stmt.extract_float(0), 1.5F) << "a DOUBLE PRECISION column read as float must narrow";
        EXPECT_TRUE(std::signbit(stmt.extract_double(2))) << "negative zero must keep its sign bit";
        EXPECT_TRUE(std::isnan(stmt.extract_double(3)));
        EXPECT_TRUE(std::isinf(stmt.extract_double(4)));
    }

    TEST_F(PgBinaryDecoderTest, BoolIsASingleByte) {
        auto stmt = binary_row("SELECT true, false");
        EXPECT_TRUE(stmt.extract_bool(0));
        EXPECT_FALSE(stmt.extract_bool(1));
    }

    // The claim the issue's Definition of Done asks to VERIFY rather than assume:
    // PG's binary form for text/varchar is a raw byte copy (textsend/textrecv do
    // no encoding), so extract_text_view/extract_text_ptr need no binary branch.
    TEST_F(PgBinaryDecoderTest, TextIsByteIdenticalUnderBinaryFormat) {
        auto stmt = binary_row("SELECT 'hello'::text, ''::text, 'wide \xC3\xA9\xC3\xA8'::text, 'vc'::varchar(8)");
        EXPECT_EQ(stmt.extract_text_view(0), "hello");
        EXPECT_EQ(stmt.extract_text_view(1), "");
        EXPECT_EQ(stmt.extract_text_view(2), "wide \xC3\xA9\xC3\xA8");
        EXPECT_EQ(stmt.extract_text_view(3), "vc");
    }

    // BYTEA arrives as raw bytes under binary format — the "\x..." hex decode the
    // text path performs must NOT run, or every byte pair collapses to one.
    TEST_F(PgBinaryDecoderTest, BlobIsRawBytesUnderBinaryFormat) {
        auto        stmt = binary_row("SELECT '\\x00ff10'::bytea, ''::bytea");
        const auto* data = static_cast<const unsigned char*>(stmt.extract_blob_ptr(0));
        ASSERT_NE(data, nullptr);
        ASSERT_EQ(stmt.extract_bytes(0), 3);
        EXPECT_EQ(data[0], 0x00);
        EXPECT_EQ(data[1], 0xFF);
        EXPECT_EQ(data[2], 0x10);

        EXPECT_EQ(stmt.extract_bytes(1), 0) << "an empty BYTEA is zero-length, not NULL";
    }

    TEST_F(PgBinaryDecoderTest, NullsAreStillNullAndDecodeToZero) {
        auto stmt = binary_row("SELECT NULL::int8, NULL::float8, NULL::text, NULL::bytea, NULL::bool");
        EXPECT_TRUE(stmt.is_null(0)) << "PQgetisnull does not depend on the result format";
        EXPECT_TRUE(stmt.is_null(1));
        EXPECT_TRUE(stmt.is_null(2));
        EXPECT_TRUE(stmt.is_null(3));
        EXPECT_TRUE(stmt.is_null(4));
        // A zero-length value must not be read as if it carried a payload.
        EXPECT_EQ(stmt.extract_int64(0), 0);
        EXPECT_EQ(stmt.extract_int(0), 0);
        EXPECT_DOUBLE_EQ(stmt.extract_double(1), 0.0);
        EXPECT_FLOAT_EQ(stmt.extract_float(1), 0.0F);
        EXPECT_EQ(stmt.extract_text_view(2), "");
        EXPECT_EQ(stmt.extract_blob_ptr(3), nullptr);
        EXPECT_FALSE(stmt.extract_bool(4));
    }

    // The default is unchanged: a statement nobody opted in stays on text, and
    // the text decoders keep parsing ASCII exactly as before.
    TEST_F(PgBinaryDecoderTest, TextFormatRemainsTheDefault) {
        auto stmt = conn_->prepare("SELECT 42::int8, 1.5::float8, true, '\\x00ff'::bytea");
        ASSERT_TRUE(stmt.has_value());
        auto step = stmt->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());
        EXPECT_EQ(stmt->extract_int64(0), 42);
        EXPECT_DOUBLE_EQ(stmt->extract_double(1), 1.5);
        EXPECT_TRUE(stmt->extract_bool(2));
        const auto* data = static_cast<const unsigned char*>(stmt->extract_blob_ptr(3));
        ASSERT_NE(data, nullptr);
        ASSERT_EQ(stmt->extract_bytes(3), 2);
        EXPECT_EQ(data[0], 0x00);
        EXPECT_EQ(data[1], 0xFF);
    }

    // reset() puts the format back to text, so a cached statement — reset on every
    // cache hit — can never inherit a previous caller's choice.
    TEST_F(PgBinaryDecoderTest, ResetClearsTheRequestedFormat) {
        auto stmt = conn_->prepare("SELECT 42::int8");
        ASSERT_TRUE(stmt.has_value());
        stmt->set_result_binary(true);
        stmt->reset();
        auto step = stmt->step();
        ASSERT_TRUE(step.has_value());
        ASSERT_TRUE(step.value());
        EXPECT_EQ(stmt->extract_int64(0), 42) << "after reset the statement must decode text again";
    }

} // namespace

// NOLINTEND(readability-implicit-bool-conversion,readability-identifier-length)
