find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)

# Disable module scanning for nanobind's own sources (pure C++17, no imports)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# nanobind-static needs Python headers — add BEFORE CPMAddPackage so the target
# inherits them. Use include_directories() (not target) because nanobind creates
# the target internally.
include_directories(SYSTEM ${Python3_INCLUDE_DIRS})

# GIT_SHALLOW FALSE: a depth-1 clone cannot reach a non-tip tag from a cold
# cache (#709).
cpmaddpackage(
  NAME
  nanobind
  GITHUB_REPOSITORY
  wjakob/nanobind
  GIT_TAG
  v2.4.0
  GIT_SHALLOW
  FALSE)

# Build nanobind's static helper library (normally done inside
# nanobind_add_module)
nanobind_build_library(nanobind-static)

# Re-enable for our binding code
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# Build the extension as a regular MODULE library instead of using
# nanobind_add_module(). This ensures _storm gets the EXACT SAME compiler flags
# as the storm library, which is critical: Clang's implicit module cache path is
# a hash of the compiler flags + include paths, so any mismatch creates a
# separate cache and `import storm;` fails.
python3_add_library(_storm MODULE WITH_SOABI bindings.cpp)

# storm static lib needs PIC so its objects can be linked into the shared
# extension.
set_target_properties(storm PROPERTIES POSITION_INDEPENDENT_CODE ON)
# DEFINE_SYMBOL="" prevents CMake from adding -D_storm_EXPORTS, which would
# change the implicit module cache hash vs the storm static library.
set_target_properties(
  _storm
  PROPERTIES CXX_STANDARD 26
             CXX_SCAN_FOR_MODULES ON
             DEFINE_SYMBOL "")
apply_clang_flags(_storm)

# Both targets MUST have identical include paths (including -isystem) for the
# implicit module cache hash to match. Add nanobind+Python includes to storm as
# SYSTEM PRIVATE.
target_include_directories(storm SYSTEM PRIVATE ${Python3_INCLUDE_DIRS}
                                                ${nanobind_SOURCE_DIR}/include)
target_include_directories(_storm SYSTEM PRIVATE ${nanobind_SOURCE_DIR}/include)

target_link_libraries(_storm PRIVATE nanobind-static storm)
link_sqlite(_storm)

set_target_properties(_storm PROPERTIES PREFIX "")

# Copy demo script next to the built extension
configure_file(demo.py "${CMAKE_CURRENT_BINARY_DIR}/demo.py" COPYONLY)

message(STATUS "Python bindings: _storm extension module enabled")
