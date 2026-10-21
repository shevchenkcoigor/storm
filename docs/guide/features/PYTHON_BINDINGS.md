# Python Bindings

Nanobind-based Python bindings for the Storm C++26 ORM. Proof of concept wrapping a `PyPerson` model with full CRUD, filtered queries, and NumPy integration.

## Files

- `python/bindings.cpp` — C++26 nanobind binding code
- `python/benchmark.py` — Fair benchmark: raw sqlite3 vs Storm wrapper
- `python/CMakeLists.txt` — Build config (critical module cache alignment)
- `python/demo.py` — Full CRUD demo script
- `python/README.md` — API documentation
- `cmake/python.cmake` — `ENABLE_PYTHON` option gate

## Build

```bash
cmake --preset ninja-python && cmake --build --preset ninja-python
cd build/python/python && python3 demo.py
PYTHONPATH=build/python/python python3 python/benchmark.py
```

Incremental rebuild (bindings.cpp only): **~1.6 seconds**

## Architecture

- **PyPerson** struct hardcoded in C++ with `[[= primary]]`
- **PersonQS** = `storm::QuerySet<PyPerson, storm::db::sqlite::Connection>`
- **consteval helpers** pre-compute `std::array<std::meta::info, N>` of field members
- **`template for` expansion statements** iterate fields at compile time for:
  - WHERE dispatch (`execute_where()`) — runtime field/op strings → compile-time Storm expressions
  - Column proxy registration (`Person.c_age`, `Person.c_name`, `Person.c_id`)

## Performance Optimizations

### Cached Module-Level QuerySet

- `std::optional<PersonQS> g_qs` — avoids per-call QuerySet construction
- Reset on `connect()` via `g_qs.emplace()`
- **Only safe for stateless ops** (insert, count, remove, bulk_insert)
- `select()` and `select_where()` MUST use fresh QS — `where()` mutates in place
- Wrong caching symptom: select_where performance drops 96%

### fast_insert(const char* name, int age)

- Accepts C primitives — skips Python Person object construction
- `const char*` zero-copy from Python buffer (nanobind native)

### fast_insert_many(names: list[str], ages: list[int])

- Loop in C++ — pays nanobind crossing cost once, not N times
- 32% faster than raw sqlite3 for N single inserts

### select_array() — NumPy Zero-Copy

- Returns `np.ndarray` with dtype `[('id','<i4'),('name','S64'),('age','<i4')]`
- Writes directly into numpy buffer via `PyObject_GetBuffer` (CPython buffer protocol)
- Layout: id@0(4) name@4(64) age@68(4) — itemsize=72, no padding
- **3.8x faster than raw sqlite3** for 10k rows

## Benchmark Results (Fair Comparison)

Uses `isolation_level=None` for raw sqlite3 (one commit per insert, matching Storm's behavior).

| Operation | raw sqlite3 | Storm | Ratio |
|---|---|---|---|
| INSERT single (per-call) | 871k ops/s | 832k–941k | **96–108%** |
| INSERT single (C++ loop) | 871k ops/s | 1,148k | **+32%** |
| INSERT bulk 10k | 3.4M rows/s | 8.2M | **+141%** |
| SELECT * 10k → list | 365 iters/s | 729 | **+100%** |
| SELECT * 10k → ndarray | 365 iters/s | 1,413 | **+287%** |
| SELECT WHERE | 530 iters/s | 1,158 | **+118%** |
| COUNT(*) | 709k ops/s | 1,227k | **+73%** |

C++ benchmark confirms **98.9% efficiency** for single insert — all Python overhead is nanobind FFI cost, not the ORM.

## Pitfalls & Lessons

### numpy.dtype() — tuple vs list

- `np.dtype((tuples...))` = `(base_dtype, shape)` — **WRONG** for structured dtype
- `np.dtype([tuples...])` with a **list** — **CORRECT**
- nanobind: use `nb::list` + `.append(nb::make_tuple(...))`, NOT outer `nb::make_tuple`

### nanobind string_view

- `std::string_view` not auto-convertible from Python `str` in nanobind
- Use `const char*` instead — zero-copy, works natively

### Python sqlite3 implicit transactions

- `isolation_level=''` (default) auto-wraps DML in `BEGIN DEFERRED...COMMIT`
- Use `isolation_level=None` (autocommit) for fair per-insert benchmarks
- Without this, raw sqlite3 appears ~40% faster than it really is

## Key Patterns

### Lambda capture inside expansion statements

Variables inside `template for` CANNOT be captured by lambdas. Use a helper lambda outside:

```cpp
auto register_proxy = [&person_cls](const std::string& name) {
    person_cls.def_prop_ro_static(("c_" + name).c_str(), [name](nb::handle) {
        return FieldProxy{name};
    });
};
template for (constexpr auto member : kPyPersonFields) {
    register_proxy(std::string(std::meta::identifier_of(member)));
}
```

### Module cache alignment (CRITICAL)

Clang's implicit module cache hash = compiler flags + include paths. `storm` and `_storm` MUST match:

- `Python3_add_library` instead of `nanobind_add_module()` (full flag control)
- `POSITION_INDEPENDENT_CODE ON` on storm target
- `DEFINE_SYMBOL ""` on _storm (prevents `-D_storm_EXPORTS`)
- Same Python + nanobind include dirs added to BOTH targets

### Pythonic WHERE API

```python
storm.select_where(Person.c_age > 30)        # FieldProxy.__gt__ → FilterExpr
storm.select_where(Person.c_name.like("C%"))  # FieldProxy.like() → FilterExpr
```

`FilterExpr{field_name, op, value}` → `execute_where()` dispatches via expansion statement.

## Phase 2 Discussion (NOT implemented)

- Current PoC requires hardcoded C++ struct — Python users can't define custom models
- **Codegen approach**: Python script generates C++ struct → recompile `.so` (~1.6s)
- **Runtime approach**: Type-erased `DynamicRow` — no recompilation needed
- **Long-term**: Wait for P2996 to merge into mainline LLVM
