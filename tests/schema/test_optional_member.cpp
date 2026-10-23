#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

// NOLINTBEGIN(misc-const-correctness)

import storm_db_postgresql;
import storm_db_sqlite;
import storm_model;
import storm_orm_indexes;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

#include "../../shared/models/cascade_child.h" // NOSONAR cpp:S954
#include "../../shared/models/extended_types.h"
#include "../../shared/models/person.h"
#include "../../shared/models/set_null_child.h"
using storm::primary;
using storm::meta::is_optional_member;
using storm::meta::is_optional_type;

// ── #509: is_optional_member is the single optional-detection predicate ──────
// The structural "is this member a std::optional<T>?" check was hand-rolled at
// five sites (two in field_attr.cppm, three in statements/base.cppm), each with
// a slightly different spelling of the same reflection query. This pins the
// extracted helper's behaviour so the copies cannot silently reappear or drift.
//
// It is a MEMBER predicate (takes the data member, not its type) — that is the
// shape all five call sites needed, and it keeps the dealias/type_of step inside
// the helper where it cannot be forgotten.

namespace {

    // Reflects the member of T with the given identifier. Name-based rather than
    // positional: a member inserted into a shared model would silently shift an
    // index onto a different field and turn these assertions into a test of
    // something else, whereas a stale name fails to compile.
    template <typename T> consteval auto member_named(std::string_view name) -> std::meta::info {
        for (auto m : std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())) {
            if (std::meta::has_identifier(m) && std::meta::identifier_of(m) == name) {
                return m;
            }
        }
        // Unreachable for the names used below; a typo is a compile error here.
        std::unreachable();
    }

} // namespace

// Optional members are detected, over both a scalar and a class element type.
static_assert(is_optional_member(member_named<Person>("score")));    // optional<int>
static_assert(is_optional_member(member_named<Person>("nickname"))); // optional<string>

// Non-optional members are rejected — proves the predicate actually gates
// rather than being vacuously true. Covers the PK, a string, a scalar and a
// container.
static_assert(!is_optional_member(member_named<Person>("id")));
static_assert(!is_optional_member(member_named<Person>("name")));
static_assert(!is_optional_member(member_named<Person>("age")));
static_assert(!is_optional_member(member_named<Person>("avatar")));

// A nullable FK (std::optional<Person>) is detected: this is the shape
// ModelFkPoliciesValid tests for when it admits fk<RefAction::SetNull> (#431).
static_assert(is_optional_member(member_named<SetNullChild>("owner")));
// The non-nullable FK of the sibling CASCADE model is not.
static_assert(!is_optional_member(member_named<CascadeChild>("owner")));

// Optionals of non-scalar element types are detected too — the predicate keys
// on the std::optional template itself, never on the element type.
static_assert(is_optional_member(member_named<ExtendedTypes>("opt_color")));     // enum
static_assert(is_optional_member(member_named<ExtendedTypes>("opt_timestamp"))); // time_point
static_assert(is_optional_member(member_named<ExtendedTypes>("opt_path")));      // path
static_assert(!is_optional_member(member_named<ExtendedTypes>("uuid_field")));

// The type-level core is the same predicate without the member step — it is what
// the one type-level caller (unwrap_optional_type, for FK targets) shares.
static_assert(is_optional_type(^^std::optional<int>));
static_assert(is_optional_type(^^std::optional<Person>));
static_assert(!is_optional_type(^^int));
static_assert(!is_optional_type(^^Person));
// A non-optional specialization is rejected: the guard is not "has template
// arguments", it is "is std::optional".
static_assert(!is_optional_type(^^std::vector<int>));

// is_optional_type takes an ALREADY-DEALIASED type — it does not dealias
// internally, and an alias therefore does NOT match. Pinned as executable
// behaviour rather than prose because it is the one way to misuse the core:
// both in-tree callers dealias at their boundary (is_optional_member via
// type_of, unwrap_optional_type into its local `t`), which is what makes the
// precondition safe to keep.
using OptAlias = std::optional<int>;
static_assert(is_optional_type(std::meta::dealias(^^OptAlias)));
static_assert(!is_optional_type(^^OptAlias));
// The member-level wrapper owns the dealias step, so an aliased MEMBER type is
// detected — the property the four member-level call sites rely on.
struct AliasedOptionalMember {
    [[= primary]] int id{};
    OptAlias          score;
};
static_assert(is_optional_member(member_named<AliasedOptionalMember>("score")));

// The callers that UNWRAP rather than merely detect keep looking through the
// optional: is_text_member and is_unsigned64_member both share the detection
// half but must still reach the element type behind it.
static_assert(storm::meta::is_text_member(member_named<Person>("nickname"))); // optional<string> is text
static_assert(!storm::meta::is_text_member(member_named<Person>("score")));   // optional<int> is not
static_assert(storm::meta::is_unsigned64_member(member_named<ExtendedTypes>("big_unsigned")));
static_assert(!storm::meta::is_unsigned64_member(member_named<ExtendedTypes>("opt_int64")));

// NOLINTEND(misc-const-correctness)

TEST(OptionalMemberPredicate, CompileTimeOnly) {
    // The static_asserts above are the real test; this keeps the TU a runnable
    // GTest target so the suite reports it.
    SUCCEED();
}
