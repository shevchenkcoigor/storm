# Minimal image containing only pre-built clang-p2996 (bloomberg/clang-p2996) artifacts.
# Used as a source for COPY --from in CI images.
#
# Build context: the clang-p2996 repository root (where build/ lives).
#
#   docker build --load -t ghcr.io/spiritecosse/storm-clang:latest \
#       -f storm/docker/clang/Dockerfile ../clang-p2996
#
# NO WORKFLOW BUILDS THIS FILE. docker-ci-image.yml builds docker/ci/Dockerfile
# only, and that consumes this image as `COPY --from=...storm-clang:latest`.
# So a change here reaches anybody only after all three of these, in order:
#
#   1. rebuild + `docker push` storm-clang:latest from the clang-p2996 checkout
#   2. dispatch .github/workflows/docker-ci-image.yml (republishes storm-ci)
#   3. bump the storm-ci@sha256 digest pins — ci.yml, clang-tidy-sweep.yml and
#      bench-ratio-spike.yml; scripts/check_ci_digests.py asserts they AGREE
#      with each other, but cannot tell you they are stale
#
# Skipping step 1 is the silent failure: storm-ci rebuilds fine against the old
# base and the missing file simply stays missing.
#
FROM scratch

# Compiler binaries
COPY build/bin/clang-21        /opt/clang-p2996/build/bin/clang-21
COPY build/bin/clang-scan-deps /opt/clang-p2996/build/bin/clang-scan-deps
COPY build/bin/clang-format    /opt/clang-p2996/build/bin/clang-format
COPY build/bin/clang-tidy      /opt/clang-p2996/build/bin/clang-tidy

# clang-tidy-diff.py drives scripts/run_clang_tidy.py --diff, and therefore the
# whole local pre-commit clang-tidy gate (#643). It is not a
# build artifact — it is checked into the clang-p2996 source tree, and THIS
# image's build context is that tree's root, so copying it here ships it
# version-matched to the clang-tidy binary above by construction. Fetching it
# from llvm/llvm-project at build time instead would silently skew (a release
# branch script against a clang-21 binary).
# docker/ci/entrypoint.sh symlinks /opt/clang-p2996 to ${sourceDir}/../clang-p2996,
# so this lands on exactly the path run_clang_tidy.py expects.
COPY clang-tools-extra/clang-tidy/tool/clang-tidy-diff.py \
     /opt/clang-p2996/clang-tools-extra/clang-tidy/tool/clang-tidy-diff.py

# Coverage tools
COPY build/bin/llvm-profdata   /opt/clang-p2996/build/bin/llvm-profdata
COPY build/bin/llvm-cov        /opt/clang-p2996/build/bin/llvm-cov

# Compiler runtime (builtins, sanitizer runtimes, etc.)
COPY build/lib/clang/ /opt/clang-p2996/build/lib/clang/

# Custom libc++ / libc++abi / libunwind
COPY build/lib/x86_64-unknown-linux-gnu/ /opt/clang-p2996/build/lib/x86_64-unknown-linux-gnu/

# C++26 standard library headers
COPY build/include/ /opt/clang-p2996/build/include/

# libc++ named module sources (std.cppm / std.compat.cppm) — required for
# `import std;` support (issue #326). CMake's CXX_MODULE_STD locates these
# via libc++.modules.json (which declares path ../../share/libc++/v1/).
# cmake/libcxx.cmake creates a symlink build/share/libc++/v1 -> build/modules/c++/v1
# at configure time; include the source dir so the symlink target resolves.
COPY build/modules/ /opt/clang-p2996/build/modules/
