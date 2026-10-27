# Getting Started

Storm builds against a custom Clang with C++26 reflection (`clang-p2996`). Two
ways to get a working build environment — but first, one command that is not
about the toolchain at all.

## First, after cloning — wire the pre-commit hook

```bash
git config core.hooksPath .githooks
```

`core.hooksPath` is local git config: it lives in `.git/config`, is never
cloned, and no CI job can see it (CI does not commit). Until you set it,
`.githooks/pre-commit` — and therefore `commit.sh`, which runs clang-format,
clang-tidy, the test suite and the 100% coverage gate that CLAUDE.md rule #3
says never to skip — does not run on your commits, with nothing in the output
to say so (issue #651).

Two other places set it for you, and neither is a substitute for running it
yourself:

- a successful `cmake --preset …` (`CMakeLists.txt` sets it as a side effect),
  which is no help before your first configure, or in a clone that has no
  toolchain to configure with;
- `.claude/hooks/session-start-docker.sh`, for Claude Code sessions only.

It is idempotent, so re-running it costs nothing. To confirm:

```bash
git config --get core.hooksPath   # → .githooks
```

Note that the hook needs the toolchain to do its work: in a clone without one,
a commit touching C++ or cmake now **fails** rather than silently skipping the
checks. That is the intended direction — run the commit through the dev
container instead (`scripts/dev-container.sh exec git commit …`, see below).

## Option 1 (recommended) — Build via Docker

Use the prebuilt `storm-ci` image. It bundles clang-p2996, libc++, and all
system dependencies. No toolchain build from source.

```bash
git clone https://github.com/spiritEcosse/storm.git
cd storm

# One-shot release build inside the image.
docker run --rm -v "$(pwd):/storm" -w /storm \
    ghcr.io/spiritecosse/storm-ci:latest \
    bash -c "cmake --preset ninja-release && cmake --build --preset ninja-release"

# Or drop into an interactive shell for iterative work.
docker run --rm -it -v "$(pwd):/storm" -w /storm \
    ghcr.io/spiritecosse/storm-ci:latest
```

The image's entrypoint creates the `${sourceDir}/../clang-p2996` symlink Storm's
`CMakePresets.json` expects, regardless of where you mount the workspace.

The same image runs in CI, so a green local build is a strong predictor of
green CI.

**Need the full test/coverage loop (a live PostgreSQL), or working behind a
TLS-intercepting proxy?** `scripts/dev-container.sh` wraps the same
`docker/ci/Dockerfile` into a long-lived container plus a PostgreSQL sidecar
instead of the one-shot `docker run` above — see CLAUDE.md's Prerequisites
section (issue #628) for usage. It's what Claude Code sessions use when no
native toolchain is present.

## Option 2 — Build clang-p2996 from source

If you can't use Docker (e.g. host doesn't support it, or you're working on the
toolchain itself), follow the upstream build instructions for
[bloomberg/clang-p2996](https://github.com/bloomberg/clang-p2996) and place the
result at `../clang-p2996` (sibling to the Storm source tree). Expect a
multi-hour build.

Once `../clang-p2996/build/bin/clang` exists, the standard preset commands work:

```bash
cmake --preset ninja-release
cmake --build --preset ninja-release
```

## Interrupted CPM clone

If a configure is killed while CPM is cloning a dependency, `.cache/CPM/<name>/<hash>/` keeps a
`.git` but an empty working tree, and CPM reuses it as a cache hit forever. Configure detects this
and stops, naming the directory; delete that directory and configure again. In CI, delete
the `cpm-*` Actions cache instead.

## Python and PyYAML

The tests and benchmarks convert their YAML corpora to JSON at build time, so the Python 3
CMake finds must be able to `import yaml`. Configure checks this and stops with the
interpreter's path if it can't; it never installs anything. Either install PyYAML into that Python
(`<python> -m pip install pyyaml`; a Homebrew Python refuses a bare `pip install` as
"externally managed", so use a venv — `python3 -m venv ~/.venvs/storm &&
~/.venvs/storm/bin/pip install pyyaml` — or an OS package such as `pacman -S python-yaml` /
`apt install python3-yaml`), or point CMake at a Python that
has it — delete `CMakeCache.txt` first if a different one is already cached:

```bash
cmake --preset ninja-debug -DPython3_EXECUTABLE=/path/to/python3
```

## Next steps

- Common workflows: [COMMON_TASKS.md](COMMON_TASKS.md)
- Running tests: [TESTING.md](../testing/TESTING.md)
- Coverage: [CODE_COVERAGE.md](../testing/CODE_COVERAGE.md)
- Adding features: [ADDING_FEATURES.md](ADDING_FEATURES.md)
