# Code Formatting

Storm enforces consistent formatting for both C++ and CMake files via automated targets that run as part of the pre-commit hook.

## Tools

| Tool | Applies to | Config |
|------|-----------|--------|
| `clang-format` | `*.cpp`, `*.cppm`, `*.h`, `*.hpp` | `.clang-format` |
| `cmake-format` | `CMakeLists.txt`, `*.cmake` | cmake-format defaults |

The tools are wired into CMake via [StableCoder/cmake-scripts](https://github.com/StableCoder/cmake-scripts) (`formatting.cmake`), which is fetched automatically by CPM on first configure.

### Tool availability

`formatting.cmake` locates each tool with `find_program` at configure time and
creates **no target at all** when one is missing. A missing `cmake-format`
used to surface only later, as `ninja: error: unknown target 'cmake-format'`
when the pre-commit hook reached step 2 — a message that names the ninja
symptom, not the cause (#643). `cmake/format.cmake` therefore does two things
when `cmake-format` is absent:

- emits a configure-time `message(WARNING "cmake-format not found …")`, for a
  manual `cmake --preset`;
- defines a stub `cmake-format` target that prints the same message and fails.

The stub is what the hook relies on (#690): the configure output lands in the
log of whichever step configured `build/debug` — clang-format's, when both
formatters run on a fresh tree — but the build output of the stub always lands
in the cmake-format step's own log. `scripts/tests/test_cmake_format_target.sh`
covers absent (fresh and configured tree) and present; `commit.sh` runs it
when `cmake/format.cmake` or the test is staged. It is not wired into CI.

| Tool | Where it comes from |
|---|---|
| `clang-format` | the clang-p2996 toolchain — `cmake/format.cmake` pins `CLANG_FORMAT_EXE` to `../clang-p2996/build/bin/clang-format` rather than searching `PATH` |
| `cmake-format` | `PATH`. The `docker/ci` image installs it from PyPI (`cmakelang`, version-pinned) into a venv at `/opt/cmakelang`, symlinked to `/usr/local/bin/cmake-format`. There is no Arch/Manjaro or AUR package, and a venv keeps it from colliding with the image's pacman-managed Python packages. |

On a host toolchain (no container), install `cmake-format` yourself or the
hook's cmake-format step cannot run. Match the version `docker/ci/Dockerfile`
pins rather than taking the latest: cmake-format rewrites files in place, so a
different version reformats the whole tree on your first commit. That
Dockerfile is the single source of truth for the version — this page
deliberately does not repeat the number.

## CMake Target Architecture

`cmake/format.cmake` calls two functions from `formatting.cmake`:

```cmake
clang_format(storm-clang-format ${FORMAT_CPP_SOURCES})
cmake_format(storm-cmake-format ${FORMAT_CMAKE_FILES})
```

Each function registers a **named sub-target** and attaches it to a shared **umbrella target**:

| Call | Sub-target | Umbrella target |
|------|-----------|-----------------|
| `clang_format(storm-clang-format ...)` | `storm-clang-format` | `format` |
| `cmake_format(storm-cmake-format ...)` | `storm-cmake-format` | `cmake-format` |

The `storm-` prefix prevents name collisions if multiple sub-projects ever add their own formatting targets to the same umbrella.

Neither target runs at configure time — they are **lazy**: they only execute when explicitly built.

## Running Manually

```bash
# Format all C++ sources
cmake --build --preset ninja-debug --target format

# Format all CMake files
cmake --build --preset ninja-debug --target cmake-format

# Or invoke sub-targets directly
cmake --build --preset ninja-debug --target storm-clang-format
cmake --build --preset ninja-debug --target storm-cmake-format
```

The debug preset must be configured first (`cmake --preset ninja-debug`). The pre-commit hook does this automatically if needed.

## Pre-commit Integration

`commit.sh` runs formatting automatically before every commit. The hook uses smart skipping based on staged files:

| Staged files | clang-format | cmake-format |
|---|---|---|
| No C++ or CMake | skipped | skipped |
| CMake only | skipped | runs |
| C++ (any) | runs | runs if CMake also staged |

After formatting (and after `clang-tidy --fix`), the hook re-stages modified files with `git add -u` so the auto-fixed versions are included in the commit.

## `.clang-format` Settings

Key rules (see `.clang-format` for the full config):

| Setting | Value | Reason |
|---------|-------|--------|
| `BasedOnStyle` | `LLVM` | Baseline |
| `ColumnLimit` | `120` | Wider for template-heavy code |
| `IndentWidth` | `4` | Readability |
| `PointerAlignment` | `Left` | `int* p` style |
| `SortIncludes` | `false` | Preserve module import order |
| `IncludeBlocks` | `Preserve` | Keep `import` / `#include` groups stable |
| `Standard` | `Latest` | Parses C++26 constructs and modules correctly |

## File Scope

`FORMAT_CPP_SOURCES` and `FORMAT_CMAKE_FILES` are collected with `GLOB_RECURSE` from the project root, excluding `build/` and `third_party/` directories. This means formatting covers:

- `src/**` — ORM modules
- `tests/**` — test sources
- `benchmarks/**` — benchmark sources
- `cmake/**` — all CMake modules
- Root `CMakeLists.txt`
