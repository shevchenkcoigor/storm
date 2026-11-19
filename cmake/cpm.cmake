# CPM.cmake bootstrap (#851). cmake/CPM_<version>.cmake is the upstream release
# asset vendored byte for byte, so configuring never fetches it. It is excluded
# from cmake-format (cmake/format.cmake). CPM_SHA256 is its SHA256;
# scripts/ci/check_cpm_vendored.py (CI's script-guards job) fails when the name,
# CPM_DOWNLOAD_VERSION and CPM_SHA256 disagree. To bump the version run
# `scripts/update_cpm.py <version>` and paste the two values it prints
# (docs/internals/building/COMMON_TASKS.md).
set(CPM_DOWNLOAD_VERSION 0.40.5)
set(CPM_SHA256 c46b876ae3b9f994b4f05a4c15553e0485636862064f1fcc9d8b4f832086bc5d)

# CPM caches CPM_DIRECTORY (CACHE INTERNAL) and the vendored file returns early,
# defining nothing, when it differs from its own directory. A build tree
# configured before #851 cached <build>/cmake, where CPM used to be downloaded,
# so drop a value pointing elsewhere.
get_filename_component(_storm_cpm_dir "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
if(DEFINED CACHE{CPM_DIRECTORY} AND NOT CPM_DIRECTORY STREQUAL _storm_cpm_dir)
  unset(CPM_DIRECTORY CACHE)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/CPM_${CPM_DOWNLOAD_VERSION}.cmake)
