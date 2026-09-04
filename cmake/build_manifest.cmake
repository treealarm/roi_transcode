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

# §55 asks the manifest for the bit depth of libx264 and libx265, and the two have to be
# asked differently -- which is the whole reason this is twelve lines rather than two.
#
# libx264 answers at compile time: X264_BIT_DEPTH in x264_config.h, where 0 is not "none"
# but "both", a build that carries 8- and 10-bit code and picks at runtime. Spelling that
# out matters, because "0" in a manifest reads as a missing value.
#
# libx265 has no such macro. Its depth is x265_max_bit_depth, a symbol that only exists once
# something links it, so the answer comes from compiling and running a three-line probe.
# Cross-compiling cannot run it and says so rather than guessing: an artefact whose manifest
# claims a depth nobody measured is worse than one that admits it does not know.
set(SVE_X264_BIT_DEPTH "unknown")
if(X264_FOUND)
    find_file(SVE_X264_CONFIG_H x264_config.h PATHS ${X264_INCLUDE_DIRS} NO_DEFAULT_PATH)
    if(SVE_X264_CONFIG_H)
        file(STRINGS "${SVE_X264_CONFIG_H}" _x264_depth_line REGEX "define +X264_BIT_DEPTH")
        # The value, not the 264 in the macro's own name -- which is what a bare [0-9]+
        # matches, and what this said until it was read back out of the generated header.
        string(REGEX MATCH "X264_BIT_DEPTH[ \t]+([0-9]+)" _ "${_x264_depth_line}")
        set(_x264_depth "${CMAKE_MATCH_1}")
        if(_x264_depth STREQUAL "0")
            set(SVE_X264_BIT_DEPTH "8,10 (chosen at runtime)")
        elseif(NOT _x264_depth STREQUAL "")
            set(SVE_X264_BIT_DEPTH "${_x264_depth}")
        endif()
    endif()
endif()

set(SVE_X265_BIT_DEPTH "unknown")
if(X265_FOUND AND NOT CMAKE_CROSSCOMPILING)
    set(_x265_probe "${CMAKE_CURRENT_BINARY_DIR}/x265_bit_depth_probe.cpp")
    file(WRITE "${_x265_probe}"
"#include <x265.h>
#include <cstdio>
int main() { std::printf(\"%d\", x265_max_bit_depth); }
")
    try_run(_x265_run _x265_build "${CMAKE_CURRENT_BINARY_DIR}/x265_probe" "${_x265_probe}"
        # Both directories, not just the includes: pkg-config reports the library as a bare
        # name, so without the link directory the probe fails to link and the manifest
        # quietly says "unknown" -- which it did, and only reading the generated header
        # showed it.
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${X265_INCLUDE_DIRS}"
                    "-DLINK_DIRECTORIES=${X265_LIBRARY_DIRS}"
        LINK_LIBRARIES ${X265_LIBRARIES}
        RUN_OUTPUT_VARIABLE _x265_depth_out)
    if(_x265_build AND _x265_run EQUAL 0 AND NOT _x265_depth_out STREQUAL "")
        string(STRIP "${_x265_depth_out}" SVE_X265_BIT_DEPTH)
    endif()
endif()

# Beside the generated registry, and for the same reason: ta_vms embeds this project with
# add_subdirectory(), where CMAKE_BINARY_DIR is the embedder's build root and this one is
# not. Every consumer takes the directory from SVE_BUILD_MANIFEST_DIR rather than deriving
# it again -- python/ is a subdirectory and would derive a different one.
set(SVE_BUILD_MANIFEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_manifest.h.in"
    "${SVE_BUILD_MANIFEST_DIR}/sve_build_manifest.h" @ONLY)
