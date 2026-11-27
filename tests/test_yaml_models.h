#pragma once

/**
 * @file test_yaml_models.h
 * @brief The YAML `model:` table of the unified corpus, and the JOIN member resolver (#834, #869, #872).
 *
 * The YAML names a model and, in `join.fk` / `join.fks`, the members to join on. Both are resolved
 * here, at compile time, by reflection: a model through YAML_MODELS, a member by its identifier on
 * the case's model, or on another YAML model for a cross-model FK selector (`bug.author`, #398).
 *
 * Its own header (it was part of test_unified_yaml_body.h) because the runners' QueryBuilder needs
 * the resolver, and the resolver needs the table.
 *
 * Include AFTER `import storm;` — the model headers below need it — and after
 * shared/query_builder.hpp (find_member).
 */

#include <array>
#include <cstddef>
#include <meta>
#include <string>
#include <string_view>
#include <vector>

#include "../shared/models/extended_types.h"
#include "../shared/models/message.h"
#include "../shared/models/person.h"
#include "../shared/models/simple_record.h"
#include "test_key_models.h"      // needs <meta>, <string> (above)
#include "test_relation_models.h" // needs <meta>, <string>, <vector> (above)

namespace storm::test {

// YAML `model:` name -> model type (#834). Names are explicit rather than derived from
// identifier_of: the corpus spells SimpleRecord as "simple". `dataset` is the one dataset that
// creates and seeds the model's tables (#869); empty means the fixture's own tables (any dataset).
struct YamlModel {
    std::string_view name;
    std::meta::info type;
    std::string_view dataset;
};

// The relation models of dataset `relations` (tests/test_relation_models.h).
inline constexpr std::string_view RELATIONS_DATASET = "relations";
// The key-shape models of dataset `keys` (tests/test_key_models.h, #872).
inline constexpr std::string_view KEYS_DATASET = "keys";

inline constexpr std::array YAML_MODELS{
    YamlModel{.name = "person", .type = ^^Person},
    YamlModel{.name = "message", .type = ^^Message},
    YamlModel{.name = "simple", .type = ^^SimpleRecord},
    YamlModel{.name = "extended", .type = ^^ExtendedTypes},
    YamlModel{.name = "student", .type = ^^storm::benchmark::m2m::BStudent, .dataset = RELATIONS_DATASET},
    YamlModel{.name = "course", .type = ^^storm::benchmark::m2m::BCourse, .dataset = RELATIONS_DATASET},
    YamlModel{.name = "member", .type = ^^storm::test::relation_models::BMember, .dataset = RELATIONS_DATASET},
    YamlModel{.name = "pupil", .type = ^^storm::test::relation_models::BPupil, .dataset = RELATIONS_DATASET},
    YamlModel{.name = "author", .type = ^^storm::test::relation_models::BAuthor, .dataset = RELATIONS_DATASET},
    YamlModel{.name = "reporter", .type = ^^storm::test::relation_models::BReporter, .dataset = RELATIONS_DATASET},
    YamlModel{.name = "bug", .type = ^^storm::test::relation_models::BBug, .dataset = RELATIONS_DATASET},
    YamlModel{.name = "uuid_pk", .type = ^^UuidPkModel, .dataset = KEYS_DATASET},
    YamlModel{.name = "uuid_ref", .type = ^^storm::test::key_models::KUuidRef, .dataset = KEYS_DATASET},
    YamlModel{.name = "timestamped", .type = ^^TimestampedRecord, .dataset = KEYS_DATASET},
    YamlModel{.name = "stock", .type = ^^storm::test::key_models::KStock, .dataset = KEYS_DATASET},
    YamlModel{.name = "shipment", .type = ^^storm::test::key_models::KShipment, .dataset = KEYS_DATASET},
    YamlModel{.name = "ticket", .type = ^^storm::test::key_models::KTicket, .dataset = KEYS_DATASET},
};

consteval auto yaml_model_index(std::string_view name) -> std::size_t {
    for (std::size_t i = 0; i < YAML_MODELS.size(); ++i)
        if (YAML_MODELS[i].name == name)
            return i;
    return YAML_MODELS.size();
}

// True when YAML model `model` exists and may run on `dataset` (#869): a model tied to one dataset
// (the relation models, whose tables only dataset `relations` creates; the key-shape models of
// dataset `keys`, #872) needs that dataset.
consteval auto yaml_model_dataset_matches(std::string_view model, std::string_view dataset) -> bool {
    const std::size_t i = yaml_model_index(model);
    return i < YAML_MODELS.size() && (YAML_MODELS[i].dataset.empty() || YAML_MODELS[i].dataset == dataset);
}

// A JOIN member as the YAML names it (#869): `courses` is a member of Model (an FK, an m2m or a
// reverse_fk member); `bug.author` is the member `author` of YAML model `bug`, the cross-model FK
// selector join<fields::BBug.author>(). A null info when either part names nothing, which the shared
// QueryBuilder rejects at compile time; whether the member can be joined at all is join<>'s own
// JoinableFields constraint.
template <typename Model> consteval auto yaml_join_member(std::string_view name) -> std::meta::info {
    const std::size_t dot = name.find('.');
    if (dot == std::string_view::npos)
        return storm::orm::query_builder::find_member(^^Model, name);
    const std::size_t owner = yaml_model_index(name.substr(0, dot));
    if (owner == YAML_MODELS.size())
        return std::meta::info{};
    return storm::orm::query_builder::find_member(YAML_MODELS[owner].type, name.substr(dot + 1));
}

// The QueryBuilder fk_resolver of every YAML case.
template <typename Model> struct YamlJoinResolver {
    consteval auto operator()(std::string_view name) const -> std::meta::info { return yaml_join_member<Model>(name); }
};
template <typename Model> inline constexpr YamlJoinResolver<Model> yaml_join_resolver{};

} // namespace storm::test
