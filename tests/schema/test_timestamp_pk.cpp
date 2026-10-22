#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

import storm;
import storm_orm_statements_update_grammar; // UpdateGrammar / UpsertGrammar — PK gate on the auto_update SET tail (#511); not re-exported by `storm`
import storm_orm_statements_upsert_grammar;
import std;

#include "../../shared/models/person.h" // NOSONAR cpp:S954
#include "../../shared/models/task.h"
#include "../../shared/models/timestamped_record.h"
#include "test_fixture.h"
using storm::auto_update;
using storm::field_specs_for;
using storm::primary;
using storm::primary_autoincrement;
using storm::primary_part;
using storm::QuerySet;
using storm::orm::statements::ModelTimestampPkValid;

// Tests for ModelTimestampPkValid<T> (#511): auto_create / auto_update on a
// primary-key member is a compile-time error.
//
// The bug being closed: UpdateGrammar::is_unlisted_auto_update (and its
// UpsertGrammar twin) appended ANY auto_update field to the SET clause without
// excluding the primary key, so a PK carrying auto_update was stamped now() by
// qs.where(...).update<...>() — the statement rewrote the key it matched on,
// the same class of bug #501 fixed for the explicit SET targets.
//
// DECIDED 2026-07-29: reject the model rather than silently ignore the
// annotation, keeping one frame with ModelPrimaryKeyValid (#500) — a wrong PK
// annotation is a NAMED compile-time error. The predicate gate is applied as
// well, so the SET clause cannot emit a key column even if this concept is
// later loosened; see the predicate-gate section below.
//
// Note on reachability: a SINGLE-PK time_point model is independently rejected
// by PrimaryKeyType<T> (#505), but only incidentally — time_point is a class
// type that falls off the end of that concept's integral/UUID whitelist, is not
// named in it, and was never pinned by a test. Composite primary_part members
// are exempt from PrimaryKeyType entirely (PrimaryKeyType, model_concepts.cppm), so the composite
// models below are the genuinely live path.

namespace {

    // ---- Rejected: auto_update / auto_create on a single primary key ----

    struct TsPkAutoUpdate {
        [[= primary]][[= auto_update]] std::chrono::system_clock::time_point id{};
        int                                                                  payload{};
    };

    struct TsPkAutoCreate {
        [[= primary]][[= storm::auto_create]] std::chrono::system_clock::time_point id{};
        int                                                                         payload{};
    };

    // primary_autoincrement is the third PK spelling; is_primary_member covers all
    // three by construction, but pinning it completes the 3-spelling x 2-annotation
    // matrix so no spelling can regress independently.
    struct TsPkAutoincrementAutoUpdate {
        [[= primary_autoincrement]][[= auto_update]] std::chrono::system_clock::time_point id{};
        int                                                                                payload{};
    };

    // ---- Rejected: auto_update / auto_create on a COMPOSITE key part ----
    // The live path: PrimaryKeyType<T> exempts primary_part members, so nothing
    // rejected this shape before #511.

    struct TsCompositePartAutoUpdate {
        [[= primary_part]] int                                                    tenant_id{};
        [[= primary_part]][[= auto_update]] std::chrono::system_clock::time_point recorded_at{};
        int                                                                       payload{};
    };

    struct TsCompositePartAutoCreate {
        [[= primary_part]] int                                                           tenant_id{};
        [[= primary_part]][[= storm::auto_create]] std::chrono::system_clock::time_point recorded_at{};
        int                                                                              payload{};
    };

    // ---- Accepted: the normal shape — timestamps on NON-key members ----

    struct TsNonPkTimestamps {
        [[= primary_autoincrement]] int                                id{};
        [[= storm::auto_create]] std::chrono::system_clock::time_point created_at{};
        [[= auto_update]] std::chrono::system_clock::time_point        updated_at{};
        int                                                            payload{};
    };

    // ---- Accepted: a composite key whose parts carry no timestamp annotation ----

    struct TsCompositeClean {
        [[= primary_part]] int                                  tenant_id{};
        [[= primary_part]] std::int64_t                         entry_id{};
        [[= auto_update]] std::chrono::system_clock::time_point updated_at{};
        int                                                     payload{};
    };

} // namespace

// fields:: selector proxies (#518). Global scope, not nested in the anonymous
// namespace: a nested `namespace fields` would be ambiguous with the global one.
namespace fields {

    struct TsCompositeCleanT;
    consteval {
        std::meta::define_aggregate(^^TsCompositeCleanT, field_specs_for(^^TsCompositeClean));
    }
    inline constexpr TsCompositeCleanT TsCompositeClean{};

} // namespace fields

// ============================================================================
// Compile-time concept gate — ModelTimestampPkValid<T>
// ============================================================================
//
// Asserted on the GATE predicate itself rather than as !requires on a public
// call — a negative-compile test must pin the concept, not a loosely
// constrained API that could reject for an unrelated reason.

static_assert(!ModelTimestampPkValid<TsPkAutoUpdate>, "auto_update on a single primary key must be rejected");
static_assert(!ModelTimestampPkValid<TsPkAutoCreate>, "auto_create on a single primary key must be rejected");
static_assert(
        !ModelTimestampPkValid<TsPkAutoincrementAutoUpdate>,
        "auto_update on a primary_autoincrement key must be rejected"
);
static_assert(
        !ModelTimestampPkValid<TsCompositePartAutoUpdate>, "auto_update on a composite primary_part must be rejected"
);
static_assert(
        !ModelTimestampPkValid<TsCompositePartAutoCreate>, "auto_create on a composite primary_part must be rejected"
);

static_assert(ModelTimestampPkValid<TsNonPkTimestamps>, "auto_create/auto_update on non-key members must be accepted");
static_assert(
        ModelTimestampPkValid<TsCompositeClean>,
        "a composite key with no timestamp annotation on its parts must be accepted"
);

// Every in-tree model must keep satisfying the new concept — this is the
// "models without a timestamp PK are unaffected" clause of the DoD.
static_assert(ModelTimestampPkValid<Person>, "Person must be unaffected");
static_assert(ModelTimestampPkValid<Task>, "Task must be unaffected");

// ============================================================================
// Predicate gate — is_unlisted_auto_update must exclude PK members
// ============================================================================
//
// Defence in depth, behind the concept above: no primary-key member may enter
// the auto_update tail of a SET clause. Pinned on TsCompositeClean, a model the
// concept ACCEPTS, so these cannot fail merely because the model was rejected.
// See the scope note on the second assertion pair — the concept makes the gate
// itself unreachable, so only its observable consequence is asserted here.

namespace {
    using CleanGrammar   = storm::orm::statements::UpdateGrammar<TsCompositeClean>;
    using UpsertGrammarT = storm::orm::statements::UpsertGrammar<TsCompositeClean>;

    // The non-key auto_update member is still picked up (the feature still works).
    static_assert(
            CleanGrammar::template is_unlisted_auto_update<^^TsCompositeClean::payload>(^^TsCompositeClean::updated_at),
            "a non-key auto_update member must still enter the SET tail"
    );
    static_assert(
            UpsertGrammarT::template is_unlisted_auto_update<^^TsCompositeClean::payload>(
                    ^^TsCompositeClean::updated_at
            ),
            "a non-key auto_update member must still enter the upsert DO UPDATE tail"
    );

    // A PK part is refused as a SET-tail candidate.
    //
    // HONEST SCOPE: these two assertions do NOT discriminate the !is_pk_member gate —
    // tenant_id/entry_id carry no auto_update, so is_auto_update_field() already makes the
    // conjunction false and they pass with or without the gate (verified by reverting it).
    // They pin the observable contract ("no key part in the SET tail"), not the mechanism.
    //
    // The gate is NOT independently testable, and that is a consequence of the design
    // rather than a gap in the test: the only model that would discriminate it carries
    // auto_update ON a key part, and UpdateGrammar<T> instantiates BaseStatement<T>, which
    // now requires ModelTimestampPkValid<T> — so that model fails to instantiate the
    // grammar at all ("constraints not satisfied for class template 'BaseStatement'",
    // verified). The concept makes the gate unreachable; the gate is retained as defence
    // in depth for a future in which the concept is loosened, and would become testable
    // again at that point.
    static_assert(
            !CleanGrammar::template is_unlisted_auto_update<^^TsCompositeClean::payload>(^^TsCompositeClean::tenant_id),
            "a PK part must never enter the UPDATE SET tail"
    );
    static_assert(
            !UpsertGrammarT::template is_unlisted_auto_update<^^TsCompositeClean::payload>(
                    ^^TsCompositeClean::entry_id
            ),
            "a PK part must never enter the upsert DO UPDATE tail"
    );
} // namespace

// ============================================================================
// Runtime behaviour — the emitted SET clause
// ============================================================================

namespace {

    using ConnType = storm::db::sqlite::Connection;

    class TimestampPkTest : public StormTestFixture<TsCompositeClean, ConnType> {};

    // A conditional UPDATE on a model with a composite key and a non-key
    // auto_update column must SET the payload and the timestamp, and must not
    // name either key part in the SET clause.
    //
    // Deliberately a SQLite-only TEST_F rather than the usual TYPED_TEST over
    // DatabaseTypes: this asserts only to_sql() text, and the SET clause comes from a
    // consteval grammar with no dialect branch, so the PG run would assert a byte-identical
    // string. Same precedent as tests/query/test_where.cpp's non-typed fixture.
    TEST_F(TimestampPkTest, ConditionalUpdateSetClauseExcludesKeyParts) {
        QuerySet<TsCompositeClean, ConnType> qs;
        const auto                           sql = qs.where(fields::TsCompositeClean.tenant_id == 1)
                                 .update<fields::TsCompositeClean.payload>(TsCompositeClean{.payload = 7})
                                 .to_sql();
        ASSERT_TRUE(sql.has_value());

        const std::string& text  = sql.value();
        const auto         start = text.find(" SET ");
        ASSERT_NE(start, std::string::npos) << text;
        const std::string set_clause = text.substr(start, text.find(" WHERE ") - start);

        EXPECT_TRUE(set_clause.contains("payload=")) << "explicit SET target must be present: " << text;
        EXPECT_TRUE(set_clause.contains("updated_at=")) << "non-key auto_update must be stamped: " << text;
        EXPECT_FALSE(set_clause.contains("tenant_id=")) << "key part must not be in SET: " << text;
        EXPECT_FALSE(set_clause.contains("entry_id=")) << "key part must not be in SET: " << text;
    }

    // The DoD's "SQL byte-identical" clause, pinned on the emitted text rather than
    // inferred from a green suite. TimestampedRecord is the in-tree model that actually
    // exercises the auto_update tail (single PK, auto_update on a non-key member), so it is
    // the exact shape the new !is_pk_member conjunct could have perturbed.
    //
    // to_sql() INLINES bound values rather than emitting placeholders, and the auto_update
    // stamp is a wall-clock now(), so the timestamp literal cannot be spelled in the
    // expectation. The prefix and suffix around it are asserted instead — together they pin
    // the whole string except the 19 timestamp characters, and fail if the tail is dropped,
    // reordered, or gains a key column.
    class TimestampedRecordSqlTest : public StormTestFixture<TimestampedRecord, ConnType> {};

    TEST_F(TimestampedRecordSqlTest, ConditionalUpdateSqlUnchanged) {
        QuerySet<TimestampedRecord, ConnType> qs;
        const auto                            sql = qs.where(fields::TimestampedRecord.id == 1)
                                 .update<fields::TimestampedRecord.name>(TimestampedRecord{.name = "renamed"})
                                 .to_sql();
        ASSERT_TRUE(sql.has_value());

        const std::string& text = sql.value();
        EXPECT_TRUE(text.starts_with("UPDATE TimestampedRecord SET name='renamed', updated_at='")) << text;
        EXPECT_TRUE(text.ends_with("' WHERE id = 1")) << text;
        // The key column appears ONLY in the WHERE clause, never in SET (#511).
        EXPECT_EQ(text.find("id="), std::string::npos) << "no key column in SET: " << text;
        // created_at is auto_create — INSERT only, never in an UPDATE SET clause (#209).
        EXPECT_EQ(text.find("created_at"), std::string::npos) << text;
    }

} // namespace
