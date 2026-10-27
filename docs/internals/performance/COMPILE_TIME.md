# Compile-Time Analysis

Where Storm's build time actually goes, measured rather than assumed, and what
moves it. All numbers below are measurements on a 4-core dev container
(clang-p2996, `ninja-debug` unless stated), reproducible with the methods in
[Method](#method).

**Summary**: the build is dominated by the ~110 hand-written test TUs, not by
the YAML-driven test corpus. Each test TU used to pay a **~5.8 s fixed floor**
before its first assertion, **entirely frontend** — Backend is 0.03 s. The gtest
PCH removed most of that floor; what survives it is measured in
[After the PCH](#after-the-pch-issue-633).

---

## Where the time goes

Object compiles only, ninja edges deduplicated (a C++ module edge emits both
`.pcm` and `.o` and must be counted once):

| bucket | files | sec | % | avg |
|---|---:|---:|---:|---:|
| hand-written test TUs | 110 | 2558 | **85.2%** | 23.3s |
| storm library modules | 35 | 216 | 7.2% | 6.2s |
| YAML corpus TUs | 4 | 159 | **5.3%** | 39.7s |
| mock test binaries | 6 | 54 | 1.8% | 9.0s |

Total ≈ **3003 s** of compile CPU. Release is ~32% more expensive again (3954 s)
despite Debug carrying coverage instrumentation — `-O2` over this much template
instantiation costs more than the instrumentation does.

By test directory: `query/` 42.6%, `schema/` 24.3%, `crud/` 19.3%, `yaml/` 6.2%.

That table is the **baseline**, before any of [What was fixed](#what-was-fixed) —
its 23.3 s average per test TU is the pre-PCH figure, when each TU still paid the
5.8 s floor below.

### Re-measured after #634 (2026-09-08)

Everything above is arithmetic from that baseline minus per-change deltas. This
is an actual whole-build measurement of the current tree, so the two can be
compared for *shape* — but **not** subtracted for a saving: it is a different
machine, and the doc's own rule applies (read the deltas within a section, not
absolute seconds across sections).

Method exactly as in [Method](#method), and reproducible with
`scripts/ninjalog_stats.py`: `rm -rf build/debug`, configure + build
`ninja-debug` to completion, then

```
scripts/ninjalog_stats.py build/debug/.ninja_log
```

which deduplicates on `(start, end, cmdhash)` and counts object compiles only —
the 217 module-scan edges (`.ddi`/`.dd`/`.modmap`, 48 s together) are excluded,
as they are above, and reported separately so the exclusion stays visible.

| bucket | files | sec | % | avg |
|---|---:|---:|---:|---:|
| hand-written test TUs | 110 | 1518 | **73.8%** | 13.8s |
| storm library modules | 70 | 336 | 16.3% | 4.8s |
| YAML corpus TUs | 5 | 104 | 5.1% | 20.9s |
| other (gtest/gmock, std module, tools, deps) | 8 | 52 | 2.5% | 6.5s |
| mock test binaries | 6 | 47 | 2.3% | 7.9s |

Total **2059 s** of object-compile CPU over 199 edges, **397 s wall** (≈5.2x
effective parallelism on 4 cores). Test TUs come out at 110 and mock binaries at
6, both matching the baseline exactly, which is what makes the rows comparable;
the `storm library modules` row deliberately does not — 70 here counts the
synthesized-module BMI edges that the baseline's 35 (`.cppm` files only) leaves
out.

**Host for this measurement** — recorded because the baseline's is not, and the
absolute seconds mean nothing without it:

| | |
|---|---|
| CPU | Intel Xeon @ 2.80 GHz, **4 vCPU** |
| RAM | 16 GB |
| kernel / host OS | Linux 6.18.44-fc-v24, Ubuntu 24.04.4 LTS (sandboxed microVM) |
| disk | virtio `/dev/vda`, ext4 |
| container | `docker/ci/Dockerfile` (Manjaro base) via `scripts/dev-container.sh` |
| compiler | clang-p2996 21.0.0git, commit `9ffb96e3` |
| cmake / ninja | 4.4.3 / 1.13.2 (default `-j`, i.e. 6 jobs on 4 cores) |
| preset | `ninja-debug` (coverage instrumentation on) |
| tree | commit `27c0f0f` (#634 landed) |

Do **not** read `3003 -> 2059` as a 31% win. The changes in
[What was fixed](#what-was-fixed) sum to roughly -350 to -450 s; the rest of the
gap is hardware. Attributing it properly needs the baseline re-run on this same
host, which has not been done.

## The per-TU floor

This is the **pre-PCH** floor — the measurement that motivated precompiling
gtest. Probe TUs whose entire body is `int p(){return 0;}` (min of 3 runs each,
warm):

| probe | sec | delta |
|---|---:|---|
| empty | 0.0 | — |
| `#include <gtest/gtest.h>` | 2.2 | **+2.2** |
| + `import std;` | 2.4 | +0.2 |
| + `import storm;` | 4.3 | **+2.1** |
| + `test_db_helpers.h` + `shared/models.h` (the pre-#634 umbrella) | 5.8 | **+1.4** |

`-ftime-trace` on a TU containing one `EXPECT_TRUE(true)`: Frontend 6.28 s,
**Backend 0.03 s**. Parsing (`Source`) is 3.09 s of it; the rest is 448 function
and 1197 class instantiations, in a file that instantiates nothing of its own.

Two counter-intuitive results worth keeping:

- **A module import is nearly free until something uses it.** `import std;` and
  `import storm;` in isolation measure 0.0 s — BMI loading is lazy. The 2.1 s
  above appears only *alongside* gtest: the cost is reconciling storm's BMI
  against the textual libc++ declarations gtest drags in, not deserialising the
  BMI as such.
- **Inside storm, the cost is concentrated.** Over the gtest baseline:
  `storm_db_sqlite` +0.0 s, `storm_orm_utilities` +0.2 s,
  `storm_orm_statements_join` +0.8 s, `storm_orm_schema` +1.0 s,
  **`storm_orm_queryset` +1.6 s** — of a +2.1 s total for the umbrella `storm`.

  The second of these is an artefact of the empty probe body and does **not**
  survive re-measurement — see [After the PCH](#after-the-pch-issue-633).

## After the PCH (issue #633)

Numbers in this section are from a **different, faster host** than the rest of
the document (gtest alone: 1.65 s here vs 2.2 s above), so read the deltas, not
the absolute seconds.

Precompiling gtest did not only remove gtest's own share of the floor. It
removed most of `import storm;` with it, which is exactly what the causal story
above predicts — once gtest arrives already serialised, there are no textual
libc++ declarations left to reconcile against:

| probe (body `int p(){return 0;}`) | textual gtest | gtest via PCH |
|---|---:|---:|
| gtest | 1.65 | 0.04 |
| + `import std;` | 1.72 | 0.05 |
| + `import storm;` | 3.10 | 0.29 |
| **cost of `import storm;`** | **+1.38** | **+0.24** |

An empty body understates it, because declarations come out of the PCH lazily
and a probe that never mentions `EXPECT_*` never pulls them. With a body that
uses gtest for real, all three variants via the PCH:

| body | no storm | + `import storm;` | delta |
|---|---:|---:|---:|
| `EXPECT_TRUE(true)` | 0.14 | 0.65 | +0.51 |
| strings + vectors | 0.25 | 0.99 | +0.74 |
| `TYPED_TEST` | 0.23 | 1.02 | +0.79 |
| strings + vectors, textual gtest | 1.82 | 3.31 | +1.49 |

So the surviving cost of `import storm;` is **+0.5 to +0.8 s per TU**, scaling
with how much gtest the TU uses — roughly **55-90 s** over the suite, not the
~230 s that issue #633 was opened against.

### What that residual actually is

`-ftime-trace` on `import storm_orm_queryset;` against a gtest-only baseline,
same body, `-ftime-trace-granularity=0`. Of the +0.63 s:

| bucket | delta |
|---|---:|
| `ParseDeclarationOrFunctionDefinition` | +0.29 |
| `PerformPendingInstantiations` | +0.16 |
| `CodeGen Function` + `DebugType` | +0.18 |
| `InstantiateFunction` + `InstantiateClass` | +0.18 |
| `ReadAST` + `Module Load` + `Load External Specializations` | **+0.06-0.10** |

Reading the BMI is under a sixth of it. The rest is **gtest's own
instantiations getting slower**. The heaviest entries with storm imported are,
in order, `testing::internal::CmpHelperEQ`, `EqHelper::Compare`,
`PrintToString`, `UniversalPrint`, and `std::__can_be_converted_to_string_view` /
`std::is_convertible` — gtest and libc++ templates. **No storm declaration
appears anywhere in the top instantiations.**

The mechanism, then, is not that storm is expensive to load. It is that storm's
declarations join the lookup set, and every later template instantiation in the
TU pays for the larger set. That single fact predicts every other result here:
the cost tracks gtest usage rather than storm usage; `storm_orm_queryset`'s own
body costs ~0; putting `import storm;` in the PCH makes things *worse*; and
narrowing what a module exports cannot help, because the cost is in the set
existing, not in what is reachable through it.

It also scales with the size of the declaration set added, not merely with the
act of importing — leaf modules are genuinely free. Over a gtest-only baseline
of 0.24 s, with a real gtest body:

| import | sec | delta |
|---|---:|---:|
| `storm_db_concept`, `storm_db_sqlite`, `storm_orm_generator`, `storm_orm_transaction` | 0.25-0.27 | ~0.00 |
| `storm_orm_where`, `storm_orm_fields`, `storm_orm_statements_orderby` | 0.35-0.41 | +0.10-0.16 |
| `storm_orm_statements_base` | 0.49 | +0.25 |
| `storm_orm_statements_select` | 0.59 | +0.35 |
| **queryset's own imports, without `storm_orm_queryset`** | **0.91** | **+0.67** |
| `storm_orm_queryset` | 0.88 | +0.64 |
| `storm` (umbrella) | 0.95 | +0.71 |

The decisive row is the second-to-last: importing everything
`storm_orm_queryset` imports, but *not* `storm_orm_queryset` itself, costs the
same as importing it. Its own content contributes nothing measurable. The
umbrella adds only +0.07 s on top of it, so even a perfectly narrowed umbrella
is worth ~8 s over the suite.

## What one average TU is made of

[The per-TU floor](#the-per-tu-floor) and [After the PCH](#after-the-pch-issue-633) measure
probes — TUs with a synthetic body. This section measures a **real** one by ablating it and
recompiling: `tests/crud/test_conditional_update.cpp`, 449 lines, 34 `TYPED_TEST` blocks, picked
for its shape rather than its size — one `TYPED_TEST` suite over both backends, a fixture with
three models, and bodies that are query chains and assertions and nothing else, which is what
most of the 110 hand-written test TUs are. It is deliberately NOT picked by comparing its 11.48 s
against the 13.8 s per-TU mean in [Re-measured after #634](#re-measured-after-634-2026-09-08):
that mean is of per-edge **wall** times from a `-j6`-on-4-cores build, and this section's seconds
are serial, uncontended, min-of-3 replays. The two are not the same quantity. Reproducible with
`scripts/tu_ablation.py`; each column below is internally consistent, so read a column against
itself and never against another section's seconds.

| step | A: sec | A: Δ | B: sec | B: Δ | what the step adds |
|---|---:|---:|---:|---:|---|
| `n=0` — every test block deleted | 2.07 | — | 2.00 | — | includes, the gtest PCH, `import storm;`, models + `fields::` proxies |
| `trivial1-1model` — one block, body `EXPECT_TRUE(true)`, fixture on one model | 5.58 | +3.51 | 5.32 | +3.32 | gtest's typed-test registration and `StormTestFixture<Person>`'s table machinery, ×2 backends |
| `trivial1` — same, fixture on the file's three models | 6.74 | +1.16 | 5.89 | +0.57 | `ensure_tables` for `Message` and `TimestampedRecord`, ×2 |
| `full` — all 34 blocks | 11.48 | **+4.74** | 10.62 | **+4.73** | the bodies: 22 distinct WHERE shapes, 8 `update<>` packs, 117 assertions |

Two independent sessions, A and B, because one is not enough — see
[the measurement mistake](#the-measurement-mistake-this-cost-worth-not-repeating) this section
paid for. **Read the three-way split, which both sessions agree on, and not the individual
seconds:**

| layer | A | B |
|---|---:|---:|
| entering the TU | 2.07 (18%) | 2.00 (19%) |
| fixture + typed-test registration | 4.67 (41%) | 3.89 (37%) |
| the test bodies | 4.74 (41%) | 4.73 (45%) |

**The bodies are ~41-45% of the TU and the fixture is another ~37-41%.** Neither dominates,
which is why "the tests are slow" has no single fix — and the entry cost that the probe sections
above measure is the smallest of the three. `-ftime-trace` agrees on the middle layer: under the
first block's `TypeParameterizedTest` (3.12 s on the PG side) sits `TestFactoryImpl::CreateTest`
(2.82 s), then `StormTestFixture::on_setup` (1.39 s), then `storm::test::ensure_tables` (1.38 s).

What does NOT reproduce is the fixture's per-model slope: the same two extra models cost 1.16 s in
A and 0.57 s in B. Treat an extra fixture model as **~0.3-0.6 s**, and do not quote a point value.

Two further ablations say the bodies are not reachable either. Both keep every assertion and
every query shape, changing only how they are spelled:

| variant | A: Δ | B: Δ | reading |
|---|---:|---:|---|
| `merge` — all 34 bodies concatenated into ONE block, each in its own scope | −0.57 (−5.0%) | −0.17 (−1.6%) | a `TYPED_TEST` block's wrapper is nearly free |
| `sink` — every `EXPECT_*`/`ASSERT_*` replaced by a variadic no-op that still instantiates its arguments | −0.61 (−5.3%) | −0.11 (−1.0%) | so is gtest's comparison and printing machinery |
| `merge+sink` | −1.48 (−12.9%) | −0.58 (−5.5%) | **more** than additive in both sessions |

These deltas are the ones that move most between sessions — a factor of 3 — so the honest reading
is their **ceiling**, not their value: together the two levers are worth **at most ~13%, and
possibly ~5%**. The rest of the ~4.7 s is the distinct query shapes themselves — the six
comparison operators, `IN`/`BETWEEN`/`LIKE`/`IS NULL`, the `AND`/`OR` combinations the testing
checklist requires. Cutting them is the same trade as
[dropping the second backend](#dropping-the-second-backend-from-typed_test), and gets the same
answer: the coverage is cheap. So the only levers inside a test file cost either failure
granularity (one merged block reports one failure) or diagnostic quality (a no-op assertion prints
nothing), for a single-digit percentage. Neither is worth taking.

### The fixture-model audit — measured, and empty

~0.3-0.6 s per extra fixture model looks like free money: a fixture naming a model no test uses
pays it for nothing. Audited across all 116 `tests/**/*.cpp` files (the 110 above is the
`ninjalog_stats.py` bucket, which excludes the mock-binary TUs) — every `StormTestFixture<...>`
model list
against the rest of its own file — and there are **two** candidates, one of them false. `Course`
in `tests/query/test_many_to_many_modifiers.cpp` is never named again but is the m2m target of
`Student`, so its table must exist. The one real leftover is `Message` in
`tests/query/test_setop.cpp:17`. That is one model's worth across the whole tree. Recorded so
nobody re-runs the audit expecting more.

### Inside the fixture layer — and why its biggest item cannot be moved

[The decomposition above](#what-one-average-tu-is-made-of) leaves the fixture and typed-test
registration as ~37-41% of a TU, its largest unexplored layer. A ladder of hand-written probe TUs
splits it — each rung adds exactly one thing over the previous, and each is compiled by replaying a
real TU's command, the same method as `scripts/compile_time_probe.py`. The rungs are minimal TUs, so
read the deltas and not the totals; the ladder itself was ad-hoc and is not shipped, since the table
below says exactly what each rung contains:

| rung | Δ | what it adds |
|---|---:|---|
| a plain `TEST_F` over `::testing::Test` | +0.08-0.13 | gtest having a fixture at all |
| the same made `TYPED_TEST` over `DatabaseTypes` | +0.17-0.23 | **gtest's typed-test machinery, both backends** |
| a fixture touching only `QuerySet<Person, Conn>` statics | +0.67-0.75 | `set_`/`clear_default_connection`, ×2 |
| `pg_schema_init` + `rollback_test_txn` | **+1.07** | the PG isolation helpers in `tests/test_db_helpers.h` |
| `ensure_tables<Conn, Person>` | +0.85-1.14 | storm's `SchemaStatement` DDL generator, ×2 |
| two more models in the fixture | +0.30-0.45 | the per-model slope |

**gtest is ~10% of the layer, not its bulk** — the typed-test registration that the trace makes look
dominant (`TypeParameterizedTest` 3.12 s) is dominant only because everything else nests *inside*
it. The single most expensive item is `tests/test_db_helpers.h`'s six `std::format` calls: swapping
them for concatenation in the probe took that rung from +1.07 to +0.22, so **`std::format`
instantiation alone was 0.85 s per TU** — more than storm's own DDL generator — for setup that runs
once per process.

That looked like ~85 s over the suite for a test-only, coverage-neutral change. It is not
available:

- **Moving the six calls into a separately-compiled TU does not build.** Neither does replacing
  them with concatenation, nor keeping only one that formats an `int`. All three fail the same way:
  `definition with same mangled name '_ZNSt3__17vformat…' as another definition`, in *other* TUs
  (`test_upsert.cpp`, `test_select_large.cpp`, …). The header's textual `<format>` and the imported
  `std` module each define those specializations, and it is the header's own `std::format` calls —
  instantiated early, in the global module — that make them merge. Remove the instantiations and
  the two definitions collide.
- **Keeping exactly one call that formats a `std::string` does build**, which locates the
  requirement precisely: `format<std::string&>` is the specialization the reconciliation needs. But
  it saves nothing measurable — three interleaved rounds give −0.17 s and −0.08 s on two TUs, with
  the baseline side spreading wider than that. The cost is the *specialization*, not the number of
  call sites, so keeping one keeps the bill.

So the 0.85 s is real and load-bearing at once. It is a compiler-interaction cost
([COMPILER_ISSUES.md](../compiler/COMPILER_ISSUES.md) territory, and the same `<format>` fragility
[the PCH attempt hit](#import-storm-inside-the-pch--a-regression-not-a-win-633)), not a test-code
cost, and it moves only if the textual/modular `std` reconciliation stops needing a witness.

**The probe said otherwise, and the probe was wrong.** The concatenation rung compiled fine as a
standalone probe because a minimal TU never instantiates `CollatedField` — the `where.cppm`
`std::format` call whose merge the header was silently underwriting. A probe establishes a cost; it
cannot establish that a change is *possible*. Build the tree before believing a probe's saving.

## The YAML corpus is not the problem

It registers **494 of the suite's 3093 tests (16%) for 5.3% of compile time**.
Inside the heaviest of its four TUs (36.5 s), `-ftime-trace` splits as: template
instantiation ~55%, codegen ~20%, and the consteval JSON parsing that the corpus
is often blamed for — `EvaluateAsConstantExpr` 1.6 s + `EvaluateAsInitializer`
2.8 s — **~5-8%**.

Per registered test the corpus costs 0.32 s against ~0.98 s for hand-written
tests, but that comparison is confounded: hand-written TUs pay the fixed floor
110 times and test structurally heavier things. The honest statement is that the
whole question is bounded by ~160 s in either direction, so **compile time is not
a good reason to keep or drop the corpus**.

Note that the cost driver is the number of *distinct compile-time queries*, not
the corpus format. Hand-porting the same cases pays the same instantiation.

---

## What was tried and rejected

Each of these looked promising and was killed by measurement.

### ccache — safe, but the BMIs are not byte-reproducible

Measured twice, with opposite conclusions. The second measurement (ccache
**4.14**, issue #630) supersedes the first, which is kept below only to say why
it was wrong. The ccache version the first round used was not recorded, but the
difference is methodological, not a version difference — see there.

Test TUs cache beautifully: 8.8 s → **0.13 s**, 100% cacheable, direct hit.
`.cppm` compiles are 100% uncacheable (`unsupported_source_language:
c++-module`) and fall through at no cost, so the 85% of build time that lives
in the ~110 test TUs is the part in play. Two preconditions:

- `CCACHE_SLOPPINESS=pch_defines,time_macros` is **mandatory** — without it the
  gtest PCH makes every test TU uncacheable (`Result:
  could_not_use_precompiled_header`), reported as a flat 0% cacheable with no
  other explanation.
- The launcher must not reach `.cppm` compiles expecting hits; it costs nothing
  but never returns any.

**ccache 4.14 is module-aware and safe here.** Its log shows `Hashing module
file …bmi` for every BMI in CMake's `.modmap`, so a rebuilt module interface
changes the cache key. The `#embed` corpus is covered too, by a different
route: direct mode misses on those 4 TUs (`direct_cache_miss`) and ccache falls
back to running the preprocessor, whose output carries the embedded bytes
(`preprocessed_cache_hit`). Mutating `unified_cases_select.json` correctly
produced a miss.

**What kills it is that BMIs are not byte-reproducible.** Two back-to-back
`--precompile` runs of the same untouched `src/storm.cppm` differ in content
*and in size* (14,933,024 vs 14,933,008 bytes; the first difference is a
~20-byte signature region at offset 11860). `-Xclang -fno-pch-timestamp` does
not help. Because ccache hashes the BMI bytes, **every BMI rebuild permanently
invalidates every downstream test-TU entry**. On 5 test TUs:

| scenario | hits | wall |
|---|---:|---:|
| objects deleted, BMIs untouched | 5/5 | 125 ms |
| `touch` 2 module sources (content identical), BMIs rebuilt | 0/5 | 30.7 s |
| same churn again | 0/5 | 29.9 s |
| objects deleted, BMIs untouched again | 5/5 | 152 ms |

So hits occur exactly while the BMIs on disk are unchanged, which excludes
every workflow that motivates a compiler cache: `rm -rf build`, a fresh
worktree (which the branching rules mandate per branch, and which also moves
the source paths), a CI run in a fresh container, and any branch switch
touching `src/`. What remains is a branch flip or `git stash pop` touching
**only** `tests/` — real, but narrow. Not wired in.

The upstream dependency worth watching is **clang BMI reproducibility**, not
ccache's module support. Any compiler cache that hashes BMIs inherits this;
one that does not hash them is unsafe. `sccache` was not measured.

#### The superseded first round

The earlier round concluded "ccache does not hash the BMIs a TU imports" from
this observation — editing `src/orm/queryset.cppm` and recompiling a test TU
through ccache returns a HIT with a byte-identical object:

```
before editing the module : md5=7a7510190425  hits=1
after  editing the module : md5=7a7510190425  hits=2
```

That reproduces on 4.14, and is **not** a stale hit. It only appears when the
TU's command line is replayed directly, without letting ninja rebuild the BMI
first — and against an unchanged BMI on disk, that object is exactly what a
cacheless clang would produce. Driven through ninja, the BMI is rebuilt before
its dependents and ccache misses (row 2 of the table above). The `#embed` half
of that finding is likewise obsolete, per the preprocessed-mode fallback above.

### UNITY_BUILD — mutually exclusive with module scanning

CMake **silently** disables `UNITY_BUILD` whenever `CXX_SCAN_FOR_MODULES` is ON,
with no warning:

```
CMAKE_CXX_SCAN_FOR_MODULES=OFF -> "Unity" edges in build.ninja: 4
CMAKE_CXX_SCAN_FOR_MODULES=ON  -> "Unity" edges in build.ninja: 0
```

Scanning cannot be turned off for these TUs, because a TU with `import std;` and
`CXX_SCAN_FOR_MODULES OFF` fails with `fatal error: module 'std' not found` — no
module map is generated for it.

### `extern template` — no effect on time

It works mechanically (object 10.9 MB → 10.2 MB, symbols 8404 → 7736) but does
not move compile time, because it suppresses codegen and codegen is 0.03 s of the
floor. An early "-47%" reading was a cold-cache artifact of the first compile in
a session; with warm caches and min-of-3 the effect is zero or slightly negative.

### `std::visit` over `ExpressionVariant` — swapped for a `switch`, no reproducible effect

`ExpressionVariant` has **28 alternatives** (27 before `InExpression<bool>`, #625), and `bind_params_direct` is templated on the
statement type, so libc++'s `__make_fmatrix` / `__visit_alt` tower is rebuilt once per backend in
every TU that runs a WHERE. Self-time attributable to that machinery in the TU above (containment
stack over `-ftime-trace`, 20219 events) is **0.77 s of 11.48 s — 6.7%**, which projected to
~100 s over the suite: bigger than anything in [What was fixed](#what-was-fixed) except the PCH,
library-side rather than test-side, and free of any coverage trade. It was worth trying.

Replaced both visit sites with one `visit_expression` helper — a `switch` over `index()` with 27
explicit cases and a `static_assert` on `variant_size_v` as the guard against an alternative being
added without its case. It is correct (3093 tests, 3092 passed and the 1 skip is the pre-existing
PG `COLLATE NOCASE` one) and runtime-neutral: Release A/B on `Storm/WHERE/.*`, 5 interleaved
rounds of 12 repetitions, cv 1.4-2.6%, worst delta **+0.23%** — an order of magnitude under the
noise, as expected since a switch over `index()` lowers to the same jump table.

**But the compile-time win does not reproduce.** Three interleaved rounds, rebuilding and
re-measuring each side (medians):

| TU | `std::visit` | `switch` | Δ |
|---|---:|---:|---:|
| `crud/test_conditional_update.cpp` | 10.95 | 11.25 | **+0.30 (+2.7%)** |
| `query/test_where.cpp` | 10.42 | 10.04 | **−0.38 (−3.6%)** |

Opposite signs, and the within-side spread (10.96-11.30 against 10.64-11.00) is the size of the
between-side gap. The mechanism is visible in libc++: `std::get_if<N>` reaches its alternative
through `__get_alt<N>` recursing down the union tail, so the change trades one template tower for
another of about the same cost. The 0.77 s is a real *label* for the work; it is not removed by
changing how the dispatch is spelled. Reverted.

What this does not close: the 0.77 s scales with the **number of alternatives**, and 27 is a
choice — `ComparisonExpr<T>`, `BetweenExpr<T>` and `InExpression<T>` contribute one alternative
per value type. Nesting the value variant inside a single node type would make the outer variant
~5-wide. Unmeasured, and a much larger refactor (every node's `to_sql`/`bind_impl`), so it is a
hypothesis, not a plan.

#### The measurement mistake this cost, worth not repeating

The first, uncontrolled reading of this change was **11.48 s → 10.70 s (−6.8%)**, matching the
trace's 0.77 s almost exactly — which is precisely what made it convincing. It was an artifact:
the two runs sat on opposite sides of a BMI rebuild, and the doc's own warm-up rule
([Method](#method)) does not cover a cold *rebuilt* BMI. The tell was in the same table and was
missed: `n=0` moved too (2.07 → 1.94), and a variant with no test blocks at all cannot be sped up
by a change inside a WHERE visitor. **Any single before/after spanning a library edit is
worthless here; interleave rebuild-and-measure rounds and read medians per side**, exactly as
`/benchmark` requires for runtime. Same trap, different resource — see the `extern template`
entry above, whose "-47%" died the same way.

### `import storm;` inside the PCH — a regression, not a win (#633)

The obvious follow-up to precompiling gtest is to precompile the storm import
alongside it, paying the reconciliation once instead of ~110 times. It is
mechanically possible and measurably worse.

CMake does not scan the PCH source for module imports — its
`compile_commands.json` entry carries no `@...modmap` — so the PCH build fails
outright with `fatal error: module 'std' not found`. Appending a real test TU's
modmap by hand gets it to build, and then:

| PCH contents | PCH build | TU (strings) | TU (`TYPED_TEST`) |
|---|---:|---:|---:|
| gtest only (shipping) | 2.15 | **1.00** | **1.01** |
| gtest, then `import storm;` | 8.07 | 1.27 | 1.31 |
| `import storm;`, then gtest | **compile error** | — | — |

Every TU gets **0.27-0.30 s slower**, which follows directly from the mechanism
above: the import does not amortise, it enlarges the lookup set that the PCH's
own contents are then resolved against. Importing storm *before* gtest does not
compile at all — the same textual-libc++-versus-module clash that already forces
four groups of files onto `SKIP_PRECOMPILE_HEADERS` in `tests/CMakeLists.txt`.

Folding the other lever's headers in fails the same way: a PCH of gtest +
`import storm;` + `test_db_helpers.h` + `test_models.h` dies on
`test_db_helpers.h`'s global-module forward declaration of
`storm::db::sqlite::Connection` (`declaration of 'Connection' in the global
module follows declaration in module storm_db_sqlite`). Dropping storm and
precompiling `test_db_helpers.h` alone — the half that needs no storm import —
breaks 3 of 4 sampled TUs inside libc++'s `<format>`
(`call to implicitly-deleted default constructor of 'formatter<...>'`), and the
one TU that still compiles is +0.08 s. So the ~150 s `test_db_helpers.h` lever
is **not** reachable by extending the PCH.

### Narrowing `storm_orm_queryset`'s exports (#633)

Issue #633's second proposed direction. There is nothing to narrow: importing
`storm_orm_queryset`'s dependency list *without* `storm_orm_queryset` costs
+0.67 s against +0.64 s for the module itself. The facade's own content is free;
the cost is the transitive surface a QuerySet consumer needs regardless.

### Tests importing submodules instead of the umbrella (#633)

Issue #633's first proposed direction. It works and it is small.

Of the ~110 test TUs, **9** import `storm` without needing
`storm_orm_queryset` — every other TU reaches `storm::QuerySet` through
`tests/test_models.h`'s fixtures. Three of the nine cannot be converted anyway:
`schema/test_valid_foreign_key_concept.cpp` and `query/test_fields_selector.cpp`
spell `primary`, and `schema/test_dialect_concepts.cpp` calls
`storm::begin` — names that exist only in the umbrella (`primary` is
#442's top-level re-export; converting would mean reverting those tests to
`storm::meta::`).

The remaining six, replayed with their own commands from
`compile_commands.json`:

| TU | `import storm;` | submodules | delta |
|---|---:|---:|---:|
| `schema/test_stitch_key.cpp` | 1.47 | 0.45 | −1.02 |
| `schema/test_valid_field_info_concept.cpp` | 0.96 | 0.37 | −0.60 |
| `schema/test_bindable_concept.cpp` | 3.27 | 1.90 | −1.37 |
| `db/test_statement_move.cpp` | 0.84 | 0.21 | −0.63 |
| `db/test_cache_invalidation.cpp` | 2.44 | 1.21 | −1.23 |
| `db/test_sqlite_tuning.cpp` | 4.98 | 2.94 | −2.04 |
| **total** | **13.97** | **7.08** | **−6.89** |

−6.9 s of ~3000 s (0.2%), in exchange for six test files naming internal module
names. Not adopted. `errors/test_error_construction.cpp` and
`yaml/test_init_dataset_size.cpp` already import submodules directly, so the
option remains open per-file if a TU ever needs it.

### Dropping the second backend from `TYPED_TEST`

Every `TYPED_TEST` over `DatabaseTypes` instantiates its body twice, once per
backend, which looks like an obvious 2x. It is not: measured by compiling real
TUs as-is, then again with `DatabaseTypes` narrowed to SQLite alone (bodies
untouched, min of 3, ccache off).

| TU | 2 backends | 1 backend | delta |
|---|---:|---:|---:|
| `schema/test_types.cpp` | 20.45 | 16.53 | **−19.2%** |
| `query/test_distinct.cpp` | 17.11 | 15.00 | −12.3% |
| `crud/test_conditional_update.cpp` | 13.75 | 11.57 | −15.8% |
| `query/test_sql_verify.cpp` | 12.10 | 10.52 | −13.1% |
| `query/test_collate.cpp` (control) | 12.10 | 12.21 | +0.9% |
| **total** | **75.51** | **65.83** | **−12.8%** |

`test_collate.cpp` is the control: it declares `SqliteTypes`, not
`DatabaseTypes`, so the edit could not reach it. Its +0.9% is the noise floor,
which puts the other four comfortably in signal.

So the second backend costs **~13%**, not ~50% — the two instantiations share
most of their work. Over the 2558 s the hand-written test TUs cost, that is
~330 s of a ~3000 s build (~11%): the largest single lever measured so far, and
the only one still available at that size.

**Not adopted, and not recommended.** It buys ~11% by deleting the PostgreSQL
half of the suite's coverage — the thing cross-backend tests exist for, and
which CLAUDE.md requires for exactly the failure class (SQLite and PG disagree
about what is an error) that composite-PK and FK work keeps hitting. Recorded
here so the trade is known and nobody has to re-measure it: the coverage is
cheap, at 13% of test compile time.

### Replacing GoogleTest — poor return

The ceiling is gtest's 2.2 s, but any framework includes standard headers
textually, so the +2.1 s storm/libc++ reconciliation stays regardless. That
ceiling is available from a PCH instead, without rewriting 3093 tests.

### Computing `col_def_buffer` once and passing it as an NTTP — half the win (#682)

The first fix for the per-column re-evaluation [below](#exact-size-create-table-buffer-682):
evaluate `col_def_buffer<D>()` once in the table builder and pass it to
`build_column_def` as a template argument. It removes the 0.66 s template-argument
hotspot and halves `arr`, but leaves every function instantiation in place
(16270 → 16270), costs a new 0.16 s `col_buf` initializer, and keeps both the
per-column buffer copy and the hand-maintained sizers. Superseded by writing columns
straight into one buffer sized by the writer itself, which is cheaper on every
counter and deletes code instead of adding a parameter.

### A per-field reflection cache in `SchemaStatement` — more work, not less (#670)

The hypothesis: `SchemaStatement` asks the same questions of each field several
times over (`identifier_of`, `dealias`, `find_fk_primary_key` in the column
builder, the FK-clause sizer, the CREATE TABLE sizer and the index builder), so
answer them once into a `static constexpr std::array<FieldMeta, N>` and read that
everywhere. Measured as an ablation against the
[lazy CREATE TABLE change](#lazy-per-dialect-create-table-sql-670) it shipped
beside, same host, deterministic event counts:

| TU | lazy only | lazy + `FieldMeta` |
|---|---:|---:|
| `test_orm_schema.cpp` — InstantiateFunction / EvaluateAsConstantExpr | 3387 / 17733 | 3415 / 17872 |
| `test_referential_integrity.cpp` — same | 3942 / 17413 | 3975 / 17611 |

It **adds** ~30 function instantiations and ~140-200 constant evaluations per TU
and moved no wall time. The cache is itself a `static constexpr` member, so it is
built eagerly for every instantiated `SchemaStatement<T>` — whether or not that
TU ever generates DDL — and each repeated query it replaced was already cheap,
memoised reflection. Dropped.

---

## What was fixed

| change | effect |
|---|---|
| removed dead `create_table_sql` globals from `tests/test_models.h` (referenced by nothing, but forced full consteval schema generation in all 89 including TUs) | −0.7 s/TU ≈ **−62 s** |
| `#embed` corpora declared as real build inputs (`OBJECT_DEPENDS`) | correctness |
| precompiled `<gtest/gtest.h>` for `storm_tests` | −2.15 s/TU ≈ **−220 s** (understated — see below) |
| `run_clang_tidy.py --diff` no longer reports a crashed clang-tidy as a clean pass | correctness |
| split `shared/models.h` + `tests/test_models.h` so a TU pulls only the models and helpers it uses (#634) | −0.67 s/TU ≈ **−64 s** |
| `SchemaStatement::create_table_sql<D>()` builds only the dialect a TU asks for (#670) | −8% function instantiations in a mostly-single-dialect schema TU, 0 in one using both; wall time below the noise floor — see [below](#lazy-per-dialect-create-table-sql-670) |
| CREATE TABLE written straight into one exact-size buffer, measured by the writer itself (#682) | slowest TU `test_ddl_execution_audit.cpp` 13.10 → 10.33 s (−21%, median of 3 alternated rounds), −16.8% function instantiations; ~−6% instantiations in a light schema TU — see [below](#exact-size-create-table-buffer-682) |

≈ **−344 s of ~3000 s (−11%)** — a sum of per-change deltas, not a measured
total. For an actual whole-build number on the current tree (with the host it
was taken on, which this baseline lacks), see
[Re-measured after #634](#re-measured-after-634-2026-09-08).

The gtest PCH row understates what it bought. It was measured as the removal of
gtest's own share of the floor, but it also took most of `import storm;` with it
— +1.38 s per TU down to +0.24-0.79 s, roughly another 100 s — because the
reconciliation it removes is between storm's BMI and gtest's *textual* libc++
declarations. See [After the PCH](#after-the-pch-issue-633).

### Splitting `shared/models.h` per model (#634) — rejected on probes, adopted on measurement

Issue #634's proposal: every test TU parses 12 models and generates 49
field-selector proxies to use one, so give each model its own header. Measured
after the gtest PCH landed, on the same host as
[After the PCH](#after-the-pch-issue-633). Baseline **B** is
`gtest(PCH) + import storm; + import std; + test_db_helpers.h`; probes add a
generated header carrying the first N model structs and their `fields::` blocks.

The header block is genuinely expensive — more so than #634 measured, because the
PCH shrank everything around it:

| probe (empty body) | sec | Δ over B |
|---|---:|---:|
| A — without `test_db_helpers.h` | 0.40 | — |
| **B** — baseline | 0.49 | +0.09 |
| B + 1 model, no `fields::` proxy | 1.10 | +0.65 |
| B + 11 models, no proxies | 1.48 | +1.03 |
| **B + 1 model + its proxy** | **2.09** | **+1.64** |
| B + 3 models + 3 proxies | 2.11 | +1.66 |
| B + 5 models + 5 proxies | 2.42 | +1.97 |
| B + 7 models + 6 proxies | 2.54 | +2.09 |
| **B + 11 models + 6 proxies** | **2.59** | **+2.14** |
| B + full `shared/models.h` (12 + seed arrays) | 2.69 | +2.20 |
| B + full `test_models.h` (+ helpers, fixture) | 3.19 | +2.70 |

**The cost is an intercept, not a slope.** The first model and its first proxy
cost +1.64 s; the other ten models and five proxies together cost +0.50 s, about
0.05 s each. Split finer: the first annotated struct is +0.65 s, the first
`define_aggregate` + `FieldRef`/`Field` proxy is +0.99 s, each subsequent struct
~0.04 s, each subsequent proxy ~0.02 s. The seed arrays (`PEOPLE_25`,
`MESSAGES_8`) are +0.06 s — not worth moving.

Since every test TU needs at least one model, a per-model split cannot touch the
intercept. It can only recover the slope, and only for TUs that need one model.

**With a real body it recovers even less.** The probes above have trivial bodies;
a real TU calls `QuerySet<Person>` and `fields::Person`, which instantiates the
same machinery the intercept pays for. Same probes, body one `where().order_by()
.limit().select()` plus a `count().execute()`:

| probe (real body) | sec |
|---|---:|
| 1 model | 6.55 |
| 11 models | 6.88 |
| full `shared/models.h` | 7.15 |
| full `test_models.h` | 6.87 |

**~0.3-0.6 s on a ~6.9 s TU**, and note that full `test_models.h` measured
*below* full `shared/models.h` despite being a strict superset — the noise floor
at this magnitude is ~0.3 s, the same size as the effect. Extrapolated over the
89 including TUs that projects roughly **30 s of a ~3000 s build (~1%)**, against
#634's estimate of ~150 s.

The mechanism is the same one [After the PCH](#after-the-pch-issue-633) found for
`import storm;`: the cost is *entering* the reflection machinery, not the volume
pulled through it. `test_db_helpers.h`, which #634's title names alongside the
models, is +0.09 s and was never the problem.

#### What the probes missed, and why the split shipped anyway

Those probes vary only the *model* content, so they answer "how much do the
other eleven models cost" and not "how much does the umbrella cost". Two things
the split removes are outside what they varied:

Counts used below, since the earlier text says 89: **82 files** named
`test_models.h` at HEAD (81 TUs plus the umbrella's own usage example), and
**97 compilation units** reached `shared/models.h` through their include
closure — the extra 15 arrive via shared body headers. 97 is the denominator
for the extrapolation.

- **The helper block.** `make_record`/`make_updated_record`/`is_original_record`
  and `populate_join_test_data` are used by 2 and 2 TUs respectively, but every
  TU parsed them. The probe table above prices helpers+fixture together at
  +0.50 s and never separates them; the fixture is what almost every TU actually
  wanted.
- **TUs that need no shared model at all.** 14 of the 97 declare their own
  models and used the umbrella purely for `StormTestFixture`.

Measured A/B on the real tree rather than on probes — same host, same BMIs, the
split stashed and unstashed between runs, min of 3 after a warm-up, ccache off:

| TU | shared models it needs | before | after | Δ |
|---|---|---:|---:|---:|
| `db/test_pool.cpp` | Person | 7.02 | 5.64 | **−1.38** |
| `schema/test_ddl_execution_audit.cpp` | all 11 | 38.77 | 37.47 | **−1.30** |
| `crud/test_select.cpp` | Person | 11.45 | 10.31 | −1.14 |
| `schema/test_max_length.cpp` | none | 8.07 | 7.11 | −0.96 |
| `schema/test_types.cpp` | 7 | 23.90 | 23.14 | −0.76 |
| `schema/test_short_annotations.cpp` | none | 6.22 | 5.66 | −0.56 |
| `query/test_many_to_many_sqlite.cpp` | none | 10.99 | 10.64 | −0.35 |
| `query/test_where_temporal.cpp` | ExtendedTypes | 13.52 | 13.26 | −0.26 |
| `query/test_where.cpp` | 3 + seed array | 14.61 | 14.68 | +0.07 |

The decisive row is `test_ddl_execution_audit.cpp`. It needs *every* model, so
the per-model split cannot help it, and it still gained 1.30 s — that is the
helper block alone. **The intercept finding stands; it was just not the whole
cost.** Adopted on that basis, not on the per-model slope.

That row was measured with the audit TU on per-model includes. It **ships on the
umbrella instead**, deliberately: it is the only TU that wants every model, and
making it the umbrellas' one consumer is what keeps `test_models.h` and
`shared/models.h` compiled at all — a header nothing includes is never parsed and
rots silently. So its 1.30 s is given back on purpose, and the shipped saving is
the other eight rows:

mean **−0.67 s/TU**, median −0.66, 7 of 8 negative — ≈ **−64 s** over the 96
remaining TUs. Still about 2x what the probes projected, and the same magnitude
as the `create_table_sql` removal already in [What was fixed](#what-was-fixed).

### The schema module under `-ftime-trace` (#670)

Captured with `scripts/time_trace.py` (see [Method](#method)) at granularity 0,
baseline develop `b199413`. **Different host from the rest of this document**:
native macOS arm64, 8 cores, `ninja-debug` (coverage flags included), and
another worktree's build shared the machine for part of the session (load
average up to 77) — so wall times below are best-of-3 and still wander by
several seconds between rounds. The event **counts** are deterministic and are
what the conclusions rest on.

**The BMI itself** (`src/orm/schema.cppm`, traced as its `--precompile` job):
3.4 s on the quietest round, of which **`WriteAST` — serializing the BMI — is
~70% (2.3 s)** and the whole frontend ~1.0 s (`ParseClass` 907 events / 0.43 s,
the largest single one `SchemaStatement` itself at ~0.3-0.9 s;
`InstantiateFunction` 201 events, almost all libc++ `vector<string>` /
`set<string>` machinery; constant evaluation 6697 events / 0.01 s). Nothing in
the module's own body is reflection-expensive: `SchemaStatement<T>` is a template
and is not instantiated until a consumer names a model. The change below leaves
every one of these counts identical, as it should.

**The consumers** are where schema cost lands. In `tests/schema/test_orm_schema.cpp`
(12 models, mostly SQLite DDL), top-level events naming `SchemaStatement` add up
to ~1.9 s of a ~7 s TU. The trace showed why: the largest of them was the
**class** instantiation — `SchemaStatement<Person>` 2.7 s — and inside it the
`EvaluateAsInitializer` of `sqlite_create_table_sql_array_` **and**
`pg_create_table_sql_array_`, for every model, in every TU.

#### Lazy per-dialect CREATE TABLE SQL (#670)

Those two arrays were `static constexpr` data members, and Clang instantiates a
`static constexpr` member's initializer together with its class. So any TU that
instantiated `SchemaStatement<T>` for any reason — `create_index_sql<T>()`,
`create_table_sql<T>()` for one dialect — paid the full consteval CREATE TABLE
build for **both** dialects. They are now a function-local `static constexpr`
inside `create_table_sql<D>()`, generated only for the dialect a TU asks for.
DDL text is unchanged (same `build_create_table_sql<D>()`; the suite's exact-DDL
assertions pass on both backends).

| TU | InstantiateFunction | EvaluateAsConstantExpr | best-of-3 wall, median of 3 rounds |
|---|---:|---:|---:|
| `test_orm_schema.cpp` before | 3683 | 18710 | 6.98 s |
| `test_orm_schema.cpp` after | **3387 (−8.0%)** | **17733 (−5.2%)** | 6.50 s |
| `test_referential_integrity.cpp` before | 3978 | 17557 | 8.32 s |
| `test_referential_integrity.cpp` after | 3942 (−0.9%) | 17413 (−0.8%) | 7.81 s |
| `test_types.cpp` before / after | 7216 / 7216 | 42314 / 42314 | — |

Same-load single traces of `test_orm_schema.cpp` show where it went:
`SchemaStatement<Person>` class instantiation **2.72 s → 1.02 s**, `<Task>`
1.13 → 0.33 s, `<Message>` 1.08 → 0.35 s, and the `*_create_table_sql_array_`
initializers gone from the class-instantiation path entirely.

**What it is worth is bounded by how many dialects a TU uses.** A `TYPED_TEST`
over both backends asks for both, and `test_types.cpp` gets nothing — its counts
are identical. The saving is real only where a model is instantiated but one
dialect (or no CREATE TABLE at all) is used. The wall-time column moves the
right way on both TUs that changed, by ~0.5 s, but this session's noise was
larger than that — and the "after" wall times come from a separate block of rounds
(the shipped code built as an ablation tree), not interleaved with the "before"
ones — so read it as consistent with the counts rather than as a measured number — per the [Method](#method) rule, a whole-build A/B on a quiet
host is what would turn it into seconds.

**Trade-off:** the consteval guards inside `build_create_table_sql<D>()` (the PG-only `has_pinned_bigint_pk` cross-check, the sizer-overflow `std::unreachable()` backstops — one exact-length check since [#682](#exact-size-create-table-buffer-682)) now fire per dialect, only in a TU that requests that dialect's DDL for a model — not for both on every instantiation.

#### Exact-size CREATE TABLE buffer (#682)

`tests/schema/test_ddl_execution_audit.cpp` is the slowest TU in the tree: ~61
models × 2 dialects through `TYPED_TEST`, so ~122 `build_create_table_sql<D>()`
evaluations. Traced at granularity 0 (same host and method as #670 above), the
`arr` initializer in `create_table_sql<D>()` was 1.08 s over 122 evaluations, and
a further **0.66 s / 416 events** sat on one line: `build_column_def<Index, D>`
declared its buffer as `ConstexprString<col_def_buffer<D>()>`. A consteval call in
a template argument is **not memoised** — it is re-evaluated for every column
instantiation, and the #361 backstop `col.len >= col_def_buffer<D>() - 1` ran it
once more per column inside `arr`. `col_def_buffer` loops every field
(`identifier_of`) and folds `max_composite_fk_columns_len`, so the DDL build was
O(fields²) per model per dialect.

Three steps, each measured (deterministic counts, `test_ddl_execution_audit.cpp`):

| variant | InstantiateFunction | EvaluateAsConstantExpr | EvaluateAsRValue | `arr` |
|---|---:|---:|---:|---:|
| baseline (develop `82e7cfd`) | 16270 | 84303 | 66908 | 1.08 s |
| `col_def_buffer` computed once, passed as an NTTP | 16270 | 84009 | 66908 | 0.59 s (+0.16 s `col_buf`) |
| columns appended straight into the table buffer (hand sizers kept) | 15352 | 79782 | 60813 | 0.39 s |
| **+ buffer sized by running the writer against `ClauseSizer` (shipped)** | **13539 (−16.8%)** | **72521 (−14.0%)** | **55206 (−17.5%)** | 0.29 s |

What shipped is the last row. Each column is written straight into the CREATE
TABLE sink (`append_column_def<Index, D>(sql)`) — no per-column `ConstexprString`,
whose by-value copy into the table SQL was itself a measurable share of `arr` —
and the whole statement is sized by running the **same writer** against
`detail::ClauseSizer` first, the technique `build_junction_sql` already used
(#504). That made the hand-maintained sizers dead, and they were deleted:
`col_def_buffer`, `column_size_budget`/`ColumnSizeBudget`, `calculate_column_defs_size`,
`calculate_create_table_sql_size`, `fk_references_len`/`max_fk_references_len`,
`max_default_clause_len`/`max_max_length_clause_len`, and the composite-FK
column/constraint size helpers — about 300 lines whose only job was to predict
text the writer then emitted, and which #361, #412, #413, #493, #504 and #506 each
had to widen by hand when a clause grew. The buffer is now exact (it used to carry
`XL_BUFFER` = 100 bytes of slack per table), and the backstop becomes
`sql.len != size - 1` → `std::unreachable()`: if the sink and `ConstexprString`
ever counted differently the build fails rather than truncating.

Wall time, best-of-3 per round, three rounds alternated A/B/A/B/A/B with a BMI
rebuild before each (per [Method](#method)):

| TU | baseline rounds | shipped rounds | median Δ |
|---|---|---|---:|
| `test_ddl_execution_audit.cpp` | 13.04 / 13.10 / 13.43 s | 10.33 / 10.24 / 10.56 s | **−2.77 s (−21%)** |
| `test_orm_schema.cpp` (12 models, mostly one dialect) | 4.47 / 4.30 / 4.57 s | 4.99 / 4.18 / 4.25 s | −0.21 s, within noise |

The second TU's counts move the same way (InstantiateFunction 3404 → 3198, −6.1%;
EvaluateAsConstantExpr 18164 → 17231, −5.1%), but that is below its wall-time noise
floor. The saving scales with columns × dialects actually generated, so the
heavy DDL TU is where it shows. DDL text is unchanged — the writer's append
sequence is the same, only the buffer it lands in moved — and the suite's
exact-DDL assertions pass on both backends.

**What is left in the DDL path is not worth chasing.** After the change the
largest DDL events are `arr` (0.29 s / 122) and the exact sizing pass (~0.12 s /
122); everything above them in the trace is per-model concept checking (below)
and gtest. The class-scope `index_sql_strings_` is still built eagerly per model
(#670's lesson) but was not on this TU's top list.

**Follow-up, not done here:** `count_primary_key_flavours<T>()` (`model_concepts.cppm`)
is evaluated twice per model — once by `ModelPrimaryKeyValid`, once by
`ModelPrimaryKeyPartLimit` — ~0.23 s each over 61 models in this TU. Every TU that
instantiates a `BaseStatement<T>` pays it, so it is a library-wide lever outside
#682's DDL scope; a variable template (instantiated, hence memoised, once) would
halve it. Measure before adopting.

### Memoised reflection lookups (#694)

After #682 the DDL text was no longer the cost. A bottom-up micro-lab (one generated
TU per primitive, swept over N, slope fitted; Apple M1, clang-p2996 Release, no
assertions) priced what the remaining per-model work is made of:

| primitive | cost |
|---|---:|
| trivial template instantiation | ~10 µs |
| `-O0` codegen, one function | ~70 µs |
| consteval: one loop step / one `constexpr` call | ~2 µs / ~11 µs |
| `std::meta::nonstatic_data_members_of(^^T, …)` | ~1.3 ms + ~75 µs per field, **every call** |
| `std::meta::annotations_of(member)` | ~0.4 ms, every call |
| the same member list read back from a variable template | ~1.7 µs |

Stock Apple clang 17 runs the consteval calibration at the same speed, so this is
clang's tree-walking evaluator, not the toolchain build. The two `<meta>` queries are
not compiler builtins: they are libc++ code run by that evaluator (`members_of` builds a
`std::vector`, then copies and filters it twice). The rule that matters: **a consteval
call is never memoised; a template instantiation is.**

Measured against the real BMIs with generated models, instantiating `SchemaStatement<M>`
(concepts + `BaseStatement` statics) cost **~56 ms + ~4.5 ms per field**, and both
dialects' `CREATE TABLE` text only ~30 ms more. That per-model work re-derives the same
answers over and over: 26 call sites run `nonstatic_data_members_of(^^T)` for the same
`T`, and every flag predicate re-runs `annotations_of` — `is_primary_member` alone is up
to four calls per field, looped by every model concept.

The fix memoises both, in variable templates that hold **plain values**:

- **The member list:** `data_members_of_v<T>` (in `storm_orm_field_attr`) is a
  `std::array<std::meta::info, N>` built once per `T`. The 26 `^^T` scans read it. The
  `Entity` concept's structural `requires` probe stays a raw call.
- **The annotation answers:** `has_annotation_type<Tag>`, `fk_annotation_type_of`,
  `max_length_of` and `annotation_type_specializing` (in `relation_meta`) each store their
  *answer* (`bool` / `std::optional<info>` / `std::optional<std::size_t>`) per member, in a
  variable template. A predicate's `member` is not a constant expression, so it reaches
  that template through `memoised<R>` in the leaf module `storm_orm_reflection_cache`,
  which is `extract<R>(substitute(var_template, args))`.

Wall time, best-of-3 per round, three rounds alternated develop/branch, same host:

| TU | develop rounds | #694 rounds | median Δ |
|---|---|---|---:|
| `test_ddl_execution_audit.cpp` | 16.37 / 15.30 / 15.36 s | 11.31 / 11.48 / 11.34 s | **−26.2%** |
| `test_orm_schema.cpp` | 6.02 / 5.99 / 6.00 s | 5.74 / 5.74 / 5.74 s | −4.3% |
| `query/test_distinct.cpp` | 10.10 / 10.12 / 10.09 s | 9.87 / 9.88 / 9.97 s | −2.2% |

The absolute seconds are higher than #682's 10.3 s because the host was loaded; the
rounds do not overlap. Of the −26.2%, the annotation answers alone are −16.5% and the
member list adds the rest.

Two variants were measured and dropped:

| variant | DDL TU | why dropped |
|---|---:|---|
| one per-member list of annotation types, shared by every predicate | −25.3% | worse: the answers are already memoised per (tag, member), so a shared list saves only each tag's first scan, and it costs a second `annotations_of` (one to size the array, one to fill it) |
| `ConstexprString::append` as one `std::char_traits<char>::copy` instead of a per-char loop | −26.6% | noise: 4.3x faster per append in isolation (322 → 75 µs for 23 chars), but the DDL writes too few characters for it to show |

**Memoise values, never `define_static_array` storage.** The first version stored both
lists as `std::span`s into `define_static_array` storage. It passed every test and broke
`storm_bench` (Release): `storm_benchmark_*` instantiate the statements for
`storm::benchmark::User` inside their BMI, and an importer reading that span back gets
"initializer of `data_members_of_v<User>` is not a constant expression". No `tests/` TU
had that shape, a model instantiated inside a module and then used from an importer, so
`tests/schema/test_bmi_fk_module.cppm` + `test_reflection_cache_bmi.cpp` now are that
shape. Against the span version that pair does not compile (clang-p2996 crashes); a
`std::array` or a scalar crosses the BMI like any `static constexpr` value.

Three things worth knowing before reading a trace of this change:

- **Event counts go the wrong way.** `EvaluateAsConstantExpr` rises on the DDL TU, because
  every `substitute` lookup is an evaluation of its own. Each is far cheaper than the
  `annotations_of` it replaces, so here the count is not a cost proxy, unlike #670/#682,
  where it was.
- **What no cache removes:** the first reflection query on a fresh class costs ~4.5 ms
  once per class (measured in Storm's module context), and the `SchemaStatement<M>`
  class-body instantiation itself is ~13 ms per model.
- **#262 and the BMI.** A member list or memoised answer built inside a module BMI is now
  reused by its importers, so member reflections do cross a BMI on this path. The BMI
  regression test above queries annotations (FK, `max_length`) through that path and
  passes; it is the test to extend if #262 resurfaces.

### Two correctness bugs found along the way

**`#embed` is invisible to the dependency scanner.** Clang does not list an
embedded file in the depfile, so ninja recorded no edge from the consuming object
to the JSON (`ninja -t deps` showed none). Editing `unified_cases.yaml`
regenerated the JSON and left the objects stale — the suite kept running the
previous corpus, green. The benchmark corpus had the identical bug.

**A crashed clang-tidy read as success.** `--diff` counted only `": error:"` and
`": warning:"` lines, and a SIGSEGV prints neither. `--full` already treated an
unexpected crash as failure; `--diff` did not.

### A build-state trap worth recognising

If a build is interrupted (Ctrl-C, OOM kill, a killed CI job), ninja's
`.ninja_deps` can be left truncated. Every subsequent build then prints

```
ninja: warning: premature end of file; recovering
```

and rebuilds the whole `import storm;` dependency graph despite unchanged
sources — indistinguishable from "compilation is just slow". `ninja -t recompact`
followed by **one build that runs to completion uninterrupted** restores true
incremental behaviour (verified: the next build was `no work to do`, 0 s).

---

## Remaining levers

| lever | ceiling | note |
|---|---:|---|
| ~~`import storm;` per TU~~ | ~~230 s~~ | **Closed by measurement (#633).** The gtest PCH already took it from +1.38 s to +0.5-0.8 s per TU. What remains is gtest's own instantiations slowing down because storm's declarations are in the lookup set — not storm being loaded — so it is not reachable from storm's side. Both directions #633 proposed were measured and rejected above; together they are worth ~15 s. |
| ~~`test_db_helpers.h` + `shared/models.h`~~ | ~~150 s~~ | **Done (#634), for ~half the reason the issue gave.** The model cost is an *intercept* — the first model and its first `fields::` proxy are +1.64 s, the other eleven models +0.50 s — so the per-model split reaches only the slope. What made it worth doing is the part the probes bundled in with the models: the helper block every TU parsed for 2 TUs' benefit, and the 14 TUs that need no shared model at all. Measured **−0.67 s/TU ≈ −64 s** as shipped, not the ~150 s estimated nor the ~30 s the probes projected. `test_db_helpers.h` itself is +0.09 s and was never the problem. See [above](#splitting-sharedmodelsh-per-model-634--rejected-on-probes-adopted-on-measurement). |
| the test bodies themselves | ≤13% of a TU, not recommended | **Closed by measurement.** Of an average TU the bodies are ~41-45% and the fixture another ~37-41% ([above](#what-one-average-tu-is-made-of)). Inside the bodies the only levers are merging every `TYPED_TEST` into one block and neutering the assertions — together worth 12.9% in one session and 5.5% in another, so ≤13% is a ceiling, not a value. Both keep full coverage but cost failure granularity and diagnostic quality respectively. The rest is the distinct query shapes, i.e. the checklist's coverage. |
| `ExpressionVariant`'s 28 alternatives | ~100 s, unmeasured | The variant-visitation machinery is 6.7% of a TU, and it scales with the ALTERNATIVE COUNT — which is a design choice, since `ComparisonExpr<T>`/`BetweenExpr<T>`/`InExpression<T>` each contribute one per value type. Replacing `std::visit` with a `switch` does **not** reach it ([measured and reverted](#stdvisit-over-expressionvariant--swapped-for-a-switch-no-reproducible-effect)); nesting the value variant inside one node type to make the outer variant ~5-wide might, at the cost of a refactor of every node's `to_sql`/`bind_impl`. Measure a prototype before opening it as work. |
| the fixture + registration layer | ~0 reachable | **Closed by measurement.** Split into its axes [above](#inside-the-fixture-layer--and-why-its-biggest-item-cannot-be-moved): gtest's typed-test machinery is only ~10% of it, and the largest single item — 0.85 s per TU of `std::format` instantiation in `tests/test_db_helpers.h` — cannot be removed, moved, or reduced without the textual/modular `std` reconciliation collapsing in unrelated TUs. What remains is storm's `QuerySet` statics (~0.7 s) and its DDL generator (~0.9 s), both of which a test genuinely uses. |
| the reflection intercept itself | unmeasured | What the row above leaves on the table: ~1.6 s per TU to enter the machinery (first annotated struct +0.65 s, first `define_aggregate`/`FieldRef` proxy +0.99 s). Library-side, so it would benefit users and not only tests — but it is the cost of the reflection a querying TU needs anyway, so whether *any* of it is removable is unknown. Measure before opening it as work. |
| `storm_orm_schema`'s BMI serialization | ~2-7 s per library build, unmeasured as a saving | `WriteAST` is ~70% of building `schema.cppm`'s BMI; its frontend is ~1 s ([above](#the-schema-module-under--ftime-trace-670)). Paid once per library rebuild, not per test TU, so it bounds incremental rebuilds after a schema edit rather than the full build. Whether it is this module's own declarations or the imported ones it references (it imports `storm_orm_statements_base` and five more, re-exporting none) that dominate is not yet split. In the consumers, the largest remaining `SchemaStatement` instantiation is `create_index_sql`/`build_all_index_sql` (~1 s for `Person`) — single-dialect already, so laziness does not reach it. |
| clang's bytecode constant interpreter | unmeasured; blocked | `-fexperimental-new-constant-interpreter` runs plain consteval ~3.7x faster (calibration 6.77 → 1.84 s), and constant evaluation is ~44% of the heaviest TU. **Blocked (#693):** it cannot evaluate `<meta>` yet, so 33 of 44 modules fail to build with it (`value_bind` also on `std::format`); the 11 that pass save ~0.3 s once per library build. Per-module flags would not reach the cost anyway, which lands in the TUs that instantiate templates over models. Re-test on each toolchain bump. Independent of [#694](#memoised-reflection-lookups-694), which cuts the number of evaluations rather than their speed, so they would stack. |

---

## Method

Five scripts implement the measurements below, so a later investigation re-runs
them rather than rebuilding the harness (and rediscovering the mistakes in the
bullets that follow). Four of them answer "what does one TU pay" from different
angles; `ninjalog_stats.py` answers "where does the whole build's time go". They
share one replay harness, `scripts/lib/compile_replay.py`, so they agree on what
a timed compile is:

| script | answers |
|---|---|
| `scripts/compile_time_probe.py` | what a TU pays before its first assertion — generates probes carrying N models from `shared/models/`, replays a real TU's command against each, reports deltas |
| `scripts/typed_test_cost.py` | what the second backend costs — compiles real TUs as-is, then with `DatabaseTypes` narrowed to SQLite, bodies untouched |
| `scripts/ninjalog_stats.py` | where the whole build's time goes — parses `.ninja_log` into the bucket table above, deduplicating module edges and excluding scan edges |
| `scripts/tu_ablation.py` | what ONE TU's time is made of — ablates a real test file (drop bodies, merge them into one block, neuter the assertions, narrow the fixture) and recompiles each variant |
| `scripts/time_trace.py` | where ONE TU's (or one module's) time goes inside the compiler — replays it with `-ftime-trace -ftime-trace-granularity=0`, best of N, and prints clang's `Total <event>` aggregates plus the longest `InstantiateFunction`/`InstantiateClass`/`ParseClass`/constant-evaluation events, optionally `--filter`ed by detail |

```bash
scripts/dev-container.sh exec python3 scripts/compile_time_probe.py
scripts/dev-container.sh exec python3 scripts/compile_time_probe.py --body real
scripts/dev-container.sh exec python3 scripts/typed_test_cost.py
scripts/ninjalog_stats.py build/debug/.ninja_log     # after a build from scratch
scripts/dev-container.sh exec python3 scripts/tu_ablation.py \
    tests/crud/test_conditional_update.cpp --variants full,merge,sink,trivial1,n=0
scripts/dev-container.sh exec python3 scripts/time_trace.py src/orm/schema.cppm
scripts/dev-container.sh exec python3 scripts/time_trace.py \
    tests/schema/test_orm_schema.cpp --filter SchemaStatement --top 15
```

All four replay scripts refuse to run if `compile_commands.json` shows a
compiler launcher, since timing through a cache measures nothing.
`typed_test_cost.py` edits `tests/test_db_helpers.h` in place and restores it in
a `finally` block — run it on a clean tree, and check `git diff` if it is
interrupted. `tu_ablation.py` does **not** touch the tree: each variant goes to a
temporary file compiled with `-iquote <the TU's own directory>` added, so the
relative includes resolve as they do in the real build while the source stays
untouched, an interrupted run leaves nothing behind, and two runs cannot clobber
each other. `-iquote` rather than `-I` because the quoted-include search order is
*including file's directory, `-iquote`, `-I`, `-isystem`* — an appended `-I` would
land BEHIND the command's own `-I` list, where an earlier entry could shadow a
same-named header, and would widen the angle-bracket path the real build never
puts that directory on. Its ablated variants are compiled with the unused-entity warnings
suppressed — `full` included, so the rows stay comparable — because deleting
bodies orphans the file-scope helpers only those bodies used and the tree builds
with `-Werror`.

- **Per-TU timing**: replay a TU's exact command from `compile_commands.json`
  with `-o` stripped, serially, min of 3 runs. Timing through `ninja` instead adds
  the scan step and roughly ±2 s of noise; the replay harness holds ±0.1-0.3 s.
- **Always disable ccache when timing.** `CMAKE_CXX_COMPILER_LAUNCHER` set with
  `CACHE ... FORCE` survives removal of the module that set it — `cmake -U
  CMAKE_CXX_COMPILER_LAUNCHER .` clears it. A stale launcher silently served
  objects in 0.2 s and invalidated a whole measurement round.
- **Warm up before measuring.** The first compile in a session is inflated by
  cold page cache for the module BMIs — this produced a fake 47% win once.
- **A warm-up does not cover a REBUILT BMI, and one before/after pair is not a
  result.** To compare two versions of the library, the two runs necessarily sit
  on opposite sides of a BMI rebuild, and that alone moved a TU by ~0.8 s here —
  enough to manufacture a 7% win that three interleaved rounds erased. Rebuild
  and re-measure each side several times, alternating sides, and read the median
  per side, exactly as `/benchmark` requires for runtime. A cross-check that
  costs nothing: a change confined to one code path must leave the variants that
  never reach it (`n=0`) unmoved — if those shift too, the run is drifting, not
  the code. See
  [the std::visit entry](#stdvisit-over-expressionvariant--swapped-for-a-switch-no-reproducible-effect).
- **Whole-build breakdown**: parse `.ninja_log`, deduplicating on
  `(start, end, cmdhash)` — one module compile emits both a `.pcm` and a `.o`
  and logs a line per output, so the raw log double-counts every module edge.
  `scripts/ninjalog_stats.py` implements this; measure a build from scratch, as
  the log accumulates across runs.
- **Inside one TU**: `-ftime-trace`, then aggregate `InstantiateFunction` /
  `InstantiateClass` / `Source` events by `args.detail` — `scripts/time_trace.py`.
  It needs no extra preset or build tree: it replays from an existing build's
  `compile_commands.json` and writes the trace to `build/time-trace/`, so default
  builds are untouched. Prefer its event **counts** for an A/B — they are
  deterministic, while the durations carry the same noise as any replay.
- **A module interface unit is two compiler jobs.** For a `.cppm` the driver runs
  `-emit-module-interface` and then `-emit-obj` *from that BMI*, and both write
  the one `-ftime-trace` file — so a naive `-c` replay reports only the second,
  which never parses the source (`schema.cppm` read as **0.1 s** that way, against
  ~3.4 s for the real BMI build). `time_trace.py` traces a `.cppm` with
  `--precompile`, and drops the modmap's `-fmodule-output` so a replay can never
  overwrite the build's own BMI (which would make ninja rebuild every importer).
