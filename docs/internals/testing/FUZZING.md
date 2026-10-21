# FUZZING.md — libFuzzer Stress Testing

Storm uses **libFuzzer** (built into the custom Clang at `../clang-p2996/`) to stress-test the **runtime SQL binding and execution layer** — the only part of the ORM that can produce crashes or undefined behaviour at runtime. Compile-time SQL generation is safe by construction.

## What Is Fuzzed

| Harness | What it tests |
|---------|--------------|
| `fuzz_where_string` | String WHERE filters: `==`, `!=`, `LIKE`, `BETWEEN` |
| `fuzz_where_int` | Integer WHERE filters: `==`, `!=`, `>`, `<`, `IN`, `BETWEEN` |
| `fuzz_like_pattern` | LIKE patterns with adversarial input (null bytes, `%`, `_`, quotes, unicode) |
| `fuzz_batch_insert` | Batch insert sizes around the SQLite 999-parameter limit |
| `fuzz_connection_string` | Malformed connection strings passed to `DefaultConnection::set()` |

### How harnesses work

Each harness receives a random byte sequence from libFuzzer and uses it as **runtime parameter values** — the SQL template itself never changes. This is intentional: Storm's compile-time SQL generation cannot produce invalid SQL at runtime, so only the runtime binding layer (parameter values, batch sizes, connection strings) is worth fuzzing.

| Harness | What varies each iteration |
|---------|---------------------------|
| `fuzz_where_string` | Fuzz bytes → `std::string` bound to `==`, `!=`, `LIKE` WHERE parameters |
| `fuzz_where_int` | Fuzz bytes → two `int` values (via `memcpy`) bound to `==`, `!=`, `>`, `<`, `IN`, `BETWEEN` |
| `fuzz_like_pattern` | Fuzz bytes → `std::string` LIKE pattern (null bytes, `%`, `_`, quotes, unicode) |
| `fuzz_batch_insert` | Input **size** (mod 1100) → batch count; byte **values** → row field values; probes around the SQLite 999-param boundary |
| `fuzz_connection_string` | Fuzz bytes → raw connection string passed to `DefaultConnection::set()` |

SQL errors (e.g. malformed patterns) are **expected and caught** inside every harness. Only crashes or sanitizer reports (ASAN/UBSAN) indicate real bugs.

## Build

```bash
cmake --preset ninja-fuzz

# First build: run serially to warm the module cache (avoids a known
# clang-scan-deps race when the module-cache/ directory is empty)
cmake --build --preset ninja-fuzz -- -j 1

# Subsequent builds: full parallelism is safe once the cache is warm
cmake --build --preset ninja-fuzz
```

Binaries are placed in `build/fuzz/fuzz/`.

### Design notes

The fuzz preset compiles **both the storm library and fuzz harnesses** with
`-fsanitize=address,fuzzer-no-link`. Using the same compilation flags for all
targets is required to keep module cache entries under a single hash — if the
library and harnesses had different flags, clang would create two hash
subdirectories for built-in modules (`_Builtin_stddef`, etc.) in the same cache
directory and emit a fatal "defined in both" error.

Only the **link step** for harnesses uses `-fsanitize=address,fuzzer` (which
pulls in the libFuzzer runtime and replaces `main()`). The storm library
(`libstorm.a`) uses `-fsanitize=address` at link time only.

## Run a Harness

### Quick smoke test (100 iterations)

```bash
./build/fuzz/fuzz/fuzz_where_string     -runs=100
./build/fuzz/fuzz/fuzz_where_int        -runs=100
./build/fuzz/fuzz/fuzz_like_pattern     -runs=100
./build/fuzz/fuzz/fuzz_batch_insert     -runs=50
./build/fuzz/fuzz/fuzz_connection_string -runs=100
```

### Run all harnesses at once

```bash
mkdir -p corpus/where_string corpus/where_int corpus/like_pattern corpus/batch_insert corpus/connection_string crashes

for harness in fuzz_where_string fuzz_where_int fuzz_like_pattern fuzz_batch_insert fuzz_connection_string; do
    ./build/fuzz/fuzz/$harness corpus/${harness#fuzz_}/ -artifact_prefix=crashes/ -max_total_time=60
done
```

libFuzzer saves interesting inputs to the per-harness corpus directories automatically. Subsequent runs resume from the saved corpus for faster coverage.

> **Note:** libFuzzer may write binary-named corpus files directly to the working directory in addition to the specified corpus subdirectory. These files are not gitignored by pattern (binary names are unpredictable), so run `git clean -f` from the project root after a fuzz session to remove them. The `corpus/` and `crashes/` directories are gitignored and are not affected.

### Time-bounded fuzzing (recommended for development)

```bash
./build/fuzz/fuzz/fuzz_where_string -max_total_time=60
```

### Corpus-based fuzzing (finds more bugs over time)

```bash
mkdir -p corpus crashes

# Run with corpus directory — libFuzzer saves interesting inputs automatically
./build/fuzz/fuzz/fuzz_where_string corpus/ -artifact_prefix=crashes/ -max_total_time=300
```

On subsequent runs libFuzzer resumes from the saved corpus:

```bash
./build/fuzz/fuzz/fuzz_where_string corpus/ -artifact_prefix=crashes/
```

## Reproduce a Crash

When libFuzzer finds a crash it writes a file like `crash-<sha1hash>`. Reproduce it with:

```bash
./build/fuzz/fuzz/fuzz_where_string crash-<hash>
```

The ASAN stack trace points directly to the bug:

```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow
    #0 0x... in storm::orm::... storm/src/orm/statements/select.cppm:42
    #1 0x... in LLVMFuzzerTestOneInput fuzz/fuzz_where_string.cpp:38
```

## LeakSanitizer (LSAN)

LSAN is included automatically with ASAN (`-fsanitize=address`) — no extra flags are needed to enable it.

### Per-mutation vs at-exit checks

libFuzzer normally calls `__lsan_do_recoverable_leak_check()` **after every mutation** to catch leaks early. However, it automatically disables per-mutation checks when the target accumulates memory in global state across iterations — for example, a persistent SQLite connection or statement cache that lives for the whole process lifetime. From libFuzzer's perspective that memory looks like a growing leak after each iteration, but it is intentionally retained. libFuzzer backs off to avoid false positives and prints:

```
INFO: libFuzzer disabled leak detection after every mutation.
      Most likely the target function accumulates allocated
      memory in a global state w/o actually leaking it.
```

**LSAN at process exit still runs** and is a genuine check. It fires once after all iterations complete.

`fuzz_batch_insert` always triggers this message because its global DB connection and statement cache hold memory throughout the run. The other harnesses may also trigger it depending on the corpus.

### Why exit-only LSAN is still effective

Any per-insert memory leak accumulates across all iterations and becomes large and obvious at exit. For example, a 100-byte leak per insert × 50,000 runs = ~5 MB reported at shutdown. The only blind spot would be a leak that frees itself before process exit, which is pathological.

### Running LSAN explicitly

```bash
# Run all harnesses with exit-time LSAN using saved corpus (fast — reuses prior coverage)
for harness in fuzz_where_string fuzz_where_int fuzz_like_pattern fuzz_batch_insert fuzz_connection_string; do
    ASAN_OPTIONS=detect_leaks=1:leak_check_at_exit=1 \
        ./build/fuzz/fuzz/$harness corpus/${harness#fuzz_}/ -runs=50000
done
```

A clean run produces no `LeakSanitizer` output. Any leak prints a stack trace and exits with a non-zero code.

## Interpreting ASAN Output

| Report type | Meaning |
|-------------|---------|
| `heap-buffer-overflow` | Read/write past the end of a heap allocation |
| `stack-buffer-overflow` | Read/write past a stack array |
| `use-after-free` | Access to memory after it was freed |
| `heap-use-after-free` | Same as above, on the heap |
| `SEGV` | Null or wild pointer dereference |

Stack frames show `file:line` for all code compiled with debug info (`-g`). The fuzz preset enables `-g` on all harnesses.

## Fuzz Targets Are Excluded From ctest

The fuzz harnesses are **not registered with CTest**. They will not appear in `ctest --preset ninja-debug -N` and will not be run by the regular test suite.

## Adding a New Harness

1. Create `fuzz/fuzz_<name>.cpp` following the pattern in existing harnesses:
   - `LLVMFuzzerInitialize` — one-time DB setup
   - `LLVMFuzzerTestOneInput` — exercise one fuzz path; catch all exceptions
2. Add `add_fuzz_harness(fuzz_<name>)` to `fuzz/CMakeLists.txt`
3. Document it in the table at the top of this file

## References

- [libFuzzer documentation](https://llvm.org/docs/LibFuzzer.html)
- [AddressSanitizer documentation](https://clang.llvm.org/docs/AddressSanitizer.html)
- [docs/development/SANITIZERS.md](SANITIZERS.md) — ASAN/TSAN flag details for Storm
