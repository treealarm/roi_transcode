# ta_roienc: adaptive video encoding driven by external CV results.

cmake_minimum_required(VERSION 3.22)
# One version source: the manifest is the file a release already has to touch, so it is the
# one that carries the number rather than a copy kept in step by hand.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" TA_ROIENC_MANIFEST)
string(JSON TA_ROIENC_VERSION GET "${TA_ROIENC_MANIFEST}" version)

project(ta_roienc VERSION ${TA_ROIENC_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(TA_ROIENC_BUILD_TESTS       "Build the unit tests"                       ON)
# Decode on its own, without an encoder behind it: ta_roienc/decoder.h and the device selection
# beside it. On by default because Transcoder needs the same PictureDecoder and would otherwise
# have to keep a private copy of it -- the point of the component is that there is one
# hardware decode in this repository, not one per caller.
option(TA_ROIENC_BUILD_DECODE      "Build the standalone video decoder (ta_roienc/decoder.h)" ON)
option(TA_ROIENC_INSTALL           "Install the C++ package (headers, libs, pkg-config)" ON)

# Each backend is an independent optional component, and the core names none of them: what a
# build has is generated below into RegisterDeploymentEngines.
#
# The libavcodec-backed encoders (QSV, VA-API, x264/x265 through libavcodec) are off by
# default; consumers that want hardware encoding turn them on.
option(TA_ROIENC_BUILD_ENGINE_X264   "Build the direct libx264 engine component"      ON)
option(TA_ROIENC_BUILD_ENGINE_X265   "Build the direct libx265 engine component"      ON)
# Off by default because it needs the oneVPL SDK, and a component whose SDK is absent must
# not be required to build.
option(TA_ROIENC_BUILD_ENGINE_ONEVPL "Build the native Intel oneVPL engine component"  OFF)
option(TA_ROIENC_BUILD_ENGINE_FFMPEG "Build the libavcodec-backed engines"              OFF)
option(TA_ROIENC_BUILD_FFMPEG_INTEROP "Build the AVFrame interop adapter"               ON)
option(TA_ROIENC_BUILD_TRANSCODER       "Build recode_file and Transcoder"                ON)

set(TA_ROIENC_REGISTER_CALLS "")
if(TA_ROIENC_BUILD_ENGINE_X264)
    string(APPEND TA_ROIENC_REGISTER_CALLS "    if (const Verdict s = RegisterX264Engine(registry); !s.ok()) return s;\n")
endif()
if(TA_ROIENC_BUILD_ENGINE_X265)
    string(APPEND TA_ROIENC_REGISTER_CALLS "    if (const Verdict s = RegisterX265Engine(registry); !s.ok()) return s;\n")
endif()
if(TA_ROIENC_BUILD_ENGINE_ONEVPL)
    string(APPEND TA_ROIENC_REGISTER_CALLS "    if (const Verdict s = RegisterOneVplEngine(registry); !s.ok()) return s;\n")
endif()
if(TA_ROIENC_BUILD_ENGINE_FFMPEG)
    string(APPEND TA_ROIENC_REGISTER_CALLS "    if (const Verdict s = RegisterFfmpegEngines(registry); !s.ok()) return s;\n")
endif()
set(TA_ROIENC_GENERATED_REGISTRY "${CMAKE_CURRENT_BINARY_DIR}/generated/deployment_engines.cpp")
configure_file(cmake/deployment_engines.cpp.in "${TA_ROIENC_GENERATED_REGISTRY}" @ONLY)

set(TA_ROIENC_WARNINGS
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
message(STATUS "ta_roienc FFmpeg: ${FFMPEG_INCLUDE_DIRS}")

# Each codec component declares its own library rather than relying on vcpkg putting the
# headers in the same include directory as FFmpeg's.
pkg_check_modules(X264 x264)
pkg_check_modules(X265 x265)

# ---------------------------------------------------------------------------
# ta_roienc -- the core
#
# Objects to importance to regions to a quantiser map, the encoder that drives it, and the
# registry that says which backends this deployment has. It links nothing: no FFmpeg, no
# codec, no vendor SDK, so its pkg-config Requires.private is empty.
# ---------------------------------------------------------------------------
add_library(ta_roienc
    src/adaptive/weights.cpp
    src/adaptive/object_cleanup.cpp
    src/adaptive/object_shapes.cpp
    src/adaptive/weight_grid.cpp
    src/adaptive/qp_grid.cpp
    src/adaptive/roi_memory.cpp
    src/adaptive/bit_allowance.cpp
    src/adaptive/qp_rate_limit.cpp
    src/adaptive/box_steadier.cpp
    src/adaptive/roi_delay.cpp
    src/metadata/cbor_io.cpp
    src/metadata/sei_record.cpp
    src/metadata/metadata_file.cpp
    src/engine/common/engine_catalog.cpp
    src/core/engine_order.cpp
    src/core/coding_profile.cpp
    src/core/log.cpp
    src/core/memory_kind.cpp
    src/core/rules.cpp
    src/core/pixel_layout.cpp
    src/core/effort.cpp
    src/core/roi_picture_pipeline.cpp
    src/core/picture_view.cpp
    src/core/roi_coder.cpp
    src/core/encode_core.cpp
    src/core/engine_deleters.cpp
)
target_include_directories(ta_roienc
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    # The arithmetic tests and Transcoder include these headers directly. BUILD_INTERFACE
    # only -- these are internal headers and are not installed, so an installed consumer
    # never sees the path.
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive>
)
target_compile_definitions(ta_roienc PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ta_roienc PRIVATE ${TA_ROIENC_WARNINGS})
# The type comes from BUILD_SHARED_LIBS rather than an explicit STATIC, so a shared build
# yields libta_roienc.so.<major>. Static stays the default: consumers link it statically through
# add_subdirectory().
set_target_properties(ta_roienc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

add_library(ta_roienc::ta_roienc ALIAS ta_roienc)

# A component links the core and is linked by the deployment, never the other way round.
# The core must not gain a dependency on a backend: that would put libx264 in front of
# everyone who embeds us.
function(ta_roienc_add_backend_component name)
    target_include_directories(${name}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_compile_options(${name} PRIVATE ${TA_ROIENC_WARNINGS})
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${name} PUBLIC ta_roienc)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# ---------------------------------------------------------------------------
# ta_roienc_engine_x264 / ta_roienc_engine_x265
#
# libx264 and libx265 through their own APIs. Separate components because they are separate
# libraries with separate APIs.
# ---------------------------------------------------------------------------
if(TA_ROIENC_BUILD_ENGINE_X264)
    add_library(ta_roienc_engine_x264 src/engine/x264/x264_engine.cpp)
    ta_roienc_add_backend_component(ta_roienc_engine_x264)
    target_include_directories(ta_roienc_engine_x264 SYSTEM PRIVATE ${X264_INCLUDE_DIRS})
    target_link_directories(ta_roienc_engine_x264 PUBLIC $<BUILD_INTERFACE:${X264_LIBRARY_DIRS}>)
    target_link_libraries(ta_roienc_engine_x264 PUBLIC ${X264_LIBRARIES})
    add_library(ta_roienc::engine_x264_static ALIAS ta_roienc_engine_x264)
endif()

if(TA_ROIENC_BUILD_ENGINE_X265)
    add_library(ta_roienc_engine_x265 src/engine/x265/x265_engine.cpp)
    ta_roienc_add_backend_component(ta_roienc_engine_x265)
    target_include_directories(ta_roienc_engine_x265 SYSTEM PRIVATE ${X265_INCLUDE_DIRS})
    target_link_directories(ta_roienc_engine_x265 PUBLIC $<BUILD_INTERFACE:${X265_LIBRARY_DIRS}>)
    target_link_libraries(ta_roienc_engine_x265 PUBLIC ${X265_LIBRARIES})
    add_library(ta_roienc::engine_x265_static ALIAS ta_roienc_engine_x265)
endif()

# ---------------------------------------------------------------------------
# ta_roienc_engine_onevpl
#
# Intel oneVPL through its own API: vendor silicon without libavcodec.
# ---------------------------------------------------------------------------
if(TA_ROIENC_BUILD_ENGINE_ONEVPL)
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
        message(FATAL_ERROR "TA_ROIENC_BUILD_ENGINE_ONEVPL needs the oneVPL SDK (libvpl)")
    endif()
    add_library(ta_roienc_engine_onevpl src/engine/onevpl_native/onevpl_native_engine.cpp)
    ta_roienc_add_backend_component(ta_roienc_engine_onevpl)
    target_include_directories(ta_roienc_engine_onevpl SYSTEM PRIVATE ${VPL_INCLUDE_DIRS})
    target_link_directories(ta_roienc_engine_onevpl PUBLIC $<BUILD_INTERFACE:${VPL_LIBRARY_DIRS}>)
    target_link_libraries(ta_roienc_engine_onevpl PUBLIC ${VPL_LIBRARIES})
    add_library(ta_roienc::engine_onevpl_static ALIAS ta_roienc_engine_onevpl)
endif()

# ---------------------------------------------------------------------------
# ta_roienc_ffmpeg_interop
#
# Turns an AVFrame into a Picture and nothing else. libavutil alone: it does not encode, so
# libavcodec has no business here.
# ---------------------------------------------------------------------------
if(TA_ROIENC_BUILD_FFMPEG_INTEROP)
    add_library(ta_roienc_ffmpeg_interop
        src/core/av_picture_ref.cpp
        src/core/av_picture_bridge.cpp
        src/core/av_log_bridge.cpp
        src/engine/common/av_pixel_layout.cpp
    )
    target_include_directories(ta_roienc_ffmpeg_interop
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_include_directories(ta_roienc_ffmpeg_interop SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(ta_roienc_ffmpeg_interop PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(ta_roienc_ffmpeg_interop PUBLIC ta_roienc ${FFMPEG_LIBRARIES})
    target_compile_options(ta_roienc_ffmpeg_interop PRIVATE ${TA_ROIENC_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# ta_roienc_engine_ffmpeg
#
# libx264, libx265, QSV and VA-API through libavcodec.
# ---------------------------------------------------------------------------
if(TA_ROIENC_BUILD_ENGINE_FFMPEG)
    add_library(ta_roienc_engine_ffmpeg
        src/engine/common/engine_common.cpp
        src/engine/software/software_engine.cpp
        src/engine/onevpl/onevpl_engine.cpp
        src/engine/ffmpeg/vaapi_engine.cpp
    )
    ta_roienc_add_backend_component(ta_roienc_engine_ffmpeg)
    target_include_directories(ta_roienc_engine_ffmpeg SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(ta_roienc_engine_ffmpeg PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(ta_roienc_engine_ffmpeg PUBLIC ${FFMPEG_LIBRARIES})
endif()

# ---------------------------------------------------------------------------
# ta_roienc_decode -- decode without an encoder
#
# Transcoder and a standalone consumer decode through exactly the same code. libavcodec and
# libavutil, and no libavformat: nothing here demuxes.
# ---------------------------------------------------------------------------
if(TA_ROIENC_BUILD_DECODE)
    add_library(ta_roienc_decode
        src/core/picture_input.cpp
        src/decode/decode_engine.cpp
        src/decode/decode_hardware.cpp
        src/decode/decode_core.cpp
    )
    target_include_directories(ta_roienc_decode
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/decode)
    target_include_directories(ta_roienc_decode SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(ta_roienc_decode PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(ta_roienc_decode PUBLIC ta_roienc ${FFMPEG_LIBRARIES})
    # AvPictureBridge turns the decoder's AVFrame into a Picture. Required rather than
    # optional: without the adapter there is no way to hand a decoded frame out through the
    # public interface, so a decode component without it would compile and be useless.
    if(NOT TA_ROIENC_BUILD_FFMPEG_INTEROP)
        message(FATAL_ERROR "TA_ROIENC_BUILD_DECODE needs TA_ROIENC_BUILD_FFMPEG_INTEROP")
    endif()
    target_link_libraries(ta_roienc_decode PUBLIC ta_roienc_ffmpeg_interop)
    target_compile_options(ta_roienc_decode PRIVATE ${TA_ROIENC_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# ta_roienc_transcoder -- recode_file and Transcoder
#
# A demuxer, a decoder and a muxer around the encoder. They are the reason libavformat
# appears anywhere, and keeping them in their own target is what stops that spreading.
# ---------------------------------------------------------------------------
if(TA_ROIENC_BUILD_TRANSCODER)
    if(NOT TA_ROIENC_BUILD_DECODE)
        message(FATAL_ERROR "TA_ROIENC_BUILD_TRANSCODER needs TA_ROIENC_BUILD_DECODE: Transcoder decodes")
    endif()
    add_library(ta_roienc_transcoder
        src/core/recode_file.cpp
        src/core/transcoder.cpp
        src/core/file_muxer.cpp
    )
    target_include_directories(ta_roienc_transcoder
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_include_directories(ta_roienc_transcoder SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(ta_roienc_transcoder PUBLIC $<BUILD_INTERFACE:${FFMPEG_LIBRARY_DIRS}>)
    endif()
    target_link_libraries(ta_roienc_transcoder PUBLIC ta_roienc ta_roienc_decode ${FFMPEG_LIBRARIES})
    if(TA_ROIENC_BUILD_FFMPEG_INTEROP)
        target_link_libraries(ta_roienc_transcoder PUBLIC ta_roienc_ffmpeg_interop)
    endif()
    target_compile_options(ta_roienc_transcoder PRIVATE ${TA_ROIENC_WARNINGS})
    target_compile_definitions(ta_roienc_transcoder PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# ta_roienc_deployment -- the generated aggregator, and why it is not in the core
#
# RegisterDeploymentEngines calls one function per enabled component, so whatever holds it
# refers to every component in the build. In the core that would make the components
# transitive dependencies of ta_roienc: a test linking the core alone could no longer link,
# because the core would name libx264's registration function.
#
# So it lives on the deployment's side of the line. The core declares the function and
# calls it lazily; this target defines it. A test that never touches backend selection
# never pulls the object that needs it, which is how static archives work and why the
# arithmetic tests still link against the core by itself.
#
# An OBJECT library, not a static one, and that is the whole trick. The core has an
# undefined RegisterDeploymentEngines and the components depend on the core, so CMake
# topologically sorts the core last and an archive holding the definition would sit in
# front of the reference that needs it. Object files go onto the link line unconditionally
# and satisfy any archive whatever the order, so the cycle never has to be declared.
# ---------------------------------------------------------------------------
add_library(ta_roienc_deployment OBJECT "${TA_ROIENC_GENERATED_REGISTRY}")
target_include_directories(ta_roienc_deployment PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(ta_roienc_deployment PRIVATE ${TA_ROIENC_WARNINGS})

# What a consumer links to get "everything this build has". Kept as one name so that
# add_subdirectory() users do not have to learn the component list.
add_library(ta_roienc_all INTERFACE)
target_link_libraries(ta_roienc_all INTERFACE ta_roienc $<TARGET_OBJECTS:ta_roienc_deployment>)
foreach(component ta_roienc_engine_ffmpeg ta_roienc_ffmpeg_interop ta_roienc_decode
                  ta_roienc_transcoder ta_roienc_engine_x264 ta_roienc_engine_x265
                  ta_roienc_engine_onevpl)
    if(TARGET ${component})
        target_link_libraries(ta_roienc_all INTERFACE ${component})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Install and export
#
# find_package(ta_roienc CONFIG) yields ta_roienc::ta_roienc, and each component is its own
# explicitly linkable target. Each pkg-config file declares only what its own unit links: the core's
# Requires.private is empty, because the core links nothing.
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

if(TA_ROIENC_INSTALL)

set(TA_ROIENC_INSTALL_TARGETS ta_roienc ta_roienc_all)
set(TA_ROIENC_INSTALLED_COMPONENTS "")
foreach(component ta_roienc_engine_x264 ta_roienc_engine_x265 ta_roienc_engine_onevpl
                  ta_roienc_engine_ffmpeg ta_roienc_ffmpeg_interop ta_roienc_decode
                  ta_roienc_transcoder)
    if(TARGET ${component})
        list(APPEND TA_ROIENC_INSTALL_TARGETS ${component})
        list(APPEND TA_ROIENC_INSTALLED_COMPONENTS ${component})
    endif()
endforeach()

install(TARGETS ${TA_ROIENC_INSTALL_TARGETS} EXPORT ta_roiencTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ta_roienc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
install(EXPORT ta_roiencTargets NAMESPACE ta_roienc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ta_roienc)

configure_package_config_file(cmake/ta_roiencConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ta_roiencConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ta_roienc)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ta_roiencConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ta_roiencConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/ta_roiencConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ta_roienc)

# One .pc per installable unit, each declaring only what it actually links.
function(ta_roienc_write_pc target library description requires)
    set(TA_ROIENC_PC_LIBRARY "${library}")
    set(TA_ROIENC_PC_DESCRIPTION "${description}")
    set(TA_ROIENC_PC_REQUIRES_PRIVATE "${requires}")
    configure_file(cmake/ta_roienc.pc.in "${CMAKE_CURRENT_BINARY_DIR}/${target}.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${target}.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endfunction()

ta_roienc_write_pc(ta_roienc ta_roienc "Adaptive ROI video encoding -- core" "")
if(TARGET ta_roienc_engine_x264)
    ta_roienc_write_pc(ta_roienc-backend-x264 ta_roienc_engine_x264
        "Adaptive ROI video encoding -- direct libx264 backend" "x264")
endif()
if(TARGET ta_roienc_engine_x265)
    ta_roienc_write_pc(ta_roienc-backend-x265 ta_roienc_engine_x265
        "Adaptive ROI video encoding -- direct libx265 backend" "x265")
endif()
if(TARGET ta_roienc_ffmpeg_interop)
    # libavutil and not libavcodec: the adapter converts a frame, it does not encode.
    ta_roienc_write_pc(ta_roienc-ffmpeg-interop ta_roienc_ffmpeg_interop
        "Adaptive ROI video encoding -- AVFrame interop" "libavutil")
endif()

endif()  # TA_ROIENC_INSTALL

# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------
if(TA_ROIENC_BUILD_TESTS)
    enable_testing()

    add_executable(object_cleanup_test tests/unit/object_cleanup_test.cpp)
    target_link_libraries(object_cleanup_test PRIVATE ta_roienc)
    target_compile_options(object_cleanup_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME object_cleanup COMMAND object_cleanup_test)

    add_executable(object_shapes_test tests/unit/object_shapes_test.cpp)
    target_link_libraries(object_shapes_test PRIVATE ta_roienc)
    target_compile_options(object_shapes_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME object_shapes COMMAND object_shapes_test)

    add_executable(weighting_test tests/unit/weighting_test.cpp)
    target_link_libraries(weighting_test PRIVATE ta_roienc)
    target_compile_options(weighting_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME weighting COMMAND weighting_test)

    add_executable(roi_memory_test tests/unit/roi_memory_test.cpp)
    target_link_libraries(roi_memory_test PRIVATE ta_roienc)
    target_compile_options(roi_memory_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME roi_memory COMMAND roi_memory_test)

    add_executable(bit_allowance_test tests/unit/bit_allowance_test.cpp)
    target_link_libraries(bit_allowance_test PRIVATE ta_roienc)
    target_compile_options(bit_allowance_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME bit_allowance COMMAND bit_allowance_test)

    # roi_coder_test drives the pipeline through a backend that fails on purpose, so it
    # needs the internal header the pipeline lives behind: src/core is PRIVATE on ta_roienc
    # because these are not installed, and the test is inside the project rather than a
    # consumer of it.
    # ta_roienc_all rather than ta_roienc: the fake backend is the only one this test registers, but
    # default_engine_catalog() still has to resolve, and RegisterDeploymentEngines is
    # generated into the deployment object rather than the core.
    add_executable(roi_coder_test tests/unit/roi_coder_test.cpp)
    target_link_libraries(roi_coder_test PRIVATE ta_roienc_all)
    target_include_directories(roi_coder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(roi_coder_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME roi_coder COMMAND roi_coder_test)

    # The facade under a backend that fails on purpose. Needs the internal assembly header
    # for the same reason roi_coder_test needs roi_coder.h: the seam is inside the
    # project, not in include/.
    add_executable(encode_faults_test tests/unit/encode_faults_test.cpp)
    target_link_libraries(encode_faults_test PRIVATE ta_roienc_all)
    target_include_directories(encode_faults_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(encode_faults_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME encode_faults COMMAND encode_faults_test)

    add_executable(region_rules_test tests/unit/region_rules_test.cpp)
    target_link_libraries(region_rules_test PRIVATE ta_roienc)
    target_compile_options(region_rules_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME region_rules COMMAND region_rules_test)

    add_executable(shapes_test tests/unit/shapes_test.cpp)
    target_link_libraries(shapes_test PRIVATE ta_roienc)
    target_compile_options(shapes_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME shapes COMMAND shapes_test)

    # Two encoders at once, and the shared first-use state underneath them. Links ta_roienc_all
    # rather than ta_roienc: what it checks is that the *registry* and the backend singletons
    # behind it are safe to reach simultaneously, and a build with no backends registered
    # would pass it by having nothing to contend over.
    find_package(Threads REQUIRED)
    add_executable(parallel_runs_test tests/unit/parallel_runs_test.cpp)
    target_link_libraries(parallel_runs_test PRIVATE ta_roienc_all Threads::Threads)
    target_compile_options(parallel_runs_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME parallel_runs COMMAND parallel_runs_test)

    # cbor_io_test compiles the encoder directly rather than linking the library: it checks a
    # format against RFC 8949's own vectors, and has no business needing FFmpeg to do it.
    add_executable(cbor_io_test tests/unit/cbor_io_test.cpp src/metadata/cbor_io.cpp)
    target_compile_options(cbor_io_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME cbor_io COMMAND cbor_io_test)

    add_executable(sei_record_test tests/unit/sei_record_test.cpp)
    target_link_libraries(sei_record_test PRIVATE ta_roienc)
    target_compile_options(sei_record_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME sei_record COMMAND sei_record_test)

    # Covers AvPictureBridge, the AVFrame interop, so unlike the tests above there is a
    # libavutil behind this one.
    add_executable(picture_test tests/unit/picture_test.cpp)
    target_link_libraries(picture_test PRIVATE ta_roienc_all)
    target_compile_options(picture_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME picture COMMAND picture_test)

    # encode_test drives the public encoder end to end on the software backend: no GPU,
    # but a real libx264 behind it.
    add_executable(encode_test tests/unit/encode_test.cpp)
    target_link_libraries(encode_test PRIVATE ta_roienc_all)
    target_compile_options(encode_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME encode COMMAND encode_test)

    # The decode side of the public API, on the software path: no GPU needed, and the
    # stream it decodes comes out of the encoder in the same binary. Guarded because
    # ta_roienc_decode is an option -- a consumer that only encodes does not build it.
    if(TARGET ta_roienc_decode)
        add_executable(decode_test tests/unit/decode_test.cpp)
        target_link_libraries(decode_test PRIVATE ta_roienc_all)
        target_compile_options(decode_test PRIVATE ${TA_ROIENC_WARNINGS})
        add_test(NAME decode COMMAND decode_test)
    endif()

    # Log events and levels, plus the FFmpeg routing. Reaches live_tuning_warnings.h
    # directly: the guards are where the fallback and refusal events are emitted, and
    # provoking each one through the public API would need a backend that lacks the
    # capability rather than a test that says so.
    add_executable(log_test tests/unit/log_test.cpp)
    target_link_libraries(log_test PRIVATE ta_roienc_all)
    target_include_directories(log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_compile_options(log_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME log COMMAND log_test)

    # The runtime guards, tested directly: they are pure functions.
    add_executable(live_tuning_warnings_test tests/unit/live_tuning_warnings_test.cpp)
    target_link_libraries(live_tuning_warnings_test PRIVATE ta_roienc_all)
    target_include_directories(live_tuning_warnings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_compile_options(live_tuning_warnings_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME live_tuning_warnings COMMAND live_tuning_warnings_test)

    # Every enumerator has a name of its own, asked of the library's own naming functions.
    add_executable(labels_test tests/unit/labels_test.cpp)
    target_link_libraries(labels_test PRIVATE ta_roienc_all)
    target_include_directories(labels_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit)
    target_compile_options(labels_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME labels COMMAND labels_test)

    # Runs where a backend reports main10 and skips with the encoder's own words where it
    # does not.
    add_executable(ten_bit_samples_test tests/unit/ten_bit_samples_test.cpp)
    target_link_libraries(ten_bit_samples_test PRIVATE ta_roienc_all)
    target_compile_options(ten_bit_samples_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME ten_bit_samples COMMAND ten_bit_samples_test)

    # Encode, decode, frame count, timestamps and end of stream across every backend in the
    # build. Absent hardware is a skip inside the test, so this runs everywhere and reports
    # how much of it ran.
    add_executable(engine_conformance_test tests/unit/engine_conformance_test.cpp)
    target_link_libraries(engine_conformance_test PRIVATE ta_roienc_all)
    target_compile_options(engine_conformance_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME engine_conformance COMMAND engine_conformance_test)

    add_executable(api_surface_test tests/unit/api_surface_test.cpp)
    target_link_libraries(api_surface_test PRIVATE ta_roienc_all)
    target_compile_options(api_surface_test PRIVATE ${TA_ROIENC_WARNINGS})
    add_test(NAME api_surface COMMAND api_surface_test)

    # Every public header on its own, compiled with include/ and nothing else in front of
    # it. One translation unit per header, generated: a header that forgot an include of
    # its own would still compile inside the umbrella, because whatever it needed had
    # already been pulled in by a sibling above it. This is what makes "include only what
    # you need" a property rather than an intention.
    file(GLOB TA_ROIENC_PUBLIC_HEADERS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/ta_roienc/*.h)
    set(TA_ROIENC_SELFCONTAINED_SOURCES "")
    foreach(header ${TA_ROIENC_PUBLIC_HEADERS})
        string(REPLACE "/" "_" stem "${header}")
        string(REPLACE ".h" "" stem "${stem}")
        set(generated "${CMAKE_CURRENT_BINARY_DIR}/selfcontained/${stem}.cpp")
        file(WRITE "${generated}" "#include \"${header}\"\n")
        list(APPEND TA_ROIENC_SELFCONTAINED_SOURCES "${generated}")
    endforeach()
    add_library(public_headers_selfcontained OBJECT ${TA_ROIENC_SELFCONTAINED_SOURCES})
    target_include_directories(public_headers_selfcontained
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # picture.h is the public header that names an FFmpeg type, so this target needs libavutil
    # on its path to compile it. That does not weaken what the target checks --
    # self-containment -- nor the FFmpeg-free claim: headers_isolated below is what
    # checks that, and it still sees include/ and nothing else.
    target_include_directories(public_headers_selfcontained
        SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_compile_options(public_headers_selfcontained PRIVATE ${TA_ROIENC_WARNINGS})

    # And this one holds the other half of that line. api_surface_test links ta_roienc, so
    # it inherits ${FFMPEG_INCLUDE_DIRS} and would keep compiling if a public header
    # started including libavcodec. This target sees include/ and nothing else, links
    # nothing, and is the only place the "no FFmpeg in the public surface" claim is
    # actually checked. It is a compile test: building it is passing it.
    add_library(headers_isolated OBJECT tests/unit/headers_isolated.cpp)
    target_include_directories(headers_isolated
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(headers_isolated PRIVATE ${TA_ROIENC_WARNINGS})
endif()
