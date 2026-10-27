if(ENABLE_TESTS)
  message(STATUS "Testing is enabled.")

  cpmaddpackage(
    NAME
    googletest
    GITHUB_REPOSITORY
    google/googletest
    GIT_TAG
    v1.15.2
    GIT_SHALLOW
    FALSE
    VERSION
    1.15.2
    OPTIONS
    "INSTALL_GTEST OFF")

  # Mark gtest/gmock headers as SYSTEM so their warnings (e.g.
  # -Wcharacter-conversion in gtest-printers.h) are not promoted to errors by
  # the -Werror policy on Storm-owned targets (issue #317).
  foreach(_gtest_tgt gtest gtest_main gmock gmock_main)
    if(TARGET ${_gtest_tgt})
      get_target_property(_gtest_inc ${_gtest_tgt}
                          INTERFACE_INCLUDE_DIRECTORIES)
      if(_gtest_inc)
        set_target_properties(
          ${_gtest_tgt} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                   "${_gtest_inc}")
      endif()
    endif()
  endforeach()

  enable_testing()
  add_subdirectory(tests)
else()
  message(STATUS "Testing is disabled.")
endif()
