cmake_minimum_required(VERSION 3.22)
project(roi-transcode CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

set(FFMPEG_DEV_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-dev")
set(FFMPEG_SYS_INCLUDE
    "${FFMPEG_DEV_ROOT}/usr/include/x86_64-linux-gnu")

# Prefer vendored Ubuntu headers + system .so (has hevc_qsv). vcpkg ffmpeg in this
# tree is built without qsv and would win pkg-config under the toolchain.
if(EXISTS "${FFMPEG_SYS_INCLUDE}/libavcodec/avcodec.h")
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_SYS_INCLUDE}")
    set(FFMPEG_LIBDIR /usr/lib/x86_64-linux-gnu)
    set(FFMPEG_LIBRARIES
        ${FFMPEG_LIBDIR}/libavformat.so.60
        ${FFMPEG_LIBDIR}/libavcodec.so.60
        ${FFMPEG_LIBDIR}/libavutil.so.58
        ${FFMPEG_LIBDIR}/libswscale.so.7
        ${FFMPEG_LIBDIR}/libswresample.so.4
    )
    unset(FFMPEG_LIBRARY_DIRS)
    message(STATUS "FFmpeg headers: ${FFMPEG_INCLUDE_DIRS} (system .so)")
else()
    pkg_check_modules(FFMPEG REQUIRED
        libavformat
        libavcodec
        libavutil
        libswscale
        libswresample
    )
    message(STATUS "FFmpeg via pkg-config: ${FFMPEG_INCLUDE_DIRS}")
endif()

set(ROI_COMMON_SOURCES
    src/qp_delta_map.cpp
    src/roi_json.cpp
    src/roi_map_builder.cpp
    src/transcoder.cpp
)

add_executable(roi-transcode
    src/main.cpp
    ${ROI_COMMON_SOURCES}
)

target_include_directories(roi-transcode PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${FFMPEG_INCLUDE_DIRS}
)

if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(roi-transcode PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()

target_link_libraries(roi-transcode PRIVATE ${FFMPEG_LIBRARIES})

target_compile_definitions(roi-transcode PRIVATE _CRT_SECURE_NO_WARNINGS)

target_compile_options(roi-transcode PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

add_executable(roi_map_builder_test
    src/roi_map_builder_test.cpp
    src/qp_delta_map.cpp
    src/roi_map_builder.cpp
)

target_include_directories(roi_map_builder_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(roi_map_builder_test PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Optional gRPC + Dapr service (needs vcpkg toolchain with grpc/protobuf).
option(ROI_BUILD_SVC "Build roi-transcode-svc (gRPC/Dapr)" ON)
if(ROI_BUILD_SVC)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)

    set(ROI_PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/roi_transcode.proto)
    set(DAPR_PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/dapr/dapr_runtime.proto)

    add_executable(roi-transcode-svc
        src/svc_main.cpp
        src/job/roi_job_manager.cpp
        src/grpc/roi_transcode_service.cpp
        src/grpc/dapr_publisher.cpp
        src/grpc/path_guard.cpp
        ${ROI_COMMON_SOURCES}
    )

    protobuf_generate(
        TARGET roi-transcode-svc
        IMPORT_DIRS ${CMAKE_SOURCE_DIR}/proto
        PROTOS ${ROI_PROTO_FILE} ${DAPR_PROTO_FILE}
    )

    protobuf_generate(
        TARGET roi-transcode-svc
        LANGUAGE grpc
        IMPORT_DIRS ${CMAKE_SOURCE_DIR}/proto
        PROTOS ${ROI_PROTO_FILE} ${DAPR_PROTO_FILE}
        PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        GENERATE_EXTENSIONS .grpc.pb.cc .grpc.pb.h
    )

    target_include_directories(roi-transcode-svc PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_BINARY_DIR}
        ${FFMPEG_INCLUDE_DIRS}
    )

    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(roi-transcode-svc PRIVATE ${FFMPEG_LIBRARY_DIRS})
    endif()

    target_link_libraries(roi-transcode-svc PRIVATE
        ${FFMPEG_LIBRARIES}
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
    )

    target_compile_definitions(roi-transcode-svc PRIVATE _CRT_SECURE_NO_WARNINGS)

    target_compile_options(roi-transcode-svc PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
        $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )

    add_executable(roi_grpc_smoke
        tools/roi_grpc_smoke.cpp
    )
    protobuf_generate(
        TARGET roi_grpc_smoke
        IMPORT_DIRS ${CMAKE_SOURCE_DIR}/proto
        PROTOS ${ROI_PROTO_FILE}
    )
    protobuf_generate(
        TARGET roi_grpc_smoke
        LANGUAGE grpc
        IMPORT_DIRS ${CMAKE_SOURCE_DIR}/proto
        PROTOS ${ROI_PROTO_FILE}
        PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        GENERATE_EXTENSIONS .grpc.pb.cc .grpc.pb.h
    )
    target_include_directories(roi_grpc_smoke PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(roi_grpc_smoke PRIVATE
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
    )
endif()
