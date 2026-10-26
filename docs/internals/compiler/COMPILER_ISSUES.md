# Compiler Issues and Workarounds

Storm ORM uses experimental Clang with C++26 reflection. Here are known issues and workarounds.

## Compiler Requirements

- **Clang fork**: clang-p2996 with reflection support
- **Location**: `../clang-p2996/`
- **Custom libc++**: With reflection support
- **Module scanning**: clang-scan-deps
- **Flags**: `-freflection -fannotation-attributes`

## Known Issues

### 1. Module Cache Corruption

**Symptom**: `module '_Builtin_stdint' is defined in both [same_path] and [same_path]`

**Cause**: Module cache corruption with C++26 modules + GoogleTest + custom libc++

**Workaround**: Simply run the build command again - second attempt usually succeeds

```bash
ninja storm_tests  # May fail
ninja storm_tests  # Will succeed
```

**Nuclear option**:
```bash
rm -rf ~/.cache/clang/ModuleCache
ninja storm_tests
```

### 2. std::mutex in Modules

**Status (since #326 `import std;` migration)**: `std::mutex` /
`std::condition_variable` work in a module purview when supplied by `import std;`.
`src/db/pool.cppm` uses both directly and is validated under TSAN (1924/1924, zero
data races) and ASAN/UBSAN. The earlier textual `#include <mutex>` /
`#include <condition_variable>` in the global module fragment were dropped — the
types now come from the std module.

**Historical symptom** (pre-#326, header-unit world): the compiler could crash
when `std::mutex` was pulled in via a header-unit `import <mutex>;`. If you hit a
mutex-related compiler crash, prefer `import std;` over a header-unit import, and
keep mutex-using code in a module purview rather than a textual header included
after `import std;` (see Finding B in §9).

### 3. std::inplace_vector Not Available

**Symptom**: `header file <inplace_vector> cannot be imported`

**Workaround**: Use `std::array` with manual index tracking

**Example**:
```cpp
// Instead of: std::inplace_vector<T, N>
std::array<T, N> data;
size_t size = 0;
```

### 4. C Headers Cannot Be Imported

**Symptom**: `header file <cassert> cannot be imported because it is not known to be a header unit`

**Workaround**: Include C headers in module preamble (before `export module`)

**Example**:
```cpp
#include <cassert>    // Before export module
#include <cstring>

export module storm_orm_statements_base;
```

### 5. Most Vexing Parse with ConstexprString

**Symptom**: `type 'const std::array<char, N>' does not provide a call operator`

**Workaround**: Use braced initialization or direct member access

**Example**:
```cpp
// Wrong: Parentheses interpreted as function declaration
std::string str(array.data(), array.size());

// Right: Braced initialization
std::string str{array.data(), array.size()};

// Right: ConstexprString member access
std::string str{cs.data.data(), cs.len};
```

### 6. Missing Statement Methods

**Symptom**: `no member named 'column_count' in 'storm::db::sqlite::Statement'`

**Workaround**: Use raw SQLite handle via `stmt->handle()`

**Example**:
```cpp
// Instead of: stmt->column_count()
sqlite3_column_count(stmt->handle())
```

### 7. clang-p2996 libc++ Modules Layout Mismatch

**Symptom** (when enabling CMake's `import std;` support):
```
CMake Error: Cannot find source file:
  <LIBCXX_ROOT>/build/share/libc++/v1/std.compat.cppm
```

**Cause**: `<LIBCXX_ROOT>/build/lib/x86_64-…/libc++.modules.json` declares the
sources at `share/libc++/v1/std.cppm`, but the build places them under
`build/modules/c++/v1/`. Plain libc++ installs expose them via `share/`; the
clang-p2996 build directory does not.

**Workaround**: `cmake/libcxx.cmake` creates a symlink
`build/share/libc++/v1 -> build/modules/c++/v1` at configure time. The logic
is idempotent and won't overwrite a real directory if one exists.

**Related**: Issue [#326](https://github.com/spiritEcosse/storm/issues/326)
tracks the staged migration from per-header `import std.<sub>;` to a single
`import std;`.

### 8. CMake Picks libstdc++ Instead of libc++ for `import std`

**Symptom** (on any target with `CXX_MODULE_STD ON`):

```
[2/11] Scanning /usr/include/c++/16.1.1/bits/std.compat.cc for CXX dependencies
[3/11] Scanning /usr/include/c++/16.1.1/bits/std.cc for CXX dependencies
FAILED: CMakeFiles/__cmake_cxx_std_26.dir/usr/include/c++/16.1.1/bits/std.cc.o.ddi
…
/usr/include/c++/16.1.1/bits/std.cc:26:10: fatal error: 'bits/stdc++.h' file not found
```

**Cause**: CMake's `Clang-CXX-CXXImportStd.cmake` runs `clang++
-print-file-name=libstdc++.modules.json` when `CMAKE_CXX_STANDARD_LIBRARY`
isn't explicitly `libc++`. Clang resolves that against the host GCC's
`/usr/lib64/libstdc++.modules.json` and CMake then tries to compile GCC's
`bits/std.cc` with `-nostdinc++` and our libc++ include paths, which breaks
on `bits/stdc++.h`.

Pinning `CMAKE_CXX_STANDARD_LIBRARY` alone is not enough — the variable
isn't always consulted, and `CMAKE_CXX_STDLIB_MODULES_JSON` (CMake 4.2+) is
the direct override.

**Workaround**: top-level `CMakeLists.txt` sets `CMAKE_CXX_STDLIB_MODULES_JSON`
before `project()`, guarded by `if(DEFINED LIBCXX_ROOT)`. The path depends on
the host that built clang-p2996: the Linux/Docker (amd64) build installs under
a target-triple subdirectory
(`build/lib/x86_64-unknown-linux-gnu/libc++.modules.json`); a native
macOS/arm64 build installs flat (`build/lib/libc++.modules.json`), since
`LLVM_ENABLE_RUNTIMES` only creates a per-triple subdirectory when more than
one target is configured. `cmake/clang_p2996_host.cmake` resolves this ONCE
(shelling out to `uname -m`, since `CMAKE_HOST_SYSTEM_PROCESSOR` isn't
resolved yet before `project()`) and is `include()`d by both
`CMakeLists.txt` and `cmake/libcxx.cmake` — the latter's analogous
`LIBCXX_INCLUDE_DIR`/`_storm_libcxx_lib_dir` split reuses the same
resolution rather than re-deriving it.

On the native macOS/arm64 flat layout, `_storm_libcxx_lib_dir` (used for
`-L`/`-rpath`) is clang-p2996's whole LLVM `build/lib` tree, not a narrow
per-triple runtime dir like the Linux layout — a wider link search path, and
it means the resulting binaries link BOTH `@rpath/libunwind.1.dylib` (from
this dir) and macOS's system `libSystem.B.dylib` (which re-exports its own
`_Unwind_*`). This works — clang-p2996's libc++abi loads before libSystem,
so the throw path stays self-consistent — but if exception behavior ever
looks wrong specifically on native macOS builds, the duplicate unwinder is
worth checking first.

**Related**: Issue [#326](https://github.com/spiritEcosse/storm/issues/326).

### 9. `import std;` Migration — Findings & Header Rules

The tree consumes the C++26 named module via a single `import std;` (issues
[#326](https://github.com/spiritEcosse/storm/issues/326) /
[#332](https://github.com/spiritEcosse/storm/issues/332)) instead of per-header
`import <header>;` header units. Four findings govern how std headers interact
with the module.

**Finding A — `import std;` does NOT export `std::meta::`.**
libc++'s `std.cppm` `#include`s `<meta>` internally, but the reflection symbols
(`std::meta::info`, `std::meta::identifier_of`, the `^^` splice, …) are NOT
re-exported across the module boundary (clang-p2996 limitation, same family as
the annotations-lost-across-BMI issue). **Any TU using `std::meta::` MUST still
textually `#include <meta>`.** `import std;` alone is insufficient for reflection
code.

**Finding B — `#include <meta>` (and any textual std header) must come BEFORE the
imports in a non-module TU.** `<meta>` transitively pulls textual libc++ headers
(`<optional>` → `<compare>` → … → `promote.h`). If `import std;` (or `import
storm;`, which transitively imports std) is processed first, the later textual
headers redefine entities the std module already owns
(`error: redefinition of '__promote_t'`). In **module units** this is a non-issue
— the global module fragment is a separate context. In **textual headers /
plain TUs**, put textual std `#include`s before the imports, or drop them and let
`import std;` supply the types.

**Finding C — clang-tidy needs the module BMIs built first.** clang-tidy replays
`build/release/compile_commands.json` but does NOT reconstruct CMake's
`CXX_MODULE_STD` plumbing. An `import std;` TU fails with
`module 'std' not found` (and `@…modmap` not found) unless the release build has
already produced `std.pcm` + the per-TU `.modmap`. `commit.sh` builds the module
BMIs before clang-tidy, detected via the synthesized `__cmake_cxx_std_26` target
in `build/release/build.ninja` (NOT a `-fmodule-file=std=` flag — CMake's
import-std support wires the std module via ninja dyndep, which never appears in
`compile_commands.json`).

**Finding D — sanitizer/instrumentation flags must precede `add_library(storm)`.**
`cmake/sanitizers.cmake` applies `-fsanitize=…` globally via
`add_compile_options()` / `add_link_options()`, which only affect targets created
AFTER the include. The CMake-synthesized `__cmake_cxx_std_26` std-module target
compiles libc++'s `std.cppm`; if it misses the sanitizer flag, its
container-annotation calls (`__sanitizer_annotate_contiguous_container`)
link-fail with an undefined reference. `CMakeLists.txt` therefore includes the
sanitizer plumbing **before** `add_library(${PROJECT_NAME})`. Also: a build dir
configured before the import-std gate landed must be **wiped**, not
reconfigured — `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` is a before-`project()` gate.

#### Which std `#include`s can be folded into `import std;`

| Header class | Rule |
|---|---|
| `<cassert>` | **KEEP** — `assert` is a macro; modules cannot export macros |
| `<meta>` | **KEEP** — `std::meta::` not re-exported (Finding A) |
| POSIX (`<csignal>`, `<sys/*.h>`, `<unistd.h>`, `<termios.h>`, `<poll.h>`) | **KEEP** — not part of `import std;` |
| third-party (`<sqlite3.h>`, `<plf_hive/…>`, `<libpq-fe.h>`, `<uuid.h>`) | **KEEP** — not std |
| pure-library (`<vector>`, `<string>`, `<format>`, `<utility>`, …) | **FOLD where context allows** (see below) |

Folding a pure-library header is only safe per location:

- **GMF of a `.cppm` that already has `import std;` in its purview** → redundant,
  drop it (e.g. `distinct.cppm` dropped `<utility>`; `pool.cppm` dropped
  `<mutex>` / `<condition_variable>`).
- **A `.cppm` GMF whose textual model header needs the type before the imports**
  → **KEEP** (e.g. `crud_benchmark.cppm` / `query_benchmark.cppm` keep `<tuple>`
  because `models.hpp`'s `Indexes<T>::type` is parsed in the GMF, before the
  purview's `import std;`; dropping it triggers the Finding B `__promote_t`
  redefinition).
- **A `.cppm` with NO `import std;`** (`parser`, `registry`, `wire`,
  `socket_server`, `tui` bench modules) → **KEEP textual**. Adding `import std;`
  would force `CXX_MODULE_STD ON` onto the `storm_tests` target (which compiles
  some of these as its own FILE_SET units); these modules deliberately stay
  header-free to avoid that.
- **A textual header `#include`d AFTER `import std;`** (dashboard
  `args/backup/db/events.hpp`, `fuzz/fuzz_models.h`) → **MUST drop** the std
  `#include`s (keeping them re-pulls libc++ headers after the module → Finding
  B). The single consumer (`main.cpp` / the fuzz harness) supplies them via
  `import std;`, with textual `<meta>` placed before its imports where the TU
  uses reflection. `fuzz_models.h` dropped its `<string>`; the harness TUs that
  use `std::meta::` keep textual `<meta>` before `import storm; import std;`.

### 10. `annotation_of_type` Segfaults on BMI-Imported Member Reflections

**Problem**: Calling `std::meta::annotation_of_type<X>(member)` on a reflection
that was **created in another module TU** (e.g. returned by an exported consteval
function and consumed across the BMI boundary) segfaults the compiler (exit 139,
crash in `ExprConstant.cpp` metafunction evaluation). In constraint-satisfaction
contexts the crash can instead surface as a misleading
`error: cannot take the reflection of an overload set`.

**Structural metafunctions are safe** on the same BMI-crossing reflection:
`is_nonstatic_data_member`, `parent_of`, `identifier_of`, `has_identifier` all
work. Only annotation lookup breaks (same family as the annotations-lost-across-BMI
issue, clang-p2996 [#262](https://github.com/bloomberg/clang-p2996/issues/262)).

**Solution**: Re-derive the member from `^^T` locally before reading annotations —
match by identifier, then call `annotation_of_type` on the freshly derived info:

```cpp
// ❌ Crashes when Member crossed a BMI boundary
bool primary = meta::is_primary_member(Member); // scans annotations_of(Member)

// ✅ Safe — annotations read from a locally derived reflection
for (auto m : storm::meta::data_members_of_v<T>) {   // #694: the memoised member list
    if (std::meta::identifier_of(m) == std::meta::identifier_of(Member)) {
        bool primary = meta::is_primary_member(m);
        // ...
    }
}
```

Found in #388: the `FKFieldOf<T, Member>` concept (model_concepts.cppm) constrains
`join<fields::T.field>()` and must work with FK reflections produced by the benchmark
registry module (`storm_benchmark_registry::resolve_fk_field`).

**Since #694 the "locally derived" list can itself come from a BMI.** `data_members_of_v<T>`
is a variable template: when a module instantiates it (the `storm_benchmark_*` shape), its
importers reuse that BMI copy, so the members re-derived from it are BMI-deserialised too.
That path does not crash on the current toolchain:
`tests/schema/test_reflection_cache_bmi.cpp` reads annotations through it (`FKFieldOf`,
`fk_on_delete_action_of`, `max_length_of`, `is_relation_field`) on members the module
already listed. If this section's crash comes back, that test is the canary.

### 11. Explicit Specialization in a Multi-GMF Header — Ambiguous Member Lookup

**Problem**: An explicit specialization of a module-owned class template (e.g.
`template <> struct storm::Indexes<Person>`), placed in a header that is
textually included in **several module TUs' global module fragments** plus a
plain TU that imports those modules, can make member lookup ambiguous:

```
src/orm/indexes.cppm:27:27: error: reference to 'type' is ambiguous
shared/models/person.h:41:11: note: candidate found by name lookup is 'storm::Indexes<Person>::type'
  (identical note repeated once per TU that carries the header)
```

Each GMF inclusion produces its own copy of the specialization; clang-p2996
should merge them as one entity but fails when the specialization's members
carry reflection NTTPs (`Index<^^Person::department, ...>` — same family as
cross-BMI reflection-equality issues, see §10). The failure is **latent**: it
only fires once the import graph gives the consumer TU more than one BMI path
to the primary template. Found in #464: `storm_bench`'s `register.cpp` (textual
`shared/models/person.h` + imports of 3 bench modules whose GMFs also include it)
compiled fine until #458 added a second import path to `storm_orm_indexes`
(`storm → insert → upsert_grammar → indexes`), then broke deterministically.

**Solution**: For model headers consumed by multiple module TUs, do not
explicitly specialize the trait. Declare the index list as a nested typedef
inside the model instead — the constrained partial specialization in
`indexes.cppm` (single owner, exactly one visible declaration) picks it up:

```cpp
// ❌ Explicit specialization — one copy per including TU, fragile merging
template <> struct storm::Indexes<Person> {
    using type = std::tuple<storm::Index<^^Person::department, ^^Person::age>>;
};

// ✅ Nested-typedef opt-in — merged as part of Person itself
struct Person {
    // ... fields ...
    using storm_indexes = std::tuple<storm::Index<^^Person::department, ^^Person::age>>;
};
```

Explicit specializations remain fine in single-TU contexts (test files,
`tools/storm_schema`). Note: since #466 re-enabled the `ninja-release` CI job
(the historical clang-scan-deps crash no longer reproduces with the clang-p2996
build in the pinned CI image), CI compiles `storm_bench` on every PR — a green
CI now does cover this class of bench-only breakage.

### 12. Non-Templated `requires`-Expression Hard-Errors Instead of Evaluating False

**Problem**: A `requires`-expression whose parameter type is a concrete type (not itself a
template parameter of the enclosing declaration) can hard-error instead of evaluating to `false`
when the checked expression is ill-formed, contradicting [expr.prim.req]:

```cpp
// ❌ Hard-errors on this compiler instead of making the requires-expression `false`
static_assert(!requires(storm::UUID u) { fields::SomeModel.some_field.between(u, u); });
```

Found in #609 while pinning a compile-time rejection (`.between()` on a `storm::UUID` column,
constrained via a `requires`-clause per rule 11 above): even though the checked member call fails
overload resolution due to a genuine constraint violation — exactly the case a `requires`-expression
is designed to swallow — clang-p2996 emits the underlying "no matching member function" diagnostic
as a hard error at the `static_assert` site instead of treating the whole expression as `false`.

**Solution**: Wrap the check in a named `concept` parameterized on the operand type, matching the
project's existing `CanWhereBetween`-style idiom (`tests/query/test_fk_column_name_orderby_aggregate_body.h`).
Making the `requires`-expression depend on a template parameter of the concept, rather than a
concrete parameter type, restores proper SFINAE behavior:

```cpp
// ✅ Named concept — the requires-expression is now dependent on V, SFINAE-friendly
template <typename V>
concept CanBetweenUuidId = requires(V v) { fields::SomeModel.some_field.between(v, v); };
static_assert(!CanBetweenUuidId<storm::UUID>);
```

### 13. PCH + `import std;` — Ambiguous String Types, and clang-tidy Crashes

`tests/CMakeLists.txt` precompiles `<gtest/gtest.h>` for `storm_tests` (measured
−1.9 to −2.4 s per TU; the gtest header is 2.2 s of an ~5.8 s per-TU floor that
is 100% frontend — Backend is 0.03 s). Two clang-p2996 defects come with it.

**(a) Ambiguous string types, order-dependent.** Reaching libc++ through the PCH,
completing an entity that lazily deserializes `<__fwd/string.h>` makes
`std::__1::string` visible alongside the `std` module's `std::string`. From that
point on, *every* mention of a string type in the TU is ambiguous:

```cpp
static_assert(sizeof(std::filesystem::path) > 0);   // the trigger
static_assert(sizeof(std::string) > 0);             // error: reference to 'string' is ambiguous
```

Swap the two lines and it compiles. Known triggers: `std::filesystem::path`,
`storm::UUID`. A textual `#include <string>` does **not** help — textually
including the *trigger's* own header does, but that cuts against the `import std;`
convention (Finding 9), so the fix is to opt the TU out:

```cmake
set_property(SOURCE path/to/tu.cpp PROPERTY SKIP_PRECOMPILE_HEADERS ON)
```

Currently opted out: `schema/test_bindable_concept.cpp`,
`schema/test_dialect_concepts.cpp`. This is **not** limited to compile-time-only
TUs — any TU naming a trigger before a string type can hit it. It is always a
hard compile error, never silent.

The module interface units in the target's `FILE_SET` are opted out for a
different reason: the PCH would force gtest's textual libc++ surface in ahead of
their `module;` declaration and bake that into the BMIs every consumer imports.

**(b) clang-tidy SIGSEGVs on a TU compiled with the PCH flags**, once any check
is enabled. Same crash signature as the `benchmarks/schema.cppm` entry in
`scripts/lib/clang_tidy_skiplist.sh`. The crash prints no `: error:` or
`: warning:` line, so a checker counting only those reads it as a clean pass.

This is currently **unreachable**: the PCH is applied only to `storm_tests`, and
`tests/.clang-tidy` disables every check (`Checks: '-*'`, issue #590), so
clang-tidy exits before building an AST. Verified that no `src/` entry carries
the flags (74 files, 0 with PCH). The guards below exist so that re-enabling
checks for `tests/**` cannot silently un-lint the tree. `scripts/run_clang_tidy.sh` therefore strips
`-Xclang -include-pch` / `-include` / `-Winvalid-pch` from a temp copy of
`compile_commands.json` before replaying commands (semantically identical: the
PCH holds only `<gtest/gtest.h>`, which the TU includes textually anyway), and
`--diff` now fails on `PLEASE submit a bug report` instead of reporting success.
**Removing either guard re-breaks the tidy gate silently.**

## Debugging Tips

1. **Clean build**: `rm -rf build/ && cmake --preset ninja-debug`
2. **Clear module cache**: `rm -rf ~/.cache/clang/ModuleCache`
3. **Verbose output**: `cmake --build --preset ninja-debug -v`
4. **Module graph**: `clang-scan-deps --format=p1689 --`

## See Also

- [Module System](../architecture/MODULE_SYSTEM.md) - How modules are organized
- [Adding Features](../building/ADDING_FEATURES.md) - Development workflow
