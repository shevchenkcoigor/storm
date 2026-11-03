# storm_schema is a debug-build artefact for the user-facing Atlas migration
# flow (used only by tests/atlas.hcl + scripts/ci/test-atlas-migration.sh, both
# gated to ninja-debug). It calls storm_enable_migrations() which registers the
# makemigrations / migrate target names. The bench dashboard (issue #247) also
# calls storm_enable_migrations() under ENABLE_BENCH and would collide on those
# names, so skip storm_schema when ENABLE_BENCH is on. The collision is only
# theoretical because today's CI never enables both: ninja-debug runs the atlas
# integration test, ninja-release runs the dashboard.
if(NOT ENABLE_BENCH)
  add_subdirectory(storm_schema)
endif()
