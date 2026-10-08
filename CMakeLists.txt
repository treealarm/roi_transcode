# smart-video-encoder: adaptive video encoding driven by external CV results.

cmake_minimum_required(VERSION 3.22)
# One version source: the manifest is the file a release already has to touch, so it is the
# one that carries the number rather than a copy kept in step by hand.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" SVE_MANIFEST)
string(JSON SVE_VERSION GET "${SVE_MANIFEST}" version)

project(smart-video-encoder VERSION ${SVE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SVE_BUILD_TESTS       "Build the unit tests"                       ON)
# Decode on its own, without an encoder behind it: sve/decoder.h and the device selection
# beside it. On by default because Session needs the same FrameDecoder and would otherwise
# have to keep a private copy of it -- the point of the component is that there is one
# hardware decode in this repository, not one per caller.
option(SVE_BUILD_DECODE      "Build the standalone video decoder (sve/decoder.h)" ON)
option(SVE_INSTALL           "Install the C++ package (headers, libs, pkg-config)" ON)

# Each backend is an independent optional component, and the core names none of them: what a
# build has is generated below into RegisterDeploymentBackends.
#
# The libavcodec-backed encoders (QSV, VA-API, x264/x265 through libavcodec) are off by
# default; consumers that want hardware encoding turn them on.
option(SVE_BUILD_BACKEND_X264   "Build the direct libx264 backend component"      ON)
option(SVE_BUILD_BACKEND_X265   "Build the direct libx265 backend component"      ON)
# Off by default because it needs the oneVPL SDK, and a component whose SDK is absent must
# not be required to build.
option(SVE_BUILD_BACKEND_ONEVPL "Build the native Intel oneVPL backend component"  OFF)
option(SVE_BUILD_BACKEND_FFMPEG "Build the libavcodec-backed backends"            OFF)
option(SVE_BUILD_FFMPEG_INTEROP "Build the AVFrame interop adapter"               ON)
option(SVE_BUILD_EXAMPLES       "Build transcode_file and Session"                ON)

set(SVE_REGISTER_CALLS "")
if(SVE_BUILD_BACKEND_X264)
    string(APPEND SVE_REGISTER_CALLS "    if (const Status s = RegisterX264Backend(registry); !s.ok()) return s;\n")
endif()
if(SVE_BUILD_BACKEND_X265)
    string(APPEND SVE_REGISTER_CALLS "    if (const Status s = RegisterX265Backend(registry); !s.ok()) return s;\n")
endif()
if(SVE_BUILD_BACKEND_ONEVPL)
    string(APPEND SVE_REGISTER_CALLS "    if (const Status s = RegisterOneVplBackend(registry); !s.ok()) return s;\n")
endif()
if(SVE_BUILD_BACKEND_FFMPEG)
    string(APPEND SVE_REGISTER_CALLS "    if (const Status s = RegisterFfmpegBackends(registry); !s.ok()) return s;\n")
endif()
set(SVE_GENERATED_REGISTRY "${CMAKE_CURRENT_BINARY_DIR}/generated/deployment_backends.cpp")
configure_file(cmake/deployment_backends.cpp.in "${SVE_GENERATED_REGISTRY}" @ONLY)

set(SVE_WARNINGS
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswscale
)
message(STATUS "smart-video-encoder FFmpeg: ${FFMPEG_INCLUDE_DIRS}")

# Each codec component declares its own library rather than relying on vcpkg putting the
# headers in the same include directory as FFmpeg's.
pkg_check_modules(X264 x264)
pkg_check_modules(X265 x265)

# ---------------------------------------------------------------------------
# sve -- the core
#
# Objects to importance to regions to a quantiser map, the encoder that drives it, and the
# registry that says which backends this deployment has. It links nothing: no FFmpeg, no
# codec, no vendor SDK, so its pkg-config Requires.private is empty.
# ---------------------------------------------------------------------------
add_library(sve
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
    src/metadata/cbor.cpp
    src/metadata/sei_payload.cpp
    src/metadata/sidecar.cpp
    src/backend/common/backend_registry.cpp
    src/core/backend_selection.cpp
    src/core/codec_profile.cpp
    src/core/logging.cpp
    src/core/memory_type.cpp
    src/core/policy.cpp
    src/core/pixel_format.cpp
    src/core/preset.cpp
    src/core/frame_roi_pipeline.cpp
    src/core/raw_frame.cpp
    src/core/roi_encoder.cpp
    src/core/smart_video_encoder.cpp
    src/core/backend_deleters.cpp
)
target_include_directories(sve
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    # The arithmetic tests and Session include these headers directly. BUILD_INTERFACE
    # only -- these are internal headers and are not installed, so an installed consumer
    # never sees the path.
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive>
)
target_compile_definitions(sve PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(sve PRIVATE ${SVE_WARNINGS})
# The type comes from BUILD_SHARED_LIBS rather than an explicit STATIC, so a shared build
# yields libsve.so.<major>. Static stays the default: consumers link it statically through
# add_subdirectory().
set_target_properties(sve PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

add_library(sve::sve ALIAS sve)

# A component links the core and is linked by the deployment, never the other way round.
# The core must not gain a dependency on a backend: that would put libx264 in front of
# everyone who embeds us.
function(sve_add_backend_component name)
    target_include_directories(${name}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_compile_options(${name} PRIVATE ${SVE_WARNINGS})
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${name} PUBLIC sve)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# ---------------------------------------------------------------------------
# sve_backend_x264 / sve_backend_x265
#
# libx264 and libx265 through their own APIs. Separate components because they are separate
# libraries with separate APIs.
# ---------------------------------------------------------------------------
if(SVE_BUILD_BACKEND_X264)
    add_library(sve_backend_x264 src/backend/x264/x264_backend.cpp)
    sve_add_backend_component(sve_backend_x264)
    target_include_directories(sve_backend_x264 SYSTEM PRIVATE ${X264_INCLUDE_DIRS})
    target_link_directories(sve_backend_x264 PUBLIC $<BUILD_INTERFACE:${X264_LIBRARY_DIRS}>)
    target_link_libraries(sve_backend_x264 PUBLIC ${X264_LIBRARIES})
    add_library(sve::backend_x264_static ALIAS sve_backend_x264)
endif()

if(SVE_BUILD_BACKEND_X265)
    add_library(sve_backend_x265 src/backend/x265/x265_backend.cpp)
    sve_add_backend_component(sve_backend_x265)
    target_include_directories(sve_backend_x265 SYSTEM PRIVATE ${X265_INCLUDE_DIRS})
    target_link_directories(sve_backend_x265 PUBLIC $<BUILD_INTERFACE:${X265_LIBRARY_DIRS}>)
    target_link_libraries(sve_backend_x265 PUBLIC ${X265_LIBRARIES})
    add_library(sve::backend_x265_static ALIAS sve_backend_x265)
endif()

# ---------------------------------------------------------------------------
# sve_backend_onevpl
#
# Intel oneVPL through its own API: vendor silicon without libavcodec.
# ---------------------------------------------------------------------------
if(SVE_BUILD_BACKEND_ONEVPL)
    pkg_check_modules(VPL vpl)
    if(NOT VPL_FOUND)
        find_path(VPL_INCLUDE_DIR vpl/mfxvideo.h)
        find_library(VPL_LIBRARY NAMES vpl)
        if(VPL_INCLUDE_DIR AND VPL_LIBRARY)
            set(VPL_INCLUDE_DIRS "${VPL_INCLUDE_DIR}")
            set(VPL_LIBRARIES "${VPL_LIBRARY}")
            set(VPL_FOUND TRUE)
        endif()
    endif()
    if(NOT VPL_FOUND)
        message(FATAL_ERROR "SVE_BUILD_BACKEND_ONEVPL needs the oneVPL SDK (libvpl)")
    endif()
    add_library(sve_backend_onevpl src/backend/onevpl_native/onevpl_native_backend.cpp)
    sve_add_backend_component(sve_backend_onevpl)
    target_include_directories(sve_backend_onevpl SYSTEM PRIVATE ${VPL_INCLUDE_DIRS})
    target_link_directories(sve_backend_onevpl PUBLIC $<BUILD_INTERFACE:${VPL_LIBRARY_DIRS}>)
    target_link_libraries(sve_backend_onevpl PUBLIC ${VPL_LIBRARIES})
    add_library(sve::backend_onevpl_static ALIAS sve_backend_onevpl)
endif()

# ---------------------------------------------------------------------------
# sve_ffmpeg_interop
#
# Turns an AVFrame into a Frame and nothing else. libavutil alone: it does not encode, so
# libavcodec has no business here.
# ---------------------------------------------------------------------------
if(SVE_BUILD_FFMPEG_INTEROP)
    add_library(sve_ffmpeg_interop
        src/core/av_frame_ref.cpp
        src/core/av_frame_adapter.cpp
        src/core/ffmpeg_log.cpp
        src/backend/common/av_pixel_format.cpp
    )
    target_include_directories(sve_ffmpeg_interop
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_include_directories(sve_ffmpeg_interop SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sve_ffmpeg_interop PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(sve_ffmpeg_interop PUBLIC sve ${FFMPEG_LIBRARIES})
    target_compile_options(sve_ffmpeg_interop PRIVATE ${SVE_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# sve_backend_ffmpeg
#
# libx264, libx265, QSV and VA-API through libavcodec.
# ---------------------------------------------------------------------------
if(SVE_BUILD_BACKEND_FFMPEG)
    add_library(sve_backend_ffmpeg
        src/backend/common/backend_common.cpp
        src/backend/software/software_backend.cpp
        src/backend/onevpl/onevpl_backend.cpp
        src/backend/ffmpeg/vaapi_backend.cpp
    )
    sve_add_backend_component(sve_backend_ffmpeg)
    target_include_directories(sve_backend_ffmpeg SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sve_backend_ffmpeg PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(sve_backend_ffmpeg PUBLIC ${FFMPEG_LIBRARIES})
endif()

# ---------------------------------------------------------------------------
# sve_decode -- decode without an encoder
#
# Session and a standalone consumer decode through exactly the same code. libavcodec and
# libavutil, and no libavformat: nothing here demuxes.
# ---------------------------------------------------------------------------
if(SVE_BUILD_DECODE)
    add_library(sve_decode
        src/core/frame_input.cpp
        src/decode/decode_backend.cpp
        src/decode/decode_device.cpp
        src/decode/video_decoder.cpp
    )
    target_include_directories(sve_decode
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/decode)
    target_include_directories(sve_decode SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sve_decode PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(sve_decode PUBLIC sve ${FFMPEG_LIBRARIES})
    # AvFrameAdapter turns the decoder's AVFrame into a Frame. Required rather than
    # optional: without the adapter there is no way to hand a decoded frame out through the
    # public interface, so a decode component without it would compile and be useless.
    if(NOT SVE_BUILD_FFMPEG_INTEROP)
        message(FATAL_ERROR "SVE_BUILD_DECODE needs SVE_BUILD_FFMPEG_INTEROP")
    endif()
    target_link_libraries(sve_decode PUBLIC sve_ffmpeg_interop)
    target_compile_options(sve_decode PRIVATE ${SVE_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# sve_examples -- transcode_file and Session
#
# A demuxer, a decoder and a muxer around the encoder. They are the reason libavformat
# appears anywhere, and keeping them in their own target is what stops that spreading.
# ---------------------------------------------------------------------------
if(SVE_BUILD_EXAMPLES)
    if(NOT SVE_BUILD_DECODE)
        message(FATAL_ERROR "SVE_BUILD_EXAMPLES needs SVE_BUILD_DECODE: Session decodes")
    endif()
    add_library(sve_examples
        src/core/transcode_file.cpp
        src/core/session.cpp
        src/core/file_mux_sink.cpp
    )
    target_include_directories(sve_examples
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_include_directories(sve_examples SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sve_examples PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(sve_examples PUBLIC sve sve_decode ${FFMPEG_LIBRARIES})
    if(SVE_BUILD_FFMPEG_INTEROP)
        target_link_libraries(sve_examples PUBLIC sve_ffmpeg_interop)
    endif()
    target_compile_options(sve_examples PRIVATE ${SVE_WARNINGS})
    target_compile_definitions(sve_examples PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# sve_deployment -- the generated aggregator, and why it is not in the core
#
# RegisterDeploymentBackends calls one function per enabled component, so whatever holds it
# refers to every component in the build. In the core that would make the components
# transitive dependencies of sve: a test linking the core alone could no longer link,
# because the core would name libx264's registration function.
#
# So it lives on the deployment's side of the line. The core declares the function and
# calls it lazily; this target defines it. A test that never touches backend selection
# never pulls the object that needs it, which is how static archives work and why the
# arithmetic tests still link against the core by itself.
#
# An OBJECT library, not a static one, and that is the whole trick. The core has an
# undefined RegisterDeploymentBackends and the components depend on the core, so CMake
# topologically sorts the core last and an archive holding the definition would sit in
# front of the reference that needs it. Object files go onto the link line unconditionally
# and satisfy any archive whatever the order, so the cycle never has to be declared.
# ---------------------------------------------------------------------------
add_library(sve_deployment OBJECT "${SVE_GENERATED_REGISTRY}")
target_include_directories(sve_deployment PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(sve_deployment PRIVATE ${SVE_WARNINGS})

# What a consumer links to get "everything this build has". Kept as one name so that
# add_subdirectory() users do not have to learn the component list.
add_library(sve_all INTERFACE)
target_link_libraries(sve_all INTERFACE sve $<TARGET_OBJECTS:sve_deployment>)
foreach(component sve_backend_ffmpeg sve_ffmpeg_interop sve_decode sve_examples
                  sve_backend_x264 sve_backend_x265 sve_backend_onevpl)
    if(TARGET ${component})
        target_link_libraries(sve_all INTERFACE ${component})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Install and export
#
# find_package(sve CONFIG) yields sve::sve, and each component is its own explicitly
# linkable target. Each pkg-config file declares only what its own unit links: the core's
# Requires.private is empty, because the core links nothing.
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

if(SVE_INSTALL)

set(SVE_INSTALL_TARGETS sve sve_all)
set(SVE_INSTALLED_COMPONENTS "")
foreach(component sve_backend_x264 sve_backend_x265 sve_backend_onevpl sve_backend_ffmpeg
                  sve_ffmpeg_interop sve_decode sve_examples)
    if(TARGET ${component})
        list(APPEND SVE_INSTALL_TARGETS ${component})
        list(APPEND SVE_INSTALLED_COMPONENTS ${component})
    endif()
endforeach()

install(TARGETS ${SVE_INSTALL_TARGETS} EXPORT sveTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/sve DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
install(EXPORT sveTargets NAMESPACE sve::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sve)

configure_package_config_file(cmake/sveConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/sveConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sve)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/sveConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sveConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/sveConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sve)

# One .pc per installable unit, each declaring only what it actually links.
function(sve_write_pc target library description requires)
    set(SVE_PC_LIBRARY "${library}")
    set(SVE_PC_DESCRIPTION "${description}")
    set(SVE_PC_REQUIRES_PRIVATE "${requires}")
    configure_file(cmake/sve.pc.in "${CMAKE_CURRENT_BINARY_DIR}/${target}.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${target}.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endfunction()

sve_write_pc(sve sve "Adaptive ROI video encoding -- core" "")
if(TARGET sve_backend_x264)
    sve_write_pc(sve-backend-x264 sve_backend_x264
        "Adaptive ROI video encoding -- direct libx264 backend" "x264")
endif()
if(TARGET sve_backend_x265)
    sve_write_pc(sve-backend-x265 sve_backend_x265
        "Adaptive ROI video encoding -- direct libx265 backend" "x265")
endif()
if(TARGET sve_ffmpeg_interop)
    # libavutil and not libavcodec: the adapter converts a frame, it does not encode.
    sve_write_pc(sve-ffmpeg-interop sve_ffmpeg_interop
        "Adaptive ROI video encoding -- AVFrame interop" "libavutil")
endif()

endif()  # SVE_INSTALL

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
if(SVE_BUILD_TESTS)
    enable_testing()

    add_executable(object_normalizer_test tests/unit/object_normalizer_test.cpp)
    target_link_libraries(object_normalizer_test PRIVATE sve)
    target_compile_options(object_normalizer_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME object_normalizer COMMAND object_normalizer_test)

    add_executable(object_geometry_test tests/unit/object_geometry_test.cpp)
    target_link_libraries(object_geometry_test PRIVATE sve)
    target_compile_options(object_geometry_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME object_geometry COMMAND object_geometry_test)

    add_executable(importance_test tests/unit/importance_test.cpp)
    target_link_libraries(importance_test PRIVATE sve)
    target_compile_options(importance_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME importance COMMAND importance_test)

    add_executable(temporal_roi_test tests/unit/temporal_roi_test.cpp)
    target_link_libraries(temporal_roi_test PRIVATE sve)
    target_compile_options(temporal_roi_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME temporal_roi COMMAND temporal_roi_test)

    add_executable(bitrate_budget_test tests/unit/bitrate_budget_test.cpp)
    target_link_libraries(bitrate_budget_test PRIVATE sve)
    target_compile_options(bitrate_budget_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME bitrate_budget COMMAND bitrate_budget_test)

    # roi_encoder_test drives the pipeline through a backend that fails on purpose, so it
    # needs the internal header the pipeline lives behind: src/core is PRIVATE on sve
    # because these are not installed, and the test is inside the project rather than a
    # consumer of it.
    # sve_all rather than sve: the fake backend is the only one this test registers, but
    # default_backend_registry() still has to resolve, and RegisterDeploymentBackends is
    # generated into the deployment object rather than the core.
    add_executable(roi_encoder_test tests/unit/roi_encoder_test.cpp)
    target_link_libraries(roi_encoder_test PRIVATE sve_all)
    target_include_directories(roi_encoder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(roi_encoder_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME roi_encoder COMMAND roi_encoder_test)

    # The facade under a backend that fails on purpose. Needs the internal assembly header
    # for the same reason roi_encoder_test needs roi_encoder.h: the seam is inside the
    # project, not in include/.
    add_executable(encoder_faults_test tests/unit/encoder_faults_test.cpp)
    target_link_libraries(encoder_faults_test PRIVATE sve_all)
    target_include_directories(encoder_faults_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(encoder_faults_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME encoder_faults COMMAND encoder_faults_test)

    add_executable(region_policy_test tests/unit/region_policy_test.cpp)
    target_link_libraries(region_policy_test PRIVATE sve)
    target_compile_options(region_policy_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME region_policy COMMAND region_policy_test)

    add_executable(geometry_test tests/unit/geometry_test.cpp)
    target_link_libraries(geometry_test PRIVATE sve)
    target_compile_options(geometry_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME geometry COMMAND geometry_test)

    # Two encoders at once, and the shared first-use state underneath them. Links sve_all
    # rather than sve: what it checks is that the *registry* and the backend singletons
    # behind it are safe to reach simultaneously, and a build with no backends registered
    # would pass it by having nothing to contend over.
    find_package(Threads REQUIRED)
    add_executable(concurrency_test tests/unit/concurrency_test.cpp)
    target_link_libraries(concurrency_test PRIVATE sve_all Threads::Threads)
    target_compile_options(concurrency_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME concurrency COMMAND concurrency_test)

    # cbor_test compiles the encoder directly rather than linking the library: it checks a
    # format against RFC 8949's own vectors, and has no business needing FFmpeg to do it.
    add_executable(cbor_test tests/unit/cbor_test.cpp src/metadata/cbor.cpp)
    target_compile_options(cbor_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME cbor COMMAND cbor_test)

    add_executable(sei_payload_test tests/unit/sei_payload_test.cpp)
    target_link_libraries(sei_payload_test PRIVATE sve)
    target_compile_options(sei_payload_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME sei_payload COMMAND sei_payload_test)

    # Covers AvFrameAdapter, the AVFrame interop, so unlike the tests above there is a
    # libavutil behind this one.
    add_executable(frame_test tests/unit/frame_test.cpp)
    target_link_libraries(frame_test PRIVATE sve_all)
    target_compile_options(frame_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME frame COMMAND frame_test)

    # encoder_test drives the public encoder end to end on the software backend: no GPU,
    # but a real libx264 behind it.
    add_executable(encoder_test tests/unit/encoder_test.cpp)
    target_link_libraries(encoder_test PRIVATE sve_all)
    target_compile_options(encoder_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME encoder COMMAND encoder_test)

    # The decode side of the public API, on the software path: no GPU needed, and the
    # stream it decodes comes out of the encoder in the same binary. Guarded because
    # sve_decode is an option -- a consumer that only encodes does not build it.
    if(TARGET sve_decode)
        add_executable(decoder_test tests/unit/decoder_test.cpp)
        target_link_libraries(decoder_test PRIVATE sve_all)
        target_compile_options(decoder_test PRIVATE ${SVE_WARNINGS})
        add_test(NAME decoder COMMAND decoder_test)
    endif()

    # Log events and levels, plus the FFmpeg routing. Reaches encoder_runtime_warnings.h
    # directly: the guards are where the fallback and refusal events are emitted, and
    # provoking each one through the public API would need a backend that lacks the
    # capability rather than a test that says so.
    add_executable(logging_test tests/unit/logging_test.cpp)
    target_link_libraries(logging_test PRIVATE sve_all)
    target_include_directories(logging_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_compile_options(logging_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME logging COMMAND logging_test)

    # The runtime guards, tested directly: they are pure functions.
    add_executable(runtime_warnings_test tests/unit/runtime_warnings_test.cpp)
    target_link_libraries(runtime_warnings_test PRIVATE sve_all)
    target_include_directories(runtime_warnings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_compile_options(runtime_warnings_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME runtime_warnings COMMAND runtime_warnings_test)

    # Every enumerator has a name of its own, asked of the library's own naming functions.
    add_executable(names_test tests/unit/names_test.cpp)
    target_link_libraries(names_test PRIVATE sve_all)
    target_include_directories(names_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_compile_options(names_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME names COMMAND names_test)

    # Runs where a backend reports main10 and skips with the encoder's own words where it
    # does not.
    add_executable(ten_bit_test tests/unit/ten_bit_test.cpp)
    target_link_libraries(ten_bit_test PRIVATE sve_all)
    target_compile_options(ten_bit_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME ten_bit COMMAND ten_bit_test)

    # Encode, decode, frame count, timestamps and end of stream across every backend in the
    # build. Absent hardware is a skip inside the test, so this runs everywhere and reports
    # how much of it ran.
    add_executable(backend_conformance_test tests/unit/backend_conformance_test.cpp)
    target_link_libraries(backend_conformance_test PRIVATE sve_all)
    target_compile_options(backend_conformance_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME backend_conformance COMMAND backend_conformance_test)

    add_executable(public_api_test tests/unit/public_api_test.cpp)
    target_link_libraries(public_api_test PRIVATE sve_all)
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
    # frame.h is the public header that names an FFmpeg type, so this target needs libavutil
    # on its path to compile it. That does not weaken what the target checks --
    # self-containment -- nor the FFmpeg-free claim: public_headers_isolated below is what
    # checks that, and it still sees include/ and nothing else.
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
