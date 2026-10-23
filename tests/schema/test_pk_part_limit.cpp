#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import std;
import storm_orm_utilities;
import storm_orm_statements_base;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "../../shared/models/task.h"
using storm::primary_part;
using storm::orm::statements::BaseStatement;
using storm::orm::statements::ModelPrimaryKeyPartLimit;
using storm::orm::utilities::StitchKey;

// Tests for ModelPrimaryKeyPartLimit<T> (#537): a composite primary key with more
// parts than the m2m/reverse-FK stitch key can hold is a compile-time error.
//
// The bug being closed: utilities::StitchKey — the map key the two-query eager load
// stitches Q2 rows onto their Q1 owner with — is a FIXED 32-byte inline buffer, and
// every part writes exactly one 8-byte word. Four parts fill it exactly; a fifth
// writes past the end. The only guard was assert(len_ + sizeof(word) <= CAPACITY)
// in StitchKey::append_word, which NDEBUG compiles out — so it was absent from
// exactly the Release configuration where the overrun would matter. A 5-part
// composite PK was a silent buffer overrun in a release build.
//
// DECIDED per #537 option 1: reject the model, rather than grow CAPACITY. The size
// is load-bearing — #504 measured a wider key costing ~4% on the stitch hot path,
// which is why single-PK models bypass StitchKey for a bare std::uint64_t — and
// folding surplus parts was rejected in #504 because two distinct keys colliding in
// the fold MIS-STITCHES rows onto the wrong owner (a correctness regression, not a
// recoverable map collision).

namespace {

    // ---- Accepted: 2, 3 and 4 parts — at and below the ceiling ----

    struct PkParts2 {
        [[= primary_part]] int          tenant_id{};
        [[= primary_part]] std::int64_t entry_id{};
        int                             payload{};
    };

    struct PkParts3 {
        [[= primary_part]] int          tenant_id{};
        [[= primary_part]] std::string  sku{};
        [[= primary_part]] std::int64_t entry_id{};
        int                             payload{};
    };

    // The exact ceiling: 4 x 8 == StitchKey::CAPACITY, filling the buffer with zero
    // slack. This is the boundary case the gate must ACCEPT — an off-by-one in the
    // comparison would reject it, silently narrowing what the ORM supports.
    struct PkParts4 {
        [[= primary_part]] int          tenant_id{};
        [[= primary_part]] std::string  sku{};
        [[= primary_part]] std::int64_t entry_id{};
        [[= primary_part]] int          region_id{};
        int                             payload{};
    };

    // ---- Rejected: 5 parts — one past the ceiling, the overrun this closes ----

    struct PkParts5 {
        [[= primary_part]] int          tenant_id{};
        [[= primary_part]] std::string  sku{};
        [[= primary_part]] std::int64_t entry_id{};
        [[= primary_part]] int          region_id{};
        [[= primary_part]] int          shard_id{};
        int                             payload{};
    };

    // Well past the ceiling — pins that the gate is a bound, not a check for exactly
    // one surplus part.
    struct PkParts7 {
        [[= primary_part]] int          a{};
        [[= primary_part]] int          b{};
        [[= primary_part]] int          c{};
        [[= primary_part]] int          d{};
        [[= primary_part]] int          e{};
        [[= primary_part]] std::string  f{};
        [[= primary_part]] std::int64_t g{};
        int                             payload{};
    };

} // namespace

// ============================================================================
// The derived limit — StitchKey::MAX_PARTS
// ============================================================================
//
// MAX_PARTS is derived from CAPACITY rather than written as a literal, so that the
// concept below cannot go stale if CAPACITY is ever changed. Pinned here because
// that derivation is the whole reason the gate stays correct: were MAX_PARTS
// hardcoded and CAPACITY lowered, the concept would keep admitting keys that no
// longer fit — reintroducing the overrun this issue closes.

// The load-bearing one: holds under ANY capacity, so it keeps guarding the derivation
// if CAPACITY is ever raised.
static_assert(
        StitchKey::MAX_PARTS * sizeof(std::uint64_t) == StitchKey::CAPACITY,
        "MAX_PARTS must be exactly the capacity in 8-byte words — every part writes one word"
);
// Pins today's ceiling, which the docs and the model_concepts.cppm rationale both quote as 4.
// NOTE: raising CAPACITY (the escape hatch model_concepts.cppm describes, for a real >4-part key)
// means updating this line together with the PkParts5 / PkParts7 expectations below —
// they encode "one past the ceiling" and "well past it" as literal part counts.
static_assert(StitchKey::MAX_PARTS == 4, "the documented ceiling today is 4 parts");

// ============================================================================
// Compile-time concept gate — ModelPrimaryKeyPartLimit<T>
// ============================================================================
//
// Asserted on the GATE predicate itself rather than as !requires on a public call —
// a negative-compile test must pin the concept, not a loosely constrained API that
// could reject for an unrelated reason (matching test_timestamp_pk.cpp, #511).

static_assert(ModelPrimaryKeyPartLimit<PkParts2>, "a 2-part composite PK must be accepted");
static_assert(ModelPrimaryKeyPartLimit<PkParts3>, "a 3-part composite PK must be accepted");
static_assert(ModelPrimaryKeyPartLimit<PkParts4>, "a 4-part composite PK fills StitchKey exactly and must be accepted");

static_assert(!ModelPrimaryKeyPartLimit<PkParts5>, "a 5-part composite PK overflows StitchKey and must be rejected");
static_assert(
        !ModelPrimaryKeyPartLimit<PkParts7>,
        "a 7-part composite PK must be rejected — the gate is a bound, not an off-by-one check"
);

// Single-PK models take the bare std::uint64_t stitch path and never build a
// StitchKey at all, so they are unaffected by construction. Pinned on in-tree models
// (the "existing models keep compiling" clause) rather than left implicit.
static_assert(ModelPrimaryKeyPartLimit<Person>, "Person must be unaffected");
static_assert(ModelPrimaryKeyPartLimit<Task>, "Task must be unaffected");

// ============================================================================
// The gate is REACHED — an over-wide model cannot instantiate a statement class
// ============================================================================
//
// The assertions above pin the predicate; this pins that it is actually ANDed into
// the BaseStatement constraint list. Without this, the concept could be correct and
// entirely unwired — the model would still compile, and still overrun. Pinned on
// BaseStatement, the class that owns the key, rather than on a public QuerySet call
// that might reject for its own unrelated reasons.
//
// Wrapped in a variable template so the failed-constraint case is a dependent SFINAE
// soft-fail; naming the constrained specialization directly in a namespace-scope
// `requires` is eagerly instantiated by clang-p2996 and hard-errors instead of
// yielding false. Same pattern and same spelling as test_composite_primary_key.cpp
// (#500) and test_annotation_conflicts.cpp (#492).

template <class M> constexpr bool base_statement_instantiable = requires { typename BaseStatement<M>; };

static_assert(
        !base_statement_instantiable<PkParts5>,
        "a 5-part composite PK must not instantiate BaseStatement — the concept must be on its constraint list"
);
static_assert(!base_statement_instantiable<PkParts7>, "a 7-part composite PK must not instantiate BaseStatement");
static_assert(
        base_statement_instantiable<PkParts4>,
        "a 4-part composite PK must still instantiate BaseStatement — the ceiling is inclusive"
);
// The probe must be able to report true at all — otherwise the two negatives above
// would pass even if it were broken and always false.
static_assert(base_statement_instantiable<Person>, "probe sanity: an ordinary model instantiates");

// #566 — the m2m JUNCTION path (SchemaStatement<Owner>::junction_table_sql()) adds
// no separate gate for a composite-PK RELATED side and needs none: every junction
// helper (junction_side_column_count -> BaseStatement<SideType>, inside
// build_junction_sql) instantiates BaseStatement<RelatedType> the same way a
// standalone model does, so the base_statement_instantiable<PkParts5> assertion
// above already IS the proof for that path too — reached one level down through a
// different call chain, not a different constraint. Verified manually (an owner
// with an m2m field to PkParts5, built through ninja-debug): the build fails with
// "does not satisfy 'ModelPrimaryKeyPartLimit'" at that exact instantiation point.

// ============================================================================
// Runtime behaviour — a 4-part key round-trips through StitchKey
// ============================================================================
//
// The boundary the gate admits must actually WORK, not merely compile: four parts
// must fill the buffer, stay distinguishable, and hash into a map. Asserted directly
// on StitchKey (the unit under the gate) rather than through a full m2m eager load,
// which tests/query/test_composite_fk_join.cpp already covers for 2-part owners.

namespace {

    // Build the widest key the gate admits, mixing both appender kinds — an all-int64
    // key would not exercise append_string's word-writing at the last slot, which is
    // the one adjacent to the end of the buffer.
    auto make_four_part_key(std::int64_t tenant, std::string_view sku, std::int64_t entry, std::int64_t region)
            -> StitchKey {
        StitchKey key;
        key.append_int64(tenant);
        key.append_string(sku);
        key.append_int64(entry);
        key.append_int64(region);
        return key;
    }

    TEST(PkPartLimitTest, FourPartKeyEqualsItself) {
        EXPECT_EQ(make_four_part_key(1, "sku-1", 2, 3), make_four_part_key(1, "sku-1", 2, 3));
    }

    // Each of the four slots must be load-bearing: changing ONLY that part must change
    // the key. A part written past the buffer, or folded into an earlier word, would
    // let one of these compare equal.
    TEST(PkPartLimitTest, EveryOneOfTheFourPartsDiscriminates) {
        const auto base = make_four_part_key(1, "sku-1", 2, 3);
        EXPECT_NE(base, make_four_part_key(9, "sku-1", 2, 3)) << "part 1 must discriminate";
        EXPECT_NE(base, make_four_part_key(1, "sku-9", 2, 3)) << "part 2 must discriminate";
        EXPECT_NE(base, make_four_part_key(1, "sku-1", 9, 3)) << "part 3 must discriminate";
        EXPECT_NE(base, make_four_part_key(1, "sku-1", 2, 9)) << "part 4 must discriminate";
    }

    // The LAST slot is the one at the buffer's edge — the part a 5th append would
    // overrun past. Pinned separately as a map round-trip: the stitch reaches the key
    // only through hash + operator==, so both must see the final word.
    TEST(PkPartLimitTest, FourPartKeysStitchDistinctlyInAMap) {
        std::unordered_map<StitchKey, int> map;
        map[make_four_part_key(1, "sku-1", 2, 3)] = 10;
        map[make_four_part_key(1, "sku-1", 2, 4)] = 20; // differs in the final slot only

        ASSERT_EQ(map.size(), 2U) << "keys differing only in the last part must not collide into one entry";
        EXPECT_EQ(map.at(make_four_part_key(1, "sku-1", 2, 3)), 10);
        EXPECT_EQ(map.at(make_four_part_key(1, "sku-1", 2, 4)), 20);
    }

} // namespace
