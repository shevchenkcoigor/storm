# Resolves the on-disk layout of a LIBCXX_ROOT (clang-p2996) build for the
# current host, ONCE, in this single file — the shared source of truth for every
# consumer: - CMakeLists.txt (before project()) derives
# CMAKE_CXX_STDLIB_MODULES_JSON from _storm_libcxx_modules_json. -
# cmake/libcxx.cmake (after project()) derives compile/link flags from
# _storm_libcxx_lib_dir / _storm_libcxx_build_include_dir. -
# scripts/ci/tests/test_libcxx_modules_symlink.py's harness include()s this file
# directly, so its scenarios exercise the real production logic rather than a
# re-implementation of it.
#
# Idempotent: safe to include() more than once per configure (e.g. once from
# CMakeLists.txt, again from cmake/libcxx.cmake) — resolves nothing twice.
if(DEFINED _storm_libcxx_lib_dir)
  return()
endif()

if(NOT DEFINED LIBCXX_ROOT)
  message(
    FATAL_ERROR
      "LIBCXX_ROOT is required. Use a CMake preset or set -DLIBCXX_ROOT=<path>."
  )
endif()

# The Linux/Docker-built clang-p2996 (amd64) installs runtime libs/headers under
# a target-triple subdirectory (build/lib/<triple>,
# build/include/<triple>/c++/v1) — a property of LLVM_ENABLE_RUNTIMES
# configuring more than one target. A native single-target build (e.g.
# macOS/arm64) installs flat, with no triple subdirectory. In practice this
# correlates 1:1 with host OS/arch for the two builds this project supports, so
# branch on host OS/arch — CMAKE_HOST_SYSTEM_PROCESSOR isn't resolved this early
# (before project()), so shell out to `uname -m` instead — and then VALIDATE the
# resolved paths actually exist, so a real mismatch (e.g. a future single-target
# Linux build) fails loudly here instead of silently emitting dead -I/-L paths
# that clang would just ignore. _storm_host_arch may already be set by a caller
# (the test harness forces it, to exercise a host other than the one actually
# running the tests) — only shell out when it isn't.
if(NOT DEFINED _storm_host_arch)
  execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE _storm_host_arch
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _storm_uname_result)
  if(NOT _storm_uname_result EQUAL 0)
    message(
      FATAL_ERROR
        "Failed to run `uname -m` to detect the host architecture (needed to "
        "locate clang-p2996's install layout).")
  endif()
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND _storm_host_arch STREQUAL
                                               "x86_64")
  set(_storm_libcxx_lib_dir "${LIBCXX_ROOT}/build/lib/x86_64-unknown-linux-gnu")
  set(_storm_libcxx_build_include_dir
      "${LIBCXX_ROOT}/build/include/x86_64-unknown-linux-gnu/c++/v1")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND _storm_host_arch STREQUAL
                                                    "arm64")
  # Flat layout: _storm_libcxx_lib_dir is clang-p2996's whole LLVM build/lib
  # tree (~190 component subdirs, 100+ static libs), not a narrow per-triple
  # runtime dir like the Linux layout — a broader -L/-rpath than Linux, but
  # libc++/libc++abi/libunwind still resolve correctly there since that's where
  # this build's LLVM_ENABLE_RUNTIMES step installs them. No separate per-triple
  # include dir exists to add.
  set(_storm_libcxx_lib_dir "${LIBCXX_ROOT}/build/lib")
  set(_storm_libcxx_build_include_dir "")
else()
  message(
    FATAL_ERROR
      "Unsupported host for clang-p2996: ${_storm_host_arch} on "
      "${CMAKE_HOST_SYSTEM_NAME}. Only x86_64 Linux (Docker) and arm64 macOS "
      "(native) clang-p2996 builds are supported.")
endif()

if(NOT EXISTS "${_storm_libcxx_lib_dir}")
  message(
    FATAL_ERROR
      "clang-p2996 layout mismatch: expected runtime lib dir "
      "'${_storm_libcxx_lib_dir}' does not exist under LIBCXX_ROOT="
      "'${LIBCXX_ROOT}'. This host's clang-p2996 build may use a layout "
      "other than the one cmake/clang_p2996_host.cmake expects for "
      "${CMAKE_HOST_SYSTEM_NAME}/${_storm_host_arch}.")
endif()
if(_storm_libcxx_build_include_dir AND NOT EXISTS
                                       "${_storm_libcxx_build_include_dir}")
  message(
    FATAL_ERROR
      "clang-p2996 layout mismatch: expected per-triple include dir "
      "'${_storm_libcxx_build_include_dir}' does not exist under LIBCXX_ROOT="
      "'${LIBCXX_ROOT}'.")
endif()

set(_storm_libcxx_modules_json "${_storm_libcxx_lib_dir}/libc++.modules.json")
