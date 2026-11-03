#!/bin/bash
# Tests for cmake/libcxx.cmake symlink behavior (issue #326, phase 1).
#
# Strategy: build a fake LIBCXX_ROOT layout in a temp dir, invoke
# cmake/libcxx.cmake from a minimal harness CMakeLists, and assert the
# symlink (or its absence) per scenario.
#
# Each scenario is a function named scenario_<tag>. The dispatcher loop
# below handles all per-test boilerplate (tmpdir, fake root, cmake invoke,
# cleanup) so scenarios only encode their setup tweaks and assertions.
#
# Prerequisites: cmake at or above the project's own cmake_minimum_required
# (read from CMakeLists.txt below), and ninja. Both are checked and reported
# up front (issue #645) rather than assumed: the generated harness declares
# that same floor and configures with -G Ninja, so either one missing aborts
# every configure before cmake/libcxx.cmake is ever included, and the
# scenarios then fail for a reason none of them is about.

set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
LIBCXX_CMAKE="$REPO_ROOT/cmake/libcxx.cmake"

# The floor the generated harness declares (run_harness). READ from the
# project's own CMakeLists.txt rather than restated here, so the harness keeps
# mirroring the floor it exists to mirror after a bump, and so the probe below
# guards the same number the harness asks for.
HARNESS_CMAKE_MIN="$(sed -n \
    's/^[[:space:]]*cmake_minimum_required(VERSION \([0-9][0-9.]*\)).*/\1/p' \
    "$REPO_ROOT/CMakeLists.txt" | head -1)"
if [[ -z "$HARNESS_CMAKE_MIN" ]]; then
    echo "FAIL: no cmake_minimum_required(VERSION ...) in $REPO_ROOT/CMakeLists.txt" >&2
    exit 1
fi
readonly HARNESS_CMAKE_MIN

# Emitted by cmake/libcxx.cmake before any of the logic under test, so its
# presence in a scenario's log proves the include was actually REACHED — the
# distinction between "asserted on" and "trivially true because cmake never
# got there" (see scenario_refuses_when_share_is_real_dir).
readonly LIBCXX_REACHED_MARKER="Using custom libcxx from:"

# How much of a failed configure's log to inline into the failure message.
readonly CMAKE_LOG_TAIL_LINES=20

# Named once — three scenarios report it and SonarCloud (S1192) rejects the
# repeated literal.
readonly CONFIGURE_FAILED_MSG="cmake configure failed"

PASS=0
FAIL=0
FAILED_TESTS=()

# Per-scenario state, populated by the dispatcher before calling the scenario.
TMP=""
LIBCXX=""
LINK_PATH=""
LINK_TARGET=""
HARNESS_RC=0
CURRENT_TAG=""

fail() {
    local msg="$1"
    echo "  FAIL: $msg"
    FAIL=$((FAIL+1))
    FAILED_TESTS+=("$CURRENT_TAG")
    return 0
}

pass() {
    local msg="$1"
    echo "  PASS: $msg"
    PASS=$((PASS+1))
    return 0
}

# run_scenario deletes the tmpdir on its way out, so a failure that only NAMED
# the log ("see $TMP/harness/cmake.log") pointed at a path that no longer
# existed by the time anyone read the message (issue #645). Inline the tail
# instead, prefixed so it reads as part of the failure and not as test output.
fail_with_cmake_log() {
    local msg="$1"
    local log="$TMP/harness/cmake.log"
    if [[ ! -f "$log" ]]; then
        fail "$msg (no cmake.log was produced at $log)"
        return 0
    fi
    fail "$msg; last $CMAKE_LOG_TAIL_LINES lines of cmake.log:"
    tail -n "$CMAKE_LOG_TAIL_LINES" "$log" | sed 's/^/    | /'
    return 0
}

# "3.28.3" -> 3028. Major/minor only, which is all the floor comparison needs.
cmake_version_key() {
    local major minor
    IFS=. read -r major minor _ <<< "$1"
    printf '%d' "$(( 10#${major:-0} * 1000 + 10#${minor:-0} ))"
    return 0
}

# Echoes the running cmake's version, or nothing when there is no cmake on
# PATH or its --version output does not parse. The caller tells those two
# apart (they call for different fixes) rather than reporting both as absent.
detected_cmake_version() {
    local line
    line="$(cmake --version 2>/dev/null | head -1)"
    if [[ "$line" =~ ([0-9]+\.[0-9]+(\.[0-9]+)?) ]]; then
        echo "${BASH_REMATCH[1]}"
    fi
    return 0
}

# Reports the tools that are about to run — one line of version would have
# collapsed #645 to a five-second diagnosis — and stops with a single
# explanatory line when either is unusable, instead of letting every scenario
# fail opaquely inside cmake_minimum_required or inside generator selection.
check_prerequisites() {
    local cmake_path version reason=""
    cmake_path="$(command -v cmake || true)"
    version="$(detected_cmake_version)"

    if [[ -z "$cmake_path" ]]; then
        echo "cmake: not found on PATH; harness requires >= $HARNESS_CMAKE_MIN"
        reason="no cmake on PATH"
    elif [[ -z "$version" ]]; then
        echo "cmake: unparseable version from $cmake_path: \"$(cmake --version 2>&1 | head -1)\""
        reason="the cmake at $cmake_path reports no version this script can read"
    else
        echo "cmake: $version ($cmake_path); harness requires >= $HARNESS_CMAKE_MIN"
        if (( $(cmake_version_key "$version") < $(cmake_version_key "$HARNESS_CMAKE_MIN") )); then
            reason="cmake $version is older than the $HARNESS_CMAKE_MIN this harness requires"
        fi
    fi

    # run_harness pins -G Ninja, so a missing ninja fails every configure in
    # exactly the opaque way an old cmake does — #645's shape reached through
    # the other prerequisite. Probe it here rather than discovering it six
    # scenarios later.
    if ! command -v ninja > /dev/null; then
        echo "ninja: not found on PATH; the harness configures with -G Ninja"
        reason="${reason:+$reason; }no ninja on PATH"
    fi

    if [[ -z "$reason" ]]; then
        echo
        return 0
    fi

    # In CI the toolchain is supplied by the pinned storm-ci image, so a
    # missing prerequisite means a broken image rather than a contributor's
    # older machine — skipping there would report green for a job whose test
    # never ran.
    if [[ -n "${CI:-}" ]]; then
        echo "FAIL: $reason; every configure would fail before anything here is exercised." >&2
        echo "      CI takes its toolchain from the pinned storm-ci image, so this is a broken image." >&2
        exit 1
    fi

    echo "SKIP: $reason; every configure would fail before anything here is exercised."
    echo "      Upgrade the tool."
    echo
    echo "Results: 0 passed, 0 failed (all scenarios skipped)"
    exit 0
}

mtime_of() {
    local path="$1"
    local mtime
    mtime="$(stat -c '%Y' "$path" 2>/dev/null || stat -f '%m' "$path")"
    echo "$mtime"
    return 0
}

# layout: "both" (default; a superset safe for the symlink-behavior
# scenarios, which don't care which host-branch resolves) | "triple" (ONLY
# the Linux/amd64 per-triple layout — no flat build/lib/libc++.dylib-style
# dir) | "flat" (ONLY the macOS-style flat layout — no triple subdirectory).
# The narrower layouts let a host-branch scenario prove resolution picked
# the RIGHT one: if the logic resolved the other layout instead, the
# now-validating cmake/clang_p2996_host.cmake would FATAL_ERROR on a path
# that was never created here, rather than the test passing by accident on
# an overlapping fixture.
make_fake_libcxx_root() {
    local root="$1" layout="${2:-both}"
    mkdir -p "$root/build/modules/c++/v1" "$root/build/include/c++/v1"
    : > "$root/build/modules/c++/v1/std.cppm"
    : > "$root/build/modules/c++/v1/std.compat.cppm"
    if [[ "$layout" == "triple" || "$layout" == "both" ]]; then
        mkdir -p "$root/build/include/x86_64-unknown-linux-gnu/c++/v1" \
                 "$root/build/lib/x86_64-unknown-linux-gnu"
    fi
    if [[ "$layout" == "flat" ]]; then
        # Flat layout only — no triple subdirectory at all.
        mkdir -p "$root/build/lib"
    fi
    return 0
}

# Scenarios may set these (in their pre_<tag> hook, which runs BEFORE the
# fake root is built) to force a host other than this machine's real one —
# so both the Linux/amd64 and Darwin/arm64 branches, and the error path, are
# exercisable regardless of which host actually runs this script — and to
# pick a narrower fake-root layout than the "both" default.
EXTRA_CMAKE_SETUP=""
FAKE_ROOT_LAYOUT="both"

run_harness() {
    local workdir="$1" libcxx_root="$2"
    mkdir -p "$workdir"
    cat > "$workdir/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION $HARNESS_CMAKE_MIN)
project(libcxx_symlink_harness NONE)
set(LIBCXX_ROOT "$libcxx_root")
$EXTRA_CMAKE_SETUP
# Includes the SAME production file CMakeLists.txt and cmake/libcxx.cmake
# use — not a re-implementation — so these scenarios exercise the real
# host-layout resolution logic, including its EXISTS validation.
include("$LIBCXX_CMAKE")
message(STATUS "STORM_TEST_LIB_DIR=\${_storm_libcxx_lib_dir}")
message(STATUS "STORM_TEST_BUILD_INCLUDE_DIR=\${LIBCXX_BUILD_INCLUDE_DIR}")
message(STATUS "STORM_TEST_MODULES_JSON=\${_storm_libcxx_modules_json}")
EOF
    local rc=0
    # -G Ninja explicitly: storm always builds with Ninja (CMakePresets.json),
    # and unlike this Mac (Xcode Command Line Tools ships `make`), the
    # storm-ci container only installs ninja — CMake's platform-default
    # generator ("Unix Makefiles") has no build program there at all.
    (cd "$workdir" && cmake -G Ninja -B build . > cmake.log 2>&1) || rc=$?
    return "$rc"
}

# Dispatcher: calls pre_<tag> (optional setup the scenario wants — may set
# EXTRA_CMAKE_SETUP / FAKE_ROOT_LAYOUT — BEFORE the fake root and cmake run),
# builds the fake root, runs cmake, then calls the scenario for assertions.
run_scenario() {
    local tag="$1"
    CURRENT_TAG="$tag"
    echo "TEST: $tag"

    TMP="$(mktemp -d)"
    LIBCXX="$TMP/libcxx"
    LINK_PATH="$LIBCXX/build/share/libc++/v1"
    LINK_TARGET="$LIBCXX/build/modules/c++/v1"

    EXTRA_CMAKE_SETUP=""
    FAKE_ROOT_LAYOUT="both"
    if declare -F "pre_$tag" > /dev/null; then
        "pre_$tag"
    fi
    make_fake_libcxx_root "$LIBCXX" "$FAKE_ROOT_LAYOUT"

    HARNESS_RC=0
    run_harness "$TMP/harness" "$LIBCXX" || HARNESS_RC=$?

    "scenario_$tag"

    rm -rf "$TMP"
    return 0
}

# ---- scenarios ------------------------------------------------------------

scenario_creates_symlink_when_missing() {
    if [[ $HARNESS_RC -ne 0 ]]; then
        fail_with_cmake_log "$CONFIGURE_FAILED_MSG"; return
    fi
    if [[ ! -L "$LINK_PATH" ]]; then
        fail "expected symlink at $LINK_PATH, but it does not exist"; return
    fi
    local target
    target="$(readlink "$LINK_PATH")"
    if [[ "$target" != "$LINK_TARGET" ]]; then
        fail "symlink points to '$target', expected '$LINK_TARGET'"; return
    fi
    if [[ ! -f "$LINK_PATH/std.cppm" ]]; then
        fail "symlink does not resolve std.cppm"; return
    fi
    pass "symlink created and resolves correctly"
}

pre_idempotent_when_symlink_exists() {
    mkdir -p "$LIBCXX/build/share/libc++"
    ln -sfn "$LINK_TARGET" "$LINK_PATH"
    SAVED_MTIME="$(mtime_of "$LINK_PATH")"
    return 0
}

scenario_idempotent_when_symlink_exists() {
    if [[ $HARNESS_RC -ne 0 ]]; then
        fail_with_cmake_log "$CONFIGURE_FAILED_MSG"; return
    fi
    local after
    after="$(mtime_of "$LINK_PATH")"
    if [[ "$SAVED_MTIME" != "$after" ]]; then
        fail "existing correct symlink was modified (mtime changed)"; return
    fi
    pass "existing correct symlink was left untouched"
}

pre_refuses_when_share_is_real_dir() {
    mkdir -p "$LINK_PATH"
    : > "$LINK_PATH/keep.txt"
    return 0
}

scenario_refuses_when_share_is_real_dir() {
    # This is the one scenario with no HARNESS_RC check, by design: cmake may
    # or may not FATAL_ERROR here, and what MUST hold either way is filesystem
    # state. But that makes both assertions below trivially true for a
    # configure that never reached cmake/libcxx.cmake — which is exactly why
    # #645's run still reported "1 passed": this scenario went green without
    # exercising anything. Require proof the include was reached first.
    if ! grep -q "$LIBCXX_REACHED_MARKER" "$TMP/harness/cmake.log" 2> /dev/null; then
        fail_with_cmake_log "cmake never reached cmake/libcxx.cmake"; return
    fi
    if [[ -L "$LINK_PATH" ]]; then
        fail "real directory was replaced with a symlink"; return
    fi
    if [[ ! -f "$LINK_PATH/keep.txt" ]]; then
        fail "existing content under share/libc++/v1 was destroyed"; return
    fi
    pass "real directory at share/libc++/v1 was preserved"
}

# Host branch scenarios below force CMAKE_HOST_SYSTEM_NAME/_storm_host_arch
# via EXTRA_CMAKE_SETUP rather than relying on the runner's real host, so
# both branches (and the error path) are exercised regardless of whether
# this script runs on macOS/arm64 or Linux/amd64.

grep_test_var() {
    local varname="$1"
    grep -o "STORM_TEST_${varname}=.*" "$TMP/harness/cmake.log" \
        | tail -1 | sed "s/^STORM_TEST_${varname}=//"
    return 0
}

# Shared assertion for the two host-branch scenarios below: cmake must have
# succeeded, and _storm_libcxx_lib_dir/LIBCXX_BUILD_INCLUDE_DIR/
# _storm_libcxx_modules_json must resolve to exactly the expected triple
# (include dir "" means "unset, no duplicate -I"). Each scenario's fake root
# contains ONLY its own layout (see FAKE_ROOT_LAYOUT in its pre_ hook) — if
# resolution picked the wrong branch, cmake/clang_p2996_host.cmake's own
# EXISTS validation would FATAL_ERROR on a path this fixture never created,
# rather than the test passing by accident on an overlapping fixture.
assert_resolved_paths() {
    local expected_lib_dir="$1" expected_include_dir="$2" expected_json="$3" ok_msg="$4"
    if [[ $HARNESS_RC -ne 0 ]]; then
        fail_with_cmake_log "$CONFIGURE_FAILED_MSG"; return
    fi
    local lib_dir include_dir json
    lib_dir="$(grep_test_var LIB_DIR)"
    include_dir="$(grep_test_var BUILD_INCLUDE_DIR)"
    json="$(grep_test_var MODULES_JSON)"
    if [[ "$lib_dir" != "$expected_lib_dir" ]]; then
        fail "lib dir resolved to '$lib_dir', expected '$expected_lib_dir'"; return
    fi
    if [[ "$include_dir" != "$expected_include_dir" ]]; then
        fail "include dir resolved to '$include_dir', expected '$expected_include_dir'"; return
    fi
    if [[ "$json" != "$expected_json" ]]; then
        fail "modules.json resolved to '$json', expected '$expected_json'"; return
    fi
    pass "$ok_msg"
}

pre_resolves_linux_amd64_triple_paths() {
    EXTRA_CMAKE_SETUP='set(CMAKE_HOST_SYSTEM_NAME "Linux")
set(_storm_host_arch "x86_64")'
    FAKE_ROOT_LAYOUT="triple"
    return 0
}

scenario_resolves_linux_amd64_triple_paths() {
    assert_resolved_paths \
        "$LIBCXX/build/lib/x86_64-unknown-linux-gnu" \
        "$LIBCXX/build/include/x86_64-unknown-linux-gnu/c++/v1" \
        "$LIBCXX/build/lib/x86_64-unknown-linux-gnu/libc++.modules.json" \
        "Linux/amd64 resolves the triple-subdirectory layout"
    return 0
}

pre_resolves_darwin_arm64_flat_paths() {
    EXTRA_CMAKE_SETUP='set(CMAKE_HOST_SYSTEM_NAME "Darwin")
set(_storm_host_arch "arm64")'
    FAKE_ROOT_LAYOUT="flat"
    return 0
}

scenario_resolves_darwin_arm64_flat_paths() {
    assert_resolved_paths \
        "$LIBCXX/build/lib" \
        "" \
        "$LIBCXX/build/lib/libc++.modules.json" \
        "Darwin/arm64 resolves the flat layout with no duplicate -I"
    return 0
}

pre_rejects_unsupported_host() {
    EXTRA_CMAKE_SETUP='set(CMAKE_HOST_SYSTEM_NAME "Windows")
set(_storm_host_arch "x86_64")'
    return 0
}

scenario_rejects_unsupported_host() {
    if [[ $HARNESS_RC -eq 0 ]]; then
        fail "cmake configure succeeded for an unsupported host; expected FATAL_ERROR"; return
    fi
    if ! grep -q "Unsupported host for clang-p2996" "$TMP/harness/cmake.log"; then
        fail_with_cmake_log "expected a clear 'Unsupported host' error"; return
    fi
    pass "unsupported host fails configure with a clear error"
}

# ---- run ------------------------------------------------------------------

check_prerequisites

for tag in \
    creates_symlink_when_missing \
    idempotent_when_symlink_exists \
    refuses_when_share_is_real_dir \
    resolves_linux_amd64_triple_paths \
    resolves_darwin_arm64_flat_paths \
    rejects_unsupported_host
do
    run_scenario "$tag"
done

echo
echo "Results: $PASS passed, $FAIL failed"
if [[ $FAIL -gt 0 ]]; then
    echo "Failed tests: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
