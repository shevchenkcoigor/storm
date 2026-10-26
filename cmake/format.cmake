set(CLANG_FORMAT_EXE
    "${CMAKE_SOURCE_DIR}/../clang-p2996/build/bin/clang-format"
    CACHE FILEPATH "Path to clang-format" FORCE)

include(${cmake-scripts_SOURCE_DIR}/formatting.cmake)

# CONFIGURE_DEPENDS is intentionally omitted from both globs below. Using it
# causes an infinite CMake re-run loop: the globs match *.cmake files inside
# .cache/CPM/, which CPM populates during configure. Each new package adds files
# there, making the glob result change on every run and triggering another
# re-configure indefinitely. Without CONFIGURE_DEPENDS, adding new source files
# after configure requires a manual cmake re-run to appear in the format targets
# — an acceptable trade-off.

# C++ sources — grab everything, exclude build/third_party
file(GLOB_RECURSE FORMAT_CPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
list(FILTER FORMAT_CPP_SOURCES EXCLUDE REGEX
     ".*/build/.*|.*/third_party/.*|.*/.cache/.*")

# CMake files — grab everything, exclude build/third_party/.cache
file(GLOB_RECURSE FORMAT_CMAKE_FILES
     "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cmake")
list(FILTER FORMAT_CMAKE_FILES EXCLUDE REGEX
     ".*/build/.*|.*/third_party/.*|.*/.cache/.*")

clang_format(storm-clang-format ${FORMAT_CPP_SOURCES})
cmake_format(storm-cmake-format ${FORMAT_CMAKE_FILES})

# cmake_format() above is a no-op when cmake-format is absent: it creates no
# target, so commit.sh's cmake-format step failed with `ninja: error: unknown
# target 'cmake-format'`, naming the ninja symptom rather than the missing tool
# (#643). A configure-time WARNING alone is not enough: it lands in whichever
# commit.sh step configured the tree — clang-format's when both formatters run
# on a fresh tree — not in cmake-format's (#690). So define a stub target that
# fails at BUILD time with the reason, in the output of whoever builds it, and
# keep the WARNING for a manual configure. CMAKE_FORMAT_EXE is set by
# find_program() inside cmake-scripts' formatting.cmake.
if(NOT CMAKE_FORMAT_EXE)
  string(
    CONCAT _cmake_format_missing
           "cmake-format not found — install cmakelang at the version "
           "docker/ci/Dockerfile pins, then re-run cmake. See "
           "docs/internals/building/FORMATTING.md (Tool availability).")
  message(WARNING "${_cmake_format_missing}")
  add_custom_target(
    cmake-format
    COMMAND ${CMAKE_COMMAND} -E echo "${_cmake_format_missing}"
    COMMAND ${CMAKE_COMMAND} -E false
    VERBATIM)
endif()
