# storm_verify_cpm_cache(<cache-dir>) — fail at CONFIGURE time when a CPM
# checkout in the source cache is a bare `.git` with an empty working tree
# (#705).
#
# An interrupted `git clone` (a configure killed mid-checkout) leaves
# <cache>/<name>/<hash>/ holding a complete .git and nothing else. CPM only
# checks that the directory exists, so it reuses the checkout on every later
# configure and the build dies with a confusing `include could not find
# requested file`. The cache is disposable, but deleting it here could race a
# concurrent configure that is still cloning, so this names the directories and
# leaves the delete to the user. An entry whose package lock is held is
# mid-clone elsewhere and is skipped. Call this before the first
# cpmaddpackage().
function(storm_verify_cpm_cache cache_dir)
  if(cache_dir STREQUAL "" OR NOT IS_DIRECTORY "${cache_dir}")
    return()
  endif()
  file(GLOB storm_cpm_entries "${cache_dir}/*/*")
  set(storm_cpm_broken "")
  foreach(entry IN LISTS storm_cpm_entries)
    if(NOT EXISTS "${entry}/.git")
      continue()
    endif()
    # CPM clones under <cache>/<name>/cmake.lock: a held lock means another
    # configure is cloning this entry right now, so it is not broken, just
    # early.
    get_filename_component(storm_cpm_pkg_dir "${entry}" DIRECTORY)
    file(
      LOCK "${storm_cpm_pkg_dir}/cmake.lock"
      GUARD FUNCTION
      TIMEOUT 0
      RESULT_VARIABLE storm_cpm_lock)
    if(NOT storm_cpm_lock EQUAL 0)
      continue()
    endif()
    file(GLOB storm_cpm_content "${entry}/*")
    list(FILTER storm_cpm_content EXCLUDE REGEX "/\\.git$")
    if(storm_cpm_content STREQUAL "")
      list(APPEND storm_cpm_broken "${entry}")
    endif()
  endforeach()
  if(storm_cpm_broken STREQUAL "")
    return()
  endif()
  list(JOIN storm_cpm_broken "\n  " storm_cpm_listing)
  list(JOIN storm_cpm_broken " " storm_cpm_rm_args)
  message(
    FATAL_ERROR
      "A CPM checkout has a .git but an empty working tree (an interrupted "
      "clone). CPM reuses it as a cache hit, so it will never be repaired:\n"
      "  ${storm_cpm_listing}\n"
      "Delete it and configure again:\n"
      "  rm -rf ${storm_cpm_rm_args}")
endfunction()
