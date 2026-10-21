#ifndef STORM_TESTS_TEST_PG_BINARY_MODELS_H
#define STORM_TESTS_TEST_PG_BINARY_MODELS_H

/**
 * @file test_pg_binary_models.h
 * @brief Model fixtures + compile-time classification gates for #600 Phase 1.
 *
 * IMPORTANT: like the shared model headers, this one names storm:: types and must only
 * be included AFTER `import storm;` (NOSONAR cpp:S954 at the include site).
 *
 * Split across two test TUs: test_pg_binary_format.cpp (the low-level decoders,
 * driven through Statement::set_result_binary directly) and
 * test_pg_binary_roundtrip.cpp (live ORM round-trips on PostgreSQL).
 *
 * Models live at namespace scope, NOT in an anonymous namespace: the fields::
 * proxies below are `inline constexpr` at global scope, so an internal-linkage
 * model would give each TU a different type behind one external-linkage name.
 */
#include <meta>

#include "../../shared/models/extended_types.h"
#include "../../shared/models/message.h"
#include "../../shared/models/person.h"
#include "../../shared/models/simple_record.h"
#include "../../shared/models/timestamped_record.h"
#include "../../shared/models/uuid_pk_model.h"
import storm_orm_statements_base;
import storm_orm_statements_projection;
using storm::field_specs_for;
using storm::full_unsigned;
using storm::many_to_many;
using storm::primary;
using storm::signed_storage;
using storm::UUID;
using storm::orm::statements::BaseStatement;

// ── The models under test ───────────────────────────────────────────────────
//
// libpq's result format is a per-STATEMENT flag, so Storm asks for binary only
// when EVERY column a statement returns decodes as a plain byte reinterpretation.
// These three models cover the three verdicts: all-safe, mixed (one unsafe column
// must drag the whole statement back to text), and wholly unsafe.

// Every column is a plain byte reinterpretation under PG's binary result format:
// BOOLEAN (1 byte), BIGINT/REAL/DOUBLE PRECISION (network-order fixed width),
// TEXT and BYTEA (raw bytes).
struct BinSafeRow {
    [[= primary]] int id{};
    std::int64_t big{};
    double dbl{};
    float flt{};
    bool flag{};
    std::string label;
    std::filesystem::path file_path;
    std::vector<std::uint8_t> payload;
    std::optional<int> opt_int;
    std::optional<double> opt_dbl;
    std::optional<std::string> opt_text;
    [[= signed_storage]] std::uint64_t signed_u64{};
};

// ONE unsafe column (TIMESTAMP) among otherwise-safe ones. The format flag is per
// statement, so this whole model stays on text — and every column, safe and unsafe
// alike, must still round-trip.
struct BinMixedRow {
    [[= primary]] int id{};
    int n{};
    std::string label;
    std::chrono::system_clock::time_point stamp{};
};

// The four groups deliberately left on the text path, in one model: NUMERIC
// (full_unsigned), UUID, DATE, TIMESTAMP. Each has a PG binary wire form that is
// NOT its text form, so misclassifying any of them corrupts it silently.
struct BinUnsafeRow {
    [[= primary]] int id{};
    [[= full_unsigned]] std::uint64_t huge{};
    UUID uid{};
    std::chrono::year_month_day day{std::chrono::year{2024} / std::chrono::March / std::chrono::day{17}};
    std::chrono::system_clock::time_point stamp{};
};

// m2m pair whose scalar columns are ALL binary-safe. Relation members are filtered
// out of all_members_ before the classification runs, so an m2m owner is the one
// shape that reaches the binary path WITH a relation attached — the Q1 leg of the
// two-query eager load (#391).
struct BinTag {
    [[= primary]] int id{};
    std::string label;
};

struct BinDoc {
    [[= primary]] int id{};
    std::string title;
    std::int64_t rank{};
    [[= many_to_many<>]] std::vector<BinTag> tags;
};

namespace fields {

struct BinSafeRowT;
consteval { std::meta::define_aggregate(^^BinSafeRowT, field_specs_for(^^BinSafeRow)); }
inline constexpr BinSafeRowT BinSafeRow{};

struct BinUnsafeRowT;
consteval { std::meta::define_aggregate(^^BinUnsafeRowT, field_specs_for(^^BinUnsafeRow)); }
inline constexpr BinUnsafeRowT BinUnsafeRow{};

struct BinDocT;
consteval { std::meta::define_aggregate(^^BinDocT, field_specs_for(^^BinDoc)); }
inline constexpr BinDocT BinDoc{};

} // namespace fields

namespace storm::test::binfmt {

using PgConn = storm::db::postgresql::Connection;

template <typename T> using Base = BaseStatement<T>;

// The projection statement a values<>() call on BinUnsafeRow resolves to, named so
// its classification can be asserted (see projection_pg_binary_safe_).
template <std::meta::info... FieldInfos>
using ValuesOf =
    storm::orm::statements::ProjectionStatement<BinUnsafeRow, PgConn, storm::orm::statements::ProjectionMode::Values,
                                                FieldInfos...>;

} // namespace storm::test::binfmt

// ── Compile-time classification ─────────────────────────────────────────────
//
// The gate deciding whether a statement may ask for binary results at all.
// Asserted here rather than inferred from the round-trips: a model wrongly
// classified UNSAFE still returns correct values (just over the text path), so no
// runtime assertion can see that half of the decision.

static_assert(storm::test::binfmt::Base<BinSafeRow>::pg_binary_safe_row_,
              "bool/int/int64/double/float/text/path/blob/optional are safe");
static_assert(!storm::test::binfmt::Base<BinMixedRow>::pg_binary_safe_row_,
              "one TIMESTAMP column must disqualify the WHOLE row — the format flag is per statement");
static_assert(!storm::test::binfmt::Base<BinUnsafeRow>::pg_binary_safe_row_,
              "NUMERIC/UUID/DATE/TIMESTAMP are not byte reinterpretations");
static_assert(storm::test::binfmt::Base<Person>::pg_binary_safe_row_,
              "the in-tree default model is all-safe (this is the hot path)");
static_assert(storm::test::binfmt::Base<SimpleRecord>::pg_binary_safe_row_,
              "id/name/value is the simplest all-safe shape");
static_assert(!storm::test::binfmt::Base<ExtendedTypes>::pg_binary_safe_row_,
              "ExtendedTypes carries date/time/UUID/full_unsigned columns — it must stay on text");
static_assert(!storm::test::binfmt::Base<Message>::pg_binary_safe_row_,
              "an FK member's columns are the TARGET's key; classifying them is deferred, so FK models stay on text");
static_assert(!storm::test::binfmt::Base<TimestampedRecord>::pg_binary_safe_row_,
              "auto_create/auto_update columns are TIMESTAMP — not safe");
static_assert(!storm::test::binfmt::Base<UuidPkModel>::pg_binary_safe_row_,
              "a UUID key is a 16-byte binary value, not its 36-char text");
// An m2m owner: the relation member is filtered out of all_members_ before the fold
// runs, so the model classifies on its scalar columns alone and Q1 goes binary.
static_assert(storm::test::binfmt::Base<BinDoc>::pg_binary_safe_row_,
              "an m2m container member is not a column and must not disqualify Q1");
static_assert(storm::test::binfmt::Base<BinTag>::pg_binary_safe_row_, "the related model is all-safe too");

// ── Per-projection classification ───────────────────────────────────────────
//
// The reason distinct.cppm needs its OWN predicate instead of reusing
// pg_binary_safe_row_: projecting a safe column out of an otherwise-unsafe model
// is still safe. Asserted rather than inferred — a projection wrongly classified
// unsafe returns identical values over the text path, so no round-trip can see it.
static_assert(
    storm::test::binfmt::ValuesOf<^^BinUnsafeRow::id>::projection_pg_binary_safe_,
    "a safe column projected out of an unsafe model goes binary — the whole point of the per-projection predicate");
static_assert(!storm::test::binfmt::ValuesOf<^^BinUnsafeRow::id, ^^BinUnsafeRow::huge>::projection_pg_binary_safe_,
              "one unsafe column in the projection disqualifies it, exactly as for a whole row");
// The four unsafe groups, each projected alone. `huge` is the load-bearing one:
// full_unsigned shares std::uint64_t with signed_storage, so ONLY the annotation
// distinguishes them — a type-level predicate alone would call this safe.
static_assert(!storm::test::binfmt::ValuesOf<^^BinUnsafeRow::huge>::projection_pg_binary_safe_,
              "full_unsigned is NUMERIC(20,0), not BIGINT");
static_assert(!storm::test::binfmt::ValuesOf<^^BinUnsafeRow::uid>::projection_pg_binary_safe_, "UUID is 16 raw bytes");
static_assert(!storm::test::binfmt::ValuesOf<^^BinUnsafeRow::day>::projection_pg_binary_safe_,
              "DATE is int32 days since 2000-01-01");
static_assert(!storm::test::binfmt::ValuesOf<^^BinUnsafeRow::stamp>::projection_pg_binary_safe_,
              "TIMESTAMP is int64 microseconds");

#endif // STORM_TESTS_TEST_PG_BINARY_MODELS_H
