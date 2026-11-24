# Pre-Commit Hook

Storm's pre-commit hook (`commit.sh`) runs format → tests. clang-tidy, the 100% coverage gate and the benchmark build are CI-only (#910, #793).
This page also documents **clang-tidy** in detail, because it has several
modes with different scopes and runs in two CI places instead of the hook.

## Where clang-tidy runs (#910)

It used to be a pre-commit step. It is not any more: it needed the native toolchain, a
configured `build/release` and the module BMIs (~764 s on a cold tree), and a commit made
in the prebuilt `storm-ci` image could not run it. It now runs:

| Where | Mode | Scope | Gates |
|---|---|---|---|
| `ci.yml`, `clang-tidy` job (PR and merge queue; only when C++ or the tidy plumbing changed, #913) | `--diff --base <base sha>` | lines the PR changed | `CI gate` — a warning on a changed line fails the job |
| `clang-tidy-sweep.yml` (Mondays 04:00 UTC, `workflow_dispatch`) | `--all` | the whole tree | nothing; the run turns red |
| your machine, on demand | `--diff` / `--diff --base <ref>` / `--full` | see below | nothing |

The CI step sits right after that leg's build, so it reuses the tree's
`compile_commands.json` and BMIs and costs no second build. Run it locally before
pushing if you want the feedback early: `./scripts/tidy/run_clang_tidy.py --diff --base origin/develop`
(needs a configured and built `build/release`).

## Self-healing on a fresh/stale worktree (#489)

`commit.sh` ensures `build/debug` is configured and built before it formats and
runs `storm_tests` directly (see
[TESTING.md#local-test-suite-speed](../testing/TESTING.md#local-test-suite-speed)
for why it doesn't use `ctest`), so a fresh worktree doesn't fail with a missing
test binary. The guard is file-check-and-build: a no-op (fast) on an
already-warm build directory, so this doesn't add cost on the common path — it only
matters the first time you commit from a new worktree. `build/release` is not
touched: it only ever served clang-tidy (#910).

### A failed self-heal configure blocks the commit with the CMake error (#674)

Every self-heal `cmake --preset` — `build/debug` for clang-format,
cmake-format and tests — runs through `configure_if_missing` *inside*
the step that needs the tree. It configures only when the sentinel file
(`build.ninja`) is absent, so the warm path never runs cmake. When the configure fails, that step fails: the
CMake error is in the step's own log (`/tmp/storm_*.log`, path printed under the
failed step), and the commit is blocked.

Before #674 these configures discarded their output and ignored their exit
status, so a failed configure surfaced one step later as
`ninja: error: loading 'build.ninja': No such file or directory`, with the CMake
error logged nowhere. `scripts/commit/tests/test_commit_self_heal_configure.py` covers each
site with a stub `cmake` and runs locally with no toolchain; `commit.sh` runs it
when `commit.sh`, the test, or a script the test copies is staged; CI's `script-guards` job runs it on every PR (`pre_commit_guards.py --all`).

Configure output — WARNINGs included — lands in the log of the step that ran
the configure. On a fresh tree where both formatters run, that is clang-format's,
so a configure-time "cmake-format not found" would miss the cmake-format step
that then fails. `cmake/format.cmake` closes that gap with a stub `cmake-format`
target that fails naming the missing tool, so the reason is in the cmake-format
step's own log whichever step configured the tree (#690; see
[FORMATTING.md](FORMATTING.md#tool-availability)). Its self-test,
`scripts/commit/tests/test_cmake_format_target.py`, runs in `commit.sh` when staged and in CI's `script-guards` job on every
PR (#708).

`commit.sh` runs under `#!/bin/bash`, which on macOS is **bash 3.2**, so it uses
no bash-4 constructs (`mapfile`, `local -n`, associative arrays, `${v,,}`).
`test_coverage_gate.py` scans `commit.sh` and `coverage-run-batched.sh` for
the common bash-4 constructs, so CI's bash 5 cannot hide a regression.

## The three clang-tidy modes

| Mode | What it scans | Where it runs | Blocks on |
|---|---|---|---|
| `--diff` *(default)* | Lines staged (`git diff --cached`), or with `--base <ref>` the lines changed since `<ref>` (`git diff <ref>...HEAD`) | CI on every PR (`--base`); locally on demand | New warnings on those lines |
| `--full` | Whole files that are staged (or changed since `--base <ref>`) | Locally, on demand | Any warning in any of those files |
| `--all` | Every C++ file in the tree | Scheduled CI (Mondays 04:00 UTC) | Nothing — report only |

### `--diff` mode (default)

The default since Issue #262. It pipes `git diff -U0 --cached` (or, with
`--base <ref>`, `git diff -U0 <ref>...HEAD`, which is what CI passes because nothing
is staged there, #910) into `clang-tidy-diff.py` from the clang-p2996 toolchain. The
diagnostic engine only emits warnings on lines the change actually touches.
`--base` needs the merge base in the checkout (CI unshallows first), refuses an
unknown ref with exit 2, and cannot be combined with `--fix` (a CI checkout is not
ours to edit).

`clang-tidy-diff.py` is a script checked into the clang-p2996 **source** tree
(`clang-tools-extra/clang-tidy/tool/`), not a build artifact next to the
`clang-tidy` binary, so `docker/clang/Dockerfile` has to copy it explicitly —
which it now does, version-matched to the binary it ships (#643). Only `--diff`
needs it; `--full` and `--all` (the scheduled sweep) drive `clang-tidy`
directly and are unaffected by its absence.

**Mechanically:** clang-tidy always parses the whole translation unit — C++
type resolution needs the full file. `--diff` mode does not change that. It
sets `-line-filter` on the clang-tidy invocation, so clang-tidy still walks
every AST node but suppresses the diagnostic when the source location is
outside the changed hunks. `--diff` and `--full` therefore have roughly the
same per-file CPU cost — the difference is output policy, not parse scope.

**Why this is the default:** an author who edits one function in a 600-line
module shouldn't have to fix unrelated accumulated drift before their change can merge. Drift cleanup happens on its own schedule (see `--all`).

```bash
./scripts/tidy/run_clang_tidy.py --diff                       # staged lines, check-only
./scripts/tidy/run_clang_tidy.py --diff --fix                 # apply auto-fixes to staged lines, then re-check
./scripts/tidy/run_clang_tidy.py --diff --base origin/develop # what CI runs on your PR
```

What can still fail your PR in `--diff` mode:

- A warning on a line you added or changed.
- A new warning that a fix on an adjacent line uncovered.

What will **not** block it:

- Pre-existing warnings on lines you didn't touch — even in the same file.

### `--full` mode

The pre-#262 default. Scans whole files that are staged (or changed since `--base <ref>`). Useful when you are
deliberately cleaning up drift in a specific module and want clang-tidy to
report everything in that file, not just your delta.

```bash
./scripts/tidy/run_clang_tidy.py --full --fix          # local one-off
```

`--full` is **not** the default. It blocks on any pre-existing drift in a file
you happen to touch, which punishes the wrong author. Use `--diff` for everyday
work. (`STORM_TIDY_FULL=1` was the pre-commit switch for it and went with the
hook step, #910.)

### `--all` mode

Whole-tree sweep. Runs on the `clang-tidy weekly sweep` workflow every Monday
at 04:00 UTC, and on demand via `workflow_dispatch`. Output goes to the
workflow run's **Summary** tab.

```bash
./scripts/tidy/run_clang_tidy.py --all                 # run locally (slow)
```

**Never gates merges.** The sweep is scheduled-only, so it cannot block a PR or
the merge queue on `develop`, but any warning, error or unexpected crash turns the
run red. Its job is to surface accumulated drift within a week instead of within
months — the gap that let 48 warnings accumulate over ~3 months before Issue #262.

Per-PR gating of `--all` is a separate opt-in goal once the existing baseline is clean;
the PR gate is `--diff --base` (see the table at the top).

## Parse failures on `src/*.cppm`

Since the 2026-05-11 clang-p2996 rebuild, every `src/*.cppm` parses cleanly
under clang-tidy. If clang-tidy reports `Found compiler error` on a `src/`
module file, **`run_clang_tidy.py` will now fail loudly** with:

```
❌ <file> (PARSE FAILURE — toolchain or build state is broken)
   clang-tidy could not parse this file. Re-run cmake --preset
   ninja-release and rebuild before retrying. See Issue #262.
```

The cause is almost always one of:

1. Stale `build/release/` from before a clang-p2996 update — `rm -rf build/release && cmake --preset ninja-release && cmake --build --preset ninja-release`.
2. A regression in the clang-p2996 binary — file an upstream issue and fall back to `--full` with the affected `.cppm` added back to `KNOWN_UNPARSEABLE` (in `scripts/tidy/clang_tidy_skiplist.py`).

**Silent skipping is gone.** The unconditional `*.cppm` skip in
`is_known_unparseable` was the root cause of Issue #262 — clang-tidy could
parse those files for months before anyone noticed, and the accumulated drift
all surfaced at once when the build state changed.

`is_known_unparseable`, `is_always_skip_file`, and `filter_skiplist_from_diff`
live in `scripts/tidy/clang_tidy_skiplist.py` (extracted from the shell runner in
#550, ported to Python with it in #692), imported by all three modes — the single place both the PR
`--diff` path and the weekly `--all` sweep consult for which files clang-tidy
cannot parse. `--diff` mode used to also run a post-hoc regex over the error
text it collected, meant to strip toolchain/reflection parse noise the way the
skip list already does for known files; it was retired in #550 because it was
unanchored (matched anywhere on a diagnostic line) and had, in review during
#518, matched a genuine error on a real Storm file (`src/orm/fields.cppm`)
alongside the toolchain noise it targeted. A file that starts producing this
noise now needs an explicit, reviewed entry in `clang_tidy_skiplist.py` —
the same requirement every file already on that list satisfies. `fuzz/*.cpp`
and `fuzz/fuzz_models.h` were added to the list as part of #550 itself: they
were never actually on it before, silently saved from blocking commits only
by the retired regex's generic `module`/`import` alternatives — the sentence
below was aspirational for `fuzz/*` until this fix made it true.

Skip list now covers only files that genuinely can't be parsed standalone:

- `tests/*`, `benchmarks/*`, `fuzz/*`, `shared/*` — import Storm modules
  (`shared/query_builder.hpp` is a pseudo-module header that needs `import storm;`, and so are its two
  parts `query_builder_resolve.hpp` / `query_builder_values.hpp`, #869)

## Author workflow

### When your PR introduces a new warning

`--diff --base` will catch it in the PR's `clang-tidy` CI job (or locally, if you run it first). Either fix the warning, or — if the warning
is wrong and the check needs an exception — update `.clang-tidy` in the same
PR with a clear inline comment explaining why.

### When the weekly sweep reports new drift

The sweep never gates a merge (it is scheduled-only), but any warning or error
turns the run red. There is no automatic ticket. If you see a red run, check the
Summary tab, then open a focused cleanup PR or file an issue —
similar to how PR #263 cleaned the post-#262 baseline.

### Bypassing pre-commit

Don't. `--no-verify` is forbidden by repo convention
(see [CLAUDE.md](https://github.com/spiritEcosse/storm/blob/develop/CLAUDE.md) Critical Safety Rules).

## Related

- [Issue #262](https://github.com/spiritEcosse/storm/issues/262) — structural fix for silent drift
- [PR #263](https://github.com/spiritEcosse/storm/pull/263) — cleanup of the 48 accumulated warnings
- [FORMATTING.md](FORMATTING.md) — clang-format / cmake-format stages of the same hook
- [Issue #910](https://github.com/spiritEcosse/storm/issues/910) — moving clang-tidy from the hook to the PR
