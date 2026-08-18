cmake_minimum_required(VERSION 3.22)
project(roi-transcode CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

set(FFMPEG_DEV_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-dev"
    CACHE PATH "Vendored FFmpeg headers; point elsewhere or clear to force pkg-config")
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

set(ROI_WARNINGS
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# ---------------------------------------------------------------------------
# roi_geometry -- turning boxes (later: masks) into the rectangles an encoder
# takes. Pure arithmetic: no FFmpeg, so it stays unit-testable on its own.
# ---------------------------------------------------------------------------
add_library(roi_geometry STATIC
    core/src/roi_map_builder.cpp
    core/src/geometry.cpp
    core/src/qp_delta_map.cpp
)
target_include_directories(roi_geometry
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include
           ${CMAKE_CURRENT_SOURCE_DIR}/core/src
)
target_compile_options(roi_geometry PRIVATE ${ROI_WARNINGS})

# ---------------------------------------------------------------------------
# roi_core -- the module a separate project embeds. FFmpeg and the standard
# library, nothing else: no gRPC, no Dapr, no JSON, no ta_vms headers.
# ---------------------------------------------------------------------------
add_library(roi_core STATIC
    core/src/transcoder.cpp
    core/src/roi_encoder.cpp
    core/src/file_mux_sink.cpp
    core/src/frame_input.cpp
    core/src/session.cpp
)

target_include_directories(roi_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/src
)
target_include_directories(roi_core SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})

if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(roi_core PUBLIC ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(roi_core PUBLIC roi_geometry ${FFMPEG_LIBRARIES})
target_compile_definitions(roi_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi_core PRIVATE ${ROI_WARNINGS})

# ---------------------------------------------------------------------------
# roi_json -- roi.json <-> RoiDocument. Separate from roi_core so embedding the
# encoder does not drag in nlohmann; only the CLI and the service parse files.
# ---------------------------------------------------------------------------
add_library(roi_json STATIC json/roi_json.cpp)
target_include_directories(roi_json
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/json
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(roi_json PUBLIC roi_core)
target_compile_options(roi_json PRIVATE ${ROI_WARNINGS})

# ---------------------------------------------------------------------------
# Adapters
# ---------------------------------------------------------------------------
add_executable(roi-transcode cli/main.cpp)
target_link_libraries(roi-transcode PRIVATE roi_core roi_json)
target_compile_definitions(roi-transcode PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi-transcode PRIVATE ${ROI_WARNINGS})

add_executable(roi-frame-push cli/frame_push.cpp)
# Reaches into core/src for FileMuxSink: it is part of this build, not a
# consumer, and reimplementing a muxer here would prove nothing.
target_include_directories(roi-frame-push PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/src)
target_link_libraries(roi-frame-push PRIVATE roi_core roi_json)
target_compile_definitions(roi-frame-push PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi-frame-push PRIVATE ${ROI_WARNINGS})

enable_testing()

add_executable(roi_map_builder_test tests/roi_map_builder_test.cpp)
# Links the geometry only -- these are pure functions and the test has no
# business pulling FFmpeg in to check arithmetic.
target_link_libraries(roi_map_builder_test PRIVATE roi_geometry)
target_compile_options(roi_map_builder_test PRIVATE ${ROI_WARNINGS})
add_test(NAME roi_map_builder COMMAND roi_map_builder_test)

add_executable(geometry_test tests/geometry_test.cpp)
target_link_libraries(geometry_test PRIVATE roi_geometry)
target_compile_options(geometry_test PRIVATE ${ROI_WARNINGS})
add_test(NAME geometry COMMAND geometry_test)

# Optional gRPC + Dapr service (needs vcpkg toolchain with grpc/protobuf).
option(ROI_BUILD_SVC "Build roi-transcode-svc (gRPC/Dapr)" ON)
if(ROI_BUILD_SVC)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)

    # The protos live in the repository-wide proto/ directory, next to every other
    # ta_vms service contract. Overridable so an extracted copy can point elsewhere;
    # roi_core itself needs none of this, only the service adapter does.
    set(ROI_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto" CACHE PATH
        "Directory holding roi_transcode.proto and dapr/dapr_runtime.proto")
    cmake_path(SET ROI_PROTO_DIR NORMALIZE "${ROI_PROTO_DIR}")
    set(ROI_PROTO_FILE ${ROI_PROTO_DIR}/roi_transcode.proto)
    set(DAPR_PROTO_FILE ${ROI_PROTO_DIR}/dapr/dapr_runtime.proto)

    add_executable(roi-transcode-svc
        svc/svc_main.cpp
        svc/job/roi_job_manager.cpp
        svc/grpc/roi_transcode_service.cpp
        svc/grpc/dapr_publisher.cpp
        svc/grpc/path_guard.cpp
    )

    protobuf_generate(
        TARGET roi-transcode-svc
        IMPORT_DIRS ${ROI_PROTO_DIR}
        PROTOS ${ROI_PROTO_FILE} ${DAPR_PROTO_FILE}
    )

    protobuf_generate(
        TARGET roi-transcode-svc
        LANGUAGE grpc
        IMPORT_DIRS ${ROI_PROTO_DIR}
        PROTOS ${ROI_PROTO_FILE} ${DAPR_PROTO_FILE}
        PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        GENERATE_EXTENSIONS .grpc.pb.cc .grpc.pb.h
    )

    target_include_directories(roi-transcode-svc PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/svc
        ${CMAKE_CURRENT_BINARY_DIR}
    )

    target_link_libraries(roi-transcode-svc PRIVATE
        roi_core
        roi_json
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
    )

    target_compile_definitions(roi-transcode-svc PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(roi-transcode-svc PRIVATE ${ROI_WARNINGS})

    add_executable(roi_grpc_smoke tools/roi_grpc_smoke.cpp)
    protobuf_generate(
        TARGET roi_grpc_smoke
        IMPORT_DIRS ${ROI_PROTO_DIR}
        PROTOS ${ROI_PROTO_FILE}
    )
    protobuf_generate(
        TARGET roi_grpc_smoke
        LANGUAGE grpc
        IMPORT_DIRS ${ROI_PROTO_DIR}
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
    target_compile_options(roi_grpc_smoke PRIVATE ${ROI_WARNINGS})
endif()
