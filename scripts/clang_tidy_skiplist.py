#!/usr/bin/env python3
"""Which files clang-tidy must not be pointed at (issues #550, #692).

The single source of truth consulted by scripts/run_clang_tidy.py's three modes
(--diff/--full/--all) and by commit.sh's BMI probe. --diff drops matching files'
diff sections before clang-tidy-diff.py ever sees them (filter_skiplist_from_diff);
--full/--all skip them after the fact when clang-tidy fails to parse them. Keeping
both lists in one module is what makes "stays consistent" (#550 DoD) mechanical
rather than a convention several call sites have to remember.

    clang_tidy_skiplist.py --keep      paths on stdin -> the ones not on either list

Stdlib only, no syntax newer than Python 3.9.
"""
import sys
from typing import Iterable, Iterator

# Known-skip list: files clang-tidy cannot parse and we accept that.
#
# These are precise file paths, not directory wildcards. The former broad
# "tests/*|benchmarks/*" wildcard was replaced in Issue #308 because 39 test
# files and most bench files parse fine - the wildcard was silently masking
# parse failures in files that should be clean.
#
# Files genuinely unparseable:
#   shared/query_builder.hpp - pseudo-module header; must be included after
#       `import storm;` so clang-tidy sees it without the BMI and fails.
#
#   Test headers that must come after `import storm;` - clang-tidy parses
#   them standalone and hits missing storm symbols:
#   tests/test_models.h, tests/test_seed_helpers.h, tests/test_select_runner.h,
#   tests/test_write_runner.h, tests/test_yaml_register.h,
#   tests/query/test_aggregate_fixture.h, tests/query/test_m2m_models.h,
#   tests/query/test_fields_models.h, tests/crud/test_composite_pk_models.h,
#   tests/schema/test_bmi_fk_models.h (#694 cross-BMI fixture; linted through
#   test_bmi_fk_module.cppm and test_reflection_cache_bmi.cpp),
#   tests/test_parser.hpp, tests/test_unified_yaml_body.h,
#   tests/tools/storm_schema/models.h, shared/models.h
#
#   tests/test_unified_yaml_body.h joined with the YAML-harness split (#561).
#   It needs `import storm;` like its siblings - that alone is the reason, since
#   #582 deleted the static_assert on STORM_UNIFIED_CASES_FILE this entry used to
#   cite (the header no longer names that macro in code at all). Do not prune the
#   entry on the strength of the macro being gone. The code is still linted
#   through the four tests/yaml/test_unified_yaml_*.cpp TUs that include it, each
#   of which has a real compile-commands entry.
#
#   tests/test_parser.hpp is listed above for symmetry, but its standalone failure
#   is neither storm nor a missing BMI: since #582 it #errors immediately unless
#   the including TU defined STORM_UNIFIED_CASES_FILE, and what it imports is
#   storm_benchmark_schema, not storm.
#
#   The 12 *_body.h files are the compile-time TU-split bodies (each holds the
#   shared TYPED_TEST_SUITE/TYPED_TEST content for one _sqlite.cpp/_pg.cpp pair):
#   tests/crud/test_composite_fk_join_body.h, tests/crud/test_composite_pk_crud_body.h,
#   tests/crud/test_crud_body.h, tests/query/test_aggregate_having_body.h,
#   tests/query/test_aggregate_optional_body.h, tests/query/test_composite_m2m_junction_body.h,
#   tests/query/test_composite_m2m_through_body.h,
#   tests/query/test_fk_column_name_orderby_aggregate_body.h,
#   tests/query/test_many_to_many_body.h, tests/query/test_many_to_many_multi_body.h,
#   tests/query/test_reverse_fk_body.h, tests/schema/test_fk_fields_body.h.
#   Same shape as tests/test_unified_yaml_body.h above: each guards on two macros
#   (STORM_SPLIT_TYPES / STORM_SPLIT_TYPE_NAMES) that the including .cpp defines
#   before the #include, so parsed standalone the #error guard fires. Linted
#   through the 24 tests/**/*_sqlite.cpp / *_pg.cpp TUs that include them, each
#   with a real compile-commands entry.
#
#   shared/models/*.h and tests/test_{fixture,record_helpers,join_data,
#   person_seed_fixture}.h joined with the #634 split, for the reason their
#   umbrellas were already here: each carries [[= storm::*]] annotations, a
#   `consteval` storm::field_specs_for call, or storm::QuerySet, so a standalone
#   parse fails the same way shared/models.h and tests/test_models.h do. They are
#   linted through the ~97 TUs that include them. shared/models/color.h is the one
#   exception - a bare enum with no storm dependency - so it stays off the list and
#   is linted directly.
#
#   tests/tools/storm_schema/models.h and shared/models.h joined the list with
#   the fields:: proxies (#518). Both call storm::field_specs_for inside a
#   `consteval` block, which is a harder
#   dependency on `import storm;` than the [[= storm::*]] annotations alone -
#   shared/models.h parsed standalone before that and does not now. Their
#   #include <meta> also drags libc++'s own <meta> into a non-C++26 parse, which
#   reports `unknown type name 'concept'` against the TOOLCHAIN header; skipping
#   at the source stops that too, without a post-hoc output filter that would
#   also mask a genuinely malformed concept in Storm's own code.
#
#   benchmarks/bench_register.h - includes benchmark/benchmark.h which
#   clang-tidy cannot parse (gbench macro / linkage issue).
#
#   Benchmark textual headers - #included inside anonymous namespaces of main
#   TUs; cannot be parsed standalone (need import storm or benchmark BMI):
#   benchmarks/models.hpp, benchmarks/m2m_models.hpp, benchmarks/benchmark_tests.hpp,
#   benchmarks/dashboard/args.hpp, benchmarks/dashboard/backup.hpp,
#   benchmarks/dashboard/db.hpp, benchmarks/dashboard/events.hpp,
#   benchmarks/dashboard/tui_render.hpp, benchmarks/dashboard/models.hpp
#
#   benchmarks/schema.cppm - parses fine, but ANY ASTMatcher-based check
#       (every enabled check) SIGSEGVs clang-tidy inside
#       RecursiveASTVisitor::TraverseTemplateInstantiations on the std-module
#       VarTemplateDecl `std::__desugars_to_v` (a clang-p2996 ParentMap/AST
#       traversal bug over `import std;` instantiations). Reproduces with the
#       pre-#364 7-category config and with a single readability check, so it is
#       a toolchain crash, not a Storm-code or check-config issue. Tracked with
#       the other clang-p2996 module crashes under issue #262.
#
#   python/bindings.cpp - `import storm;`, same standalone-parse issue as the
#       test/benchmark headers above.
#
#   fuzz/*.cpp and fuzz/fuzz_models.h (#550) - same shape as the test/benchmark
#   headers above: `import storm;` + `#include <meta>`, so a standalone parse
#   fails with `module 'storm' not found` / `use of undeclared identifier
#   'storm'`. Genuinely absent from compile_commands.json too - ENABLE_FUZZING
#   is off by default even under ninja-release (see cmake/fuzz.cmake), so
#   there is no compile-database entry to fall back on even when it is on.
#   Found in review (#550): before the post-hoc output-regex filter was
#   retired, its generic `module`/`import`/`use of undeclared` alternatives
#   accidentally covered these files too, so this gap was silent - a staged
#   fuzz/ edit never actually blocked a commit, but for the wrong reason (a
#   message match, not a reviewed skip-list entry). Listing them explicitly
#   is what #550 changed the mechanism to require.
KNOWN_UNPARSEABLE = frozenset((
    "fuzz/fuzz_batch_insert.cpp",
    "fuzz/fuzz_connection_string.cpp",
    "fuzz/fuzz_like_pattern.cpp",
    "fuzz/fuzz_where_int.cpp",
    "fuzz/fuzz_where_string.cpp",
    "fuzz/fuzz_models.h",
    "shared/query_builder.hpp",
    "shared/models.h",
    "shared/models/person.h",
    "shared/models/simple_record.h",
    "shared/models/message.h",
    "shared/models/extended_types.h",
    "shared/models/timestamped_record.h",
    "shared/models/uuid_pk_model.h",
    "shared/models/uuid_pk_ref.h",
    "shared/models/task.h",
    "shared/models/cascade_child.h",
    "shared/models/set_null_child.h",
    "shared/models/restrict_child.h",
    "shared/models/people_25.h",
    "shared/models/messages_8.h",
    "tests/test_fixture.h",
    "tests/test_record_helpers.h",
    "tests/test_join_data.h",
    "tests/test_person_seed_fixture.h",
    "tests/test_models.h",
    "tests/test_seed_helpers.h",
    "tests/test_select_runner.h",
    "tests/test_write_runner.h",
    "tests/test_yaml_register.h",
    "tests/query/test_aggregate_fixture.h",
    "tests/query/test_m2m_models.h",
    "tests/query/test_fields_models.h",
    "tests/crud/test_composite_pk_models.h",
    "tests/schema/test_bmi_fk_models.h",
    "tests/test_parser.hpp",
    "tests/test_unified_yaml_body.h",
    "tests/crud/test_composite_fk_join_body.h",
    "tests/crud/test_composite_pk_crud_body.h",
    "tests/crud/test_crud_body.h",
    "tests/query/test_aggregate_having_body.h",
    "tests/query/test_aggregate_optional_body.h",
    "tests/query/test_composite_m2m_junction_body.h",
    "tests/query/test_composite_m2m_through_body.h",
    "tests/query/test_fk_column_name_orderby_aggregate_body.h",
    "tests/query/test_many_to_many_body.h",
    "tests/query/test_many_to_many_multi_body.h",
    "tests/query/test_reverse_fk_body.h",
    "tests/schema/test_fk_fields_body.h",
    "tests/tools/storm_schema/models.h",
    "benchmarks/bench_register.h",
    "benchmarks/models.hpp",
    "benchmarks/m2m_models.hpp",
    "benchmarks/benchmark_tests.hpp",
    "benchmarks/dashboard/args.hpp",
    "benchmarks/dashboard/backup.hpp",
    "benchmarks/dashboard/db.hpp",
    "benchmarks/dashboard/events.hpp",
    "benchmarks/dashboard/tui_render.hpp",
    "benchmarks/dashboard/models.hpp",
    "benchmarks/schema.cppm",
    "python/bindings.cpp",
))

# Files clang-tidy must NEVER touch - even when it can parse them cleanly. Used to
# short-circuit --full/--all and to filter --diff, so clang-tidy --fix never
# mutates the file.
#
# src/orm/generator.cppm is the upstream P2168 std::generator reference
# implementation (Lewis Baker / Corentin Jabot). clang-tidy's
# readability-identifier-naming rewrites `_T -> T` and `__manual_lifetime ->
# _manual_lifetime` on the primary template but misses the reference
# specialization, producing ill-formed code. Storm does not own this file;
# treat it as vendored - never lint, never auto-fix.
ALWAYS_SKIP = "src/orm/generator.cppm"


def is_known_unparseable(path: str) -> bool:
    return path in KNOWN_UNPARSEABLE


def is_always_skip_file(path: str) -> bool:
    return path == ALWAYS_SKIP or path.endswith("/" + ALWAYS_SKIP)


def is_skipped(path: str) -> bool:
    return is_known_unparseable(path) or is_always_skip_file(path)


def diff_path(header: str) -> str:
    """The b-side path of a `diff --git a/<p> b/<p>` header."""
    return header.rsplit(" b/", 1)[-1]


def filter_skiplist_from_diff(lines: Iterable[str]) -> Iterator[str]:
    """Drop diff sections for known-unparseable / always-skip files.

    clang-tidy-diff.py has no concept of our skip lists, so it would otherwise run
    clang-tidy on textual headers that cannot parse standalone (they need
    `import storm;` / a benchmark BMI / are reflection-annotated) and attribute
    their header-origin diagnostics to the staged lines. Since the `import std;`
    migration these standalone parses fail hard enough to emit dozens of spurious
    warnings/errors (e.g. tests/test_parser.hpp, benchmarks/dashboard/*.hpp),
    which is pure noise on the diff. Filtering at the source (vs. post-hoc output
    grepping) keeps the summary counts honest - see issue #326.

    #550 retired the post-hoc output-regex filter this replaced: it was unanchored
    text matching, matched anywhere on a diagnostic line, and had already once
    swallowed a real `unknown type name 'concept'` error reported against a genuine
    Storm file (src/orm/fields.cppm) alongside the toolchain noise it meant to
    suppress. Dropping a file's section by NAME, before clang-tidy ever runs on it,
    cannot make that mistake - a real file's diagnostics are never in scope unless
    the file itself is on the (reviewed, per-entry-justified) list above.
    """
    keep = True
    for line in lines:
        if line.startswith("diff --git "):
            keep = not is_skipped(diff_path(line.rstrip("\n")))
        if keep:
            yield line


def main(argv: list) -> int:
    if argv[1:] != ["--keep"]:
        print("usage: clang_tidy_skiplist.py --keep   (paths on stdin)", file=sys.stderr)
        return 2
    for path in (line.strip() for line in sys.stdin):
        if path and not is_skipped(path):
            print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
