# Determines the git revision of the source tree and exposes it as HP_SOURCE_REVISION.
# Called from the root CMakeLists.txt before any engine target is defined; sets HP_GIT_REVISION.

include_guard(GLOBAL)

find_package(Git QUIET)

set(HP_GIT_REVISION "unknown")

if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    # Re-run the configure step whenever HEAD moves, so a rebuilt binary always reports the revision
    # it was actually built from.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/.git/HEAD")

    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE revisionOutput
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE revisionResult)

    if(revisionResult EQUAL 0 AND NOT revisionOutput STREQUAL "")
        set(HP_GIT_REVISION "${revisionOutput}")
    else()
        message(STATUS "Hollowpoint: 'git rev-parse' failed, reporting revision 'unknown'")
    endif()
else()
    message(STATUS "Hollowpoint: not a git checkout (or git not found), reporting revision 'unknown'")
endif()

message(STATUS "Hollowpoint: source revision ${HP_GIT_REVISION}")
