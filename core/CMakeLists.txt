# roi_core on its own: the unit another project embeds.
#
# Deliberately self-contained -- add_subdirectory() this directory and you get
# the encoder and nothing else. No JSON, no protos, no gRPC, no CLI. The only
# dependency is an FFmpeg that pkg-config can find.

cmake_minimum_required(VERSION 3.22)
project(roi_core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)
message(STATUS "roi_core FFmpeg: ${FFMPEG_INCLUDE_DIRS}")

set(ROI_WARNINGS
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Boxes and masks to rectangles: pure arithmetic, so it stays unit-testable
# without FFmpeg anywhere near it.
add_library(roi_geometry STATIC
    src/roi_map_builder.cpp
    src/geometry.cpp
    src/qp_delta_map.cpp
)
target_include_directories(roi_geometry
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
           ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(roi_geometry PRIVATE ${ROI_WARNINGS})

add_library(roi_core STATIC
    src/transcode_file.cpp
    src/roi_encoder.cpp
    src/file_mux_sink.cpp
    src/frame_input.cpp
    src/session.cpp
)
target_include_directories(roi_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(roi_core SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(roi_core PUBLIC ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(roi_core PUBLIC roi_geometry ${FFMPEG_LIBRARIES})
target_compile_definitions(roi_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi_core PRIVATE ${ROI_WARNINGS})
