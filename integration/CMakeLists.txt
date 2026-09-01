# Everything around the library that the specification does not describe.
#
# The CLI, its JSON adapter and the quality metrics: the example applications of §52 and
# the harness the acceptance tests drive. Off by default, so a consumer embedding the
# library never builds any of it.
#
# The gRPC service that used to live here left on 2026-08-31 for the ta_vms repository,
# which is whose it was: Dapr, a job model and a path sandbox appear nowhere in this
# specification, and keeping them here meant shipping grpc and protobuf beside a library
# that needs neither. What stays is what the library's own checks use.
#
# The dependency arrow points one way only: integration links sve, never the reverse.

find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# roi_json -- roi.json <-> the vocabulary the library takes.
# Kept out of the library so embedding the encoder does not drag in a JSON parser.
# ---------------------------------------------------------------------------
add_library(roi_json STATIC json/roi_json.cpp)
target_include_directories(roi_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/json)
target_link_libraries(roi_json PUBLIC sve_all PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(roi_json PRIVATE ${SVE_WARNINGS})

# ---------------------------------------------------------------------------
# Command-line adapters
# ---------------------------------------------------------------------------
add_executable(roi-transcode cli/main.cpp)
# A program that loads backend plugins has to export its own symbols: the plugins carry no
# copy of the core, so their references to it bind here at load time (§26.4, and the
# packaging arrangement recorded in STATUS.md). ENABLE_EXPORTS is -rdynamic.
set_target_properties(roi-transcode PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(roi-transcode PRIVATE sve_all roi_json)
target_compile_definitions(roi-transcode PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(roi-transcode PRIVATE ${SVE_WARNINGS})

add_executable(roi-frame-push cli/frame_push.cpp)
target_link_libraries(roi-frame-push PRIVATE sve_all roi_json)
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
