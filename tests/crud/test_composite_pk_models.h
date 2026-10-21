#ifndef STORM_TESTS_TEST_COMPOSITE_PK_MODELS_H
#define STORM_TESTS_TEST_COMPOSITE_PK_MODELS_H

/**
 * @file test_composite_pk_models.h
 * @brief Composite-PK model fixtures shared by the #501 UPDATE/DELETE tests.
 *
 * IMPORTANT: like the shared model headers, this one names storm:: types and must only
 * be included AFTER `import storm;` (NOSONAR cpp:S954 at the include site).
 *
 * Split across two test TUs: test_composite_pk_sql.cpp (compile-time gates and
 * SQL text) and test_composite_pk_crud.cpp (live execution on both backends).
 */
#include "../../shared/models/person.h" // NOSONAR cpp:S954 — Person-typed FK members below
#include <meta>
using storm::field_specs_for;
using storm::many_to_many;
using storm::primary;
using storm::primary_autoincrement;
using storm::primary_part;

// Canonical 2-part composite key, both parts int.
struct OrderLine {
    [[= primary_part]] int order_id{};
    [[= primary_part]] int product_id{};
    int quantity{};
    std::string note;
};

// Mixed-type parts (int + std::string): proves the bind path dispatches per
// part type rather than assuming an integer key, and that a TEXT part is
// compared correctly in the WHERE clause.
struct Inventory {
    [[= primary_part]] int warehouse{};
    [[= primary_part]] std::string sku;
    int on_hand{};
};

// Three parts, mixed widths — the AND-join and the row-value list must not be
// arity-limited, and the consteval sizers must stay exact at N=3.
struct Ledger {
    [[= primary_part]] int region{};
    [[= primary_part]] std::string account;
    [[= primary_part]] std::int64_t period{};
    double balance{};
};

// Single-PK control. Every SQL assertion on this model must be byte-identical
// to what Storm emitted before composite support.
struct Widget {
    [[= primary]] int id{};
    std::string name;
    int weight{};
};

// The canonical association-table shape: a composite key whose parts are FKs.
// Exercises the `is_fk_field` branch of bind_one_pk_part (an FK part binds the
// REFERENCED row's key, not the whole object) and the "<name>_id" column naming
// in the WHERE clause — emitting the bare member would name a column that does
// not exist. `Person` comes from shared/models/person.h, included above.
struct StockEntry {
    [[= primary_part]][[= storm::fk<>]] Person warehouse;
    [[= primary_part]] int sku{};
    int qty{};
};

// #504 — Base⟷Owner reference cycle between OrderLineWithShipments and
// Shipment, broken the same way tests/query/test_reverse_fk_models.h breaks
// RfPerson⟷RfTask: forward-declare the owner so the container member below
// tolerates an incomplete value_type, then define it fully afterward (by
// which point OrderLineWithShipments is complete, so Shipment::line can be a
// by-value member).
struct Shipment; // forward declaration breaks the Base⟷Owner cycle

// #504 — reverse-FK destination with a composite-PK owner: "all OrderLines,
// each with the Shipments that reference them". A separate struct from
// OrderLine (not added to it directly) so existing OrderLine-shape assertions
// in the #501/#502 test suites stay untouched.
struct OrderLineWithShipments {
    [[= primary_part]] int order_id{};
    [[= primary_part]] int product_id{};
    int quantity{};
    std::string note;
    [[= storm::reverse_fk<^^Shipment>]] std::vector<Shipment> shipments;
};

// #504 — FK targeting a composite PK. Shipment references
// OrderLineWithShipments's 2-part key (order_id, product_id) via the SAME
// fk<> annotation as any single-column FK — the composite-ness lives entirely
// in the target's own declaration. Doubles as the reverse-FK owner for
// OrderLineWithShipments::shipments above: reverse_fk<^^Shipment> requires
// Shipment to carry exactly one fk<> member whose type is
// OrderLineWithShipments, which is why this targets that type and not the
// plain OrderLine (which nothing references back to).
struct Shipment {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] OrderLineWithShipments line;
    std::string carrier;
};

// #504 — many-to-many with a composite-PK side. LedgerTag is the plain
// single-PK related model; LedgerWithTags is Ledger's 3-part composite key
// (region, account, period) PLUS an m2m container, proving m2m eager-load
// stitching works when the OWNER side has a composite key.
struct LedgerTag {
    [[= primary_autoincrement]] int id{};
    std::string label;
};
struct LedgerWithTags {
    [[= primary_part]] int region{};
    [[= primary_part]] std::string account;
    [[= primary_part]] std::int64_t period{};
    double balance{};
    [[= many_to_many<>]] std::vector<LedgerTag> tags;
};

// #504 Task 7 fix — FK target with a 3-part composite PK. Ledger already
// exists (above) as a plain composite-PK model but was never used as an FK
// target anywhere, so no test exercised the JOIN ON-clause AND-chain past
// N=2 parts. LedgerEntryRef is a single such FK (3 parts, deficit 6 bytes
// under the pre-fix sizer) — kept as the minimal round-trip/JOIN-execution
// fixture. It is NOT wide enough on its own to overflow the stacked
// SMALL_BUFFER slack (calculate_complete_sql_size / calculate_join_sql_size /
// calculate_select_fields_size each add their own 10-byte pad, so ~30 bytes of
// slack absorb a single 6-byte deficit) — see LedgerEntryRefWide below for the
// fixture that actually forces a measurable, non-slack-absorbed deficit.
struct LedgerEntryRef {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] Ledger ledger;
    std::string note;
};

// #504 Task 7 fix — SIX composite-FK fields to Ledger (3 parts each), mirroring
// the WideJoin idiom (9 single-column FKs) but for composite targets. Each FK's
// ON-clause body has 2 "after-first" parts, so each contributes a 6-byte
// deficit under the pre-fix sizer (total 36 bytes) — enough to exceed the ~30
// bytes of stacked SMALL_BUFFER slack across calculate_complete_sql_size /
// calculate_join_sql_size / calculate_select_fields_size, where a single
// 3-part FK (LedgerEntryRef above) does not. This is the fixture that actually
// forces ConstexprString::append to silently truncate under the pre-fix sizer.
struct LedgerEntryRefWide {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] Ledger ledger1;
    [[= storm::fk<>]] Ledger ledger2;
    [[= storm::fk<>]] Ledger ledger3;
    [[= storm::fk<>]] Ledger ledger4;
    [[= storm::fk<>]] Ledger ledger5;
    [[= storm::fk<>]] Ledger ledger6;
};

// #504 review fix — a NULLABLE composite-FK member. Proves the composite-FK
// DDL column emission gates NOT NULL on the FK MEMBER's own optionality
// (std::optional<OrderLineWithShipments>), not on the target's PK-part type
// (which is never optional — a PK can't be nullable), and that it emits the
// suffix exactly once, not doubled.
struct OptionalShipment {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] std::optional<OrderLineWithShipments> line;
    std::string carrier;
};

// #504 review fix — a composite-FK member declared BEFORE an optional
// single-column-FK member in the same struct. Exercises extract_column_fast's
// col_idx threading: `composite` consumes 2 SQL columns
// (composite_order_id, composite_product_id), so `single` (an optional FK
// targeting single-PK Person) must read from col_idx 3, not from its member
// index (2). Before the fix, extract_optional_fk_column read column 2
// (composite_product_id) instead.
struct MixedFkOrder {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] OrderLineWithShipments composite;
    [[= storm::fk<>]] std::optional<Person> single;
};

// ── #504 Task 9: junction-table DDL when EITHER m2m side has a composite PK ──
//
// LedgerWithTags (above) covers the composite-OWNER direction (3-part owner PK,
// single-PK related). The two fixtures below cover the other two directions, so
// the "<side>_<part>" widening is exercised on both sides independently rather
// than only on the owner side:
//
//   TagRegistry     — single-PK OWNER, composite (2-part) RELATED.
//   ShelfAssignment — composite (2-part) OWNER *and* composite (3-part) RELATED,
//                     the both-sides case, where the junction carries 5 columns.

// Composite (2-part) RELATED model for the single-PK-owner direction. Both parts
// are int, so the junction's related-side columns are the simple homogeneous case.
struct CatalogEntry {
    [[= primary_part]] int catalog_id{};
    [[= primary_part]] int entry_no{};
    std::string title;
};

// Single-PK OWNER pointing at a composite-PK RELATED model: the junction must
// emit ONE owner column (tagregistry_id, the unchanged single-PK spelling) plus
// TWO related columns (CatalogEntry_catalog_id, CatalogEntry_entry_no).
struct TagRegistry {
    [[= primary_autoincrement]] int id{};
    std::string label;
    [[= many_to_many<>]] std::vector<CatalogEntry> entries;
};

// Composite (3-part) RELATED model with MIXED part types (int + string +
// int64_t) — proves the junction's related-side column TYPES are derived per
// part from the target's own PK-part types, not assumed integral. Paired with a
// composite owner below for the both-sides case.
struct StorageBin {
    [[= primary_part]] int aisle{};
    [[= primary_part]] std::string bin_code;
    [[= primary_part]] std::int64_t revision{};
    double capacity{};
};

// Composite owner (2-part) AND composite related (3-part): the junction carries
// 2 + 3 = 5 columns, all five in the PRIMARY KEY, with a 2-column FK clause to
// the owner and a 3-column FK clause to the related side. This is the widest
// junction in the tree and the fixture that would expose a byte-budget
// undercount in build_junction_sql's ConstexprString sizing (the pre-#504 fixed
// "5x name length + 256" budget has no term that scales with PK part count or
// part identifier length).
// The SECOND m2m field makes this the only composite-PK owner with more than
// one relation, which is what #392's multi-relation join<fields::T.a, fields::T.b>()
// needs to be exercised over a composite key: relation Is takes junction alias
// 2+2*Is and related alias 3+2*Is, and with a composite owner EACH relation's
// ON clause now AND-joins one equality per owner PK part. A single-relation
// composite join cannot catch an aliasing error in that arithmetic, since
// relation 0's aliases (t2/t3) are the same ones the single-relation path uses.
// LedgerTag is reused as the second, deliberately single-PK, related side so
// the two relations differ in related-side arity as well (3-part StorageBin vs
// 1-part LedgerTag) — a per-relation column count leaking across relations
// would misalign the second relation's extraction.
struct ShelfAssignment {
    [[= primary_part]] int warehouse_no{};
    [[= primary_part]] std::string shelf_code;
    int priority{};
    [[= many_to_many<>]] std::vector<StorageBin> bins;
    [[= many_to_many<>]] std::vector<LedgerTag> tags;
};

// ── #536: many_to_many_through<> when either side has a composite PK ─────────
//
// The auto-junction fixtures above (LedgerWithTags / TagRegistry /
// ShelfAssignment) cover Storm's SYNTHETIC junction table, whose columns Storm
// names itself. A through model is a different thing: a REAL user-declared
// model whose junction columns are ordinary composite-FK columns, named by the
// FK rule ("<member>_<part>", bare part identifier) rather than the junction
// rule ("<side>_<part>", part routed through append_column_name so an FK part
// gains "_id"). The two rules agree for a single-column PK and for a composite
// key whose parts are plain columns; they DIVERGE the moment a PK part is
// itself an FK. Nothing in-tree exercised that combination, which is how the
// divergence shipped.
//
// Three directions, mirroring the auto-junction trio, plus the FK-part case
// that is the actual point of divergence.

// Plain single-PK related side, reused by the composite-OWNER direction.
struct ThroughTopic {
    [[= primary_autoincrement]] int id{};
    std::string label;
};

struct RegionEntryLink; // through model, defined below (breaks the cycle)

// Composite (2-part) OWNER through an explicit junction model. Both parts are
// plain columns, so this direction alone does NOT diverge — it is the control
// that proves the through path handles a multi-column key at all.
struct RegionEntry {
    [[= primary_part]] int region{};
    [[= primary_part]] std::string code;
    std::string title;
    [[= storm::many_to_many_through<RegionEntryLink>]] std::vector<ThroughTopic> topics;
};

struct RegionEntryLink {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] RegionEntry entry;
    [[= storm::fk<>]] ThroughTopic topic;
    std::string note;
};

struct ShelfCodeLink; // through model, defined below

// Composite (2-part) RELATED side reached through an explicit junction model.
struct ShelfCode {
    [[= primary_part]] int aisle{};
    [[= primary_part]] std::string code;
    std::string label;
};

// Single-PK OWNER, composite RELATED — the mirror direction.
struct ShelfOwner {
    [[= primary_autoincrement]] int id{};
    std::string name;
    [[= storm::many_to_many_through<ShelfCodeLink>]] std::vector<ShelfCode> codes;
};

struct ShelfCodeLink {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] ShelfOwner owner;
    [[= storm::fk<>]] ShelfCode code;
};

struct FkPartLink; // through model, defined below

// THE divergence case: a composite key one of whose parts is ITSELF an FK — the
// canonical association-table shape. `warehouse` is an FK to Person (single-PK,
// from shared/models/person.h), so:
//   FK rule       (the through model's real column) → "entry_warehouse"
//   junction rule (what the m2m query path asked)   → "entry_warehouse_id"
// Before the fix the query named a column that does not exist and the eager
// load failed with "no such column: t2.entry_warehouse_id".
struct FkPartEntry {
    [[= primary_part]][[= storm::fk<>]] Person warehouse;
    [[= primary_part]] int sku{};
    std::string title;
    [[= storm::many_to_many_through<FkPartLink>]] std::vector<ThroughTopic> topics;
};

struct FkPartLink {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] FkPartEntry entry;
    [[= storm::fk<>]] ThroughTopic topic;
};

struct BothSidesLink; // through model, defined below

// Both sides composite, and the OWNER's key has an FK part: the widest through
// junction, where the owner contributes 2 columns (one of them FK-derived) and
// the related side contributes 2 more.
struct BothSidesOwner {
    [[= primary_part]][[= storm::fk<>]] Person depot;
    [[= primary_part]] int slot{};
    std::string name;
    [[= storm::many_to_many_through<BothSidesLink>]] std::vector<ShelfCode> codes;
};

struct BothSidesLink {
    [[= primary_autoincrement]] int id{};
    [[= storm::fk<>]] BothSidesOwner owner;
    [[= storm::fk<>]] ShelfCode code;
};

// fields:: selector proxies (#518) — two mechanical lines per model, no field
// names, so they cannot drift when a model gains or loses a member.
namespace fields {

struct OrderLineT;
consteval { std::meta::define_aggregate(^^OrderLineT, field_specs_for(^^OrderLine)); }
inline constexpr OrderLineT OrderLine{};

struct InventoryT;
consteval { std::meta::define_aggregate(^^InventoryT, field_specs_for(^^Inventory)); }
inline constexpr InventoryT Inventory{};

struct LedgerT;
consteval { std::meta::define_aggregate(^^LedgerT, field_specs_for(^^Ledger)); }
inline constexpr LedgerT Ledger{};

struct WidgetT;
consteval { std::meta::define_aggregate(^^WidgetT, field_specs_for(^^Widget)); }
inline constexpr WidgetT Widget{};

struct StockEntryT;
consteval { std::meta::define_aggregate(^^StockEntryT, field_specs_for(^^StockEntry)); }
inline constexpr StockEntryT StockEntry{};

struct LedgerWithTagsT;
consteval { std::meta::define_aggregate(^^LedgerWithTagsT, field_specs_for(^^LedgerWithTags)); }
inline constexpr LedgerWithTagsT LedgerWithTags{};

struct OptionalShipmentT;
consteval { std::meta::define_aggregate(^^OptionalShipmentT, field_specs_for(^^OptionalShipment)); }
inline constexpr OptionalShipmentT OptionalShipment{};

struct OrderLineWithShipmentsT;
consteval { std::meta::define_aggregate(^^OrderLineWithShipmentsT, field_specs_for(^^OrderLineWithShipments)); }
inline constexpr OrderLineWithShipmentsT OrderLineWithShipments{};

struct ShelfAssignmentT;
consteval { std::meta::define_aggregate(^^ShelfAssignmentT, field_specs_for(^^ShelfAssignment)); }
inline constexpr ShelfAssignmentT ShelfAssignment{};

struct ShipmentT;
consteval { std::meta::define_aggregate(^^ShipmentT, field_specs_for(^^Shipment)); }
inline constexpr ShipmentT Shipment{};

struct TagRegistryT;
consteval { std::meta::define_aggregate(^^TagRegistryT, field_specs_for(^^TagRegistry)); }
inline constexpr TagRegistryT TagRegistry{};

// #536 — through-model m2m over composite PKs.
struct RegionEntryT;
consteval { std::meta::define_aggregate(^^RegionEntryT, field_specs_for(^^RegionEntry)); }
inline constexpr RegionEntryT RegionEntry{};

struct ShelfOwnerT;
consteval { std::meta::define_aggregate(^^ShelfOwnerT, field_specs_for(^^ShelfOwner)); }
inline constexpr ShelfOwnerT ShelfOwner{};

struct FkPartEntryT;
consteval { std::meta::define_aggregate(^^FkPartEntryT, field_specs_for(^^FkPartEntry)); }
inline constexpr FkPartEntryT FkPartEntry{};

struct BothSidesOwnerT;
consteval { std::meta::define_aggregate(^^BothSidesOwnerT, field_specs_for(^^BothSidesOwner)); }
inline constexpr BothSidesOwnerT BothSidesOwner{};
} // namespace fields

#endif // STORM_TESTS_TEST_COMPOSITE_PK_MODELS_H
