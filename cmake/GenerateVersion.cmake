# Emits a header carrying the project version and the current `git describe`.
# Run at build time in script mode; the header is rewritten only when its
# contents change so an unchanged tree never triggers a relink. A tarball
# build with no git present degrades to an empty describe.
#
# Inputs: SIDESCOPES_VERSION, SIDESCOPES_SOURCE_DIR, SIDESCOPES_OUTPUT.

set(gitDescribe "")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${SIDESCOPES_SOURCE_DIR}
        OUTPUT_VARIABLE gitDescribe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

set(content "#pragma once
// Generated at build time by cmake/GenerateVersion.cmake. Do not edit.
#define SIDESCOPES_VERSION \"${SIDESCOPES_VERSION}\"
#define SIDESCOPES_GIT_DESCRIBE \"${gitDescribe}\"
")

set(existing "")
if(EXISTS ${SIDESCOPES_OUTPUT})
    file(READ ${SIDESCOPES_OUTPUT} existing)
endif()
if(NOT existing STREQUAL content)
    file(WRITE ${SIDESCOPES_OUTPUT} "${content}")
endif()
