#pragma once

// Storm/INSERT/* raw anchors — split out of anchors_raw.cpp to keep that file
// under the project's per-file line budget (#552). NOT a standalone
// compilation unit: textually included from inside anchors_raw.cpp's unnamed
// namespace, after die()/open_memory_db()/exec()/prepare()/step_done_or_die()
// are already declared, so it relies on those (and on <benchmark/benchmark.h>,
// <sqlite3.h>, <format>, <string>, <array> already being included there) — do
// not #include this file from anywhere else.

// The INSERT anchors model BenchPerson (id + name + age + salary, no UNIQUE),
// a deliberately narrow 4-field table — distinct from the full-Person table
// the SELECT/WHERE anchors use (kCreatePerson). Plain INTEGER PRIMARY KEY
// mirrors Storm's default schema (#379; see kCreatePerson note on AUTOINCREMENT).
constexpr auto kCreateBenchPerson = "CREATE TABLE person ("
                                    "id INTEGER PRIMARY KEY,"
                                    "name TEXT NOT NULL,"
                                    "age INTEGER NOT NULL,"
                                    "salary REAL NOT NULL"
                                    ")";

// Storm chunks a bulk INSERT at MAX_DB_VARIABLES / field_count_ rows. For the
// INSERT-benchmark model (BenchPerson: id + name + age + salary → field_count_
// = 4), that is 999 / 4 = 249 rows per multi-row VALUES statement
// (insert.cppm). N ≤ 249 → one bulk INSERT, no transaction (execute_bulk);
// N > 249 → BEGIN, one bulk INSERT per chunk, COMMIT (execute_chunked_bulk_*).
constexpr int kInsertChunkRows = 999 / 4;

// Build "INSERT INTO person(name, age, salary) VALUES (?,?,?),(?,?,?),..." for
// `rows` tuples, appending RETURNING id when `returning`. Mirrors Storm's
// multi-row VALUES SQL (insert_grammar.cppm build_bulk_insert_body).
auto build_bulk_insert_sql(int rows, bool returning) -> std::string {
    std::string sql = "INSERT INTO person(name, age, salary) VALUES ";
    for (int i = 0; i < rows; ++i) {
        sql += (i == 0) ? "(?,?,?)" : ",(?,?,?)";
    }
    if (returning) {
        sql += " RETURNING id";
    }
    return sql;
}

// Bind `rows` (name, age, salary) tuples into a prepared multi-row INSERT,
// 1-based placeholder per column. Names carry a per-call counter so rows
// accumulate without a UNIQUE collision (the table has no UNIQUE(name)),
// matching the Storm fixture's stamp_unique_names.
auto bind_bulk_rows(sqlite3_stmt* ins, int rows, int& counter) -> void {
    for (int i = 0; i < rows; ++i) {
        const std::string name = std::format("P{}", counter++);
        const int         base = i * 3;
        sqlite3_bind_text(ins, base + 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, base + 2, 30);
        sqlite3_bind_double(ins, base + 3, 50'000.0);
    }
}

// Step a bound multi-row INSERT to completion. RETURNING yields one row per
// inserted tuple (read each id so the optimizer can't elide it) before
// SQLITE_DONE; plain INSERT steps straight to SQLITE_DONE. Reset is the
// caller's job (run_chunked_pass resets every statement after its
// process_full/process_rem callback returns) — not done here, to avoid a
// double reset.
auto step_bulk_insert(sqlite3* db, sqlite3_stmt* ins, bool returning) -> void {
    if (returning) {
        while (sqlite3_step(ins) == SQLITE_ROW) {
            benchmark::DoNotOptimize(sqlite3_column_int(ins, 0));
        }
    } else if (sqlite3_step(ins) != SQLITE_DONE) {
        die(db, "insert step");
    }
}

// Shared "chunked bulk statement" shape: a full-chunk statement + count, an
// optional remainder statement + its row count, and whether the plan spans
// more than one statement (and so needs a transaction). Every bulk INSERT/
// UPDATE/DELETE anchor plan (here and in anchors_raw_crud.hpp) is built on this
// — InsertPlan adds one field for its RETURNING flag; the UPDATE_PK/DELETE_PK
// plans use it as-is or with one extra field for their own special case.
struct ChunkPlan {
    sqlite3_stmt* full_stmt      = nullptr;
    sqlite3_stmt* rem_stmt       = nullptr;
    int           full_chunks    = 0;
    int           remainder_rows = 0;
    bool          chunked        = false;
};

// Generic "BEGIN if chunked → process every full chunk → process the
// remainder, if any → COMMIT if chunked" driver — matches Storm's
// execute_bulk (no txn) vs execute_chunked_bulk_*/execute_chunked (txn)
// split (insert.cppm / erase.cppm) shared by every chunked bulk anchor in
// this file. `process_full`/`process_rem` each bind AND step their
// statement (they know whether/how to check the result — e.g. INSERT's
// RETURNING loop vs a plain step_done_or_die); this driver only resets
// afterward and manages the transaction boundary.
template <typename ProcessFull, typename ProcessRem>
auto run_chunked_pass(sqlite3* db, ChunkPlan const& plan, ProcessFull&& process_full, ProcessRem&& process_rem)
        -> void {
    if (plan.chunked) {
        exec(db, "BEGIN");
    }
    for (int c = 0; c < plan.full_chunks; ++c) {
        process_full(plan.full_stmt);
        sqlite3_reset(plan.full_stmt);
    }
    if (plan.rem_stmt != nullptr) {
        process_rem(plan.rem_stmt);
        sqlite3_reset(plan.rem_stmt);
    }
    if (plan.chunked) {
        exec(db, "COMMIT");
    }
}

// How Storm splits an N-row bulk INSERT into chunks: `full_chunks` full
// statements of kInsertChunkRows each, plus a `remainder_rows` tail (0 when
// N divides evenly). `chunked` is true when N spans more than one chunk, the
// condition under which Storm wraps the inserts in a transaction. The two
// prepared statements (one per distinct chunk shape) are built once and
// reused across timed iterations, mirroring Storm's cached prepares.
struct InsertPlan : ChunkPlan {
    bool returning = false;
};

auto make_insert_plan(sqlite3* db, int n, bool returning) -> InsertPlan {
    int const  full_chunks    = n / kInsertChunkRows;
    int const  remainder_rows = n % kInsertChunkRows;
    bool const chunked        = (full_chunks + (remainder_rows > 0 ? 1 : 0)) > 1;
    return InsertPlan{
            ChunkPlan{
                    .full_stmt      = full_chunks > 0
                                              ? prepare(db, build_bulk_insert_sql(kInsertChunkRows, returning).c_str())
                                              : nullptr,
                    .rem_stmt       = remainder_rows > 0
                                              ? prepare(db, build_bulk_insert_sql(remainder_rows, returning).c_str())
                                              : nullptr,
                    .full_chunks    = full_chunks,
                    .remainder_rows = remainder_rows,
                    .chunked        = chunked,
            },
            returning,
    };
}

// One timed batch: step every chunk in the plan, wrapped in BEGIN/COMMIT only
// when plan.chunked — matching Storm's execute_bulk (no txn) vs
// execute_chunked_bulk_* (txn) split.
auto run_insert_chunks(sqlite3* db, InsertPlan const& plan, int& counter) -> void {
    run_chunked_pass(
            db,
            plan,
            [&](sqlite3_stmt* stmt) {
                bind_bulk_rows(stmt, kInsertChunkRows, counter);
                step_bulk_insert(db, stmt, plan.returning);
            },
            [&](sqlite3_stmt* stmt) {
                bind_bulk_rows(stmt, plan.remainder_rows, counter);
                step_bulk_insert(db, stmt, plan.returning);
            }
    );
}

// Shared batch INSERT driver for both anchors. Builds the table and the
// chunk plan once, then per timed iteration inserts state.range(0) rows the
// way Storm does (see run_insert_chunks). Only the bind + step + (optional)
// BEGIN/COMMIT is timed, mirroring the Storm fixture (setup outside the loop,
// cached prepares).
//
// `returning` selects the insert() vs insert_no_return() anchor, but RETURNING
// is only actually emitted at N=1. The Storm fixture dispatches N=1 through
// the single-row insert(obj).execute() path (RETURNING id, execute_single_*)
// and N>1 through the bulk insert(span).execute() VOID path — which has no
// RETURNING clause (queryset.cppm: the default bulk .execute() returns void).
// So at N>1 both anchors are plain bulk inserts; only N=1 of the insert
// anchor reads back an id.
auto run_insert_benchmark(benchmark::State& state, bool returning) -> void {
    int const  n             = static_cast<int>(state.range(0));
    bool const use_returning = returning && n == 1;
    sqlite3*   db            = open_memory_db();
    exec(db, kCreateBenchPerson);

    InsertPlan const plan = make_insert_plan(db, n, use_returning);

    int counter = 0;
    // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores) -- Google Benchmark loop idiom: `_` is the iteration token, never read
    for (auto _ : state) {
        run_insert_chunks(db, plan, counter);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * n);
    state.SetComplexityN(state.range(0));

    // sqlite3_finalize(nullptr) is a documented no-op — no null guard needed
    // for the chunk shapes that leave one of these unset.
    sqlite3_finalize(plan.full_stmt);
    sqlite3_finalize(plan.rem_stmt);
    sqlite3_close(db);
}

// BATCH_STANDARD from benchmarks/sizes.cppm — the exact sweep Storm's INSERT
// benchmarks register, so the dashboard's (test_name, dataset_size) matcher
// pairs every Storm INSERT row with this raw baseline.
constexpr std::array kBatchStandard = {1, 10, 100, 500, 1000, 5000, 10000, 50000, 100000};

// BATCH_INSERT_EDGE from sizes.cppm — the SQLite chunk boundary for BenchPerson
// (999/4 fields = 249): 248 stays under one chunk, 249 is exactly one chunk,
// 250 spills into a second. Storm/INSERT_EDGE/insert_edge sweeps these against
// the same "insert" operation as Storm/INSERT/insert (benchmark_tests.yaml),
// so it reuses BM_Raw_Insert_Returning under a second registered name.
constexpr std::array kBatchInsertEdge = {248, 249, 250};

// Registers `fn` under `name` swept over `sizes` — shared by every BATCH_*
// anchor sweep in this file and in anchors_raw_crud.hpp.
auto register_sized_anchor(char const* name, void (*fn)(benchmark::State&), std::span<const int> sizes) -> void {
    auto* bench = benchmark::RegisterBenchmark(name, fn);
    for (int n : sizes) {
        bench->Arg(n);
    }
    bench->Complexity(benchmark::oN)->ArgName("N");
}

// Registers `fn` under `name` swept over kBatchStandard — shared by the
// INSERT anchors here and the UPDATE_PK/DELETE_PK anchors in
// anchors_raw_crud.hpp (same sweep, same ->Complexity/->ArgName shape).
auto register_batch_standard_anchor(char const* name, void (*fn)(benchmark::State&)) -> void {
    register_sized_anchor(name, fn, kBatchStandard);
}

// Storm/INSERT/insert_no_return/N:<n> — plain bulk INSERT, no RETURNING
auto BM_Raw_Insert_No_Return(benchmark::State& state) -> void {
    run_insert_benchmark(state, /*returning=*/false);
}

// Storm/INSERT/insert/N:<n> — bulk INSERT … RETURNING id (mirrors Storm's insert() path)
auto BM_Raw_Insert_Returning(benchmark::State& state) -> void {
    run_insert_benchmark(state, /*returning=*/true);
}
