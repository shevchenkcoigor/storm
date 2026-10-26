#!/bin/bash
# Tests for cmake/format.cmake's `cmake-format` target (issue #690).
#
# With cmake-format absent, upstream cmake-scripts creates no `cmake-format`
# target, so commit.sh's cmake-format step used to fail with
# `unknown target 'cmake-format'` and only a configure-time WARNING named the
# cause (#643). That warning lands in whichever step configured the tree —
# clang-format's log when both formatters run on a fresh tree (#674) — so the
# step that failed did not explain itself. cmake/format.cmake now defines a stub
# `cmake-format` target that fails at BUILD time naming the missing tool.
#
# What is asserted:
#   - absent, fresh tree: configure + build in one go, the build fails and its
#     output names the missing tool (the cmake-only-commit path);
#   - absent, configured tree: the configure output is discarded (another step
#     owned it) and the build output ALONE still names the missing tool;
#   - present: the real tool runs and the build succeeds, with no stub message.
#
# Strategy: include the REAL cmake/format.cmake from a minimal harness project,
# with cmake-scripts' formatting.cmake replaced by a stub that mirrors upstream's
# cmake_format(): it creates `cmake-format` only when CMAKE_FORMAT_EXE is set.
# CMAKE_FORMAT_EXE is passed on the command line, so the host's own cmake-format
# (present or not) never decides the outcome. No compiler is needed (project
# LANGUAGES NONE). The generator is pinned to Ninja — the one commit.sh's own
# builds use — because the docker/ci image ships ninja but not make, so the
# default Unix Makefiles generator would fail every configure there.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FORMAT_CMAKE="$REPO_ROOT/cmake/format.cmake"

# Line the stub target prints; the cmake/format.cmake message starts with it.
readonly MISSING_TOOL_MSG="cmake-format not found"

PASS=0
FAIL=0
FAILED_TESTS=()

# Per-scenario state, populated by the dispatcher.
TMP=""
CURRENT_TAG=""

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL+1))
    # One entry per scenario, not per failing assertion.
    local n=${#FAILED_TESTS[@]}
    if [[ $n -eq 0 || "${FAILED_TESTS[$((n-1))]}" != "$CURRENT_TAG" ]]; then
        FAILED_TESTS+=("$CURRENT_TAG")
    fi
    return 0
}

pass() {
    echo "  PASS: $1"
    PASS=$((PASS+1))
    return 0
}

# --- Fixtures ---

write_harness() {
    mkdir -p "$TMP/src" "$TMP/cmake-scripts"
    cat > "$TMP/cmake-scripts/formatting.cmake" <<'STUB'
function(clang_format TARGET_NAME)
endfunction()
function(cmake_format TARGET_NAME)
  if(CMAKE_FORMAT_EXE)
    add_custom_target(${TARGET_NAME} COMMAND ${CMAKE_FORMAT_EXE} -i ${ARGN})
    add_custom_target(cmake-format)
    add_dependencies(cmake-format ${TARGET_NAME})
  endif()
endfunction()
STUB
    cat > "$TMP/src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(format_harness LANGUAGES NONE)
set(cmake-scripts_SOURCE_DIR "$TMP/cmake-scripts")
include("$FORMAT_CMAKE")
EOF
    return 0
}

# A stand-in cmake-format that records it ran.
write_fake_tool() {
    cat > "$TMP/cmake-format" <<EOF
#!/bin/sh
touch "$TMP/tool-ran"
EOF
    chmod +x "$TMP/cmake-format"
    return 0
}

configure() {
    local format_exe="$1"
    cmake -G Ninja -S "$TMP/src" -B "$TMP/build" "-DCMAKE_FORMAT_EXE=$format_exe" 2>&1
}

build_target() {
    cmake --build "$TMP/build" --target cmake-format 2>&1
}

# --- Scenarios ---

# The cmake-only commit: cmake-format's own step configures, then builds.
scenario_absent_fresh_tree() {
    local out rc
    out="$(configure "" && build_target)"
    rc=$?
    if [[ $rc -ne 0 ]]; then pass "build fails (rc=$rc)"; else fail "build succeeded with the tool absent"; fi
    if grep -qF "$MISSING_TOOL_MSG" <<< "$out"; then
        pass "output names the missing tool"
    else
        fail "output does not name the missing tool"
    fi
    return 0
}

# Both formatters run: clang-format's step configured the tree, so only the
# build output reaches cmake-format's log. It must explain itself.
scenario_absent_configured_tree() {
    local out rc
    if ! configure "" > /dev/null; then
        fail "harness configure failed"
        return 0
    fi
    out="$(build_target)"
    rc=$?
    if [[ $rc -ne 0 ]]; then pass "build fails (rc=$rc)"; else fail "build succeeded with the tool absent"; fi
    if grep -qF "$MISSING_TOOL_MSG" <<< "$out"; then
        pass "build output alone names the missing tool"
    else
        fail "build output alone does not name the missing tool: $(tail -3 <<< "$out" | tr '\n' ' ')"
    fi
    if grep -qiF "unknown target" <<< "$out" || grep -qF "No rule to make target" <<< "$out"; then
        fail "build output reports the misleading unknown-target error"
    else
        pass "build output does not report an unknown target"
    fi
    return 0
}

scenario_present() {
    local out rc
    write_fake_tool
    out="$(configure "$TMP/cmake-format" && build_target)"
    rc=$?
    if [[ $rc -eq 0 ]]; then pass "build succeeds"; else fail "build failed (rc=$rc): $(tail -3 <<< "$out" | tr '\n' ' ')"; fi
    if [[ -f "$TMP/tool-ran" ]]; then pass "cmake-format ran"; else fail "cmake-format did not run"; fi
    if grep -qF "$MISSING_TOOL_MSG" <<< "$out"; then
        fail "output claims the tool is missing"
    else
        pass "output does not claim the tool is missing"
    fi
    return 0
}

# --- Dispatcher ---

run_scenario() {
    CURRENT_TAG="$1"
    TMP="$(mktemp -d)"
    echo "[$CURRENT_TAG]"
    write_harness
    "scenario_$CURRENT_TAG"
    rm -rf "$TMP"
    return 0
}

for tool in cmake ninja; do
    if ! command -v "$tool" > /dev/null; then
        echo "FAIL: $tool not found on PATH; the harness configures with -G Ninja" >&2
        exit 1
    fi
done

run_scenario absent_fresh_tree
run_scenario absent_configured_tree
run_scenario present

echo ""
echo "$PASS passed, $FAIL failed"
if [[ $FAIL -ne 0 ]]; then
    echo "Failed scenarios: ${FAILED_TESTS[*]}"
    exit 1
fi
