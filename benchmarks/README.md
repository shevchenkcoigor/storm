# Storm ORM - Compile-Time Benchmark System

**Pure C++ benchmark system using compile-time JSON parsing and template metaprogramming!**

## 🎯 Overview

The unified benchmark system is a **100% compile-time C++ solution** that loads benchmark tests from JSON at compile time using C++26 `#embed` and executes them with zero runtime dispatch overhead through template metaprogramming.

### Key Features

- ✅ **Pure C++26** - Uses `#embed` for compile-time JSON parsing
- ✅ **Google Benchmark backend** - median / mean / stddev / `BigO` / `RMS` come from gbench, not a custom runner
- ✅ **Template Metaprogramming** - Each test gets its own specialized fixture
- ✅ **Type Safety** - Field names and operators resolved at compile time
- ✅ **YAML Configuration** - Human-friendly test definitions in `benchmark_tests.yaml` (auto-converted to JSON at build time)
- ✅ **Compile-Time Field Dispatch** - Uses reflection to map field names to struct members
- ✅ **Runtime Filtering** - `--benchmark_filter=...` regex without rebuilding
- ✅ **Raw SQLite Anchors** - Sparse spot checks live in the separate `storm_anchors` binary (release-time only, not paired against per-Storm runs)
- ✅ **Batch INSERT/UPDATE/DELETE** - YAML profiles (`batch_standard`, `batch_*_edge`) drive size sweeps via gbench `Arg()`
- ✅ **Range / Complexity sweeps** - `dataset_standard` / `dataset_small` map to gbench `Range()` + `Complexity(oN)`

> **CLI**: This benchmark binary is a Google Benchmark executable. Use `--benchmark_filter`, `--benchmark_repetitions`, `--benchmark_min_time`, `--benchmark_format=json`, etc. — see [Usage](#-usage) below. The pre–#235 flags `--filter`, `--iterations`, `--scale-test`, `-c <category>`, `--quick`/`--thorough`, `--disk`/`--db=` no longer exist.

## 🚀 Quick Start

**When changing a module, run category benchmarks to verify performance:**

```bash
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/SELECT/.*'         # After changing SELECT module
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/DISTINCT/.*'       # After changing DISTINCT module
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/INSERT/.*'         # After changing INSERT module
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/AGGREGATE/.*'      # After changing AGGREGATE module
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/SELECT/.*' --benchmark_list_tests=true  # Preview which tests will run
```

### Open Issues

See [GitHub Issues (benchmarks)](https://github.com/spiritEcosse/storm/issues?q=is%3Aissue+is%3Aopen+label%3Abenchmarks) for planned improvements.

## 📺 Live dashboard

`storm_bench_dashboard` streams results to an ANSI TUI in real time and persists every result in SQLite. Two-terminal workflow:

```bash
# Terminal 1 — start the dashboard
./build/release/benchmarks/dashboard/storm_bench_dashboard

# Terminal 2 — run benchmarks with streaming enabled
STORM_BENCH_SOCKET=1 ./build/release/benchmarks/storm_bench --benchmark_filter='Storm/WHERE/.*'

# Quick smoke test (sub-second per benchmark)
STORM_BENCH_SOCKET=1 ./build/release/benchmarks/storm_bench --benchmark_filter='Storm/WHERE/.*' --benchmark_min_time=0.2s
```

Keys: `q` quit · `r` refresh from DB · `1`–`9` toggle session expand/collapse.

**Baseline comparison** — the dashboard compares each result against a previous run in real time:

```bash
./storm_bench_dashboard                          # auto: most recent full run, same branch+host
./storm_bench_dashboard --baseline branch:develop
./storm_bench_dashboard --baseline run:38
./storm_bench_dashboard --baseline none          # disable
./storm_bench_dashboard --regression-threshold 10
```

Each result line shows a delta column (`+6.3% REGRESS`, `−7.1% IMPROVE`, `—` if no baseline row). A summary line per session counts ok / improve / regress.

**Storm vs raw SQLite** — Measure Storm's efficiency against raw SQLite using the intentional `storm_anchors` subset:

```bash
# Terminal 1 — start the dashboard
./build/release/benchmarks/dashboard/storm_bench_dashboard

# Terminal 2 — stream raw SQLite baseline run to the dashboard
STORM_BENCH_SOCKET=1 ./build/release/benchmarks/storm_anchors

# Terminal 2 — now run a Storm comparison run against the raw baseline
./build/release/benchmarks/dashboard/storm_bench_dashboard --baseline raw:last
STORM_BENCH_SOCKET=1 ./build/release/benchmarks/storm_bench
```

The raw baseline covers these pinned benchmarks (exact Google Benchmark names with `/N:` suffix):
- `Storm/WHERE/where_int_comparison_gt/N:10000`
- `Storm/WHERE/where_bool_equality/N:10000`
- `Storm/WHERE/where_double_comparison/N:10000`
- `Storm/WHERE/where_int_less_than/N:10000`
- `Storm/SELECT/select/N:<n>` for every `n` in `dataset_standard`
  `{100, 1000, 10000, 100000}` (`RangeMultiplier(10)->Range(100, 100000)`, the
  same registration shape Storm's own `select` test uses)
  — these run on the **full 10-field Person** model. The raw anchor mirrors
  Storm's generated Person schema (all 10 columns, `name TEXT NOT NULL UNIQUE`,
  `is_active … DEFAULT 0`, nullable `score`/`nickname`/`avatar`) and **materializes
  each result row into a `plf::hive<PersonRow>`** — two strings, two optionals and
  a BLOB vector per row — exactly as Storm's `select().execute()` builds a
  `plf::hive<Person>`. Both sides therefore pay the same per-row construct +
  container-insert cost (fairness audit #68); a raw anchor that only stepped
  columns would read a far lighter row than Storm and understate Storm's efficiency.
- `Storm/INSERT/insert/N:<n>` and `Storm/INSERT/insert_no_return/N:<n>` for the full
  `BATCH_STANDARD` sweep `{1, 10, 100, 500, 1000, 5000, 10000, 50000, 100000}`
  (mirrors `benchmarks/sizes.cppm`). Each iteration inserts `n` rows the way Storm
  does — one multi-row `VALUES` statement per chunk of 249 rows (`999 / 4` fields),
  in a transaction only when `n` spans more than one chunk. `RETURNING id` is read
  back only for `insert/N:1`, since the Storm fixture sends `N=1` through the
  single-row `insert(obj)` path but `N>1` through the bulk `insert(span)` VOID path.
- `Storm/UPDATE_PK/update_pk/N:<n>` — same `BATCH_STANDARD` sweep, over the full
  Person model. `n==1` binds+executes once with no transaction; `n>1` reuses one
  prepared `UPDATE … WHERE id=?` statement across the whole batch inside a single
  `BEGIN`/`COMMIT`, mirroring `UpdateStatement::execute(span)`. The table also
  carries the 4 extra indexes Storm's schema generator creates for Person
  (`storm::unique`/`storm::indexed`/`storm_indexes`) beyond the SELECT/WHERE
  anchors' plain schema — omitting them would have the raw side pay less
  index-maintenance cost than Storm on every write and understate Storm's
  efficiency, the same fairness class as the AUTOINCREMENT rule below.
- `Storm/DELETE_PK/delete_pk/N:<n>` — same sweep and extra indexes. Storm's
  fixture re-seeds the table on **every iteration** before erasing
  (`CrudBenchmark::run_once`'s `reinsert_for_delete()`, timed —
  `bench_register.h`'s `run()` has no `PauseTiming`), so this anchor's timed
  body is clear+reinsert+reselect+delete, not a bare `DELETE`. Anchoring only
  the `DELETE` would produce a nonsensical ratio against what "delete_pk"
  actually measures. `n==1` issues a single `id = ?` delete (no transaction);
  `2..799` issues one `id IN (...)` statement (no transaction); `800+` chunks
  the `IN` list at 799 rows/statement inside one `BEGIN`/`COMMIT`
  (`EraseStatement`, single-PK `MAX_CHUNK_ROWS = 999*4/5 = 799`).
- `Storm/INSERT_EDGE/insert_edge/N:<n>` and `Storm/UPDATE_PK_EDGE/update_pk_edge/N:<n>`
  for `n` in `BATCH_INSERT_EDGE {248, 249, 250}` and `BATCH_UPDATE_EDGE {198, 199, 200}`
  — both sweep the same operation as the plain `insert`/`update_pk` anchors above
  (`benchmark_tests.yaml`), just at boundary sizes, and reuse those anchor
  functions verbatim under a second registered name. `insert_edge`'s 249 is a
  real chunk boundary (`999/4` fields); `update_pk_edge`'s 199 is not — the
  UPDATE path has no chunk arithmetic at all — it's swept only so that Storm
  benchmark has a raw counterpart.

(#552 tier (a) — UPDATE_PK/DELETE_PK/their `_EDGE` sweeps, plus completing
WHERE to all four benchmarks. Everything else — ~46 remaining categories
including JOIN and AGGREGATE — is deliberately left unanchored; see #552 for
the coverage analysis and why AGGREGATE/SETOP in particular are poor
candidates despite their size.)

Matched Storm rows show **efficiency as `NN.N% of raw`** (green ≥95%, red below); unmatched rows show `— (no raw)`. The session summary becomes `session: N/M matched · avg NN.N% of raw · target ≥95%`. The raw baseline is **reused across Storm sessions** — refresh is a manual step: re-run `storm_anchors`, then restart the dashboard with `--baseline raw:last`.

To extend the subset, add a benchmark to `benchmarks/anchors_raw.cpp` (or one of its textually-included `anchors_raw_*.hpp` split files — kept separate only to stay under the project's per-file line budget, not standalone headers) mirroring the target Storm benchmark's **exact** name via `->Name(...)->Arg(N)->ArgName("N")` and `state.SetComplexityN(state.range(0))`, so the `(test_name, dataset_size)` key matches the Storm row. The raw `CREATE TABLE` must also mirror Storm's generated schema — notably plain `id INTEGER PRIMARY KEY` (since #379 Storm emits plain `INTEGER PRIMARY KEY` by default; AUTOINCREMENT is opt-in via `storm::primary_autoincrement` and costs ~358 ns/insert of `sqlite_sequence` work — using AUTOINCREMENT in the raw anchor would make the INSERT comparison unfair). For SELECT-family anchors, also mirror the **full model column set** and **materialize results into a `plf::hive`** of a struct matching the model — Storm's `select()` always builds the complete entity, so a raw anchor that selects fewer columns or skips the container materialization is not a fair comparison (#68). See `benchmarks/scripts/compare_against_raw.sh` for the orchestration driver.

See [docs/internals/performance/BENCHMARK_DASHBOARD.md](../docs/internals/performance/BENCHMARK_DASHBOARD.md) for full setup, schema, baseline selectors, backup/restore, and troubleshooting.

## 📉 Regression detection

**CI does not gate on absolute walltime.** A per-PR gate that diffed `storm_bench` against a `develop` baseline artifact (±5%, Mann-Whitney U-test) was added in #241/#243 and removed in #245: absolute time on shared runners moves by ~25% between runners with no code change, so all 376 benchmarks of one doc-only PR read as "improved" (post-mortem in #551). What CI runs instead is the [storm/raw ratio gate](#stormraw-ratio-gate-githubworkflowsbench-ratioyml-752) below, which compares two binaries inside one job.

`benchmarks/scripts/compare_against_baseline.sh` remains as a **local A/B tool**: run it before and after a change on the **same machine** and it diffs the two JSONs via Google Benchmark's [`compare.py`](https://github.com/google/benchmark/blob/main/tools/compare.py) + Mann-Whitney U-test, exiting 1 on any benchmark slower than the threshold with statistical significance. No baseline is committed to the repo and CI produces none: a baseline is only meaningful on the same hardware as the comparison run.

**Local workflow** (compare two runs on one machine)

```bash
cmake --preset ninja-release && cmake --build --preset ninja-release

# 1. Run-only — produces current.json, no comparison.
./benchmarks/scripts/compare_against_baseline.sh

# 2. Save a snapshot before making changes, then diff after.
cp current.json /tmp/before.json
# ... edit code, rebuild ...
./benchmarks/scripts/compare_against_baseline.sh /tmp/before.json

# Narrow scope while iterating:
BENCH_FILTER='Storm/SELECT.*' \
    ./benchmarks/scripts/compare_against_baseline.sh /tmp/before.json

# Widen the regression threshold for noisy hardware:
REGRESSION_THRESHOLD=1.10 \
    ./benchmarks/scripts/compare_against_baseline.sh /tmp/before.json
```

Per-benchmark deltas are computed as `(current - baseline) / |baseline|`; the gate trips on `mean` or `median` aggregates only — `stddev`/`cv`/iteration-count rows are ignored. Significance gating uses the Mann-Whitney U-test at `p < UTEST_ALPHA` (default 0.05).

**Threshold knobs** (env vars, all optional):

| Var | Default | Effect |
|---|---|---|
| `REGRESSION_THRESHOLD` | `1.05` | Slowdown ratio. Accepts `1.05`, `0.05`, or `5%`. |
| `UTEST_ALPHA` | `0.05` | U-test significance bar. Set `0` to disable significance gating. |
| `BENCH_REPETITIONS` | `10` | Reps per benchmark. Lower for faster local turnaround. |
| `BENCH_FILTER` | (empty) | Google Benchmark `--benchmark_filter` regex. |
| `BENCH_MIN_TIME` | (empty) | Google Benchmark `--benchmark_min_time` (e.g. `0.5s`). |
| `PYTHON` | `python3` | Override e.g. `/path/to/venv/bin/python` if numpy/scipy aren't system-installed. |

`compare.py` requires `numpy` and `scipy` (used for the geometric mean and U-test respectively). Install via your distro's package manager (`pacman -S python-numpy python-scipy`, `apt install python3-numpy python3-scipy`, etc.) or a venv.

### Storm/raw ratio gate (`.github/workflows/bench-ratio.yml`, #752)

The absolute-walltime gate above is unreliable across shared runners, so CI also measures a **ratio**: `storm_bench` and the raw `storm_anchors` run **in the same job, interleaved** (raw, storm, raw, storm, …, 3 rounds), and the gate reads `% of raw` (raw time ÷ storm time) per benchmark. Runner-to-runner drift cancels in the ratio.

- **Triggers:** nightly (03:30 UTC), manual `workflow_dispatch`, and PRs touching `src/db/**`, `src/orm/**`, `src/storm*.cppm`, `shared/models/**`, `benchmarks/**` (except `*.md`), `cmake/libcxx.cmake` or `CMakePresets.json` (~22 min per run, so not every PR). Anything the filter misses is caught by the nightly.
- **Verdict:** `benchmarks/scripts/ratio_gate.py` — exit 0 pass, 1 a benchmark below its threshold, 2 unusable data (empty/partial pairing, drifted names, mixed time units, malformed JSON). The same script runs locally on gbench JSON from a CI run (`gh run download <id>`); it measures nothing itself. Tested by `scripts/ci/tests/test_ratio_gate.py`.
- **Thresholds:** 95% of raw (the project target), with a temporary 80% override for `Storm/INSERT/` and `Storm/INSERT_EDGE/` while the bulk INSERT gap is open (#779). A result below threshold is currently a **warning**, not a failure: single-run noise (WHERE/SELECT sat as low as 95.6% across 8 runs) needs a "K consecutive runs" rule first. Unusable data (exit 2) already fails the run.
- **Not a required check:** it is deliberately outside `ci.yml`'s `CI gate` — a path-filtered workflow that does not run would leave a required check pending forever (#638).
- **Red nightly:** a failed scheduled run opens (or comments on) a `bench-ratio: scheduled run failed` issue. Also check `gh run list --workflow bench-ratio.yml --event schedule`.

## 📦 Components

```
benchmarks/
├── main.cpp                    # Google Benchmark entry — owns <benchmark/benchmark.h>, no imports
├── register.cpp                # Storm-side bridge — `import storm_*` + builds the gbench registration table
├── bench_register.h            # POD-only handoff between main.cpp and register.cpp
├── benchmark_tests.hpp         # `#embed`-driven BENCHMARK_TESTS array (textual; consumed by register.cpp)
├── parser.cppm                 # Compile-time JSON parser (storm_benchmark_parser module)
├── schema.cppm                 # Benchmark test schema (storm_benchmark_schema module)
├── sizes.cppm                  # CRUD batch-size constexpr arrays (storm_benchmark_sizes module)
├── registry.cppm               # Compile-time model dispatch (storm_benchmark_registry module)
├── models.hpp                  # ORM model structs (textual — annotations need to be visible)
├── base.cppm                   # CRTP base for data-driven benchmarks (storm_benchmark_base module)
├── query_benchmark.cppm        # SELECT-family fixture (storm_benchmark_query module)
├── crud_benchmark.cppm         # INSERT/UPDATE/DELETE fixture (storm_benchmark_crud module)
├── anchors_raw.cpp             # `storm_anchors` binary — release-time raw SQLite spot checks
├── anchors_raw_insert.hpp      # INSERT-family anchors (textually included by anchors_raw.cpp, not standalone)
├── anchors_raw_crud.hpp        # UPDATE_PK/DELETE_PK anchors (ditto)
├── scripts/
│   ├── yaml_to_json.py             # YAML → JSON converter (runs at build time)
│   ├── compare_against_baseline.sh # Regression diff (Mann-Whitney U-test) — engine for local-dev
└── tests/
    ├── benchmark_tests.yaml   # Test definitions (human-friendly source of truth)
    └── benchmark_tests.json   # Auto-generated from YAML (loaded at compile time via #embed)
```

## 📐 Size Profiles

The benchmark system uses **size profiles** to automatically iterate over multiple sizes from a single JSON test definition. This reduces JSON redundancy from ~150 entries to ~60 entries while generating all the same benchmark variations at runtime.

### Available Profiles

| Profile | Sizes | Use Case |
|---------|-------|----------|
| `batch_standard` | 1, 10, 100, 500, 1000, 5000, 10000, 50000, 100000 | INSERT/UPDATE/DELETE operations |
| `batch_insert_edge` | 248, 249, 250 | SQLite chunk boundary (999/4 fields) |
| `batch_update_edge` | 198, 199, 200 | SQLite chunk boundary (999/5 fields) |
| `dataset_standard` | 100, 1000, 10000, 100000 | SELECT/JOIN/DISTINCT operations |
| `dataset_small` | 10000, 50000 | Aggregate operations |

### How It Works

Instead of defining 9 separate entries for each INSERT batch size:

```json
// OLD: 9 separate entries
{ "test_name": "insert_single", "batch_size": 1, "iterations": 10000 }
{ "test_name": "insert_batch_10", "batch_size": 10, "iterations": 1000 }
{ "test_name": "insert_batch_100", "batch_size": 100, "iterations": 100 }
// ... 6 more entries
```

Define one entry with a size profile:

```json
// NEW: Single entry with size profile
{
  "test_name": "insert",
  "operation": "insert",
  "size_profile": "batch_standard"
}
```

The runner automatically generates: `insert_single`, `insert_10`, `insert_100`, `insert_500`, `insert_1000`, `insert_5000`, `insert_10000`, `insert_50000`, `insert_100000` with appropriate iterations.

### Naming Convention

- **Batch operations**: Uses `_single` for size=1, `_N` for other sizes (e.g., `insert_single`, `insert_100`)
- **Dataset operations**: Uses `_N` suffix (e.g., `select_100`, `select_1000`)

### Size Profile Definitions

Defined in `benchmarks/sizes.cppm` (`storm_benchmark_sizes` module):

```cpp
namespace storm::benchmark::sizes {
    inline constexpr std::array BATCH_STANDARD = {1, 10, 100, 500, 1000, 5000, 10000, 50000, 100000};
    inline constexpr std::array BATCH_INSERT_EDGE = {248, 249, 250};
    inline constexpr std::array BATCH_UPDATE_EDGE = {198, 199, 200};
    inline constexpr std::array DATASET_STANDARD = {100, 1000, 10000, 100000};
    inline constexpr std::array DATASET_SMALL = {1000, 10000};

    constexpr int iterations_for_batch(int size);    // Returns iterations based on batch size
    constexpr int iterations_for_dataset(int size);  // Returns iterations based on dataset size
}
```

## 🚀 Usage

### Build the Benchmark System

**⚠️ CRITICAL: ONLY use Release builds for benchmarks!**

```bash
cmake --preset ninja-release -DENABLE_BENCH=ON
cmake --build --preset ninja-release
```

**Why Release-Only:**
- **Debug builds are 10-100x slower** - Results are meaningless
- **No `-O3` optimization** - No inlining, loop unrolling, or dead code elimination
- **Cannot detect real-world performance** - All measurements will be wrong
- **Benchmark comparisons invalid** - Cannot compare with other systems

**DO NOT use debug builds for benchmarking!**

`storm_bench` is a stock Google Benchmark executable — all flags are gbench's own
(`--benchmark_filter`, `--benchmark_repetitions`, `--benchmark_min_time`,
`--benchmark_format`, `--benchmark_list_tests`, `--help`, etc.). See
[Google Benchmark's user guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md)
for the full flag reference.

### List Available Tests

```bash
./build/release/benchmarks/storm_bench --benchmark_list_tests=true
```

Lists every registered gbench name, e.g. `Storm/WHERE/where_int_comparison_gt/N:10000`,
`Raw/INSERT/insert/N:1000`. Combine with `--benchmark_filter` to preview a subset:

```bash
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/SELECT/.*' --benchmark_list_tests=true
```

### Run All Benchmarks

```bash
# Run every test defined in benchmark_tests.yaml (auto-converted to JSON at build time)
./build/release/benchmarks/storm_bench
```

Each registered test runs as a normal gbench benchmark: gbench itself controls repetition
count, minimum run time, and statistical aggregation (mean/median/stddev/CV) — see
`--benchmark_repetitions`, `--benchmark_min_time`. There is no custom `--quick`/`--thorough`
mode; use gbench's own knobs to trade off runtime vs. precision, e.g.:

```bash
# Fast smoke test — short min-time, no repetitions
./build/release/benchmarks/storm_bench --benchmark_min_time=0.2s

# High-confidence run for a regression decision
./build/release/benchmarks/storm_bench --benchmark_repetitions=10 --benchmark_min_time=1s
```

### Run Benchmarks by Category

Every registered benchmark name follows `<Storm|Raw>/<CATEGORY>/<test_name>[/N:<size>]`, where
`<CATEGORY>` is the YAML `category:` field (see [Size Profiles](#-size-profiles) below for the
full category list). Filter with a regex anchored to that shape:

```bash
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/SELECT/.*'         # All SELECT-family tests
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/INSERT/.*'         # All INSERT tests
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/WHERE/.*'          # All WHERE-operator tests
./build/release/benchmarks/storm_bench --benchmark_filter='.*/insert/N:100$'        # One specific size, both sides
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/SELECT_JOIN/.*'    # SELECT + INNER JOIN only
```

Both `Storm/...` and `Raw/...` benchmarks (the raw-SQLite comparison side) match the same
`--benchmark_filter` regex, so filtering by category naturally runs both sides together.

### Custom Repetitions / Timing

```bash
# Override how many times gbench repeats each benchmark (for tighter stddev)
./build/release/benchmarks/storm_bench --benchmark_repetitions=20

# Machine-readable output for scripting / diffing
./build/release/benchmarks/storm_bench --benchmark_format=json --benchmark_out=current.json
```

See `benchmarks/scripts/compare_against_baseline.sh` (documented above under
[Regression detection](#-regression-detection)) for the supported comparison workflow — it
wraps exactly this JSON output.

## 🔧 How It Works

### 1. Compile-Time YAML→JSON Test Definitions

`benchmarks/tests/benchmark_tests.yaml` is the human-friendly source of truth. It is converted
to JSON at build time (`scripts/yaml_to_json.py`) and loaded into the binary at **compile time**
via C++26 `#embed`:

```cpp
// In parser.cppm (storm_benchmark_parser module)
constexpr const char* BENCHMARK_JSON =
#embed "tests/benchmark_tests.json"
;

// Parse JSON into C++ structs at compile time
constexpr auto BENCHMARK_TESTS = parse_benchmark_tests();
```

### 2. Registration Bridges Compile-Time Data to Google Benchmark

`register.cpp` walks `BENCHMARK_TESTS` (the `#embed`-parsed array) at program startup and builds
a `std::vector<RegisteredBenchmark>` — a POD-only table (`bench_register.h`) of `{name, setup,
run, sized, args, range_lo, range_hi, range_multiplier}`. `main.cpp` (the actual gbench entry
point) then calls `benchmark::RegisterBenchmark(name, ...)` once per entry, wiring each into
gbench's own `Range()`/`Arg()`/`Complexity(benchmark::oN)` machinery so gbench drives repetition,
timing, and dataset-size sweeps:

```cpp
// main.cpp — one real gbench registration per compile-time-known test
auto* bench_reg = benchmark::RegisterBenchmark(reg.name, [reg](benchmark::State& state) {
    reg.setup(state.range(0));
    for (auto _ : state) {
        reg.run();
    }
    state.SetComplexityN(state.range(0));
});
if (reg.sized) {
    bench_reg->RangeMultiplier(reg.range_multiplier)->Range(reg.range_lo, reg.range_hi)
              ->Complexity(benchmark::oN)->ArgName("N");
}
```

Each test's operation dispatch (WHERE / JOIN / aggregate / INSERT / ...) is still resolved at
**compile time** via `if constexpr` inside the fixture classes (`query_benchmark.cppm`,
`crud_benchmark.cppm`) — so the per-test hot loop has zero runtime branching on operation type.
What changed vs. the pre-#235 system is the *outer* layer: Google Benchmark now owns filtering,
repetition, timing, and statistical aggregation instead of a custom runner.

### 3. Field Dispatch Using Reflection

Field names in YAML are mapped to struct members at compile time using C++26 reflection:

```cpp
template<typename Model>
constexpr auto dispatch_field(std::string_view field_name) {
    constexpr auto members = members_of(^^Model);
    template for (constexpr auto member : members) {
        constexpr auto name = display_name_of(member);
        if (field_name == name) {
            return member;  // Return reflection info for field
        }
    }
}
```

## 📊 Adding New Benchmarks

### Step 1: Add Test Definition to YAML

Edit `benchmarks/tests/benchmark_tests.yaml` (the human-friendly source of truth):

**Option A: Fixed-size test** (iterations and size specified directly)

```yaml
  - name: where_string_like
    category: WHERE
    description: "WHERE with LIKE pattern (name LIKE 'Person%')"
    model: Person
    operation: where
    where_field: name
    where_op: LIKE
    where_value_string: "Person%"
    iterations: 1000
    init_dataset_size: 10000
```

**Option B: Size profile test** (auto-iterates over multiple sizes via gbench `Range()`/`Arg()`)

```yaml
  - name: select_custom
    category: SELECT_CUSTOM
    description: "Custom SELECT operation with size iteration"
    model: Person
    operation: select_custom
    size_profile: dataset_standard
```

This registers a single gbench benchmark (`Storm/SELECT_CUSTOM/select_custom`) with
`Range(100, 100000)` — gbench itself expands the sweep and reports one row per size
(`.../N:100`, `.../N:1000`, ...), rather than one YAML entry per size.

> **Note:** The YAML file is automatically converted to JSON during build via
> `scripts/yaml_to_json.py`. The C++ parser reads the generated JSON using `#embed`.

### Step 2: Wire it into the right fixture

If the operation fits the SELECT family (WHERE / JOIN / aggregates / DISTINCT / GROUP BY / etc.),
`benchmarks/query_benchmark.cppm` already dispatches via `if constexpr` on the test's flags — you
typically only edit YAML and the matching `if constexpr` branch in `QueryBenchmark::build_qs` /
`run_once`.

CRUD operations (insert / update_pk / delete_pk) live in `benchmarks/crud_benchmark.cppm`. Adding
a new CRUD-shaped operation means a new branch in `CrudBenchmark::run_once` plus a name match in
`register.cpp::is_crud`.

For genuinely new operation families, add a fixture module alongside `query_benchmark.cppm` /
`crud_benchmark.cppm`, import it from `register.cpp`, and have `register_all<>()` route the right
tests to it.

### Step 3: Rebuild and Run

```bash
cmake --build --preset ninja-release
./build/release/benchmarks/storm_bench --benchmark_filter='Storm/<category>/<name>'
```

Google Benchmark picks up the new entry automatically — `register.cpp` walks `BENCHMARK_TESTS` at
startup and registers a real gbench benchmark per test.

## 📊 Storm ORM vs Raw SQLite Comparison

Every applicable benchmark runs **two versions** of the same operation, registered as sibling
gbench benchmarks (`Storm/<category>/<name>` and `Raw/<category>/<name>`):

1. **Storm ORM**: Using the full ORM abstraction layer
2. **Raw SQLite**: Direct SQLite API calls with manual object construction

This provides an **apples-to-apples comparison** to measure the actual overhead of the ORM layer.

### Fair Comparison Methodology

**Both versions do the same work:**
- **Prepare statement once** (outside the timed loop — realistic production usage)
- Execute the same SQL query with statement reuse
- Bind the same parameters
- Extract all column values from result rows
- Construct C++ objects from the data, using the same container (`plf::hive`) Storm uses

The `storm_anchors` binary (documented under [Live dashboard](#-live-dashboard) above) pins a
small, intentional subset of these raw comparisons for release-time spot checks — see that
section for the exact schema/row-shape fairness rules (#68) and the current pinned benchmark
names.

**What the efficiency % tells you** (per CLAUDE.md's Performance Guidelines, target ≥95%):
- **≥100%**: Storm ORM is **faster** (statement caching, compile-time SQL generation)
- **90-99%**: Storm ORM has **minimal overhead** (excellent)
- **70-89%**: Storm ORM has **acceptable overhead** (good)
- **<70%**: investigate — see [docs/internals/performance/PERFORMANCE.md](../docs/internals/performance/PERFORMANCE.md)

**Why Storm can match or beat raw SQLite:**
- **Statement caching**: prepared statements reused across calls (Connection-level, see
  [STATEMENT_CACHING.md](../docs/internals/architecture/STATEMENT_CACHING.md))
- **SQL string caching**: compile-time SQL generation, no runtime concatenation
- **Inline optimizations**: compiler can fully inline ORM templates across module boundaries
  (see [MODULE_SYSTEM.md](../docs/internals/architecture/MODULE_SYSTEM.md))
- **Thread-local caching**: per-thread SQL caches eliminate contention

For current, verified efficiency numbers (not stale point-in-time snapshots), run the relevant
category locally — see [Run Benchmarks by Category](#run-benchmarks-by-category) — or check the
job summary of the latest `Bench Ratio` workflow run described under [Regression detection](#-regression-detection).

## 📝 YAML Test Schema

See `benchmarks/tests/benchmark_tests.yaml` for the full set of fields in use; the shape below
covers the common ones (not every operation uses every field):

```yaml
  - name: unique_test_identifier
    category: WHERE               # groups the gbench name: Storm/<category>/<name>
    description: "Human-readable description"
    model: Person
    operation: where               # dispatch key read by query_benchmark.cppm / crud_benchmark.cppm
    iterations: 1000               # only meaningful where gbench doesn't own repetition
    init_dataset_size: 10000       # rows pre-seeded before the timed loop
    batch_size: 1                  # CRUD operations: rows per batch (size_profile can override)
    size_profile: dataset_standard # OR explicit size — see Size Profiles above

    # WHERE-family fields
    where_field: age
    where_op: ">"                  # >|<|>=|<=|==|!=|LIKE|BETWEEN|IN
    where_value_int: 30
    where_value_string: "pattern"
    where_in_values: [25, 30, 35]

    # LIMIT/OFFSET
    limit_value: 100
    offset_value: 0

    # ORDER BY
    order_by_field: salary
    order_by_direction: DESC        # ASC|DESC
```

Valid `category` values are exactly the `category:` entries actually used in
`benchmarks/tests/benchmark_tests.yaml` — grep that file rather than trusting a fixed enum here,
since new categories are added as new operation families land.

## 🎯 Future Enhancements

See [GitHub Issues (benchmarks)](https://github.com/spiritEcosse/storm/issues?q=is%3Aissue+is%3Aopen+label%3Abenchmarks) for planned features and improvements.

## 🔍 Troubleshooting

### "No tests executed"

**Cause:** YAML file is empty or malformed, or JSON generation failed.

**Solution:**
1. Check `benchmarks/tests/benchmark_tests.yaml` is valid YAML and contains at least one test definition
2. Verify JSON was generated: `ls benchmarks/tests/benchmark_tests.json`
3. Manually regenerate: `python3 benchmarks/scripts/yaml_to_json.py`

### Compilation error: "field not found"

**Cause:** YAML references a field name that doesn't exist in the model struct.

**Solution:** Ensure `where_field` matches an exact field name in the model (case-sensitive):

```cpp
struct Person {
    int id;
    std::string name;  // Use "name" in YAML, not "Name"
    int age;           // Use "age" in YAML
};
```

### Benchmark runs but shows 0 operations / 0 items processed

**Cause:** Query returned no results or operation failed silently.

**Solution:** Check that test data was inserted correctly and WHERE conditions match some rows.

## Profiling and Performance Debugging

The custom `timing.hpp` / `timing_trace.hpp` macros from the pre-Google-Benchmark runner are gone. Use the tools Google Benchmark and the OS already give you:

- **Per-benchmark counters** — `state.counters["sql_build_ns"] = ...;` inside a fixture body. Renders inline in the report and works under `--benchmark_format=json` for diffs.
- **`perf` integration** — `perf record -- ./build/release/benchmarks/storm_bench --benchmark_filter='Storm/INSERT/insert/249'` then `perf report` for hot-function profiling. Use `--benchmark_min_time=2s` to give `perf` enough samples per fixture.
- **Hardware counters from gbench** — build with `-DBENCHMARK_ENABLE_LIBPFM=ON` then `--benchmark_perf_counters=CYCLES,INSTRUCTIONS,CACHE-MISSES`.
- **Narrowing in on a single op** — combine `--benchmark_filter` with `--benchmark_repetitions=1 --benchmark_min_time=0.05s` to make the run cheap enough for `perf stat` round-trips.

When you need ad-hoc per-call timing inside a Storm code path, drop a local `auto t = std::chrono::steady_clock::now(); ...; std::cerr << ...;` into the file under investigation and remove it before committing — there is no longer a project-wide tracing macro.

## 📚 Related Documentation

- **Performance guidelines:** [../docs/internals/performance/PERFORMANCE.md](../docs/internals/performance/PERFORMANCE.md)
- **Storm ORM architecture:** [../docs/internals/architecture/](../docs/internals/architecture/)
