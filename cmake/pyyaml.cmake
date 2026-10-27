# storm_require_pyyaml() — fail at CONFIGURE time when the Python that
# find_package(Python3) picked cannot `import yaml` (#703).
#
# tests/ and benchmarks/ run PyYAML-dependent scripts at BUILD time
# (split_unified_cases.py, yaml_to_json.py), but find_package(Python3) accepts
# any interpreter. A fresh tree that picked one without PyYAML configured fine
# and then died partway through the build with `ModuleNotFoundError: No module
# named 'yaml'`. Configure only reads the environment; it never installs
# anything. Call this right after find_package(Python3 ... Interpreter).
function(storm_require_pyyaml)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import yaml"
    RESULT_VARIABLE storm_pyyaml_result
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT storm_pyyaml_result EQUAL 0)
    message(
      FATAL_ERROR
        "PyYAML is required to build the tests and benchmarks, but the Python "
        "CMake found cannot import it:\n"
        "  ${Python3_EXECUTABLE}\n"
        "Fix it with either:\n"
        "  ${Python3_EXECUTABLE} -m pip install pyyaml\n"
        "    (a Homebrew/system Python refuses this as externally managed: use a "
        "venv, e.g. python3 -m venv ~/.venvs/storm, or your OS package, "
        "e.g. pacman -S python-yaml / apt install python3-yaml)\n"
        "  or re-configure with -DPython3_EXECUTABLE=<a python that has PyYAML>\n"
        "    (delete CMakeCache.txt first if Python3_EXECUTABLE was already cached)"
    )
  endif()
endfunction()
