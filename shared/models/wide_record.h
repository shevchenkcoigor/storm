#pragma once

/**
 * @file wide_record.h
 * @brief WideRecord — 20 non-key columns with 51-character names (under PostgreSQL's
 *        63-byte identifier limit), enough for the whole-row UPDATE SET clause
 *        ("<col>=?, " is 54 bytes per column, ~1100 in all) to outgrow a 1024-byte buffer.
 *
 * Include AFTER `import storm;` — see models.h for why.
 */

#include <meta>

struct WideRecord {
    [[= storm::primary_assigned]] int id{};
    int wide_column_padded_to_fill_the_set_clause_buffer_00{};
    int wide_column_padded_to_fill_the_set_clause_buffer_01{};
    int wide_column_padded_to_fill_the_set_clause_buffer_02{};
    int wide_column_padded_to_fill_the_set_clause_buffer_03{};
    int wide_column_padded_to_fill_the_set_clause_buffer_04{};
    int wide_column_padded_to_fill_the_set_clause_buffer_05{};
    int wide_column_padded_to_fill_the_set_clause_buffer_06{};
    int wide_column_padded_to_fill_the_set_clause_buffer_07{};
    int wide_column_padded_to_fill_the_set_clause_buffer_08{};
    int wide_column_padded_to_fill_the_set_clause_buffer_09{};
    int wide_column_padded_to_fill_the_set_clause_buffer_10{};
    int wide_column_padded_to_fill_the_set_clause_buffer_11{};
    int wide_column_padded_to_fill_the_set_clause_buffer_12{};
    int wide_column_padded_to_fill_the_set_clause_buffer_13{};
    int wide_column_padded_to_fill_the_set_clause_buffer_14{};
    int wide_column_padded_to_fill_the_set_clause_buffer_15{};
    int wide_column_padded_to_fill_the_set_clause_buffer_16{};
    int wide_column_padded_to_fill_the_set_clause_buffer_17{};
    int wide_column_padded_to_fill_the_set_clause_buffer_18{};
    int wide_column_padded_to_fill_the_set_clause_buffer_19{};
};

namespace fields {
struct WideRecordT;
consteval { std::meta::define_aggregate(^^WideRecordT, storm::field_specs_for(^^WideRecord)); }
inline constexpr WideRecordT WideRecord{};
} // namespace fields
