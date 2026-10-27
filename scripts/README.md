# Storm Scripts

Utility scripts for the Storm ORM project.

## Docker Dev Container (dev-container.sh)

Provides a working `../clang-p2996` build environment via Docker for sessions or
contributors with no native toolchain — see CLAUDE.md's Prerequisites section
(issue #628) for the full writeup and `docs/internals/building/GETTING_STARTED.md`
for the plain one-shot `docker run` alternative.

### Usage

```bash
scripts/dev-container.sh up                    # builds/starts the containers (idempotent)
scripts/dev-container.sh exec cmake --preset ninja-debug
scripts/dev-container.sh exec cmake --build --preset ninja-debug
scripts/dev-container.sh exec ./build/debug/tests/storm_tests
scripts/dev-container.sh exec ./commit.sh
scripts/dev-container.sh status                 # show container state
scripts/dev-container.sh rebuild                # drop image + containers, rebuild from scratch
scripts/dev-container.sh down                   # stop and remove the containers
```

### Notes

- `exec` runs the command natively instead (no Docker) when `../clang-p2996` is
  already present, so build commands can be prefixed with it unconditionally.
- Builds `docker/ci/Dockerfile`, adding an extra `cacerts` build context (trusted
  before `pacman -Syu`) only when a sandbox TLS-intercepting proxy CA is present
  at `/root/.ccr/ca-bundle.crt` — real CI never supplies that context.
- The PostgreSQL sidecar shares a unix-socket volume with the build container at
  `/var/run/postgresql`, matching `CMakePresets.json`'s own `STORM_PG_CONNSTR` —
  `ctest --preset` and the coverage target need no override.
- `up` serializes on a flock, so calling it before every `exec`, or from more
  than one shell concurrently, is safe.

## Clang-Tidy Script

Run clang-tidy with modernize checks on the Storm codebase, excluding third_party code.

### Prerequisites

- Release build with compile_commands.json: `cmake --preset ninja-release`

### Usage

```bash
# Check only (default)
./scripts/run_clang_tidy.sh

# Auto-apply fixes
./scripts/run_clang_tidy.sh --fix

# Limit parallel jobs
./scripts/run_clang_tidy.sh -j 4

# Include tests directory
./scripts/run_clang_tidy.sh --include-tests
```

### Options

| Option | Description |
|--------|-------------|
| `--fix` | Apply suggested fixes automatically (use with caution) |
| `-j N` | Number of parallel jobs (default: all cores) |
| `--include-tests` | Include tests/ directory (disabled by default due to gtest setup) |

### What it checks

- **Checks**: `modernize-*` (excluding `use-trailing-return-type`, `avoid-c-arrays`)
- **Files**: `src/*.cppm`, `benchmarks/*.cpp`
- **Excludes**: `third_party/`, test files (unless `--include-tests`)

### Notes

- Runs in parallel using all available CPU cores by default
- Filters out noisy clang-tidy meta-messages
- Handles clang-tidy crashes gracefully (skips crashed files)
- C++26 module support is limited - some warnings may be suppressed

---

## SonarCloud Quality Checks

Use the `sonar` CLI to inspect code quality. Auth is stored in the OS keychain via `sonar auth login`.

### Prerequisites

1. **`sonar` CLI** - Install and authenticate once:
   ```bash
   sonar auth login
   ```

### Usage

```bash
# List issues on develop branch
sonar list issues --project spiritEcosse_storm --branch develop --format table

# List issues on a PR (new code only)
sonar list issues --project spiritEcosse_storm --pull-request 48 --format table

# Quality gate status
sonar api get "/api/qualitygates/project_status?projectKey=spiritEcosse_storm&branch=develop"

# Quality gate status for a PR
sonar api get "/api/qualitygates/project_status?projectKey=spiritEcosse_storm&pullRequest=48"
```

### Using with Claude Code Slash Commands

Inside Claude Code, you can use slash commands:

```
/sonarcloud-status              # Auto-detect PR from current branch
/sonarcloud-status 48           # Check PR #48
/sonarcloud-branch              # Alias for sonarcloud-status
```

### Example

```bash
$ sonar list issues --project spiritEcosse_storm --pull-request 48 --format table

Detecting PR for branch: claude/my-feature-branch
Found PR #48

=== SonarCloud Analysis for PR #48 ===

📊 Quality Gate Status:
✅ PASSED

📈 Metrics:
  new_bugs: 0
  new_vulnerabilities: 0
  new_code_smells: 5
  new_duplicated_lines_density: 1.21

🐛 Issues Summary:
  Total Issues: 5

Top 10 Issues:

[MINOR] CODE_SMELL: Define each identifier in a dedicated statement.
  File: src/orm/statements/insert.cppm (Line 198)
  Rule: cpp:S1659
  Effort: 5min

...

📋 Code Duplications:
  New duplicated blocks: 4
  New duplicated lines: 44
  Duplication density: 1.21%

Duplication details (from PR files):

  src/orm/statements/base.cppm:
    Lines 105-119 in src/orm/statements/base.cppm
    ↔     Lines 130-144 in src/orm/statements/base.cppm

  src/orm/statements/distinct.cppm:
    Lines 194-214 in src/orm/statements/distinct.cppm
    ↔     Lines 283-303 in src/orm/statements/select.cppm

🔗 Links:
  Dashboard: https://sonarcloud.io/dashboard?id=spiritEcosse_storm&pullRequest=48
  Issues: https://sonarcloud.io/project/issues?pullRequest=48&...

=== Summary ===
✅ PR #48 is ready (quality gate passed)
```

### Notes

- **Auto-detection**: When run without arguments, the script uses `gh pr view` to find the PR associated with the current Git branch
- **Duplication details**: Shows exact line ranges where code is duplicated, including cross-file duplications
- **PR-specific metrics**: The summary metrics (blocks, lines, density) are specific to new code in the PR; detailed locations show all duplications in modified files

## Agent Frontmatter Check

Validates that every `.claude/agents/*.md` file has a single-line `description:` in its
YAML frontmatter.

### Why

Claude Code parses each agent file's frontmatter to register the agent. In YAML an unquoted
scalar ends at the first line starting in column 0, so a `description:` written across real
newlines terminates early, the parse fails, and **the agent is dropped silently** — it never
appears in the available-agents list and cannot be dispatched, with no warning emitted.

`storm-sql-reviewer` and `storm-buildsystem-reviewer` both shipped this way and were
undispatchable from the day they merged, which left CLAUDE.md rule #13 partly unenforceable
(issue #543). A diff review does not catch it: the broken file looks perfectly readable.

The fix and the enforced invariant: keep `description:` on **one physical line**, writing
newlines as literal `\n` escapes — the form used by every agent file that loads correctly.

### Usage

```bash
./scripts/check_agent_frontmatter.py              # defaults to .claude/agents
./scripts/check_agent_frontmatter.py some/dir     # validate another directory
./scripts/tests/test_check_agent_frontmatter.py   # self-test (stdlib unittest)
```

Exit code `0` = all agent files valid, `1` = at least one invalid.

### Where it runs

- **`commit.sh`** — whenever a `.claude/agents/*.md` file is staged (and runs the self-test too when the checker or its test is staged). Runs before the
  step-count early exit, since an agent-only commit skips every C++/cmake step and is
  exactly the commit that can introduce this bug.
- **CI** — the `script-guards` job in `.github/workflows/ci.yml` (`pre_commit_guards.py --all`),
  which also self-tests the validator. Emits `::error file=…` annotations so failures land on the file in the PR diff.

### Notes

- Not a strict YAML validation. The descriptions contain unquoted `:` characters that a strict
  parser rejects but Claude Code's lenient parser accepts — the physical line span, not YAML
  validity, is what distinguishes a loading file from a dropped one.
- A `.md` file with no opening `---` is treated as shared prose, not an agent, and skipped.
- An agent file with **no** `description:` at all is reported: it cannot be registered either.

## Change Classification (detect-changes.sh)

Classifies a list of changed file paths into `HAS_SRC_CHANGES` / `HAS_TEST_CHANGES` /
`HAS_CPP_CHANGES` / `HAS_CMAKE_CHANGES` / `HAS_BENCH_CHANGES` flags — one place shared by
`commit.sh` (pre-commit smart skips over staged files) and the `changes` job in
`.github/workflows/ci.yml` (skips the `test` / `coverage` / `clang-p2996-host-layout` jobs
on a PR that doesn't touch src/tests/cmake), so the two never classify the same file
differently.

### Usage

```bash
git diff --cached --name-only | ./scripts/detect-changes.sh
# HAS_SRC_CHANGES=true
# HAS_TEST_CHANGES=false
# HAS_CPP_CHANGES=true
# HAS_CMAKE_CHANGES=false
# HAS_BENCH_CHANGES=false

./scripts/tests/test_detect_changes.sh   # self-test
```

Output is `KEY=true|false` lines meant to be `eval`'d into shell variables:
```bash
eval "$(git diff --cached --name-only | ./scripts/detect-changes.sh)"
```

### Where it runs

- **`commit.sh`** — over staged files, to skip format/tidy/tests/coverage when nothing
  they could affect changed.
- **CI** — the `changes` job in `.github/workflows/ci.yml`, which also self-tests the
  script before using its output. `run_heavy` (the job's output) additionally forces a
  full run whenever the diff touches CI plumbing itself (`.github/workflows/`, `scripts/`,
  `.githooks/`, `CMakePresets.json`, `commit.sh`), regardless of the flags above.

## Script Self-Tests (scripts/tests/)

Fourteen scripts have a `test_<name>.sh` self-test under `scripts/tests/` — including four whose
subjects are not documented above (`coverage-run-batched.sh`, `ninjalog_stats.py`,
`time_trace.py`, `tu_ablation.py`), `test_pg_default_connstr.sh`, which covers the sourced helper
`pg-default-connstr.sh` (the per-OS default `STORM_PG_CONNSTR` socket: Linux
`/var/run/postgresql`, macOS `/tmp`) plus its callers `commit.sh`, `coverage-run-batched.sh`
and `dev-container.sh`, `test_line_coverage.sh`, which covers the sourced helper
`lib/line_coverage.sh` (the BSD/GNU-portable lcov line-coverage parser shared by `commit.sh`'s
gate and CI's `coverage` job, #680) plus `commit.sh`'s
`run_coverage_check()` and `show_uncovered_lines()` — the latter under `/bin/bash` too, which is
bash 3.2 on macOS — and a scan of `commit.sh` and the scripts it sources or runs for bash-4-only constructs (#689),
`test_libcxx_modules_symlink.sh`, which covers `cmake/libcxx.cmake` rather
than a script, `test_cmake_format_target.sh`, which covers `cmake/format.cmake`'s stub
`cmake-format` target — it fails naming the missing tool in the cmake-format step's own log (#690),
`test_pyyaml_check.py` (Python, plain cmake), which covers `cmake/pyyaml.cmake`'s configure-time PyYAML check (#703),
`test_cpm_cache_check.py` (Python, plain cmake), which covers `cmake/cpm-cache.cmake`'s configure-time check for a CPM checkout left empty by an interrupted clone (#705),
and `test_session_start_hook.sh`, which covers
`.claude/hooks/session-start-docker.sh` — the SessionStart hook that wires `core.hooksPath` (and
so decides whether `commit.sh` runs at all, #651) before taking any of its Docker-provisioning
early exits. `test_commit_self_heal_configure.sh` covers `commit.sh` itself: it runs the real
script in a throwaway repo against a stub `cmake`, and checks that each self-heal configure fails
its step with the CMake error in that step's log, and that a configured tree never reconfigures
(#674), and that a configured tree takes the targeted clang-tidy prereq build rather than the
full-build fallback (#689).
They are plain bash and run individually or as a suite:

```bash
for t in scripts/tests/test_*.sh; do "$t" || echo "FAILED: $t"; done
```

The agent-frontmatter checker and the helpers `commit.sh` delegates to (#688) are Python, so their
self-tests are too: `test_check_agent_frontmatter.py`, `test_check_ci_digests.py`,
`test_check_orm_backend_imports.py`, `test_uncovered_lines.py`,
`test_pre_commit_guards.py`, `test_smart_skip.py`, `test_sonar_gate.py` and `test_tidy_prereqs.py` (stdlib `unittest`, need
only `python3`, no syntax newer than 3.9) are not matched by the `*.sh` loop above — run them on their
own. Each also runs in `commit.sh` when it is staged.

Every self-test here is a row (step) of `pre_commit_guards.py`'s `GUARDS` table, except those in its
`NOT_GUARDED` map; `test_pre_commit_guards.py` fails on one in neither. CI's `script-guards` job runs
the whole table with `pre_commit_guards.py --all` — every guard, whatever changed, continuing past a
failure so one run reports them all (#708). So a new self-test reaches CI by becoming a guard row.

`commit.sh` is a thin bash orchestrator (sequencing `cmake`/`ninja`/`git`, exit codes); its logic lives in:

| Helper | What it does |
|---|---|
| `pre_commit_guards.py` | The staged-file guards as one table (trigger regex → checker/self-test → BLOCK message). Run **before** `commit.sh`'s `TOTAL_STEPS==0` early exit. Add a guard by adding a `Guard(...)` row to `GUARDS`. `--all` runs every row regardless of staged files and continues past a failure — CI's `script-guards` job (#708). |
| `sonar_gate.py` | Not a `commit.sh` helper: ci.yml's `sonar-gate` job (#711). Waits for the SonarCloud app's check run on the PR head and fails unless it is `success`; passes at once on `merge_group`/`push`/`schedule`, so the merge queue never waits on a check the app does not post. |
| `smart_skip.py` | The smart-skip decision table: reads `detect-changes.sh`'s `HAS_*` output, prints the `RUN_*` switches and `SKIP_NOTE` for `commit.sh` to `eval`. |
| `tidy_prereqs.py` | Whether clang-tidy's module BMIs exist for the staged files (#557). Prints `complete`; anything else — including a crash — is `incomplete`, so the caller rebuilds. |
| `uncovered_lines.py` | The uncovered files and line ranges `commit.sh`'s coverage step prints on a sub-100% result. Takes no arguments: it reads `build/debug/coverage/coverage-filtered.lcov` under the cwd. |

### Prerequisites

All but eight need nothing beyond bash and coreutils:

| Test | Needs |
|---|---|
| `test_libcxx_modules_symlink.sh` | **cmake at the project's own `cmake_minimum_required`** (read from `CMakeLists.txt`, today 3.30) **and ninja** — its harness declares that same floor and configures with `-G Ninja`, so either one missing aborts every configure before `cmake/libcxx.cmake` is read |
| `test_cmake_format_target.sh` | **cmake ≥ 3.16 and ninja** — its harness configures with `-G Ninja` (the docker/ci image has no `make`); no compiler (`LANGUAGES NONE`) |
| `test_cpm_cold_cache.sh` | `git` and `perl` — a static policy check that every pinned CPM/FetchContent call ends with `GIT_SHALLOW FALSE` (#709); no network, no cmake |
| `test_ninjalog_stats.sh` | `python3` (the script under test is Python) |
| `test_session_start_hook.sh` | **`git`** — its subject is git behavior, so it `git init`s a throwaway repo and makes a real commit through the wired hook (`python3` optional: without it the `settings.json` JSON-validity assertion is reported as unchecked, the grep assertions still run) |
| `test_time_trace.sh` | `python3`; no compiler — it pins the replay argv (a `.cppm` traced via `--precompile`, the modmap's `-fmodule-output` dropped) and the trace parsing, the two places a wrong answer is a plausible number rather than a crash |
| `test_commit_self_heal_configure.sh` | **`git`** — it runs `commit.sh` in a throwaway repo, which reads the staged files from git; `cmake`, `ninja` and `pg_isready` are stubbed, so no toolchain |
| `test_tu_ablation.sh` | `python3` (same as `test_ninjalog_stats.sh`); no compiler — it exercises the source transforms only, which is the point: a mis-parse there does not crash, it publishes a wrong number |
| everything else | pure bash |

`test_libcxx_modules_symlink.sh` reports the tools it is about to use on its first line of
output, then **skips** with one explanatory line when either is unusable (issue #645) rather
than reporting five opaque `cmake configure failed` scenarios — the shape Ubuntu 24.04's
cmake 3.28 produced. Under `CI` it fails instead of skipping: there the toolchain comes from
the digest-pinned `storm-ci` image, so a missing prerequisite means a broken image, not an
older machine, and a skip would report green for a test that never ran.

To run it elsewhere, upgrade the tool or use `scripts/dev-container.sh exec
scripts/tests/test_libcxx_modules_symlink.sh` — noting that `exec` runs **natively**, with
the same tools, whenever `../clang-p2996` is checked out beside the repo, so on such a host
the container is not a way around an old cmake.
