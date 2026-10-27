option(ENABLE_FUZZING
       "Build libFuzzer harnesses (requires -fsanitize=address,fuzzer support)"
       OFF)

if(ENABLE_FUZZING)
  # Compile libstorm with the same instrumentation as the fuzz harnesses
  # (-fsanitize=address,fuzzer-no-link) so the BMI hashes match across the
  # library and harnesses. fuzzer-no-link keeps the fuzzer runtime out of
  # libstorm.a — it is linked only into each harness binary (via
  # target_link_options in fuzz/CMakeLists.txt).
  set(FUZZ_INSTRUMENT_FLAGS -fsanitize=address,fuzzer-no-link -g
                            -fno-omit-frame-pointer)
  target_compile_options(storm PRIVATE ${FUZZ_INSTRUMENT_FLAGS})
  target_link_options(storm PRIVATE -fsanitize=address -g
                      -fno-omit-frame-pointer)
  message(STATUS "Fuzzing is enabled.")
  add_subdirectory(fuzz)
else()
  message(STATUS "Fuzzing is disabled.")
endif()
