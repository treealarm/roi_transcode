cmake_minimum_required(VERSION 3.22)
project(roi-transcode CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# roi_core and roi_geometry come from core/, which stands alone so another
# project can add_subdirectory() just that.
add_subdirectory(core)

find_package(nlohmann_json CONFIG REQUIRED)

set(ROI_WARNINGS
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# ---------------------------------------------------------------------------
# roi_json -- roi.json <-> RoiDocument. Separate from roi_core so embedding the
# encoder does not drag in nlohmann; only the CLI and the service parse files.
# ---------------------------------------------------------------------------
add_library(roi_json STATIC json/roi_json.cpp)
target_include_directories(roi_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/json)
target_link_libraries(roi_json PUBLIC roi_core PRIVATE nlohmann_json::nlohmann_json)
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

add_executable(roi_json_test tests/roi_json_test.cpp)
target_link_libraries(roi_json_test PRIVATE roi_json)
target_compile_options(roi_json_test PRIVATE ${ROI_WARNINGS})
add_test(NAME roi_json COMMAND roi_json_test)

# Compiles against the public headers only -- no core/src on the include path,
# which is the point: it fails if the surface starts leaking internals.
add_executable(public_api_test tests/public_api_test.cpp)
target_link_libraries(public_api_test PRIVATE roi_core)
target_compile_options(public_api_test PRIVATE ${ROI_WARNINGS})
add_test(NAME public_api COMMAND public_api_test)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/core/src
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

    add_executable(path_guard_test tests/path_guard_test.cpp svc/grpc/path_guard.cpp)
    target_include_directories(path_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/svc)
    target_compile_options(path_guard_test PRIVATE ${ROI_WARNINGS})
    add_test(NAME path_guard COMMAND path_guard_test)

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
    target_include_directories(roi_grpc_smoke SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(roi_grpc_smoke PRIVATE ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(roi_grpc_smoke PRIVATE
        ${FFMPEG_LIBRARIES}
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
    )
    target_compile_options(roi_grpc_smoke PRIVATE ${ROI_WARNINGS})
endif()
