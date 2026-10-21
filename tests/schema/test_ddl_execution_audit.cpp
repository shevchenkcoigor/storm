#include <gtest/gtest.h>
#include <meta>
#include <plf_hive/plf_hive.h> // NOSONAR cpp:S954 — must precede `import std;` (see test_m2m_models.h)

#include "test_db_helpers.h"

import storm_db_default_connection;
import storm_db_postgresql;
import storm_db_sqlite;
import storm_meta;
import storm_orm_indexes;
import storm_orm_schema;
import storm_orm_statements_insert;
import storm_orm_transaction;
import storm_orm_utilities;
import std;

// The DDL audit sweeps every shared model, so it takes the umbrella rather than
// naming eleven of the twelve (issue #634). It is also the only in-tree consumer
// of test_models.h, which is what keeps both umbrellas compile-checked — an
// umbrella nothing includes is never parsed and rots silently.
#include "test_models.h"                   // NOSONAR cpp:S954
#include "query/test_fields_models.h"      // NOSONAR cpp:S954
#include "crud/test_composite_pk_models.h" // NOSONAR cpp:S954
#include "query/test_m2m_models.h"         // NOSONAR cpp:S954
#include "query/test_reverse_fk_models.h"  // NOSONAR cpp:S954

namespace orm = storm::orm;

// Model only selects the default-connection QuerySet type — on_setup is overridden
// to create nothing, since each DDL_AUDIT_MODEL test creates its own table.
template <typename ConnType> class DdlExecutionAudit : public StormTestFixture<SimpleRecord, ConnType> {
  protected:
    auto on_setup(const std::shared_ptr<ConnType>& /*conn*/) -> void override {}
};

TYPED_TEST_SUITE(DdlExecutionAudit, DatabaseTypes);

#define DDL_AUDIT_MODEL(M)                                                                                             \
    TYPED_TEST(DdlExecutionAudit, Create##M##TableSucceeds) {                                                          \
        const auto& conn   = DefaultConnection<TypeParam>::get();                                                      \
        auto        result = orm::schema::SchemaStatement<M>::create_table_if_not_exists(conn);                        \
        ASSERT_TRUE(result.has_value()) << #M " DDL failed: " << result.error().message();                             \
    }

// Person and Message are already execution-tested in test_orm_schema.cpp's
// SchemaTest suite (CreatePersonTableSucceeds / CreateMessageTableSucceeds) — omitted here.
// Each entry below also creates its whole FK/m2m closure (create_table_if_not_exists
// recurses into parents and junctions), so a model that only ever appears as
// someone else's target is covered incidentally, not by its own line — dropping
// it from every owner's field list would silently stop exercising its DDL.
DDL_AUDIT_MODEL(SimpleRecord)
DDL_AUDIT_MODEL(ExtendedTypes)
DDL_AUDIT_MODEL(TimestampedRecord)
DDL_AUDIT_MODEL(UuidPkModel)
DDL_AUDIT_MODEL(Task)
DDL_AUDIT_MODEL(CascadeChild)
DDL_AUDIT_MODEL(SetNullChild)
DDL_AUDIT_MODEL(RestrictChild)
DDL_AUDIT_MODEL(UuidPkRef)

DDL_AUDIT_MODEL(FSPerson)
DDL_AUDIT_MODEL(FSTag)
DDL_AUDIT_MODEL(FSArticle)
DDL_AUDIT_MODEL(FSPersonExtra)

DDL_AUDIT_MODEL(Course)
DDL_AUDIT_MODEL(Student)
DDL_AUDIT_MODEL(Enrollment)
DDL_AUDIT_MODEL(Pupil)
DDL_AUDIT_MODEL(Track)
DDL_AUDIT_MODEL(Playlist)
DDL_AUDIT_MODEL(Album)
DDL_AUDIT_MODEL(Club)
DDL_AUDIT_MODEL(Member)
DDL_AUDIT_MODEL(Topic)
DDL_AUDIT_MODEL(Lesson)
DDL_AUDIT_MODEL(Tutor)
DDL_AUDIT_MODEL(RestrictedClub)
DDL_AUDIT_MODEL(RestrictedMember)

DDL_AUDIT_MODEL(RfTask)
DDL_AUDIT_MODEL(RfPerson)
DDL_AUDIT_MODEL(RfNote)
DDL_AUDIT_MODEL(RfBoard)
DDL_AUDIT_MODEL(RfReporter)
DDL_AUDIT_MODEL(RfBug)

DDL_AUDIT_MODEL(OrderLine)
DDL_AUDIT_MODEL(Inventory)
DDL_AUDIT_MODEL(Ledger)
DDL_AUDIT_MODEL(Widget)
DDL_AUDIT_MODEL(StockEntry)
DDL_AUDIT_MODEL(Shipment)
DDL_AUDIT_MODEL(OrderLineWithShipments)
DDL_AUDIT_MODEL(LedgerTag)
DDL_AUDIT_MODEL(LedgerWithTags)
DDL_AUDIT_MODEL(LedgerEntryRef)
DDL_AUDIT_MODEL(LedgerEntryRefWide)
DDL_AUDIT_MODEL(OptionalShipment)
DDL_AUDIT_MODEL(MixedFkOrder)
DDL_AUDIT_MODEL(CatalogEntry)
DDL_AUDIT_MODEL(TagRegistry)
DDL_AUDIT_MODEL(StorageBin)
DDL_AUDIT_MODEL(ShelfAssignment)
DDL_AUDIT_MODEL(ThroughTopic)
DDL_AUDIT_MODEL(RegionEntry)
DDL_AUDIT_MODEL(RegionEntryLink)
DDL_AUDIT_MODEL(ShelfCode)
DDL_AUDIT_MODEL(ShelfOwner)
DDL_AUDIT_MODEL(ShelfCodeLink)
DDL_AUDIT_MODEL(FkPartEntry)
DDL_AUDIT_MODEL(FkPartLink)
DDL_AUDIT_MODEL(BothSidesOwner)
DDL_AUDIT_MODEL(BothSidesLink)
