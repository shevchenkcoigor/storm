# Pre-Commit Hook

Storm's pre-commit hook (`commit.sh`) runs format → clang-tidy → tests → coverage.
This page documents the **clang-tidy** stage in detail, because it has three
modes with different scopes.

## Self-healing on a fresh/stale worktree (#489)

Before running clang-tidy, `commit.sh` configures the release build and then
builds clang-tidy's prerequisites, so a fresh worktree doesn't fail with
"module BMI not found" (clang-tidy on files that `import std;`/`import storm;`
need the BMIs) or a missing test binary later at the test stage (the test stage runs `storm_tests`, `storm_mock_tests`, and
`storm_pq_mock_tests` directly — see
[TESTING.md#local-test-suite-speed](../testing/TESTING.md#local-test-suite-speed)
for why it doesn't use `ctest`). It similarly ensures `build/debug` is built
before running those binaries. Both guards are file-check-and-build: a no-op
(fast) on an already-warm build directory, so this doesn't add cost on the
common path — it only matters the first time you commit from a new worktree.

### A failed self-heal configure blocks the commit with the CMake error (#674)

Every self-heal `cmake --preset` — `build/release` for the clang-tidy
prerequisites and the bench release build, `build/debug` for clang-format,
cmake-format, tests and coverage — runs through `configure_if_missing` *inside*
the step that needs the tree. It configures only when the sentinel file is
absent (`compile_commands.json` for clang-tidy, `build.ninja` elsewhere), so the
warm path never runs cmake. When the configure fails, that step fails: the
CMake error is in the step's own log (`/tmp/storm_*.log`, path printed under the
failed step), and the commit is blocked.

Before #674 these configures discarded their output and ignored their exit
status, so a failed configure surfaced one step later as
`ninja: error: loading 'build.ninja': No such file or directory`, with the CMake
error logged nowhere. `build_tidy_prereqs()` also checks for
`build/release/build.ninja` before building, so a tree that has
`compile_commands.json` but no `build.ninja` reports "not configured" instead of
a ninja error. `scripts/tests/test_commit_self_heal_configure.sh` covers each
site with a stub `cmake` and runs locally with no toolchain; `commit.sh` runs it
when `commit.sh`, the test, or a script the test copies is staged. It is not wired into CI.

### What "prerequisites" means, and why not the full target set (#557)

clang-tidy replays `build/release/compile_commands.json` and only PARSES — it
never links — so the ~110 release test TUs of `storm_tests` are pure cost to it.
`build_tidy_prereqs()` therefore builds three things instead of the default
target set: the `storm` library BMIs, the BMIs of the module targets CMake
synthesizes for module *consumers* (`CMakeFiles/*@synth_N.dir/*.bmi` — these
belong to the consumer, so `--target storm` does not produce them), and the two
mock test binaries. Measured on a cold tree, 4 cores: **764 s → 154 s**.

The synthesized-module BMIs are the subtle part. Without them clang-tidy reports
`module 'std' not found` and **degrades to a partial parse rather than failing**
— on `src/orm/utilities.cppm` that produced 4 warnings where a correct parse
produces 1, i.e. 3 false positives, which matters because the hook runs
clang-tidy with `--fix`.

A fourth step keeps that saving from being confined to `src/`-only commits:
generating every target's dyndep file materializes the per-TU modmaps for the
cost of the module scan alone — `storm_tests`' 109 modmaps take **6 s** this way
against ~600 s to compile those TUs — so staging a `tests/**` file no longer
forces the fallback.

Because a missing BMI fails silently, every uncertain path falls back to the
full build: no ninja or no `mapfile`, an empty BMI enumeration, a staged file
whose modmaps reference a BMI that is not on disk, or any unexpected failure of
the check itself (the probe must print `complete`; anything else, a crash and a
missing `python3` included, counts as incomplete).

**Staging a header still triggers the full build.** `compile_commands.json` holds
no header entries — clang-tidy lints a header through a neighbouring TU's
command — so the check cannot prove which modmaps apply and deliberately does not
guess. Conservative by design: #557 requires building when unsure, and the cost
of guessing wrong is a partial parse whose false warnings `--fix` would apply.

## The three clang-tidy modes

| Mode | What it scans | Where it runs | Blocks on |
|---|---|---|---|
| `--diff` *(default)* | Lines staged in the current commit (`git diff --cached`) | Local pre-commit | New warnings on staged lines |
| `--full` | Whole files that are staged | Local pre-commit (opt-in) | Any warning in any staged file |
| `--all` | Every C++ file in the tree | Scheduled CI (Mondays 04:00 UTC) | Nothing — report only |

### `--diff` mode (default)

The pre-commit default since Issue #262. It pipes `git diff -U0 --cached` into
`clang-tidy-diff.py` from the clang-p2996 toolchain. The diagnostic engine only
emits warnings on lines the staged commit actually touches.

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
outside the staged hunks. `--diff` and `--full` therefore have roughly the
same per-file CPU cost — the difference is output policy, not parse scope.

**Why this is the default:** an author who edits one function in a 600-line
module shouldn't have to fix unrelated accumulated drift before they can commit
their change. Drift cleanup happens on its own schedule (see `--all`).

```bash
./scripts/run_clang_tidy.sh --diff          # check-only
./scripts/run_clang_tidy.sh --diff --fix    # apply auto-fixes, then re-check
```

What can still block your commit in `--diff` mode:

- A warning on a line you added or changed.
- A new warning that a fix on an adjacent line uncovered.

What will **not** block your commit:

- Pre-existing warnings on lines you didn't touch — even in the same file.

### `--full` mode

The pre-#262 default. Scans whole files that are staged. Useful when you are
deliberately cleaning up drift in a specific module and want clang-tidy to
report everything in that file, not just your delta.

```bash
./scripts/run_clang_tidy.sh --full --fix          # local one-off
STORM_TIDY_FULL=1 git commit -m "..."             # force --full in pre-commit
```

`--full` is **not** the pre-commit default any more. It blocks the commit on
any pre-existing drift in a file you happen to touch, which punishes the
wrong author. Use `--diff` for everyday work.

### `--all` mode

Whole-tree sweep. Runs on the `clang-tidy weekly sweep` workflow every Monday
at 04:00 UTC, and on demand via `workflow_dispatch`. Output goes to the
workflow run's **Summary** tab.

```bash
./scripts/run_clang_tidy.sh --all                 # run locally (slow)
```

**Report-only.** The sweep does **not** gate merges on `develop`. Its job is
to surface accumulated drift within a week instead of within months — the gap
that let 48 warnings accumulate over ~3 months before Issue #262.

Gating `--all` is an opt-in goal once the existing baseline is clean.

## Parse failures on `src/*.cppm`

Since the 2026-05-11 clang-p2996 rebuild, every `src/*.cppm` parses cleanly
under clang-tidy. If clang-tidy reports `Found compiler error` on a `src/`
module file, **`run_clang_tidy.sh` will now fail loudly** with:

```
❌ <file> (PARSE FAILURE — toolchain or build state is broken)
   clang-tidy could not parse this file. Re-run cmake --preset
   ninja-release and rebuild before retrying. See Issue #262.
```

The cause is almost always one of:

1. Stale `build/release/` from before a clang-p2996 update — `rm -rf build/release && cmake --preset ninja-release && cmake --build --preset ninja-release`.
2. A regression in the clang-p2996 binary — file an upstream issue and fall back to `--full` with the affected `.cppm` added back to `is_known_unparseable`'s skip list (in `scripts/lib/clang_tidy_skiplist.sh`).

**Silent skipping is gone.** The unconditional `*.cppm` skip in
`is_known_unparseable` was the root cause of Issue #262 — clang-tidy could
parse those files for months before anyone noticed, and the accumulated drift
all surfaced at once when the build state changed.

`is_known_unparseable`, `is_always_skip_file`, and `filter_skiplist_from_diff`
live in `scripts/lib/clang_tidy_skiplist.sh` (extracted from `run_clang_tidy.sh`
in #550), sourced by all three modes — the single place both the pre-commit
`--diff` path and the weekly `--all` sweep consult for which files clang-tidy
cannot parse. `--diff` mode used to also run a post-hoc regex over the error
text it collected, meant to strip toolchain/reflection parse noise the way the
skip list already does for known files; it was retired in #550 because it was
unanchored (matched anywhere on a diagnostic line) and had, in review during
#518, matched a genuine error on a real Storm file (`src/orm/fields.cppm`)
alongside the toolchain noise it targeted. A file that starts producing this
noise now needs an explicit, reviewed entry in `clang_tidy_skiplist.sh` —
the same requirement every file already on that list satisfies. `fuzz/*.cpp`
and `fuzz/fuzz_models.h` were added to the list as part of #550 itself: they
were never actually on it before, silently saved from blocking commits only
by the retired regex's generic `module`/`import` alternatives — the sentence
below was aspirational for `fuzz/*` until this fix made it true.

Skip list now covers only files that genuinely can't be parsed standalone:

- `tests/*`, `benchmarks/*`, `fuzz/*`, `shared/*` — import Storm modules
  (`shared/query_builder.hpp` is a pseudo-module header that needs `import storm;`)

## Author workflow

### When your PR introduces a new warning

`--diff` will catch it on commit. Either fix the warning, or — if the warning
is wrong and the check needs an exception — update `.clang-tidy` in the same
PR with a clear inline comment explaining why.

### When the weekly sweep reports new drift

The sweep is report-only. There is no automatic ticket. If you see new entries
when checking the Summary tab, open a focused cleanup PR or file an issue —
similar to how PR #263 cleaned the post-#262 baseline.

### Bypassing pre-commit

Don't. Use `STORM_TIDY_FULL=1` to widen the scope, or fix the warning, or
adjust `.clang-tidy`. `--no-verify` is forbidden by repo convention
(see [CLAUDE.md](https://github.com/spiritEcosse/storm/blob/develop/CLAUDE.md) Critical Safety Rules).

## Related

- [Issue #262](https://github.com/spiritEcosse/storm/issues/262) — structural fix for silent drift
- [PR #263](https://github.com/spiritEcosse/storm/pull/263) — cleanup of the 48 accumulated warnings
- [FORMATTING.md](FORMATTING.md) — clang-format / cmake-format stages of the same hook
