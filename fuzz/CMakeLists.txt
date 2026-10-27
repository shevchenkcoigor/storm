project(storm_fuzz LANGUAGES CXX)

message(STATUS "Configuring fuzz harnesses")

# Compilation: -fsanitize=address,fuzzer-no-link matches the flags set on the
# storm library in cmake/fuzz.cmake. Identical compilation flags keep the
# harness TUs' module configuration consistent with the library's PCMs.
#
# Linking: -fsanitize=address,fuzzer (no "no-link") pulls in the full libFuzzer
# runtime and replaces main() with LLVMFuzzerTestOneInput-based wrapper — this
# is only applied to the harness executables, not to libstorm.a.
set(FUZZ_COMPILE_FLAGS -fsanitize=address,fuzzer-no-link -g
                       -fno-omit-frame-pointer)

set(FUZZ_LINK_FLAGS -fsanitize=address,fuzzer -g -fno-omit-frame-pointer)

function(add_fuzz_harness name)
  add_executable(${name} ${name}.cpp)
  # import std; migration (issue #326): the harness TUs now `import std;`, so
  # this target must consume the std named module.
  set_target_properties(${name} PROPERTIES CXX_MODULE_STD ON)
  apply_clang_flags(${name})
  # Treat warnings as errors on Storm-owned fuzz code (issue #317).
  target_compile_options(${name} PRIVATE ${FUZZ_COMPILE_FLAGS} -Werror)
  target_link_options(${name} PRIVATE ${FUZZ_LINK_FLAGS})
  target_link_libraries(${name} PRIVATE storm)
  link_sqlite(${name})
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                             "${CMAKE_SOURCE_DIR}/src")
endfunction()

add_fuzz_harness(fuzz_where_string)
add_fuzz_harness(fuzz_where_int)
add_fuzz_harness(fuzz_like_pattern)
add_fuzz_harness(fuzz_batch_insert)
add_fuzz_harness(fuzz_connection_string)
