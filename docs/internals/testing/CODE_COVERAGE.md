# Code Coverage Guide

This guide explains how to generate and analyze code coverage for Storm ORM.

## Quick Start

### Console Summary (Fastest)

```bash
# Configure (one-time, coverage is on by default in ninja-debug)
cmake --preset ninja-debug

# Build and run tests with coverage
cmake --build --preset ninja-debug-coverage --target coverage
```

Output shows filtered coverage with `LCOV_EXCL_*` markers applied:
```
Summary coverage rate:
  source files: 20
  lines.......: 100.0% (5713 of 5713 lines)
  functions...: 66.0% (3730 of 5649 functions)
  branches....: 91.5% (787 of 860 branches)
```

### HTML Report (Detailed)

```bash
cmake --build --preset ninja-debug-coverage --target coverage-html
```

Open `build/debug/coverage/html-filtered/index.html` in browser.

## Available Targets

| Target | Description |
|--------|-------------|
| `coverage` | Run tests + show filtered text summary (with `LCOV_EXCL_*`) |
| `coverage-html` | Run tests + generate filtered HTML report (with `LCOV_EXCL_*`) |
| `coverage-clean` | Clean all coverage data |

## Recommended Workflow

### Development (Quick Check)

```bash
cmake --build --preset ninja-debug-coverage --target coverage
```

### Before Commit (Detailed Review)

```bash
# Generate HTML to review uncovered lines
cmake --build --preset ninja-debug-coverage --target coverage-html

# Open in browser
xdg-open build/debug/coverage/html-filtered/index.html  # Linux
open build/debug/coverage/html-filtered/index.html      # macOS
```

### CI Pipeline

The `coverage` job in `.github/workflows/ci.yml` runs on every PR and nightly on
`develop` (not on the post-merge push — #700). It builds `ninja-debug`, runs the `coverage` target, and **fails the
job below 100% line coverage** — the same gate `commit.sh` applies locally, so
the threshold is no longer self-reported from one developer's machine (#528).

The job parses the **line** row of the lcov summary specifically. Function
coverage (~81%) and branch coverage (~92%) are not gated; anchoring on the wrong
row would fail immediately and look like a broken job rather than a wrong
threshold.

On both pass and failure it uploads the HTML report as a `coverage-html`
workflow artifact (14-day retention). On failure it also prints the uncovered
file/line list directly in the job log.

No external coverage service is involved. Publishing to SonarCloud requires
switching that project from Automatic Analysis to a CI-based scan — deliberately
deferred, tracked at #457.

```bash
# The filtered lcov file the job parses:
# build/debug/coverage/coverage-filtered.lcov
```

### A running PostgreSQL is required

**The 100% gate cannot be met without a live PostgreSQL server** — locally or in
CI. This is not an optimization or a nice-to-have.

Three `constexpr` transaction-nesting methods on the PG `Connection`
(`postgresql_connection.cppm` — `in_transaction`, `enter_transaction`,
`leave_transaction`) and part of `pool.cppm` are instantiated only by a live
connection. `tests/mock_libpq/` does not reach them. With PG absent the tree
measures **~99.8-99.9%**, and function coverage drops from **80.6% to 48.7%** —
that second number is the quickest way to recognize this situation.

`scripts/coverage-run-batched.sh` (and `commit.sh`) default `STORM_PG_CONNSTR` to the
local unix socket, chosen per host OS by `scripts/pg-default-connstr.sh`:
`host=/var/run/postgresql` on Linux, `host=/tmp` on macOS (Homebrew PostgreSQL). Any
other OS fails with a message naming it. An already-exported value wins. That is how
the CI coverage job points the tests at its `postgres` service container.

**Which entry point you use matters.** The `ninja-debug-coverage` *build preset*
nulls `STORM_PG_CONNSTR`, so the documented local command —
`cmake --build --preset ninja-debug-coverage --target coverage` — always gets the
script's default, whatever your shell exports. That is deliberate: it keeps the
local number reproducible instead of varying with each developer's environment.
To point local coverage somewhere else (a Docker PG on another port, say),
invoke the script directly:

```bash
STORM_PG_CONNSTR="host=localhost port=5433 dbname=storm_db user=storm_db" \
  ./scripts/coverage-run-batched.sh build/debug
```

In CI the preset's null does *not* strip the step-level value — it only clears an
inherited shell variable at preset-expansion time — which is why the service
container is reachable.

Note that `STORM_PG_CONNSTR=""` is **not** a way to disable PostgreSQL: libpq
reads an empty string as "use the `PG*` environment defaults", so it connects
anyway on a machine with `PGHOST`/`PGUSER` exported. To genuinely force
SQLite-only, clear those too (`env -u STORM_PG_CONNSTR -u PGHOST -u PGUSER …`) —
which fails the 100% gate by design.

This default previously lived in `cmake/coverage-targets.cmake` as an
unconditional override, which made the gate silently environment-dependent: it
passed only on a machine that happened to run PG at that socket, and failed for
every contributor and for CI without it. That is precisely the self-reported gate
#528 exists to eliminate.

**If coverage fails just under 100% and you expected 100%, check that PostgreSQL is
running before looking at your code.**

## Excluding Code from Coverage

Use `LCOV_EXCL_*` markers to exclude compile-time only code:

```cpp
// LCOV_EXCL_START - compile-time only
consteval auto build_sql() {
    // This code runs at compile-time, not runtime
    return ConstexprString{"SELECT * FROM table"};
}
// LCOV_EXCL_STOP

// Single line exclusion
constexpr auto value = compute_value(); // LCOV_EXCL_LINE
```

### Supported Markers

| Marker | Description |
|--------|-------------|
| `LCOV_EXCL_START` | Begin excluded block |
| `LCOV_EXCL_STOP` | End excluded block |
| `LCOV_EXCL_LINE` | Exclude single line |
| `LCOV_EXCL_BR_START` | Begin branch exclusion |
| `LCOV_EXCL_BR_STOP` | End branch exclusion |
| `LCOV_EXCL_BR_LINE` | Exclude branch on single line |

### How Filtering Works

Storm uses **llvm-cov** (not gcov) for coverage. lcov v2 processes `LCOV_EXCL_*` markers natively via `--filter region,branch_region`, but doesn't recognize `.cppm` files by default. We pass `--rc c_file_extensions=...cppm` so lcov reads our module interface files for exclusion markers.

## Coverage Output Files

```
build/debug/coverage/
├── coverage.lcov              # Raw coverage (before filtering)
├── coverage-filtered.lcov     # Final (paths + LCOV_EXCL filtered)
└── html-filtered/             # HTML report (generated by coverage-html)
    └── index.html             # Open this in browser
```

## Interpreting Results

### Line Coverage

- **100% is required** — enforced by the `commit.sh` pre-commit hook locally and
  by the `coverage` CI job on every PR (#528)
- The figure is 100% of the **filtered** set — 24 files / 8613 lines, after
  `LCOV_EXCL` markers are removed from the denominator (56 markers across 11
  files in `src/`). It is not 100% of every line in the tree
- Compile-time only code (`consteval`) must be excluded with `LCOV_EXCL_*`

### Uncovered Code Categories

1. **Error paths** (`[[unlikely]]`) - Test via mock tests, or exclude with `LCOV_EXCL_LINE`
2. **Compile-time code** (`consteval`, `constexpr`) - Exclude with `LCOV_EXCL_*`
3. **`if constexpr` branches** - Only one branch instantiated per template type, exclude other
4. **Template instantiation artifacts** - Some instantiations instrumented, others not
5. **Dead code** - Remove it
6. **Missing tests** - Add tests

### Function Coverage vs Line Coverage

Function coverage is typically lower than line coverage in Storm due to C++ template instantiation. Each template class (e.g., `EraseStatement<T>`) is instantiated per model type used in tests (`SqlitePerson`, `MockPerson`, `BatchPerson`, etc.). The compiler generates a separate copy of every method for each type, and lcov counts each copy as a distinct "function."

For example, `EraseStatement` has 8 methods × ~9 model types = ~52 "functions" in lcov. If `TxnPerson` tests only exercise single removes (never bulk or chunked), the `EraseStatement<TxnPerson>::execute_bulk` instantiation shows as unhit — even though the identical logic is fully covered via `EraseStatement<SqlitePerson>::execute_bulk`.

**Guideline**: Focus on **line coverage** (required: 100%). Function coverage gaps from unexercised template instantiations are expected and do not indicate missing test coverage.

### Mock Tests

Error handling paths are tested in `tests/mock_sqlite/test_orm_mock_errors.cpp` using `LD_PRELOAD` to inject SQLite failures.

```bash
# Run mock tests separately
cmake --build --preset ninja-debug --target coverage-run-mock
```

## Batched Test Execution

Coverage tests run via `scripts/coverage-run-batched.sh`, which executes each GTest suite
in a separate process. This works around a Clang C++26 coverage segfault that occurs when
running all ~1700 tests in a single process.

**Important**: Suites must NOT be split into individual test runs. Per-test profraw files
lose cross-module template coverage data because profiling counters for template
instantiations across module boundaries require enough test volume within a single process
to be properly recorded. Running a single test produces a profraw that shows 0 for
functions like `having()` and `first()`, but running the full suite correctly records them.

See [#176](https://github.com/spiritEcosse/storm/issues/176) for the investigation details.

### Parallel Batches (`COVERAGE_JOBS`)

Batches run in parallel by default — each suite emits an independent
`batch_${name}.profraw` file, so concurrent processes do not contend on output.

| Setting | Behaviour |
|---|---|
| (unset, default) | `nproc --ignore=2` parallel workers |
| `COVERAGE_JOBS=N` | cap concurrency at N |
| `COVERAGE_JOBS=1` | strictly serial (opt-out for shared hardware or flaky-suite debugging) |

Example: run the coverage stage with at most 4 parallel batches:

```bash
COVERAGE_JOBS=4 cmake --build --preset ninja-debug-coverage --target coverage
```

The env var is passed through to the script by the `coverage-run-main` CMake target, the
pre-commit hook (`commit.sh`), and CI workflows — no further wiring needed.

See [#268](https://github.com/spiritEcosse/storm/issues/268) for the motivation (30-minute
serial pre-commit on dev machines with PostgreSQL enabled).

## Troubleshooting

### Coverage Not Updating

```bash
cmake --build --preset ninja-debug --target coverage-clean
cmake --build --preset ninja-debug
cmake --build --preset ninja-debug-coverage --target coverage
```

### Module Cache Issues

If you get compiler crashes, the module cache may be corrupted:

```bash
rm -rf build/debug
cmake --preset ninja-debug
```

### LCOV_EXCL Markers Not Being Applied

`LCOV_EXCL_*` markers are only processed by the `coverage` target (via lcov `--filter region,branch_region`).
The `coverage-lcov` target is a raw `llvm-cov` export — it does not apply exclusion markers.

Make sure you're running `cmake --build --preset ninja-debug-coverage --target coverage`, not `coverage-lcov`.
