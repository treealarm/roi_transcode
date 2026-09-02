# Build provenance, gathered once -- §58.7 (the wheel's build manifest) and §38 (the exact
# encoder version a baseline profile is pinned to).
#
# This used to live in python/CMakeLists.txt and reached only the Python module. §38 needs
# the same facts in a tool, and two copies of "which libx264 went into this build" is the
# duplication DRY is about: the day one copy gains a field and the other does not is not
# visible in review.
#
# Must be included after pkg_check_modules for X264, X265 and FFMPEG -- it reports what they
# found, and an empty string here means the component is not in this build.

find_package(Git QUIET)
set(SVE_BUILD_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SVE_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SVE_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT SVE_GIT_DIRTY STREQUAL "")
        # Said plainly. A manifest that reports a commit for a tree with uncommitted changes
        # names a build that does not exist anywhere.
        string(APPEND SVE_BUILD_COMMIT "-dirty")
    endif()
endif()

# Which backends this artefact actually carries. §5.5 makes that a build-time decision, so
# it is a property of the artefact rather than of the project.
set(SVE_MANIFEST_FEATURES "")
foreach(pair "x264:${SVE_BUILD_BACKEND_X264}" "x265:${SVE_BUILD_BACKEND_X265}"
             "ffmpeg_backends:${SVE_BUILD_BACKEND_FFMPEG}"
             "ffmpeg_interop:${SVE_BUILD_FFMPEG_INTEROP}")
    string(REPLACE ":" ";" parts "${pair}")
    list(GET parts 0 feature_name)
    list(GET parts 1 feature_on)
    if(feature_on)
        list(APPEND SVE_MANIFEST_FEATURES "${feature_name}")
    endif()
endforeach()
string(JOIN "," SVE_MANIFEST_FEATURES_TEXT ${SVE_MANIFEST_FEATURES})

# Beside the generated registry, and for the same reason: ta_vms embeds this project with
# add_subdirectory(), where CMAKE_BINARY_DIR is the embedder's build root and this one is
# not. Every consumer takes the directory from SVE_BUILD_MANIFEST_DIR rather than deriving
# it again -- python/ is a subdirectory and would derive a different one.
set(SVE_BUILD_MANIFEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_manifest.h.in"
    "${SVE_BUILD_MANIFEST_DIR}/sve_build_manifest.h" @ONLY)
