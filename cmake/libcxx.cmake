# CMAKE_CURRENT_LIST_DIR (not a plain relative path) so this resolves correctly
# regardless of the including project's source dir — e.g. when
# scripts/ci/tests/test_libcxx_modules_symlink.sh's harness include()s this file
# directly from a throwaway project elsewhere.
include("${CMAKE_CURRENT_LIST_DIR}/clang_p2996_host.cmake")

set(LIBCXX_INCLUDE_DIR "${LIBCXX_ROOT}/build/include/c++/v1")
set(LIBCXX_BUILD_INCLUDE_DIR "${_storm_libcxx_build_include_dir}")
message(STATUS "Using custom libcxx from: ${LIBCXX_ROOT}")

# clang-p2996's libc++.modules.json declares a relative source-path back to
# build/share/libc++/v1/ (Linux: "../../share/..." from build/lib/<triple>/;
# macOS flat layout: "../share/..." from build/lib/ — one fewer ".." since
# there's no triple subdirectory to climb out of). Both conventions resolve to
# the same ${LIBCXX_ROOT}/build/share/libc++/v1, which is why the one symlink
# below serves both layouts unchanged. The build actually places std.cppm /
# std.compat.cppm under build/modules/c++/v1/, not build/share/libc++/v1/, so
# bridge the two with a symlink so CMake's `import std;` support can find the
# sources. See issue #326.
set(_storm_libcxx_modules_dir "${LIBCXX_ROOT}/build/modules/c++/v1")
set(_storm_libcxx_share_parent "${LIBCXX_ROOT}/build/share/libc++")
set(_storm_libcxx_share_link "${_storm_libcxx_share_parent}/v1")
if(EXISTS "${_storm_libcxx_modules_dir}"
   AND NOT IS_SYMLINK "${_storm_libcxx_share_link}"
   AND NOT IS_DIRECTORY "${_storm_libcxx_share_link}")
  file(MAKE_DIRECTORY "${_storm_libcxx_share_parent}")
  file(CREATE_LINK "${_storm_libcxx_modules_dir}" "${_storm_libcxx_share_link}"
       SYMBOLIC)
  message(STATUS "Created libc++ modules symlink: ${_storm_libcxx_share_link}"
                 " -> ${_storm_libcxx_modules_dir}")
endif()

add_compile_options(-nostdinc++ -I${LIBCXX_INCLUDE_DIR})
if(LIBCXX_BUILD_INCLUDE_DIR)
  add_compile_options(-I${LIBCXX_BUILD_INCLUDE_DIR})
endif()

# Reflection flags must be GLOBAL, not per-target. Clang hashes compile flags
# into the module-cache key and also stamps them into every PCM. If the flags
# differ between the producer of a PCM and a consumer that imports it, clang
# refuses to load the PCM with a "configuration mismatch / Reflection was
# disabled in precompiled file" diagnostic.
#
# Putting reflection flags inside apply_clang_flags() (target_compile_options)
# meant they didn't propagate to every PCM producer — most notably any
# auto-generated CMake-internal targets that build .pcm files. Promoting them
# here via add_compile_options() guarantees uniform reflection state across
# every TU and every PCM in the project.
add_compile_options(-freflection -fannotation-attributes -fexpansion-statements)

add_link_options(-nostdlib++ -L${_storm_libcxx_lib_dir}
                 -Wl,-rpath,${_storm_libcxx_lib_dir} -lc++ -lc++abi -lunwind)

function(apply_clang_flags target_name)
  # import std; migration (issue #326): the Clang-modules header-unit flags
  # (-fmodules -fbuiltin-module-map) were removed. They enable the Clang
  # header-unit path, which collides with `import std;` ("export declaration can
  # only be used within a module purview"). Now that the whole tree consumes the
  # std named module instead of `import <header>;` header units, these flags are
  # gone. The function is kept as a no-op so its call sites and the CMake
  # structure stay intact (and a future per-target flag can slot back in here).
  # Reflection flags are already global (see add_compile_options above).
endfunction()
