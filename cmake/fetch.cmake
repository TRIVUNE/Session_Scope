# =================================================================================================
# fetch.cmake - downloads the third-party code SessionScope builds on, pinned and checked
# =================================================================================================
#
# Everything goes into .toolchain/ (not in git), once. Each download is pinned to an exact version
# and checked, so the build always uses exactly the same code:
#   fetch_git(...)    clones a git repository at a tag, then checks the commit is the pinned one
#   fetch_file(...)   downloads one file, checked against its SHA-256 fingerprint
#   fetch_from_zip(...) downloads a zip, checks its SHA-256, keeps chosen files from inside it
# If a check fails, CMake stops with a message saying what to delete.

get_filename_component(TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../.toolchain" ABSOLUTE)
find_package(Git REQUIRED)

# fetch_git(<folder in .toolchain> <url> <tag> <commit> [RECURSIVE])
function(fetch_git folder url tag commit)
  set(dest "${TOOLCHAIN_DIR}/${folder}")
  if(NOT EXISTS "${dest}")
    message(STATUS "Downloading ${url} @ ${tag}")
    set(extra)
    if("RECURSIVE" IN_LIST ARGN)
      set(extra --recurse-submodules --shallow-submodules)
    endif()
    # core.longpaths: the VST3 SDK holds files with very long names (its documentation and sample
    # pictures). When the repository itself sits in a deep folder, the full path passes the 260
    # letters Windows normally allows and git stops with "Filename too long". With this setting
    # git writes them anyway. (We never compile those files.)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c advice.detachedHead=false -c core.longpaths=true
                            clone --quiet --depth 1 --branch ${tag} ${extra} ${url} "${dest}"
                    RESULT_VARIABLE failed)
    if(failed)
      # A half-finished download must not stay: the next run would find the folder and use it.
      file(REMOVE_RECURSE "${dest}")
      message(FATAL_ERROR "Could not download ${url} (needs git and a network connection).")
    endif()
  endif()
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dest}" rev-parse HEAD OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT actual STREQUAL commit)
    message(FATAL_ERROR "${folder}: expected commit ${commit}, found ${actual}. Delete ${dest} and configure again.")
  endif()
endfunction()

# fetch_file(<file in .toolchain> <url> <sha256>)
function(fetch_file file url sha256)
  set(dest "${TOOLCHAIN_DIR}/${file}")
  if(EXISTS "${dest}")
    file(SHA256 "${dest}" actual)
    if(actual STREQUAL sha256)
      return()
    endif()
    file(REMOVE "${dest}")   # changed or damaged: download it again
  endif()
  message(STATUS "Downloading ${url}")
  file(DOWNLOAD "${url}" "${dest}" EXPECTED_HASH SHA256=${sha256} STATUS status)
  list(GET status 0 code)
  if(code)
    file(REMOVE "${dest}")
    message(FATAL_ERROR "Could not download ${url}: ${status}")
  endif()
endfunction()

# fetch_from_zip(<folder in .toolchain> <url> <sha256> <path inside the zip>...)
# The chosen files end up directly in the folder (without their paths from inside the zip).
function(fetch_from_zip folder url sha256)
  set(dest "${TOOLCHAIN_DIR}/${folder}")
  set(all_there TRUE)
  foreach(inside IN LISTS ARGN)
    get_filename_component(name "${inside}" NAME)
    if(NOT EXISTS "${dest}/${name}")
      set(all_there FALSE)
    endif()
  endforeach()
  if(all_there)
    return()
  endif()
  set(zip "${TOOLCHAIN_DIR}/${folder}-download.zip")
  message(STATUS "Downloading ${url}")
  file(DOWNLOAD "${url}" "${zip}" EXPECTED_HASH SHA256=${sha256})
  set(unpacked "${TOOLCHAIN_DIR}/${folder}-unpacked")
  file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${unpacked}")
  file(MAKE_DIRECTORY "${dest}")
  foreach(inside IN LISTS ARGN)
    file(COPY "${unpacked}/${inside}" DESTINATION "${dest}")
  endforeach()
  file(REMOVE_RECURSE "${unpacked}" "${zip}")
endfunction()
