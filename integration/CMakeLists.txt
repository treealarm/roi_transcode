# ta_vms integration: everything that is ours rather than the customer's.
#
# The gRPC service (roitrc), its JSON adapter, the CLI and the smoke tool. None of this
# appears in TZ v2.0 §52, which describes the library alone -- keeping it in its own
# directory is what lets the tree above match that document, and what would let the
# library be extracted later by moving one directory rather than untangling two.
#
# The dependency arrow points one way only: integration links sve, never the
# reverse.

find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# roi_json -- roi.json <-> the vocabulary the library takes.
# Kept out of the library so embedding the encoder does not drag in a JSON parser.
# ---------------------------------------------------------------------------
add_library(roi_json STATIC json/roi_json.cpp)
target_include_directories(roi_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/json)
target_link_libraries(roi_json PUBLIC sve PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(roi_json PRIVATE ${SVE_WARNINGS})

# ---------------------------------------------------------------------------
# Command-line adapters
# ---------------------------------------------------------------------------
add_executable(roi-transcode cli/main.cpp)
target_link_libraries(roi-transcode PRIVATE sve roi_json)
target_compile_definitions(roi-transcode PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi-transcode PRIVATE ${SVE_WARNINGS})

add_executable(roi-frame-push cli/frame_push.cpp)
# Reaches into the library's src/core for FileMuxSink: this is part of the same build,
# not a consumer, and reimplementing a muxer here would prove nothing.
target_include_directories(roi-frame-push PRIVATE ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(roi-frame-push PRIVATE sve roi_json)
target_compile_definitions(roi-frame-push PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi-frame-push PRIVATE ${SVE_WARNINGS})

# Quality metrics (TZ v2.0 §40, §43, §44). Lives with the integration rather than the
# library because it reads the synthetic corpus's JSON ground truth, and the library does
# not depend on a JSON parser -- that separation is what check.sh verifies on every run.
add_executable(roi_metrics ${CMAKE_SOURCE_DIR}/tests/quality/roi_metrics.cpp)
target_include_directories(roi_metrics SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
if(FFMPEG_LIBRARY_DIRS)
    target_link_directories(roi_metrics PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(roi_metrics PRIVATE ${FFMPEG_LIBRARIES} nlohmann_json::nlohmann_json)
target_compile_options(roi_metrics PRIVATE ${SVE_WARNINGS})

if(SVE_BUILD_TESTS)
    add_executable(roi_json_test ${CMAKE_SOURCE_DIR}/tests/integration/roi_json_test.cpp)
    target_link_libraries(roi_json_test PRIVATE roi_json)
    target_compile_options(roi_json_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME roi_json COMMAND roi_json_test)
endif()

# ---------------------------------------------------------------------------
# The gRPC service. Protos live in the repository-wide proto/ directory, next to every
# other ta_vms service contract; overridable so an extracted copy can point elsewhere.
# ---------------------------------------------------------------------------
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(ROI_PROTO_DIR "${CMAKE_SOURCE_DIR}/../proto" CACHE PATH
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
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(roi-transcode-svc PRIVATE
    sve
    protobuf::libprotobuf
    gRPC::grpc
    gRPC::grpc++
)
target_compile_definitions(roi-transcode-svc PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi-transcode-svc PRIVATE ${SVE_WARNINGS})

if(SVE_BUILD_TESTS)
    add_executable(path_guard_test
        ${CMAKE_SOURCE_DIR}/tests/integration/path_guard_test.cpp
        svc/grpc/path_guard.cpp)
    target_include_directories(path_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/svc)
    target_compile_options(path_guard_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME path_guard COMMAND path_guard_test)
endif()

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
    roi_json
    ${FFMPEG_LIBRARIES}
    protobuf::libprotobuf
    gRPC::grpc
    gRPC::grpc++
)
target_compile_options(roi_grpc_smoke PRIVATE ${SVE_WARNINGS})
