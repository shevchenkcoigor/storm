# Storm ORM Documentation

Welcome to Storm ORM - a modern C++26 ORM library using compile-time reflection to automatically map C++ structs to database tables without macros.

**Supported backends**: SQLite, PostgreSQL

## Quick Links

- **[Main Project Guide (CLAUDE.md)](https://github.com/spiritEcosse/storm/blob/develop/CLAUDE.md)** - Quick start and common tasks
- **[Benchmarks](https://github.com/spiritEcosse/storm/blob/develop/benchmarks/README.md)** - Performance results and benchmark system

## Guide

Documentation for users of Storm ORM — patterns, features, and field/migration reference.

### Cookbook

- **[Cookbook](guide/COOKBOOK.md)** - Quick-reference patterns for common Storm ORM operations

### Features

- **[CRUD Operations](guide/features/CRUD_OPERATIONS.md)** - INSERT, UPDATE, DELETE with auto-generated IDs
- **[SELECT Queries](guide/features/SELECT_QUERIES.md)** - Optimized row extraction and statement caching
- **[WHERE Clauses](guide/features/WHERE_CLAUSES.md)** - Type-safe filtering with pure C++26 reflection
- **[JOIN Operations](guide/features/JOIN_OPERATIONS.md)** - Single and multi-FK JOINs with type erasure
- **[Batch Operations](guide/features/BATCH_OPERATIONS.md)** - Bulk INSERT/UPDATE/DELETE with smart thresholds
- **[UPSERT Operations](guide/features/UPSERT.md)** - Single-row ON CONFLICT DO UPDATE / DO NOTHING
- **[Referential Integrity](guide/features/REFERENTIAL_INTEGRITY.md)** - Always-on FK constraints, junction FOREIGN KEYs, SQLite PRAGMA
- **[Indexes](guide/features/INDEXES.md)** - Single-column `indexed`/`unique` tags and composite `Index<>`/`UniqueIndex<>`
- **[Connection Tuning](guide/features/CONNECTION_TUNING.md)** - SQLite busy_timeout default, optional WAL, pooled concurrency

### Reference

- **[Field Selectors](guide/reference/FIELD_SELECTORS.md)** - The `fields::Model.field` spelling, the per-model declaration recipe, and the column/relation distinction
- **[Field Types](guide/reference/FIELD_TYPES.md)** - Supported C++ to SQLite/PostgreSQL type mappings
- **[Migrations](guide/reference/MIGRATIONS.md)** - Atlas-based schema migrations with auto-discovered models

## Internals

Documentation for Storm ORM contributors — architecture, build/test workflow, and performance/compiler internals.

### Architecture

- **[Overview](internals/architecture/OVERVIEW.md)** - High-level architecture overview
- **[Design Decisions](internals/architecture/DESIGN_DECISIONS.md)** - Key architectural decisions and rationale
- **[C++26 Reflection](internals/architecture/REFLECTION.md)** - How Storm uses std::meta for ORM mapping
- **[SQL Generation](internals/architecture/SQL_GENERATION.md)** - Compile-time SQL generation with ConstexprString
- **[Module System](internals/architecture/MODULE_SYSTEM.md)** - Database-agnostic module design with zero-cost cross-module inlining
- **[Statement Caching](internals/architecture/STATEMENT_CACHING.md)** - Single Connection-level statement cache achieving near-raw SQLite performance
- **[Compile-Time vs Runtime](internals/architecture/COMPILE_TIME_VS_RUNTIME.md)** - WHERE expression design tradeoffs and performance analysis
- **[Composite & Non-Integer Primary Keys](internals/architecture/COMPOSITE_PRIMARY_KEYS.md)** - Multi-column and UUID keys: per-issue design notes and the failure each prevents
- **[PostgreSQL Binary Results](internals/architecture/POSTGRESQL_BINARY_RESULTS.md)** - Automatic binary-format optimization for SELECT extraction on PostgreSQL

### Building

- **[Getting Started](internals/building/GETTING_STARTED.md)** - First-time setup, build via Docker (recommended) or from source
- **[Common Tasks](internals/building/COMMON_TASKS.md)** - Adding operations, common development workflows
- **[Adding Features](internals/building/ADDING_FEATURES.md)** - How to add new database operations
- **[Pre-Commit](internals/building/PRE_COMMIT.md)** - where clang-tidy runs (PR `--diff --base`, weekly sweep; not the hook, #910), its modes (`--diff` / `--full` / `--all`), parse-failure handling
- **[Formatting](internals/building/FORMATTING.md)** - clang-format and cmake-format targets, pre-commit integration, and `.clang-format` settings

### Testing

- **[Testing](internals/testing/TESTING.md)** - Test framework, PostgreSQL test isolation, running tests
- **[Code Coverage](internals/testing/CODE_COVERAGE.md)** - How to generate and analyze test coverage
- **[Sanitizers](internals/testing/SANITIZERS.md)** - ASAN+UBSAN and TSAN presets, what each sanitizer catches, MSAN status
- **[Fuzzing](internals/testing/FUZZING.md)** - libFuzzer stress testing of the runtime SQL binding and execution layer
- **[MSAN libc++ Fix](internals/testing/MSAN_LIBC_FIX.md)** - In-progress fix for the MSAN + import std; + GTest false positive

### Performance

- **[Performance](internals/performance/PERFORMANCE.md)** - Performance guidelines, hot-path tips, and benchmarking/testing workflow
- **[Compile Time](internals/performance/COMPILE_TIME.md)** - Where build time goes, measured; what moves it and what was rejected
- **[Benchmark Dashboard](internals/performance/BENCHMARK_DASHBOARD.md)** - Real-time TUI for storm_bench: setup, schema, backup/restore, troubleshooting
- **[JOIN Analysis](internals/performance/JOIN_ANALYSIS.md)** - JOIN performance deep dive
- **[DISTINCT Analysis](internals/performance/DISTINCT_ANALYSIS.md)** - DISTINCT performance analysis

### Compiler

- **[Compiler Issues](internals/compiler/COMPILER_ISSUES.md)** - Known workarounds for clang-p2996
- **[Compiler Attributes](internals/compiler/COMPILER_ATTRIBUTES.md)** - Guide for using hot, flatten, and always_inline attributes
- **[C++26 Coding Standards](internals/compiler/CPP26_CODING_STANDARDS.md)** - Modern C++ best practices and patterns
- **[Code Quality](internals/compiler/CODE_QUALITY.md)** - Lint hook thresholds and `.lint-skip` exemption policy

## Open Issues

All open tasks are tracked as [GitHub Issues](https://github.com/spiritEcosse/storm/issues).
