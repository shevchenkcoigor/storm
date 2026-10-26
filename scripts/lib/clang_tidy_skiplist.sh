# Shared clang-tidy skip-list classification, sourced (not executed) by
# scripts/run_clang_tidy.sh's three modes (--diff/--full/--all).
#
# Extracted from run_clang_tidy.sh in #550 so the classification can be
# unit-tested in isolation (scripts/tests/test_run_clang_tidy_skiplist.sh)
# without needing a live clang-tidy binary or compile_commands.json. Both
# is_known_unparseable() and is_always_skip_file() are the single source of
# truth consulted by every mode — --diff drops matching files' diff hunks
# before clang-tidy-diff.py ever sees them (filter_skiplist_from_diff below),
# --full/--all skip them post-hoc in run_tidy()'s has_compile_error branch.
# Keeping them in one sourced file is what makes "stays consistent" (#550
# DoD) mechanical rather than a convention two call sites have to remember.

# Known-skip list: files clang-tidy cannot parse and we accept that.
#
# Returns 0 (true) if a file is expected to fail clang-tidy parsing.
#
# These are precise file paths, not directory wildcards. The former broad
# "tests/*|benchmarks/*" wildcard was replaced in Issue #308 because 39 test
# files and most bench files parse fine — the wildcard was silently masking
# parse failures in files that should be clean.
#
# Files genuinely unparseable:
#   shared/query_builder.hpp — pseudo-module header; must be included after
#       `import storm;` so clang-tidy sees it without the BMI and fails.
#
#   Test headers that must come after `import storm;` — clang-tidy parses
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
#   It needs `import storm;` like its siblings — that alone is the reason, since
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
#   exception — a bare enum with no storm dependency — so it stays off the list and
#   is linted directly.
#
#   tests/tools/storm_schema/models.h and shared/models.h joined the list with
#   the fields:: proxies (#518). Both call storm::field_specs_for inside a
#   `consteval` block, which is a harder
#   dependency on `import storm;` than the [[= storm::*]] annotations alone —
#   shared/models.h parsed standalone before that and does not now. Their
#   #include <meta> also drags libc++'s own <meta> into a non-C++26 parse, which
#   reports `unknown type name 'concept'` against the TOOLCHAIN header; skipping
#   at the source stops that too, without a post-hoc output filter that would
#   also mask a genuinely malformed concept in Storm's own code.
#
#   benchmarks/bench_register.h — includes benchmark/benchmark.h which
#   clang-tidy cannot parse (gbench macro / linkage issue).
#
#   Benchmark textual headers — #included inside anonymous namespaces of main
#   TUs; cannot be parsed standalone (need import storm or benchmark BMI):
#   benchmarks/models.hpp, benchmarks/m2m_models.hpp, benchmarks/benchmark_tests.hpp,
#   benchmarks/dashboard/args.hpp, benchmarks/dashboard/backup.hpp,
#   benchmarks/dashboard/db.hpp, benchmarks/dashboard/events.hpp,
#   benchmarks/dashboard/tui_render.hpp, benchmarks/dashboard/models.hpp
#
#   benchmarks/schema.cppm — parses fine, but ANY ASTMatcher-based check
#       (every enabled check) SIGSEGVs clang-tidy inside
#       RecursiveASTVisitor::TraverseTemplateInstantiations on the std-module
#       VarTemplateDecl `std::__desugars_to_v` (a clang-p2996 ParentMap/AST
#       traversal bug over `import std;` instantiations). Reproduces with the
#       pre-#364 7-category config and with a single readability check, so it is
#       a toolchain crash, not a Storm-code or check-config issue. Tracked with
#       the other clang-p2996 module crashes under issue #262.
#
#   python/bindings.cpp — `import storm;`, same standalone-parse issue as the
#       test/benchmark headers above.
#
#   fuzz/*.cpp and fuzz/fuzz_models.h (#550) — same shape as the test/benchmark
#   headers above: `import storm;` + `#include <meta>`, so a standalone parse
#   fails with `module 'storm' not found` / `use of undeclared identifier
#   'storm'`. Genuinely absent from compile_commands.json too — ENABLE_FUZZING
#   is off by default even under ninja-release (see cmake/fuzz.cmake), so
#   there is no compile-database entry to fall back on even when it is on.
#   Found in review (#550): before the post-hoc output-regex filter was
#   retired, its generic `module`/`import`/`use of undeclared` alternatives
#   accidentally covered these files too, so this gap was silent — a staged
#   fuzz/ edit never actually blocked a commit, but for the wrong reason (a
#   message match, not a reviewed skip-list entry). Listing them explicitly
#   is what #550 changed the mechanism to require.
is_known_unparseable() {
    local file="$1"
    case "$file" in
        fuzz/fuzz_batch_insert.cpp) return 0 ;;
        fuzz/fuzz_connection_string.cpp) return 0 ;;
        fuzz/fuzz_like_pattern.cpp) return 0 ;;
        fuzz/fuzz_where_int.cpp) return 0 ;;
        fuzz/fuzz_where_string.cpp) return 0 ;;
        fuzz/fuzz_models.h) return 0 ;;
        shared/query_builder.hpp) return 0 ;;
        shared/models.h) return 0 ;;
        shared/models/person.h) return 0 ;;
        shared/models/simple_record.h) return 0 ;;
        shared/models/message.h) return 0 ;;
        shared/models/extended_types.h) return 0 ;;
        shared/models/timestamped_record.h) return 0 ;;
        shared/models/uuid_pk_model.h) return 0 ;;
        shared/models/uuid_pk_ref.h) return 0 ;;
        shared/models/task.h) return 0 ;;
        shared/models/cascade_child.h) return 0 ;;
        shared/models/set_null_child.h) return 0 ;;
        shared/models/restrict_child.h) return 0 ;;
        shared/models/people_25.h) return 0 ;;
        shared/models/messages_8.h) return 0 ;;
        tests/test_fixture.h) return 0 ;;
        tests/test_record_helpers.h) return 0 ;;
        tests/test_join_data.h) return 0 ;;
        tests/test_person_seed_fixture.h) return 0 ;;
        tests/test_models.h) return 0 ;;
        tests/test_seed_helpers.h) return 0 ;;
        tests/test_select_runner.h) return 0 ;;
        tests/test_write_runner.h) return 0 ;;
        tests/test_yaml_register.h) return 0 ;;
        tests/query/test_aggregate_fixture.h) return 0 ;;
        tests/query/test_m2m_models.h) return 0 ;;
        tests/query/test_fields_models.h) return 0 ;;
        tests/crud/test_composite_pk_models.h) return 0 ;;
        tests/schema/test_bmi_fk_models.h) return 0 ;;
        tests/test_parser.hpp) return 0 ;;
        tests/test_unified_yaml_body.h) return 0 ;;
        tests/crud/test_composite_fk_join_body.h) return 0 ;;
        tests/crud/test_composite_pk_crud_body.h) return 0 ;;
        tests/crud/test_crud_body.h) return 0 ;;
        tests/query/test_aggregate_having_body.h) return 0 ;;
        tests/query/test_aggregate_optional_body.h) return 0 ;;
        tests/query/test_composite_m2m_junction_body.h) return 0 ;;
        tests/query/test_composite_m2m_through_body.h) return 0 ;;
        tests/query/test_fk_column_name_orderby_aggregate_body.h) return 0 ;;
        tests/query/test_many_to_many_body.h) return 0 ;;
        tests/query/test_many_to_many_multi_body.h) return 0 ;;
        tests/query/test_reverse_fk_body.h) return 0 ;;
        tests/schema/test_fk_fields_body.h) return 0 ;;
        tests/tools/storm_schema/models.h) return 0 ;;
        benchmarks/bench_register.h) return 0 ;;
        benchmarks/models.hpp) return 0 ;;
        benchmarks/m2m_models.hpp) return 0 ;;
        benchmarks/benchmark_tests.hpp) return 0 ;;
        benchmarks/dashboard/args.hpp) return 0 ;;
        benchmarks/dashboard/backup.hpp) return 0 ;;
        benchmarks/dashboard/db.hpp) return 0 ;;
        benchmarks/dashboard/events.hpp) return 0 ;;
        benchmarks/dashboard/tui_render.hpp) return 0 ;;
        benchmarks/dashboard/models.hpp) return 0 ;;
        benchmarks/schema.cppm) return 0 ;;
        python/bindings.cpp) return 0 ;;
        *) return 1 ;;
    esac
}
export -f is_known_unparseable

# Files clang-tidy must NEVER touch — even when it can parse them cleanly.
# Used to short-circuit run_tidy() (full/all modes) and filter_skiplist_from_diff()
# (--diff mode) so clang-tidy --fix never mutates the file.
#
# src/orm/generator.cppm is the upstream P2168 std::generator reference
# implementation (Lewis Baker / Corentin Jabot). clang-tidy's
# readability-identifier-naming rewrites `_T → T` and `__manual_lifetime →
# _manual_lifetime` on the primary template but misses the reference
# specialization, producing ill-formed code. Storm does not own this file;
# treat it as vendored — never lint, never auto-fix.
is_always_skip_file() {
    local file="$1"
    case "$file" in
        src/orm/generator.cppm) return 0 ;;
        */src/orm/generator.cppm) return 0 ;;
        *) return 1 ;;
    esac
}
export -f is_always_skip_file

# Drop diff sections for known-unparseable / always-skip files BEFORE
# clang-tidy-diff.py sees them. clang-tidy-diff.py has no concept of our
# skip-list, so it would otherwise run clang-tidy on textual headers that
# cannot parse standalone (they need `import storm;` / a benchmark BMI / are
# reflection-annotated) and attribute their header-origin diagnostics to the
# staged lines. Since the `import std;` migration these standalone parses
# fail hard enough to emit dozens of spurious warnings/errors (e.g.
# tests/test_parser.hpp, benchmarks/dashboard/*.hpp), which is pure noise on
# the diff. Filtering at the source (vs. post-hoc output grepping) keeps the
# summary counts honest and reuses the single is_known_unparseable() source
# of truth — see issue #326, and #550 for why the post-hoc output-regex
# filter this replaced was retired: it was unanchored text matching, matched
# anywhere on a diagnostic line, and had already once swallowed a real
# `unknown type name 'concept'` error reported against a genuine Storm file
# (src/orm/fields.cppm) alongside the toolchain noise it meant to suppress.
# Dropping a file's diff section by NAME, before clang-tidy ever runs on it,
# cannot make that mistake — a real file's diagnostics are never in scope to
# begin with unless the file itself is on the (reviewed, per-entry-justified)
# skip list above.
filter_skiplist_from_diff() {
    local keep=1 path
    while IFS= read -r line; do
        if [[ "$line" == "diff --git "* ]]; then
            # path is the b-side: "diff --git a/<p> b/<p>"
            path="${line##* b/}"
            # Drop both unparseable files AND always-skip vendored files
            # (e.g. generator.cppm) — the latter must never be linted in any
            # mode, but clang-tidy-diff.py has no concept of either skip-list.
            if is_known_unparseable "$path" || is_always_skip_file "$path"; then
                keep=0
            else
                keep=1
            fi
        fi
        [[ "$keep" == 1 ]] && printf '%s\n' "$line"
    done
    return 0
}
export -f filter_skiplist_from_diff
