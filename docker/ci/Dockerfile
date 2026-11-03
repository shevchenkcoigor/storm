# Storm CI/dev image: Arch base + system deps + prebuilt clang-p2996.
#
# Used as the `container:` for GitHub Actions CI jobs and as the recommended
# build environment for outside contributors who don't want to build clang-p2996
# from source.
#
#   docker build -t storm-ci:dev -f docker/ci/Dockerfile .
#   docker run --rm -v "$(pwd):/storm" -w /storm storm-ci:dev \
#       cmake --preset ninja-release && cmake --build --preset ninja-release
#
# A sandbox whose outbound HTTPS is TLS-intercepted can't `pacman -Syu` against that
# proxy's default-untrusted CA. Supply it as an ADDITIONAL BUILD CONTEXT named
# `cacerts` (any directory containing *.crt files) and it's trusted before
# pacman touches the network; real CI never passes this build context, so
# CACERTS defaults to `scratch` (empty) and nothing changes for it:
#
#   docker build --build-context cacerts=/root/.ccr \
#       -t storm-ci:sandbox -f docker/ci/Dockerfile .
#
# Pass it by hand when building in a sandbox whose proxy intercepts TLS.
ARG CACERTS=scratch
FROM ${CACERTS} AS cacerts

FROM manjarolinux/base:20260315

# Trust any *.crt supplied via the `cacerts` build context above (a no-op
# when CACERTS defaults to `scratch` — nothing to copy) before touching the
# network to install system dependencies. valgrind is included for future
# CodSpeed Macro / cachegrind work; lcov so the same image works for coverage
# builds. github-cli + jq + python-numpy/scipy are for the bench.yml
# regression gate (#241): gh fetches the develop-baseline-latest artifact and
# posts the PR comment, jq renders the comparison table, numpy/scipy back
# compare.py's Mann-Whitney U-test in benchmarks/scripts/compare_against_baseline.sh.
COPY --from=cacerts / /tmp/extra-ca/
RUN find /tmp/extra-ca -name '*.crt' -exec cp {} /etc/ca-certificates/trust-source/anchors/ \; \
    && trust extract-compat \
    && rm -rf /tmp/extra-ca \
    && pacman -Syu --noconfirm \
    && pacman -S --noconfirm --needed \
        cmake ninja gcc sqlite postgresql-libs \
        python python-yaml python-numpy python-scipy git valgrind lcov \
        github-cli jq \
    && pacman -Scc --noconfirm

# Prebuilt clang-p2996 from the sibling data-only image.
COPY --from=ghcr.io/spiritecosse/storm-clang:latest \
     /opt/clang-p2996 /opt/clang-p2996

# Post-COPY setup, in ONE layer (docker:S7031 forbids consecutive RUNs, and
# they would be a pointless extra layer anyway):
#
#   1. clang/clang++ symlinks — storm-clang ships clang-21 only.
#   2. cmake-format, the tool behind commit.sh's cmake-format step (#643).
#
# On (2): pip is the only source — neither `cmake-format` nor `cmakelang` exists
# in the Arch repos or the AUR, so pacman cannot supply it. It goes in its own
# venv rather than system-wide via `pip --break-system-packages`, so it cannot
# collide with the pacman-managed python packages above (python-numpy/scipy);
# the symlink is what puts it on PATH, which is all StableCoder's
# find_program(CMAKE_FORMAT_EXE "cmake-format") needs to create the target.
#
# THIS PIN IS THE SINGLE SOURCE OF TRUTH for the cmake-format version. It is
# pinned because cmake-format rewrites files in place during the pre-commit
# hook, so a floating version would silently reformat the tree; the docs point
# here rather than repeating the number. This whole block sits AFTER the
# multi-GB toolchain COPY above, so editing it never invalidates that layer.
#
# --cert: pacman gets the sandbox proxy CA via the trust store above, but pip
# uses vendored certifi and would not, so hand it the same bundle explicitly.
# Harmless when there is no proxy — it is the ordinary system CA bundle.
#
# The smoke test formats a real (tiny) file rather than running --version, which
# only prints a constant: cmakelang is a 2021 release on a rolling-Python base,
# so a compat break must fail the image build, not someone's pre-commit run.
# clang-tidy-diff.py, step 3 of the RUN below — TRANSITIONAL and self-healing
# (#643). Its proper home is docker/clang/Dockerfile, which copies it straight
# out of the clang-p2996 tree it is built from. But nothing in CI builds that
# image — it is republished by hand — so until someone does, the base above
# simply does not carry the file and commit.sh's clang-tidy --diff step cannot
# run in the container at all.
#
# Hence: fetch ONLY IF ABSENT. The moment storm-clang is rebuilt with its COPY,
# the base carries the file, this step finds it and does nothing. It retires
# itself with no further edit — delete it whenever you like after that.
#
# Pinned to the revision the shipped toolchain was built from, never a branch
# name: a branch drifts silently against a clang-21 binary, which is exactly the
# skew #643 rejected. Bump it together with the toolchain.
#
# Fetched with python (always present) rather than curl (not in the pacman list
# above), and ast.parse'd before install: a rate-limited or proxied fetch hands
# back a small HTML error page, and writing that to disk would turn a clean
# "file not found" into a baffling Python syntax error mid-commit.
ARG CLANG_TIDY_DIFF_REV=9ffb96e3

RUN ln -s clang-21 /opt/clang-p2996/build/bin/clang \
    && ln -s clang-21 /opt/clang-p2996/build/bin/clang++ \
    && ctd=/opt/clang-p2996/clang-tools-extra/clang-tidy/tool/clang-tidy-diff.py \
    && if [ ! -f "$ctd" ]; then \
        mkdir -p "$(dirname "$ctd")" \
        && python -c "import urllib.request,ast,sys; u='https://raw.githubusercontent.com/bloomberg/clang-p2996/'+sys.argv[1]+'/clang-tools-extra/clang-tidy/tool/clang-tidy-diff.py'; d=urllib.request.urlopen(u,timeout=120).read().decode(); ast.parse(d); open(sys.argv[2],'w').write(d)" \
            "$CLANG_TIDY_DIFF_REV" "$ctd" \
        && echo "clang-tidy-diff.py fetched at rev $CLANG_TIDY_DIFF_REV"; \
       else echo "clang-tidy-diff.py already in base image — nothing to do"; fi \
    && python -m venv /opt/cmakelang \
    && /opt/cmakelang/bin/pip install --no-cache-dir \
        --cert /etc/ssl/certs/ca-certificates.crt cmakelang==0.6.13 \
    && ln -s /opt/cmakelang/bin/cmake-format /usr/local/bin/cmake-format \
    && printf 'if(A)\nmessage("x")\nendif()\n' > /tmp/smoke.cmake \
    && cmake-format /tmp/smoke.cmake > /dev/null \
    && rm /tmp/smoke.cmake

ENV LIBCXX_ROOT=/opt/clang-p2996 \
    PATH=/opt/clang-p2996/build/bin:$PATH

# Storm's CMakePresets.json references `${sourceDir}/../clang-p2996`. The
# entrypoint creates that symlink at $(dirname $PWD) on every container start,
# so the image works regardless of where the workspace is mounted. GitHub
# Actions `container:` jobs override ENTRYPOINT and need a per-job shim step
# (`ln -s /opt/clang-p2996 ../clang-p2996`) — see .github/workflows/ci.yml.
COPY docker/ci/entrypoint.sh /usr/local/bin/storm-ci-entrypoint
RUN chmod +x /usr/local/bin/storm-ci-entrypoint

ENTRYPOINT ["/usr/local/bin/storm-ci-entrypoint"]
CMD ["bash"]
