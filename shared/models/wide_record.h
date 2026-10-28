#pragma once

/**
 * @file wide_record.h
 * @brief WideRecord — 70 non-key columns, enough for the whole-row UPDATE SET clause
 *        ("wide_column_NN=?, " is 17 bytes per column) to outgrow a 1024-byte buffer.
 *
 * Include AFTER `import storm;` — see models.h for why.
 */

#include <meta>

struct WideRecord {
    [[= storm::primary_assigned]] int id{};
    int wide_column_00{};
    int wide_column_01{};
    int wide_column_02{};
    int wide_column_03{};
    int wide_column_04{};
    int wide_column_05{};
    int wide_column_06{};
    int wide_column_07{};
    int wide_column_08{};
    int wide_column_09{};
    int wide_column_10{};
    int wide_column_11{};
    int wide_column_12{};
    int wide_column_13{};
    int wide_column_14{};
    int wide_column_15{};
    int wide_column_16{};
    int wide_column_17{};
    int wide_column_18{};
    int wide_column_19{};
    int wide_column_20{};
    int wide_column_21{};
    int wide_column_22{};
    int wide_column_23{};
    int wide_column_24{};
    int wide_column_25{};
    int wide_column_26{};
    int wide_column_27{};
    int wide_column_28{};
    int wide_column_29{};
    int wide_column_30{};
    int wide_column_31{};
    int wide_column_32{};
    int wide_column_33{};
    int wide_column_34{};
    int wide_column_35{};
    int wide_column_36{};
    int wide_column_37{};
    int wide_column_38{};
    int wide_column_39{};
    int wide_column_40{};
    int wide_column_41{};
    int wide_column_42{};
    int wide_column_43{};
    int wide_column_44{};
    int wide_column_45{};
    int wide_column_46{};
    int wide_column_47{};
    int wide_column_48{};
    int wide_column_49{};
    int wide_column_50{};
    int wide_column_51{};
    int wide_column_52{};
    int wide_column_53{};
    int wide_column_54{};
    int wide_column_55{};
    int wide_column_56{};
    int wide_column_57{};
    int wide_column_58{};
    int wide_column_59{};
    int wide_column_60{};
    int wide_column_61{};
    int wide_column_62{};
    int wide_column_63{};
    int wide_column_64{};
    int wide_column_65{};
    int wide_column_66{};
    int wide_column_67{};
    int wide_column_68{};
    int wide_column_69{};
};

namespace fields {
struct WideRecordT;
consteval { std::meta::define_aggregate(^^WideRecordT, storm::field_specs_for(^^WideRecord)); }
inline constexpr WideRecordT WideRecord{};
} // namespace fields
