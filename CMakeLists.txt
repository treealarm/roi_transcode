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
# sve_adaptive -- §52 src/adaptive
#
# Objects to importance to encoder regions. Pure arithmetic with no FFmpeg anywhere near
# it, which is what keeps it unit-testable without a codec: the tests that check region
# ordering and budget truncation link this and nothing else.
# ---------------------------------------------------------------------------
add_library(sve_adaptive STATIC
    src/adaptive/importance.cpp
    src/adaptive/object_normalizer.cpp
    src/adaptive/object_geometry.cpp
    src/adaptive/importance_map.cpp
    src/adaptive/qp_mapper.cpp
    src/adaptive/temporal_roi.cpp
    src/adaptive/bitrate_budget.cpp
    src/adaptive/qp_slew.cpp
    src/adaptive/box_smoother.cpp
    src/adaptive/roi_lag.cpp
    src/metadata/sei_payload.cpp
)
target_include_directories(sve_adaptive
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive
)
target_compile_options(sve_adaptive PRIVATE ${SVE_WARNINGS})

# ---------------------------------------------------------------------------
# sve -- §52 src/core plus, for now, the encoder backends
#
# The unit another project embeds: add_subdirectory() this directory and you get the
# encoder and nothing else. Its only dependency is an FFmpeg that pkg-config can find.
# ---------------------------------------------------------------------------
add_library(sve STATIC
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
    src/core/frame_roi_pipeline.cpp
    src/core/roi_encoder.cpp
    src/core/smart_video_encoder.cpp
    src/core/file_mux_sink.cpp
    src/core/frame.cpp
    src/core/frame_input.cpp
    src/core/session.cpp
)
target_include_directories(sve
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(sve SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(sve PUBLIC ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(sve PUBLIC sve_adaptive ${FFMPEG_LIBRARIES})
target_compile_definitions(sve PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(sve PRIVATE ${SVE_WARNINGS})

# Names earlier versions of this library answered to. Kept only until integration/ moves
# onto the §8 API; they are removed together with the `smartvideo` namespace alias in
# include/sve/sve.h, and one target should not answer to three names for longer than a
# rename takes.
add_library(smartvideo ALIAS sve)
add_library(smartvideo_adaptive ALIAS sve_adaptive)
add_library(roi_core ALIAS sve)
add_library(roi_geometry ALIAS sve_adaptive)

# ---------------------------------------------------------------------------
# Tests -- §52 tests/unit
# ---------------------------------------------------------------------------
if(SVE_BUILD_TESTS)
    enable_testing()

    add_executable(object_normalizer_test tests/unit/object_normalizer_test.cpp)
    target_link_libraries(object_normalizer_test PRIVATE sve_adaptive)
    target_compile_options(object_normalizer_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME object_normalizer COMMAND object_normalizer_test)

    add_executable(object_geometry_test tests/unit/object_geometry_test.cpp)
    target_link_libraries(object_geometry_test PRIVATE sve_adaptive)
    target_compile_options(object_geometry_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME object_geometry COMMAND object_geometry_test)

    add_executable(importance_test tests/unit/importance_test.cpp)
    target_link_libraries(importance_test PRIVATE sve_adaptive)
    target_compile_options(importance_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME importance COMMAND importance_test)

    add_executable(temporal_roi_test tests/unit/temporal_roi_test.cpp)
    target_link_libraries(temporal_roi_test PRIVATE sve_adaptive)
    target_compile_options(temporal_roi_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME temporal_roi COMMAND temporal_roi_test)

    add_executable(bitrate_budget_test tests/unit/bitrate_budget_test.cpp)
    target_link_libraries(bitrate_budget_test PRIVATE sve_adaptive)
    target_compile_options(bitrate_budget_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME bitrate_budget COMMAND bitrate_budget_test)

    add_executable(v3_policy_test tests/unit/v3_policy_test.cpp)
    target_link_libraries(v3_policy_test PRIVATE sve_adaptive)
    target_compile_options(v3_policy_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME v3_policy COMMAND v3_policy_test)

    add_executable(geometry_test tests/unit/geometry_test.cpp)
    target_link_libraries(geometry_test PRIVATE sve_adaptive)
    target_compile_options(geometry_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME geometry COMMAND geometry_test)

    add_executable(sei_payload_test tests/unit/sei_payload_test.cpp)
    target_link_libraries(sei_payload_test PRIVATE sve_adaptive)
    target_compile_options(sei_payload_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME sei_payload COMMAND sei_payload_test)

    # Compiles against the public headers only -- no src/ on the include path. That is the
    # point of it: it stops compiling the moment an internal type leaks into the surface.
    # frame_test links sve: Frame and WrapRawFrame are the FFmpeg interop, so unlike the
    # tests above there is a libavutil behind this one.
    add_executable(frame_test tests/unit/frame_test.cpp)
    target_link_libraries(frame_test PRIVATE sve)
    target_compile_options(frame_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME frame COMMAND frame_test)

    # encoder_test drives the public encoder end to end on the software backend: no GPU,
    # but a real libx264 behind it, so it links sve like frame_test does.
    add_executable(encoder_test tests/unit/encoder_test.cpp)
    target_link_libraries(encoder_test PRIVATE sve)
    target_compile_options(encoder_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME encoder COMMAND encoder_test)

    add_executable(public_api_test tests/unit/public_api_test.cpp)
    target_link_libraries(public_api_test PRIVATE sve)
    target_compile_options(public_api_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME public_api COMMAND public_api_test)

    # Every public header on its own, compiled with include/ and nothing else in front of
    # it. One translation unit per header, generated: a header that forgot an include of
    # its own would still compile inside the umbrella, because whatever it needed had
    # already been pulled in by a sibling above it. This is what makes "include only what
    # you need" a property rather than an intention.
    file(GLOB SVE_PUBLIC_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/sve/*.h)
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
    # frame.h is the one public header that names an FFmpeg type (TZ v3.4 §8.5 and §8.10),
    # so this target needs libavutil on its path to compile that one alone. It does not
    # weaken what the target checks -- self-containment -- and it does not weaken the
    # FFmpeg-free claim either: public_headers_isolated below is what checks that, and it
    # still sees include/ and nothing else.
    target_include_directories(public_headers_selfcontained
        SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_compile_options(public_headers_selfcontained PRIVATE ${SVE_WARNINGS})

    # And this one holds the other half of that line. public_api_test links sve, so
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
