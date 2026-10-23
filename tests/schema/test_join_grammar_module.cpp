#include <gtest/gtest.h>
#include <meta>

import std;
import storm_orm_field_attr;
import storm_orm_statements_base;
import storm_orm_statements_join_grammar;
import storm_orm_utilities;
// Deliberately NOT importing storm_orm_statements_join or storm: this TU proves the shared
// consteval Q2 SQL-text helpers live in their own grammar module, reachable without the join
// statements or a connection type (#671).

namespace {
    using namespace storm::orm::statements;
    using storm::orm::utilities::ConstexprString;

    struct JgOwner {
        [[= storm::meta::primary]] int id{};
        std::string                    name;
    };

    struct JgPair {
        [[= storm::meta::primary_part]] int a{};
        [[= storm::meta::primary_part]] int b{};
        int                                 qty{};
    };

    // Composite key whose first part is itself an FK — the shape where the Auto and Through
    // junction naming rules diverge (#536).
    struct JgFkPart {
        [[= storm::meta::primary_part]][[= storm::meta::fk<>]] JgOwner owner;
        [[= storm::meta::primary_part]] int                            sku{};
    };

    // Related model with a single-column FK and a composite FK (N columns, #504).
    struct JgChild {
        [[= storm::meta::primary]] int  id{};
        [[= storm::meta::fk<>]] JgOwner parent;
        [[= storm::meta::fk<>]] JgPair  pair;
        int                             qty{};
    };

    using OwnerBase  = BaseStatement<JgOwner>;
    using PairBase   = BaseStatement<JgPair>;
    using FkPartBase = BaseStatement<JgFkPart>;
    using ChildBase  = BaseStatement<JgChild>;

    constexpr std::size_t buffer_size = 256;
    using Buffer                      = ConstexprString<buffer_size>;

    constexpr auto pair_target_pk  = ChildBase::find_fk_primary_key_members<JgPair>();
    constexpr auto owner_target_pk = ChildBase::find_fk_primary_key_members<JgOwner>();
} // namespace

TEST(JoinGrammarModule, PkPartListNamesEveryKeyColumn) {
    constexpr auto pair = []() consteval {
        Buffer out;
        append_pk_part_list<PairBase>(out);
        return out;
    }();
    constexpr auto fk_part = []() consteval {
        Buffer out;
        append_pk_part_list<FkPartBase>(out);
        return out;
    }();
    EXPECT_EQ(pair.view(), "a, b");
    EXPECT_EQ(fk_part.view(), "owner_id, sku");
    EXPECT_EQ(pk_part_list_size<PairBase>(), pair.view().size());
    EXPECT_EQ(pk_part_list_size<FkPartBase>(), fk_part.view().size());
}

TEST(JoinGrammarModule, InSubqueryOpenWidensOnlyWhenAsked) {
    constexpr auto single = []() consteval {
        Buffer out;
        append_in_subquery_open<OwnerBase, false>(out);
        return out;
    }();
    constexpr auto widened = []() consteval {
        Buffer out;
        append_in_subquery_open<PairBase, true>(out);
        return out;
    }();
    constexpr auto widened_fk_part = []() consteval {
        Buffer out;
        append_in_subquery_open<FkPartBase, true>(out);
        return out;
    }();
    EXPECT_EQ(single.view(), " IN (SELECT id FROM JgOwner");
    EXPECT_EQ(widened.view(), " IN (SELECT a, b FROM JgPair");
    EXPECT_EQ(widened_fk_part.view(), " IN (SELECT owner_id, sku FROM JgFkPart");
    EXPECT_EQ((in_subquery_open_size<OwnerBase, false>()), single.view().size());
    EXPECT_EQ((in_subquery_open_size<PairBase, true>()), widened.view().size());
}

TEST(JoinGrammarModule, RelationColumnsExpandCompositeFkParts) {
    constexpr auto columns = []() consteval {
        Buffer out;
        append_relation_columns<ChildBase>(out, "t3.");
        return out;
    }();
    EXPECT_EQ(columns.view(), ", t3.id, t3.parent_id, t3.pair_a, t3.pair_b, t3.qty");
    EXPECT_EQ(relation_columns_size<ChildBase>("t3."), columns.view().size());
}

TEST(JoinGrammarModule, JunctionSideColumnsFollowTheirNamingRule) {
    constexpr auto single = []() consteval {
        Buffer out;
        append_junction_side_col_list<OwnerBase, JunctionNaming::Auto>(out, "JgOwner", "t2.");
        return out;
    }();
    constexpr auto auto_junction = []() consteval {
        Buffer out;
        append_junction_side_col_list<FkPartBase, JunctionNaming::Auto>(out, "JgFkPart", "t2.");
        return out;
    }();
    constexpr auto through = []() consteval {
        Buffer out;
        append_junction_side_col_list<FkPartBase, JunctionNaming::Through>(out, "line", "t2.");
        return out;
    }();
    EXPECT_EQ(single.view(), "t2.JgOwner_id");
    EXPECT_EQ(auto_junction.view(), "t2.JgFkPart_owner_id, t2.JgFkPart_sku");
    EXPECT_EQ(through.view(), "t2.line_owner, t2.line_sku");
}

// The two naming rules agree wherever no key part is an FK: a plain composite side spells the
// same columns either way.
TEST(JoinGrammarModule, JunctionNamingRulesAgreeOnPlainCompositeParts) {
    constexpr auto auto_junction = []() consteval {
        Buffer out;
        append_junction_side_col_list<PairBase, JunctionNaming::Auto>(out, "x", "t2.");
        return out;
    }();
    constexpr auto through = []() consteval {
        Buffer out;
        append_junction_side_col_list<PairBase, JunctionNaming::Through>(out, "x", "t2.");
        return out;
    }();
    EXPECT_EQ(auto_junction.view(), "t2.x_a, t2.x_b");
    EXPECT_EQ(through.view(), auto_junction.view());
}

TEST(JoinGrammarModule, OwnerFkColumnListAliasesEachPart) {
    constexpr auto single = []() consteval {
        Buffer out;
        append_owner_fk_col_list(out, ^^JgChild::parent, owner_target_pk);
        return out;
    }();
    constexpr auto composite = []() consteval {
        Buffer out;
        append_owner_fk_col_list(out, ^^JgChild::pair, pair_target_pk);
        return out;
    }();
    EXPECT_EQ(single.view(), "t2.parent_id");
    EXPECT_EQ(composite.view(), "t2.pair_a, t2.pair_b");
    EXPECT_EQ(owner_fk_col_list_size(^^JgChild::parent, owner_target_pk), single.view().size());
    EXPECT_EQ(owner_fk_col_list_size(^^JgChild::pair, pair_target_pk), composite.view().size());
}

TEST(JoinGrammarModule, Q2SelectHeadPrefixesOwnerKeyToRelationColumns) {
    constexpr auto head = []() consteval {
        Buffer out;
        append_q2_select_head_multi<ChildBase>(out, ^^JgChild::pair, pair_target_pk, "t2.");
        return out;
    }();
    // N=1 collapses to the single "t2.<fk>_id" owner column (the pre-#504 form).
    constexpr auto single_head = []() consteval {
        Buffer out;
        append_q2_select_head_multi<ChildBase>(out, ^^JgChild::parent, owner_target_pk, "t2.");
        return out;
    }();
    EXPECT_EQ(head.view(), "SELECT t2.pair_a, t2.pair_b, t2.id, t2.parent_id, t2.pair_a, t2.pair_b, t2.qty");
    EXPECT_EQ(single_head.view(), "SELECT t2.parent_id, t2.id, t2.parent_id, t2.pair_a, t2.pair_b, t2.qty");
    EXPECT_EQ(
            q2_select_head_multi_owner_size(^^JgChild::pair, pair_target_pk) + relation_columns_size<ChildBase>("t2."),
            head.view().size()
    );
}
