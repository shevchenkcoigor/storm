#include <gtest/gtest.h>
#include <meta>

#include "test_db_helpers.h"

// This TU supplies the SQLite half of test_composite_m2m_through_body.h;
// test_composite_m2m_through_pg.cpp supplies the other half. See that header's own comment
// for the compile-time-TU-split rationale.
#define STORM_SPLIT_TYPES DatabaseTypesSqliteHalf
#define STORM_SPLIT_TYPE_NAMES DatabaseTypesSqliteHalfNames
#include "test_composite_m2m_through_body.h"
#undef STORM_SPLIT_TYPE_NAMES
#undef STORM_SPLIT_TYPES
