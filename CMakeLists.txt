# smart-video-encoder: adaptive video encoding driven by external CV results.
#
# Layout follows TZ v2.0 §52. What that document describes is the library alone --
# include/, src/, tests/, benchmarks/. Our gRPC service, its JSON adapter and the ta_vms
# scripts are not part of the deliverable and live under integration/, behind an option
# that is off by default so a consumer building the library never sees gRPC at all.

cmake_minimum_required(VERSION 3.22)
project(smart-video-encoder CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SVE_BUILD_TESTS       "Build the unit tests"                       ON)
option(SVE_BUILD_INTEGRATION "Build the ta_vms gRPC service, CLI and tools" OFF)

set(SVE_WARNINGS
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)
message(STATUS "smart-video-encoder FFmpeg: ${FFMPEG_INCLUDE_DIRS}")

# ---------------------------------------------------------------------------
# smartvideo_adaptive -- §52 src/adaptive
#
# Objects to importance to encoder regions. Pure arithmetic with no FFmpeg anywhere near
# it, which is what keeps it unit-testable without a codec: the tests that check region
# ordering and budget truncation link this and nothing else.
# ---------------------------------------------------------------------------
add_library(smartvideo_adaptive STATIC
    src/adaptive/object_normalizer.cpp
    src/adaptive/object_geometry.cpp
    src/adaptive/importance_map.cpp
    src/adaptive/qp_mapper.cpp
)
target_include_directories(smartvideo_adaptive
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive
)
target_compile_options(smartvideo_adaptive PRIVATE ${SVE_WARNINGS})

# ---------------------------------------------------------------------------
# smartvideo -- §52 src/core plus, for now, the encoder backends
#
# The unit another project embeds: add_subdirectory() this directory and you get the
# encoder and nothing else. Its only dependency is an FFmpeg that pkg-config can find.
# ---------------------------------------------------------------------------
add_library(smartvideo STATIC
    src/backend/common/backend_common.cpp
    src/backend/common/backend_registry.cpp
    src/backend/onevpl/onevpl_backend.cpp
    src/backend/ffmpeg/vaapi_backend.cpp
    src/backend/nvenc/nvenc_backend.cpp
    src/backend/software/software_backend.cpp
    src/core/memory_type.cpp
    src/core/pixel_format.cpp
    src/core/preset.cpp
    src/core/transcode_file.cpp
    src/core/roi_encoder.cpp
    src/core/file_mux_sink.cpp
    src/core/frame_input.cpp
    src/core/session.cpp
)
target_include_directories(smartvideo
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(smartvideo SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(smartvideo PUBLIC ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(smartvideo PUBLIC smartvideo_adaptive ${FFMPEG_LIBRARIES})
target_compile_definitions(smartvideo PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(smartvideo PRIVATE ${SVE_WARNINGS})

# Names the rest of ta_vms and any embedder can keep using.
add_library(roi_core ALIAS smartvideo)
add_library(roi_geometry ALIAS smartvideo_adaptive)

# ---------------------------------------------------------------------------
# Tests -- §52 tests/unit
# ---------------------------------------------------------------------------
if(SVE_BUILD_TESTS)
    enable_testing()

    add_executable(object_normalizer_test tests/unit/object_normalizer_test.cpp)
    target_link_libraries(object_normalizer_test PRIVATE smartvideo_adaptive)
    target_compile_options(object_normalizer_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME object_normalizer COMMAND object_normalizer_test)

    add_executable(object_geometry_test tests/unit/object_geometry_test.cpp)
    target_link_libraries(object_geometry_test PRIVATE smartvideo_adaptive)
    target_compile_options(object_geometry_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME object_geometry COMMAND object_geometry_test)

    add_executable(geometry_test tests/unit/geometry_test.cpp)
    target_link_libraries(geometry_test PRIVATE smartvideo_adaptive)
    target_compile_options(geometry_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME geometry COMMAND geometry_test)

    # Compiles against the public headers only -- no src/ on the include path. That is the
    # point of it: it stops compiling the moment an internal type leaks into the surface.
    add_executable(public_api_test tests/unit/public_api_test.cpp)
    target_link_libraries(public_api_test PRIVATE smartvideo)
    target_compile_options(public_api_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME public_api COMMAND public_api_test)

    # Every public header on its own, compiled with include/ and nothing else in front of
    # it. One translation unit per header, generated: a header that forgot an include of
    # its own would still compile inside the umbrella, because whatever it needed had
    # already been pulled in by a sibling above it. This is what makes "include only what
    # you need" a property rather than an intention.
    file(GLOB SVE_PUBLIC_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/smartvideo/*.h)
    set(SVE_SELFCONTAINED_SOURCES "")
    foreach(header ${SVE_PUBLIC_HEADERS})
        string(REPLACE "/" "_" stem "${header}")
        string(REPLACE ".h" "" stem "${stem}")
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/selfcontained/${stem}.cpp")
        file(WRITE "${generated}" "#include \"${header}\"\n")
        list(APPEND SVE_SELFCONTAINED_SOURCES "${generated}")
    endforeach()
    add_library(public_headers_selfcontained OBJECT ${SVE_SELFCONTAINED_SOURCES})
    target_include_directories(public_headers_selfcontained
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(public_headers_selfcontained PRIVATE ${SVE_WARNINGS})

    # And this one holds the other half of that line. public_api_test links smartvideo, so
    # it inherits ${FFMPEG_INCLUDE_DIRS} and would keep compiling if a public header
    # started including libavcodec. This target sees include/ and nothing else, links
    # nothing, and is the only place the "no FFmpeg in the public surface" claim is
    # actually checked. It is a compile test: building it is passing it.
    add_library(public_headers_isolated OBJECT tests/unit/public_headers_isolated.cpp)
    target_include_directories(public_headers_isolated
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(public_headers_isolated PRIVATE ${SVE_WARNINGS})
endif()

if(SVE_BUILD_INTEGRATION)
    add_subdirectory(integration)
endif()
