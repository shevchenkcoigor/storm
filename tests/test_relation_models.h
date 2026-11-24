#pragma once

/**
 * @file test_relation_models.h
 * @brief The relation models of dataset `relations` of the unified YAML corpus (#869).
 *
 * BStudent / BCourse are the m2m benchmark's own models (benchmarks/m2m_models.hpp), reused as they are;
 * the rest are test-only, so they live here rather than in a header storm_bench compiles. Together they
 * cover each shape join<> takes: one auto m2m (BStudent), two auto m2m on one owner (BMember), m2m through a
 * model (BPupil / BEnrollment), reverse_fk (BAuthor / BPost), and two FKs to one model (BBug / BReporter).
 *
 * Include AFTER `import storm;` and after <meta>, <string> and <vector>: benchmarks/m2m_models.hpp,
 * included below, declares its fields:: proxies with std::meta::define_aggregate and needs all three.
 */

#include "../benchmarks/m2m_models.hpp"

namespace storm::test::relation_models {

using storm::benchmark::m2m::BCourse;
using storm::benchmark::m2m::BStudent;

struct BClub {
    [[= storm::primary]] int id{};
    std::string name;
};

// Two auto-junction relations on one owner: several relations in one join<> (#392).
struct BMember {
    [[= storm::primary]] int id{};
    std::string name;
    [[= storm::many_to_many<>]] std::vector<BCourse> courses;
    [[= storm::many_to_many<>]] std::vector<BClub> clubs;
};

// m2m through an explicit junction model (#203 Phase 2); the through model is only named by the
// annotation, so a forward declaration is enough here.
struct BEnrollment;

struct BPupil {
    [[= storm::primary]] int id{};
    std::string name;
    [[= storm::many_to_many_through<BEnrollment>]] std::vector<BCourse> courses;
};

struct BEnrollment {
    [[= storm::primary]] int id{};
    [[= storm::fk<>]] BPupil pupil;
    [[= storm::fk<>]] BCourse course;
    std::string grade;
};

// reverse_fk (#398): each author with the posts pointing at it. The annotation names the owner type,
// so BPost need not be complete here.
struct BPost;

struct BAuthor {
    [[= storm::primary]] int id{};
    std::string name;
    [[= storm::reverse_fk<^^BPost>]] std::vector<BPost> posts;
};

struct BPost {
    [[= storm::primary]] int id{};
    std::string title;
    [[= storm::fk<>]] BAuthor author;
};

// Two FKs to one model: several FKs in one join<>, and the cross-model FK selector that picks between
// them on BReporter. The YAML spells it `fk: bug.author` / `bug.reviewer`; no fields:: proxy is declared
// for these models, the YAML resolver reaches ^^BBug::author / ::reviewer by reflection.
struct BReporter {
    [[= storm::primary]] int id{};
    std::string name;
};

struct BBug {
    [[= storm::primary]] int id{};
    std::string summary;
    [[= storm::fk<>]] BReporter author;
    [[= storm::fk<>]] BReporter reviewer;
};

} // namespace storm::test::relation_models
