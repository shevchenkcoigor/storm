module;

#include <meta>

export module storm;

import std;

// Public surface: every name these modules export is part of `import storm;`.
export import storm_model;                 // annotations, fields:: selector machinery
export import storm_db_default_connection; // BasicDefaultConnection
export import storm_defaults;              // QuerySet / DefaultConnection / *Statement aliases
export import storm_orm_transaction;       // begin / transaction / TransactionGuard
export import storm_db_concept;            // Error, JournalMode, connection concepts
export import storm_db_sqlite;             // sqlite::Connection / Config
export import storm_db_postgresql;         // postgresql::Connection
export import storm_db_pool;               // ConnectionPool / PoolConfig
export import storm_orm_where;             // WHERE expressions
export import storm_orm_fields;            // field_specs_for, FieldRef / RelationRef
export import storm_orm_queryset;          // BasicQuerySet
export import storm_orm_indexes;           // Index / UniqueIndex / Indexes
export import storm_orm_schema;            // SchemaStatement

// Implementation modules: reachable (so the types the public API returns stay
// usable) but NOT re-exported. Code that names their internals — statement
// classes, grammars, model concepts, SQL-building helpers — imports the specific
// module explicitly. The few user-facing names declared inside them are
// re-exported individually below, in their original namespaces.
import storm_orm_generator;
import storm_orm_utilities;
import storm_orm_statements_base;
import storm_orm_statements_insert;
import storm_orm_statements_upsert_grammar;
import storm_orm_statements_select;
import storm_orm_statements_projection;
import storm_orm_statements_join;
import storm_orm_statements_setop;
import storm_orm_statements_orderby;

export namespace storm {
    constexpr auto version = "0.1.0";

    using storm::generator; // NOLINT(misc-unused-using-decls) — rows() return type
    using storm::UUID;      // NOLINT(misc-unused-using-decls) — UUID field / PK type

    // `using namespace storm;` + bare `Collate::NoCase`, as the user guide writes it (#679)
    using storm::orm::utilities::Collate; // NOLINT(misc-unused-using-decls)
} // namespace storm

export namespace storm::orm::utilities {
    using storm::orm::utilities::Collate; // NOLINT(misc-unused-using-decls) — .collate() / order_by<F, Collate::…>
} // namespace storm::orm::utilities

export namespace storm::orm::statements {
    using storm::orm::statements::InsertOptions; // NOLINT(misc-unused-using-decls) — insert(span, InsertOptions{…})
    using storm::orm::statements::ReturnId;      // NOLINT(misc-unused-using-decls) — insert<ReturnId::…>
} // namespace storm::orm::statements
