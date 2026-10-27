cpmaddpackage(
  NAME
  stduuid
  GITHUB_REPOSITORY
  mariusbancila/stduuid
  GIT_TAG
  v1.2.3
  GIT_SHALLOW
  FALSE
  VERSION
  1.2.3
  OPTIONS
  "UUID_SYSTEM_GENERATOR OFF"
  "UUID_USING_CXX20_SPAN ON")

target_link_libraries(${PROJECT_NAME} PUBLIC stduuid)
