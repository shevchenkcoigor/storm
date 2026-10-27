module;

#include <meta>

// Default-backend aliases. The ORM templates (`Basic*`) are written against the
// DatabaseConnection concept and name no backend; this module is the ONE place
// that picks SQLite as the default, so no generic module's import graph reaches
// the SQLite backend. (Build-level: the single `storm` library target still
// compiles and links every backend.) Each alias keeps the original
// public name and namespace, so `QuerySet<Person>` and
// `QuerySet<Person, postgresql::Connection>` read exactly as before.
// scripts/check_orm_backend_imports.py keeps the generic modules backend-free.
export module storm_defaults;

import storm_db_concept;
import storm_db_default_connection;
import storm_db_sqlite;
import storm_orm_field_attr; // storm::meta::Entity
import storm_orm_queryset;
import storm_orm_statements_erase;
import storm_orm_statements_insert;
import storm_orm_statements_select;
import storm_orm_statements_update;

export namespace storm::db {

    template <DatabaseConnection ConnType = sqlite::Connection>
    using DefaultConnection = BasicDefaultConnection<ConnType>;

} // namespace storm::db

export namespace storm::orm::statements {

    template <typename T, storm::db::DatabaseConnection ConnType = storm::db::sqlite::Connection>
    using SelectStatement = BasicSelectStatement<T, ConnType>;

    template <typename T, storm::db::DatabaseConnection ConnType = storm::db::sqlite::Connection>
    using InsertStatement = BasicInsertStatement<T, ConnType>;

    template <typename T, storm::db::DatabaseConnection ConnType = storm::db::sqlite::Connection>
    using UpdateStatement = BasicUpdateStatement<T, ConnType>;

    template <typename T, storm::db::DatabaseConnection ConnType = storm::db::sqlite::Connection>
    using EraseStatement = BasicEraseStatement<T, ConnType>;

} // namespace storm::orm::statements

export namespace storm {

    template <class T, storm::db::DatabaseConnection ConnType = storm::db::sqlite::Connection, bool Finalized = false>
        requires storm::meta::Entity<T>
    using QuerySet = BasicQuerySet<T, ConnType, Finalized>;

} // namespace storm
