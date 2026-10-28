#include <gtest/gtest.h>
#include "test_db_helpers.h"

import storm;
import std;

#include "../../shared/models/wide_record.h" // NOSONAR cpp:S954
#include "test_fixture.h"
using storm::QuerySet;

// NOLINTBEGIN(readability-implicit-bool-conversion)

// Whole-row UPDATE on a model whose SET clause exceeds the fixed 1024-byte
// buffer in UpdateGrammar::build_field_assignments. The buffer silently
// truncated, so the statement either failed to prepare or updated too few columns.
template <typename ConnType> class WideModelUpdateTest : public StormTestFixture<WideRecord, ConnType> {};

TYPED_TEST_SUITE(WideModelUpdateTest, DatabaseTypes);

TYPED_TEST(WideModelUpdateTest, UpdateWritesEveryColumn) {
    QuerySet<WideRecord, TypeParam> qs;

    WideRecord record{.id = 1};
    record.wide_column_00 = 100;
    record.wide_column_01 = 101;
    record.wide_column_02 = 102;
    record.wide_column_03 = 103;
    record.wide_column_04 = 104;
    record.wide_column_05 = 105;
    record.wide_column_06 = 106;
    record.wide_column_07 = 107;
    record.wide_column_08 = 108;
    record.wide_column_09 = 109;
    record.wide_column_10 = 110;
    record.wide_column_11 = 111;
    record.wide_column_12 = 112;
    record.wide_column_13 = 113;
    record.wide_column_14 = 114;
    record.wide_column_15 = 115;
    record.wide_column_16 = 116;
    record.wide_column_17 = 117;
    record.wide_column_18 = 118;
    record.wide_column_19 = 119;
    record.wide_column_20 = 120;
    record.wide_column_21 = 121;
    record.wide_column_22 = 122;
    record.wide_column_23 = 123;
    record.wide_column_24 = 124;
    record.wide_column_25 = 125;
    record.wide_column_26 = 126;
    record.wide_column_27 = 127;
    record.wide_column_28 = 128;
    record.wide_column_29 = 129;
    record.wide_column_30 = 130;
    record.wide_column_31 = 131;
    record.wide_column_32 = 132;
    record.wide_column_33 = 133;
    record.wide_column_34 = 134;
    record.wide_column_35 = 135;
    record.wide_column_36 = 136;
    record.wide_column_37 = 137;
    record.wide_column_38 = 138;
    record.wide_column_39 = 139;
    record.wide_column_40 = 140;
    record.wide_column_41 = 141;
    record.wide_column_42 = 142;
    record.wide_column_43 = 143;
    record.wide_column_44 = 144;
    record.wide_column_45 = 145;
    record.wide_column_46 = 146;
    record.wide_column_47 = 147;
    record.wide_column_48 = 148;
    record.wide_column_49 = 149;
    record.wide_column_50 = 150;
    record.wide_column_51 = 151;
    record.wide_column_52 = 152;
    record.wide_column_53 = 153;
    record.wide_column_54 = 154;
    record.wide_column_55 = 155;
    record.wide_column_56 = 156;
    record.wide_column_57 = 157;
    record.wide_column_58 = 158;
    record.wide_column_59 = 159;
    record.wide_column_60 = 160;
    record.wide_column_61 = 161;
    record.wide_column_62 = 162;
    record.wide_column_63 = 163;
    record.wide_column_64 = 164;
    record.wide_column_65 = 165;
    record.wide_column_66 = 166;
    record.wide_column_67 = 167;
    record.wide_column_68 = 168;
    record.wide_column_69 = 169;
    ASSERT_TRUE(qs.insert(record).execute().has_value());

    WideRecord updated{.id = 1};
    updated.wide_column_00 = 1000;
    updated.wide_column_01 = 1001;
    updated.wide_column_02 = 1002;
    updated.wide_column_03 = 1003;
    updated.wide_column_04 = 1004;
    updated.wide_column_05 = 1005;
    updated.wide_column_06 = 1006;
    updated.wide_column_07 = 1007;
    updated.wide_column_08 = 1008;
    updated.wide_column_09 = 1009;
    updated.wide_column_10 = 1010;
    updated.wide_column_11 = 1011;
    updated.wide_column_12 = 1012;
    updated.wide_column_13 = 1013;
    updated.wide_column_14 = 1014;
    updated.wide_column_15 = 1015;
    updated.wide_column_16 = 1016;
    updated.wide_column_17 = 1017;
    updated.wide_column_18 = 1018;
    updated.wide_column_19 = 1019;
    updated.wide_column_20 = 1020;
    updated.wide_column_21 = 1021;
    updated.wide_column_22 = 1022;
    updated.wide_column_23 = 1023;
    updated.wide_column_24 = 1024;
    updated.wide_column_25 = 1025;
    updated.wide_column_26 = 1026;
    updated.wide_column_27 = 1027;
    updated.wide_column_28 = 1028;
    updated.wide_column_29 = 1029;
    updated.wide_column_30 = 1030;
    updated.wide_column_31 = 1031;
    updated.wide_column_32 = 1032;
    updated.wide_column_33 = 1033;
    updated.wide_column_34 = 1034;
    updated.wide_column_35 = 1035;
    updated.wide_column_36 = 1036;
    updated.wide_column_37 = 1037;
    updated.wide_column_38 = 1038;
    updated.wide_column_39 = 1039;
    updated.wide_column_40 = 1040;
    updated.wide_column_41 = 1041;
    updated.wide_column_42 = 1042;
    updated.wide_column_43 = 1043;
    updated.wide_column_44 = 1044;
    updated.wide_column_45 = 1045;
    updated.wide_column_46 = 1046;
    updated.wide_column_47 = 1047;
    updated.wide_column_48 = 1048;
    updated.wide_column_49 = 1049;
    updated.wide_column_50 = 1050;
    updated.wide_column_51 = 1051;
    updated.wide_column_52 = 1052;
    updated.wide_column_53 = 1053;
    updated.wide_column_54 = 1054;
    updated.wide_column_55 = 1055;
    updated.wide_column_56 = 1056;
    updated.wide_column_57 = 1057;
    updated.wide_column_58 = 1058;
    updated.wide_column_59 = 1059;
    updated.wide_column_60 = 1060;
    updated.wide_column_61 = 1061;
    updated.wide_column_62 = 1062;
    updated.wide_column_63 = 1063;
    updated.wide_column_64 = 1064;
    updated.wide_column_65 = 1065;
    updated.wide_column_66 = 1066;
    updated.wide_column_67 = 1067;
    updated.wide_column_68 = 1068;
    updated.wide_column_69 = 1069;
    auto result            = qs.update(updated).execute();
    ASSERT_TRUE(result.has_value()) << "wide UPDATE must prepare and run";

    auto rows = qs.where(fields::WideRecord.id == 1).select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    const WideRecord& fetched = *rows.value().begin();
    EXPECT_EQ(fetched.wide_column_00, 1000) << "wide_column_00";
    EXPECT_EQ(fetched.wide_column_01, 1001) << "wide_column_01";
    EXPECT_EQ(fetched.wide_column_02, 1002) << "wide_column_02";
    EXPECT_EQ(fetched.wide_column_03, 1003) << "wide_column_03";
    EXPECT_EQ(fetched.wide_column_04, 1004) << "wide_column_04";
    EXPECT_EQ(fetched.wide_column_05, 1005) << "wide_column_05";
    EXPECT_EQ(fetched.wide_column_06, 1006) << "wide_column_06";
    EXPECT_EQ(fetched.wide_column_07, 1007) << "wide_column_07";
    EXPECT_EQ(fetched.wide_column_08, 1008) << "wide_column_08";
    EXPECT_EQ(fetched.wide_column_09, 1009) << "wide_column_09";
    EXPECT_EQ(fetched.wide_column_10, 1010) << "wide_column_10";
    EXPECT_EQ(fetched.wide_column_11, 1011) << "wide_column_11";
    EXPECT_EQ(fetched.wide_column_12, 1012) << "wide_column_12";
    EXPECT_EQ(fetched.wide_column_13, 1013) << "wide_column_13";
    EXPECT_EQ(fetched.wide_column_14, 1014) << "wide_column_14";
    EXPECT_EQ(fetched.wide_column_15, 1015) << "wide_column_15";
    EXPECT_EQ(fetched.wide_column_16, 1016) << "wide_column_16";
    EXPECT_EQ(fetched.wide_column_17, 1017) << "wide_column_17";
    EXPECT_EQ(fetched.wide_column_18, 1018) << "wide_column_18";
    EXPECT_EQ(fetched.wide_column_19, 1019) << "wide_column_19";
    EXPECT_EQ(fetched.wide_column_20, 1020) << "wide_column_20";
    EXPECT_EQ(fetched.wide_column_21, 1021) << "wide_column_21";
    EXPECT_EQ(fetched.wide_column_22, 1022) << "wide_column_22";
    EXPECT_EQ(fetched.wide_column_23, 1023) << "wide_column_23";
    EXPECT_EQ(fetched.wide_column_24, 1024) << "wide_column_24";
    EXPECT_EQ(fetched.wide_column_25, 1025) << "wide_column_25";
    EXPECT_EQ(fetched.wide_column_26, 1026) << "wide_column_26";
    EXPECT_EQ(fetched.wide_column_27, 1027) << "wide_column_27";
    EXPECT_EQ(fetched.wide_column_28, 1028) << "wide_column_28";
    EXPECT_EQ(fetched.wide_column_29, 1029) << "wide_column_29";
    EXPECT_EQ(fetched.wide_column_30, 1030) << "wide_column_30";
    EXPECT_EQ(fetched.wide_column_31, 1031) << "wide_column_31";
    EXPECT_EQ(fetched.wide_column_32, 1032) << "wide_column_32";
    EXPECT_EQ(fetched.wide_column_33, 1033) << "wide_column_33";
    EXPECT_EQ(fetched.wide_column_34, 1034) << "wide_column_34";
    EXPECT_EQ(fetched.wide_column_35, 1035) << "wide_column_35";
    EXPECT_EQ(fetched.wide_column_36, 1036) << "wide_column_36";
    EXPECT_EQ(fetched.wide_column_37, 1037) << "wide_column_37";
    EXPECT_EQ(fetched.wide_column_38, 1038) << "wide_column_38";
    EXPECT_EQ(fetched.wide_column_39, 1039) << "wide_column_39";
    EXPECT_EQ(fetched.wide_column_40, 1040) << "wide_column_40";
    EXPECT_EQ(fetched.wide_column_41, 1041) << "wide_column_41";
    EXPECT_EQ(fetched.wide_column_42, 1042) << "wide_column_42";
    EXPECT_EQ(fetched.wide_column_43, 1043) << "wide_column_43";
    EXPECT_EQ(fetched.wide_column_44, 1044) << "wide_column_44";
    EXPECT_EQ(fetched.wide_column_45, 1045) << "wide_column_45";
    EXPECT_EQ(fetched.wide_column_46, 1046) << "wide_column_46";
    EXPECT_EQ(fetched.wide_column_47, 1047) << "wide_column_47";
    EXPECT_EQ(fetched.wide_column_48, 1048) << "wide_column_48";
    EXPECT_EQ(fetched.wide_column_49, 1049) << "wide_column_49";
    EXPECT_EQ(fetched.wide_column_50, 1050) << "wide_column_50";
    EXPECT_EQ(fetched.wide_column_51, 1051) << "wide_column_51";
    EXPECT_EQ(fetched.wide_column_52, 1052) << "wide_column_52";
    EXPECT_EQ(fetched.wide_column_53, 1053) << "wide_column_53";
    EXPECT_EQ(fetched.wide_column_54, 1054) << "wide_column_54";
    EXPECT_EQ(fetched.wide_column_55, 1055) << "wide_column_55";
    EXPECT_EQ(fetched.wide_column_56, 1056) << "wide_column_56";
    EXPECT_EQ(fetched.wide_column_57, 1057) << "wide_column_57";
    EXPECT_EQ(fetched.wide_column_58, 1058) << "wide_column_58";
    EXPECT_EQ(fetched.wide_column_59, 1059) << "wide_column_59";
    EXPECT_EQ(fetched.wide_column_60, 1060) << "wide_column_60";
    EXPECT_EQ(fetched.wide_column_61, 1061) << "wide_column_61";
    EXPECT_EQ(fetched.wide_column_62, 1062) << "wide_column_62";
    EXPECT_EQ(fetched.wide_column_63, 1063) << "wide_column_63";
    EXPECT_EQ(fetched.wide_column_64, 1064) << "wide_column_64";
    EXPECT_EQ(fetched.wide_column_65, 1065) << "wide_column_65";
    EXPECT_EQ(fetched.wide_column_66, 1066) << "wide_column_66";
    EXPECT_EQ(fetched.wide_column_67, 1067) << "wide_column_67";
    EXPECT_EQ(fetched.wide_column_68, 1068) << "wide_column_68";
    EXPECT_EQ(fetched.wide_column_69, 1069) << "wide_column_69";
}

// Conditional bulk UPDATE: the explicit-pack SET clause (build_conditional_set_clause).
TYPED_TEST(WideModelUpdateTest, ConditionalUpdateWritesEveryColumn) {
    QuerySet<WideRecord, TypeParam> qs;

    WideRecord record{.id = 1};
    ASSERT_TRUE(qs.insert(record).execute().has_value());

    WideRecord proto{.id = 1};
    proto.wide_column_00 = 2000;
    proto.wide_column_01 = 2001;
    proto.wide_column_02 = 2002;
    proto.wide_column_03 = 2003;
    proto.wide_column_04 = 2004;
    proto.wide_column_05 = 2005;
    proto.wide_column_06 = 2006;
    proto.wide_column_07 = 2007;
    proto.wide_column_08 = 2008;
    proto.wide_column_09 = 2009;
    proto.wide_column_10 = 2010;
    proto.wide_column_11 = 2011;
    proto.wide_column_12 = 2012;
    proto.wide_column_13 = 2013;
    proto.wide_column_14 = 2014;
    proto.wide_column_15 = 2015;
    proto.wide_column_16 = 2016;
    proto.wide_column_17 = 2017;
    proto.wide_column_18 = 2018;
    proto.wide_column_19 = 2019;
    proto.wide_column_20 = 2020;
    proto.wide_column_21 = 2021;
    proto.wide_column_22 = 2022;
    proto.wide_column_23 = 2023;
    proto.wide_column_24 = 2024;
    proto.wide_column_25 = 2025;
    proto.wide_column_26 = 2026;
    proto.wide_column_27 = 2027;
    proto.wide_column_28 = 2028;
    proto.wide_column_29 = 2029;
    proto.wide_column_30 = 2030;
    proto.wide_column_31 = 2031;
    proto.wide_column_32 = 2032;
    proto.wide_column_33 = 2033;
    proto.wide_column_34 = 2034;
    proto.wide_column_35 = 2035;
    proto.wide_column_36 = 2036;
    proto.wide_column_37 = 2037;
    proto.wide_column_38 = 2038;
    proto.wide_column_39 = 2039;
    proto.wide_column_40 = 2040;
    proto.wide_column_41 = 2041;
    proto.wide_column_42 = 2042;
    proto.wide_column_43 = 2043;
    proto.wide_column_44 = 2044;
    proto.wide_column_45 = 2045;
    proto.wide_column_46 = 2046;
    proto.wide_column_47 = 2047;
    proto.wide_column_48 = 2048;
    proto.wide_column_49 = 2049;
    proto.wide_column_50 = 2050;
    proto.wide_column_51 = 2051;
    proto.wide_column_52 = 2052;
    proto.wide_column_53 = 2053;
    proto.wide_column_54 = 2054;
    proto.wide_column_55 = 2055;
    proto.wide_column_56 = 2056;
    proto.wide_column_57 = 2057;
    proto.wide_column_58 = 2058;
    proto.wide_column_59 = 2059;
    proto.wide_column_60 = 2060;
    proto.wide_column_61 = 2061;
    proto.wide_column_62 = 2062;
    proto.wide_column_63 = 2063;
    proto.wide_column_64 = 2064;
    proto.wide_column_65 = 2065;
    proto.wide_column_66 = 2066;
    proto.wide_column_67 = 2067;
    proto.wide_column_68 = 2068;
    proto.wide_column_69 = 2069;
    auto result          = qs.where(fields::WideRecord.id == 1)
                          .template update<
                                  fields::WideRecord.wide_column_00,
                                  fields::WideRecord.wide_column_01,
                                  fields::WideRecord.wide_column_02,
                                  fields::WideRecord.wide_column_03,
                                  fields::WideRecord.wide_column_04,
                                  fields::WideRecord.wide_column_05,
                                  fields::WideRecord.wide_column_06,
                                  fields::WideRecord.wide_column_07,
                                  fields::WideRecord.wide_column_08,
                                  fields::WideRecord.wide_column_09,
                                  fields::WideRecord.wide_column_10,
                                  fields::WideRecord.wide_column_11,
                                  fields::WideRecord.wide_column_12,
                                  fields::WideRecord.wide_column_13,
                                  fields::WideRecord.wide_column_14,
                                  fields::WideRecord.wide_column_15,
                                  fields::WideRecord.wide_column_16,
                                  fields::WideRecord.wide_column_17,
                                  fields::WideRecord.wide_column_18,
                                  fields::WideRecord.wide_column_19,
                                  fields::WideRecord.wide_column_20,
                                  fields::WideRecord.wide_column_21,
                                  fields::WideRecord.wide_column_22,
                                  fields::WideRecord.wide_column_23,
                                  fields::WideRecord.wide_column_24,
                                  fields::WideRecord.wide_column_25,
                                  fields::WideRecord.wide_column_26,
                                  fields::WideRecord.wide_column_27,
                                  fields::WideRecord.wide_column_28,
                                  fields::WideRecord.wide_column_29,
                                  fields::WideRecord.wide_column_30,
                                  fields::WideRecord.wide_column_31,
                                  fields::WideRecord.wide_column_32,
                                  fields::WideRecord.wide_column_33,
                                  fields::WideRecord.wide_column_34,
                                  fields::WideRecord.wide_column_35,
                                  fields::WideRecord.wide_column_36,
                                  fields::WideRecord.wide_column_37,
                                  fields::WideRecord.wide_column_38,
                                  fields::WideRecord.wide_column_39,
                                  fields::WideRecord.wide_column_40,
                                  fields::WideRecord.wide_column_41,
                                  fields::WideRecord.wide_column_42,
                                  fields::WideRecord.wide_column_43,
                                  fields::WideRecord.wide_column_44,
                                  fields::WideRecord.wide_column_45,
                                  fields::WideRecord.wide_column_46,
                                  fields::WideRecord.wide_column_47,
                                  fields::WideRecord.wide_column_48,
                                  fields::WideRecord.wide_column_49,
                                  fields::WideRecord.wide_column_50,
                                  fields::WideRecord.wide_column_51,
                                  fields::WideRecord.wide_column_52,
                                  fields::WideRecord.wide_column_53,
                                  fields::WideRecord.wide_column_54,
                                  fields::WideRecord.wide_column_55,
                                  fields::WideRecord.wide_column_56,
                                  fields::WideRecord.wide_column_57,
                                  fields::WideRecord.wide_column_58,
                                  fields::WideRecord.wide_column_59,
                                  fields::WideRecord.wide_column_60,
                                  fields::WideRecord.wide_column_61,
                                  fields::WideRecord.wide_column_62,
                                  fields::WideRecord.wide_column_63,
                                  fields::WideRecord.wide_column_64,
                                  fields::WideRecord.wide_column_65,
                                  fields::WideRecord.wide_column_66,
                                  fields::WideRecord.wide_column_67,
                                  fields::WideRecord.wide_column_68,
                                  fields::WideRecord.wide_column_69>(proto)
                          .execute();
    ASSERT_TRUE(result.has_value()) << "wide conditional UPDATE must prepare and run";

    auto rows = qs.where(fields::WideRecord.id == 1).select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    const WideRecord& fetched = *rows.value().begin();
    EXPECT_EQ(fetched.wide_column_00, 2000) << "wide_column_00";
    EXPECT_EQ(fetched.wide_column_01, 2001) << "wide_column_01";
    EXPECT_EQ(fetched.wide_column_02, 2002) << "wide_column_02";
    EXPECT_EQ(fetched.wide_column_03, 2003) << "wide_column_03";
    EXPECT_EQ(fetched.wide_column_04, 2004) << "wide_column_04";
    EXPECT_EQ(fetched.wide_column_05, 2005) << "wide_column_05";
    EXPECT_EQ(fetched.wide_column_06, 2006) << "wide_column_06";
    EXPECT_EQ(fetched.wide_column_07, 2007) << "wide_column_07";
    EXPECT_EQ(fetched.wide_column_08, 2008) << "wide_column_08";
    EXPECT_EQ(fetched.wide_column_09, 2009) << "wide_column_09";
    EXPECT_EQ(fetched.wide_column_10, 2010) << "wide_column_10";
    EXPECT_EQ(fetched.wide_column_11, 2011) << "wide_column_11";
    EXPECT_EQ(fetched.wide_column_12, 2012) << "wide_column_12";
    EXPECT_EQ(fetched.wide_column_13, 2013) << "wide_column_13";
    EXPECT_EQ(fetched.wide_column_14, 2014) << "wide_column_14";
    EXPECT_EQ(fetched.wide_column_15, 2015) << "wide_column_15";
    EXPECT_EQ(fetched.wide_column_16, 2016) << "wide_column_16";
    EXPECT_EQ(fetched.wide_column_17, 2017) << "wide_column_17";
    EXPECT_EQ(fetched.wide_column_18, 2018) << "wide_column_18";
    EXPECT_EQ(fetched.wide_column_19, 2019) << "wide_column_19";
    EXPECT_EQ(fetched.wide_column_20, 2020) << "wide_column_20";
    EXPECT_EQ(fetched.wide_column_21, 2021) << "wide_column_21";
    EXPECT_EQ(fetched.wide_column_22, 2022) << "wide_column_22";
    EXPECT_EQ(fetched.wide_column_23, 2023) << "wide_column_23";
    EXPECT_EQ(fetched.wide_column_24, 2024) << "wide_column_24";
    EXPECT_EQ(fetched.wide_column_25, 2025) << "wide_column_25";
    EXPECT_EQ(fetched.wide_column_26, 2026) << "wide_column_26";
    EXPECT_EQ(fetched.wide_column_27, 2027) << "wide_column_27";
    EXPECT_EQ(fetched.wide_column_28, 2028) << "wide_column_28";
    EXPECT_EQ(fetched.wide_column_29, 2029) << "wide_column_29";
    EXPECT_EQ(fetched.wide_column_30, 2030) << "wide_column_30";
    EXPECT_EQ(fetched.wide_column_31, 2031) << "wide_column_31";
    EXPECT_EQ(fetched.wide_column_32, 2032) << "wide_column_32";
    EXPECT_EQ(fetched.wide_column_33, 2033) << "wide_column_33";
    EXPECT_EQ(fetched.wide_column_34, 2034) << "wide_column_34";
    EXPECT_EQ(fetched.wide_column_35, 2035) << "wide_column_35";
    EXPECT_EQ(fetched.wide_column_36, 2036) << "wide_column_36";
    EXPECT_EQ(fetched.wide_column_37, 2037) << "wide_column_37";
    EXPECT_EQ(fetched.wide_column_38, 2038) << "wide_column_38";
    EXPECT_EQ(fetched.wide_column_39, 2039) << "wide_column_39";
    EXPECT_EQ(fetched.wide_column_40, 2040) << "wide_column_40";
    EXPECT_EQ(fetched.wide_column_41, 2041) << "wide_column_41";
    EXPECT_EQ(fetched.wide_column_42, 2042) << "wide_column_42";
    EXPECT_EQ(fetched.wide_column_43, 2043) << "wide_column_43";
    EXPECT_EQ(fetched.wide_column_44, 2044) << "wide_column_44";
    EXPECT_EQ(fetched.wide_column_45, 2045) << "wide_column_45";
    EXPECT_EQ(fetched.wide_column_46, 2046) << "wide_column_46";
    EXPECT_EQ(fetched.wide_column_47, 2047) << "wide_column_47";
    EXPECT_EQ(fetched.wide_column_48, 2048) << "wide_column_48";
    EXPECT_EQ(fetched.wide_column_49, 2049) << "wide_column_49";
    EXPECT_EQ(fetched.wide_column_50, 2050) << "wide_column_50";
    EXPECT_EQ(fetched.wide_column_51, 2051) << "wide_column_51";
    EXPECT_EQ(fetched.wide_column_52, 2052) << "wide_column_52";
    EXPECT_EQ(fetched.wide_column_53, 2053) << "wide_column_53";
    EXPECT_EQ(fetched.wide_column_54, 2054) << "wide_column_54";
    EXPECT_EQ(fetched.wide_column_55, 2055) << "wide_column_55";
    EXPECT_EQ(fetched.wide_column_56, 2056) << "wide_column_56";
    EXPECT_EQ(fetched.wide_column_57, 2057) << "wide_column_57";
    EXPECT_EQ(fetched.wide_column_58, 2058) << "wide_column_58";
    EXPECT_EQ(fetched.wide_column_59, 2059) << "wide_column_59";
    EXPECT_EQ(fetched.wide_column_60, 2060) << "wide_column_60";
    EXPECT_EQ(fetched.wide_column_61, 2061) << "wide_column_61";
    EXPECT_EQ(fetched.wide_column_62, 2062) << "wide_column_62";
    EXPECT_EQ(fetched.wide_column_63, 2063) << "wide_column_63";
    EXPECT_EQ(fetched.wide_column_64, 2064) << "wide_column_64";
    EXPECT_EQ(fetched.wide_column_65, 2065) << "wide_column_65";
    EXPECT_EQ(fetched.wide_column_66, 2066) << "wide_column_66";
    EXPECT_EQ(fetched.wide_column_67, 2067) << "wide_column_67";
    EXPECT_EQ(fetched.wide_column_68, 2068) << "wide_column_68";
    EXPECT_EQ(fetched.wide_column_69, 2069) << "wide_column_69";
}

// Upsert DO UPDATE: the excluded.<col> list (build_excluded_set_clause).
TYPED_TEST(WideModelUpdateTest, UpsertDoUpdateWritesEveryColumn) {
    QuerySet<WideRecord, TypeParam> qs;

    WideRecord record{.id = 1};
    ASSERT_TRUE(qs.insert(record).execute().has_value());

    WideRecord conflicting{.id = 1};
    conflicting.wide_column_00 = 3000;
    conflicting.wide_column_01 = 3001;
    conflicting.wide_column_02 = 3002;
    conflicting.wide_column_03 = 3003;
    conflicting.wide_column_04 = 3004;
    conflicting.wide_column_05 = 3005;
    conflicting.wide_column_06 = 3006;
    conflicting.wide_column_07 = 3007;
    conflicting.wide_column_08 = 3008;
    conflicting.wide_column_09 = 3009;
    conflicting.wide_column_10 = 3010;
    conflicting.wide_column_11 = 3011;
    conflicting.wide_column_12 = 3012;
    conflicting.wide_column_13 = 3013;
    conflicting.wide_column_14 = 3014;
    conflicting.wide_column_15 = 3015;
    conflicting.wide_column_16 = 3016;
    conflicting.wide_column_17 = 3017;
    conflicting.wide_column_18 = 3018;
    conflicting.wide_column_19 = 3019;
    conflicting.wide_column_20 = 3020;
    conflicting.wide_column_21 = 3021;
    conflicting.wide_column_22 = 3022;
    conflicting.wide_column_23 = 3023;
    conflicting.wide_column_24 = 3024;
    conflicting.wide_column_25 = 3025;
    conflicting.wide_column_26 = 3026;
    conflicting.wide_column_27 = 3027;
    conflicting.wide_column_28 = 3028;
    conflicting.wide_column_29 = 3029;
    conflicting.wide_column_30 = 3030;
    conflicting.wide_column_31 = 3031;
    conflicting.wide_column_32 = 3032;
    conflicting.wide_column_33 = 3033;
    conflicting.wide_column_34 = 3034;
    conflicting.wide_column_35 = 3035;
    conflicting.wide_column_36 = 3036;
    conflicting.wide_column_37 = 3037;
    conflicting.wide_column_38 = 3038;
    conflicting.wide_column_39 = 3039;
    conflicting.wide_column_40 = 3040;
    conflicting.wide_column_41 = 3041;
    conflicting.wide_column_42 = 3042;
    conflicting.wide_column_43 = 3043;
    conflicting.wide_column_44 = 3044;
    conflicting.wide_column_45 = 3045;
    conflicting.wide_column_46 = 3046;
    conflicting.wide_column_47 = 3047;
    conflicting.wide_column_48 = 3048;
    conflicting.wide_column_49 = 3049;
    conflicting.wide_column_50 = 3050;
    conflicting.wide_column_51 = 3051;
    conflicting.wide_column_52 = 3052;
    conflicting.wide_column_53 = 3053;
    conflicting.wide_column_54 = 3054;
    conflicting.wide_column_55 = 3055;
    conflicting.wide_column_56 = 3056;
    conflicting.wide_column_57 = 3057;
    conflicting.wide_column_58 = 3058;
    conflicting.wide_column_59 = 3059;
    conflicting.wide_column_60 = 3060;
    conflicting.wide_column_61 = 3061;
    conflicting.wide_column_62 = 3062;
    conflicting.wide_column_63 = 3063;
    conflicting.wide_column_64 = 3064;
    conflicting.wide_column_65 = 3065;
    conflicting.wide_column_66 = 3066;
    conflicting.wide_column_67 = 3067;
    conflicting.wide_column_68 = 3068;
    conflicting.wide_column_69 = 3069;
    auto result                = qs.insert(conflicting)
                          .template on_conflict<fields::WideRecord.id>()
                          .template update<
                                  fields::WideRecord.wide_column_00,
                                  fields::WideRecord.wide_column_01,
                                  fields::WideRecord.wide_column_02,
                                  fields::WideRecord.wide_column_03,
                                  fields::WideRecord.wide_column_04,
                                  fields::WideRecord.wide_column_05,
                                  fields::WideRecord.wide_column_06,
                                  fields::WideRecord.wide_column_07,
                                  fields::WideRecord.wide_column_08,
                                  fields::WideRecord.wide_column_09,
                                  fields::WideRecord.wide_column_10,
                                  fields::WideRecord.wide_column_11,
                                  fields::WideRecord.wide_column_12,
                                  fields::WideRecord.wide_column_13,
                                  fields::WideRecord.wide_column_14,
                                  fields::WideRecord.wide_column_15,
                                  fields::WideRecord.wide_column_16,
                                  fields::WideRecord.wide_column_17,
                                  fields::WideRecord.wide_column_18,
                                  fields::WideRecord.wide_column_19,
                                  fields::WideRecord.wide_column_20,
                                  fields::WideRecord.wide_column_21,
                                  fields::WideRecord.wide_column_22,
                                  fields::WideRecord.wide_column_23,
                                  fields::WideRecord.wide_column_24,
                                  fields::WideRecord.wide_column_25,
                                  fields::WideRecord.wide_column_26,
                                  fields::WideRecord.wide_column_27,
                                  fields::WideRecord.wide_column_28,
                                  fields::WideRecord.wide_column_29,
                                  fields::WideRecord.wide_column_30,
                                  fields::WideRecord.wide_column_31,
                                  fields::WideRecord.wide_column_32,
                                  fields::WideRecord.wide_column_33,
                                  fields::WideRecord.wide_column_34,
                                  fields::WideRecord.wide_column_35,
                                  fields::WideRecord.wide_column_36,
                                  fields::WideRecord.wide_column_37,
                                  fields::WideRecord.wide_column_38,
                                  fields::WideRecord.wide_column_39,
                                  fields::WideRecord.wide_column_40,
                                  fields::WideRecord.wide_column_41,
                                  fields::WideRecord.wide_column_42,
                                  fields::WideRecord.wide_column_43,
                                  fields::WideRecord.wide_column_44,
                                  fields::WideRecord.wide_column_45,
                                  fields::WideRecord.wide_column_46,
                                  fields::WideRecord.wide_column_47,
                                  fields::WideRecord.wide_column_48,
                                  fields::WideRecord.wide_column_49,
                                  fields::WideRecord.wide_column_50,
                                  fields::WideRecord.wide_column_51,
                                  fields::WideRecord.wide_column_52,
                                  fields::WideRecord.wide_column_53,
                                  fields::WideRecord.wide_column_54,
                                  fields::WideRecord.wide_column_55,
                                  fields::WideRecord.wide_column_56,
                                  fields::WideRecord.wide_column_57,
                                  fields::WideRecord.wide_column_58,
                                  fields::WideRecord.wide_column_59,
                                  fields::WideRecord.wide_column_60,
                                  fields::WideRecord.wide_column_61,
                                  fields::WideRecord.wide_column_62,
                                  fields::WideRecord.wide_column_63,
                                  fields::WideRecord.wide_column_64,
                                  fields::WideRecord.wide_column_65,
                                  fields::WideRecord.wide_column_66,
                                  fields::WideRecord.wide_column_67,
                                  fields::WideRecord.wide_column_68,
                                  fields::WideRecord.wide_column_69>()
                          .execute();
    ASSERT_TRUE(result.has_value()) << "wide upsert must prepare and run";

    auto rows = qs.where(fields::WideRecord.id == 1).select().execute();
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows.value().size(), 1U);
    const WideRecord& fetched = *rows.value().begin();
    EXPECT_EQ(fetched.wide_column_00, 3000) << "wide_column_00";
    EXPECT_EQ(fetched.wide_column_01, 3001) << "wide_column_01";
    EXPECT_EQ(fetched.wide_column_02, 3002) << "wide_column_02";
    EXPECT_EQ(fetched.wide_column_03, 3003) << "wide_column_03";
    EXPECT_EQ(fetched.wide_column_04, 3004) << "wide_column_04";
    EXPECT_EQ(fetched.wide_column_05, 3005) << "wide_column_05";
    EXPECT_EQ(fetched.wide_column_06, 3006) << "wide_column_06";
    EXPECT_EQ(fetched.wide_column_07, 3007) << "wide_column_07";
    EXPECT_EQ(fetched.wide_column_08, 3008) << "wide_column_08";
    EXPECT_EQ(fetched.wide_column_09, 3009) << "wide_column_09";
    EXPECT_EQ(fetched.wide_column_10, 3010) << "wide_column_10";
    EXPECT_EQ(fetched.wide_column_11, 3011) << "wide_column_11";
    EXPECT_EQ(fetched.wide_column_12, 3012) << "wide_column_12";
    EXPECT_EQ(fetched.wide_column_13, 3013) << "wide_column_13";
    EXPECT_EQ(fetched.wide_column_14, 3014) << "wide_column_14";
    EXPECT_EQ(fetched.wide_column_15, 3015) << "wide_column_15";
    EXPECT_EQ(fetched.wide_column_16, 3016) << "wide_column_16";
    EXPECT_EQ(fetched.wide_column_17, 3017) << "wide_column_17";
    EXPECT_EQ(fetched.wide_column_18, 3018) << "wide_column_18";
    EXPECT_EQ(fetched.wide_column_19, 3019) << "wide_column_19";
    EXPECT_EQ(fetched.wide_column_20, 3020) << "wide_column_20";
    EXPECT_EQ(fetched.wide_column_21, 3021) << "wide_column_21";
    EXPECT_EQ(fetched.wide_column_22, 3022) << "wide_column_22";
    EXPECT_EQ(fetched.wide_column_23, 3023) << "wide_column_23";
    EXPECT_EQ(fetched.wide_column_24, 3024) << "wide_column_24";
    EXPECT_EQ(fetched.wide_column_25, 3025) << "wide_column_25";
    EXPECT_EQ(fetched.wide_column_26, 3026) << "wide_column_26";
    EXPECT_EQ(fetched.wide_column_27, 3027) << "wide_column_27";
    EXPECT_EQ(fetched.wide_column_28, 3028) << "wide_column_28";
    EXPECT_EQ(fetched.wide_column_29, 3029) << "wide_column_29";
    EXPECT_EQ(fetched.wide_column_30, 3030) << "wide_column_30";
    EXPECT_EQ(fetched.wide_column_31, 3031) << "wide_column_31";
    EXPECT_EQ(fetched.wide_column_32, 3032) << "wide_column_32";
    EXPECT_EQ(fetched.wide_column_33, 3033) << "wide_column_33";
    EXPECT_EQ(fetched.wide_column_34, 3034) << "wide_column_34";
    EXPECT_EQ(fetched.wide_column_35, 3035) << "wide_column_35";
    EXPECT_EQ(fetched.wide_column_36, 3036) << "wide_column_36";
    EXPECT_EQ(fetched.wide_column_37, 3037) << "wide_column_37";
    EXPECT_EQ(fetched.wide_column_38, 3038) << "wide_column_38";
    EXPECT_EQ(fetched.wide_column_39, 3039) << "wide_column_39";
    EXPECT_EQ(fetched.wide_column_40, 3040) << "wide_column_40";
    EXPECT_EQ(fetched.wide_column_41, 3041) << "wide_column_41";
    EXPECT_EQ(fetched.wide_column_42, 3042) << "wide_column_42";
    EXPECT_EQ(fetched.wide_column_43, 3043) << "wide_column_43";
    EXPECT_EQ(fetched.wide_column_44, 3044) << "wide_column_44";
    EXPECT_EQ(fetched.wide_column_45, 3045) << "wide_column_45";
    EXPECT_EQ(fetched.wide_column_46, 3046) << "wide_column_46";
    EXPECT_EQ(fetched.wide_column_47, 3047) << "wide_column_47";
    EXPECT_EQ(fetched.wide_column_48, 3048) << "wide_column_48";
    EXPECT_EQ(fetched.wide_column_49, 3049) << "wide_column_49";
    EXPECT_EQ(fetched.wide_column_50, 3050) << "wide_column_50";
    EXPECT_EQ(fetched.wide_column_51, 3051) << "wide_column_51";
    EXPECT_EQ(fetched.wide_column_52, 3052) << "wide_column_52";
    EXPECT_EQ(fetched.wide_column_53, 3053) << "wide_column_53";
    EXPECT_EQ(fetched.wide_column_54, 3054) << "wide_column_54";
    EXPECT_EQ(fetched.wide_column_55, 3055) << "wide_column_55";
    EXPECT_EQ(fetched.wide_column_56, 3056) << "wide_column_56";
    EXPECT_EQ(fetched.wide_column_57, 3057) << "wide_column_57";
    EXPECT_EQ(fetched.wide_column_58, 3058) << "wide_column_58";
    EXPECT_EQ(fetched.wide_column_59, 3059) << "wide_column_59";
    EXPECT_EQ(fetched.wide_column_60, 3060) << "wide_column_60";
    EXPECT_EQ(fetched.wide_column_61, 3061) << "wide_column_61";
    EXPECT_EQ(fetched.wide_column_62, 3062) << "wide_column_62";
    EXPECT_EQ(fetched.wide_column_63, 3063) << "wide_column_63";
    EXPECT_EQ(fetched.wide_column_64, 3064) << "wide_column_64";
    EXPECT_EQ(fetched.wide_column_65, 3065) << "wide_column_65";
    EXPECT_EQ(fetched.wide_column_66, 3066) << "wide_column_66";
    EXPECT_EQ(fetched.wide_column_67, 3067) << "wide_column_67";
    EXPECT_EQ(fetched.wide_column_68, 3068) << "wide_column_68";
    EXPECT_EQ(fetched.wide_column_69, 3069) << "wide_column_69";
}

// NOLINTEND(readability-implicit-bool-conversion)
