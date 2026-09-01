# smart-video-encoder: adaptive video encoding driven by external CV results.
#
# Layout follows TZ v2.0 §52. What that document describes is the library alone --
# include/, src/, tests/, benchmarks/. Our gRPC service, its JSON adapter and the ta_vms
# scripts are not part of the deliverable and live under integration/, behind an option
# that is off by default so a consumer building the library never sees gRPC at all.

cmake_minimum_required(VERSION 3.22)
# One version source (§26.3, §58.7): the manifest is the file a release already has to
# touch, so it is the one that carries the number rather than a copy kept in step by hand.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json" SVE_MANIFEST)
string(JSON SVE_VERSION GET "${SVE_MANIFEST}" version)

project(smart-video-encoder VERSION ${SVE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SVE_BUILD_TESTS       "Build the unit tests"                       ON)
option(SVE_BUILD_INTEGRATION "Build the ta_vms gRPC service, CLI and tools" OFF)
# §23 names both the directory and the switch. Separate from SVE_BUILD_INTEGRATION because
# the specification does describe this one: sve-capabilities is part of the deliverable,
# where the CLI and the scripts are not.
option(SVE_BUILD_TOOLS       "Build sve-capabilities (§23)"               ON)

# Each backend is an independent optional component (TZ v4.2 §5.5), and the core names none
# of them: what a build has is generated below into RegisterDeploymentBackends, which is the
# aggregator §26.4 allows a deployment to provide.
#
# SVE_BUILD_BACKEND_FFMPEG is off by default and the others are on, which is the shape of
# §5.5's normative graph: the direct libx264 and libx265 components plus an FFmpeg interop
# that only converts frames. The libavcodec-backed encoders are not in that graph at all --
# they stay as the reference backend-compare.sh measures the direct paths against, and the
# linux-dev preset turns them on so the development loop and check.sh keep their coverage.
option(SVE_BUILD_BACKEND_X264   "Build the direct libx264 backend component"      ON)
option(SVE_BUILD_BACKEND_X265   "Build the direct libx265 backend component"      ON)
option(SVE_BUILD_BACKEND_FFMPEG "Build the libavcodec-backed backends (reference)" OFF)
option(SVE_BUILD_FFMPEG_INTEROP "Build the AVFrame interop adapter (§8.5)"        ON)
option(SVE_BUILD_EXAMPLES       "Build transcode_file and Session (§52)"          ON)

set(SVE_REGISTER_CALLS "")
if(SVE_BUILD_BACKEND_X264)
    string(APPEND SVE_REGISTER_CALLS "    if (const Status s = RegisterX264Backend(registry); !s.ok()) return s;\n")
endif()
if(SVE_BUILD_BACKEND_X265)
    string(APPEND SVE_REGISTER_CALLS "    if (const Status s = RegisterX265Backend(registry); !s.ok()) return s;\n")
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
    libswresample
)
message(STATUS "smart-video-encoder FFmpeg: ${FFMPEG_INCLUDE_DIRS}")

# Each codec component declares its own library. Until the §5.5 split these headers were
# found only because vcpkg puts them in the same include directory as FFmpeg's -- the
# direct backends compiled against libx264 while depending on FFmpeg's include path to
# find it, which is precisely the accident the split exists to remove.
pkg_check_modules(X264 x264)
pkg_check_modules(X265 x265)

# ---------------------------------------------------------------------------
# sve -- the core of §5.5's dependency graph
#
# Objects to importance to regions to a quantiser map, the encoder that drives it, and the
# registry that says which backends this deployment has. It links nothing: no FFmpeg, no
# codec, no vendor SDK. That is the property §26.3 turns into an empty Requires.private and
# §44 checks with readelf, and it is why the split exists at all.
#
# sve_adaptive is folded in. It was a separate target so that the arithmetic tests could
# link without dragging FFmpeg along; now that the core itself drags nothing, the split has
# nothing left to protect.
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
    src/core/logging.cpp
    src/core/memory_type.cpp
    src/core/pixel_format.cpp
    src/core/preset.cpp
    src/core/frame_roi_pipeline.cpp
    src/core/push_encoder.cpp
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
    # As sve_adaptive published it before the merge: the arithmetic tests and the example
    # applications include these headers directly. Narrowing that is header hygiene, not
    # the dependency split, and doing both at once would hide one behind the other.
    # BUILD_INTERFACE only -- these are internal headers and are not installed, so an
    # installed consumer never sees the path.
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive>
)
target_compile_definitions(sve PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(sve PRIVATE ${SVE_WARNINGS})
# §26.3 wants libsve.so.<major> from a shared build. The targets above take their type from
# BUILD_SHARED_LIBS rather than saying STATIC: an explicit STATIC ignores the switch, and
# the first dependency scan reported "passed" having skipped every readelf check because
# there was no shared object to read.
# Static stays the default because
# ta_vms links us statically through add_subdirectory() and changing that without its say
# is not ours to do; BUILD_SHARED_LIBS=ON is what the dependency scan of §44 configures,
# since readelf needs something to read.
set_target_properties(sve PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)
add_library(sve::sve ALIAS sve)

# A component links the core and is linked by the deployment, never the other way round.
# The core must not gain a dependency on a backend: that is the direction §5.5 draws, and
# reversing it would put libx264 back in front of everyone who embeds us.
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
# sve_backend_x264 / sve_backend_x265 -- §5.4, §5.5
#
# libx264 and libx265 through their own APIs. Separate components because they are separate
# libraries with separate APIs, which is the same reason §5.4 makes them separate backends.
# ---------------------------------------------------------------------------
if(SVE_BUILD_BACKEND_X264)
    add_library(sve_backend_x264 src/backend/x264/x264_backend.cpp)
    sve_add_backend_component(sve_backend_x264)
    target_include_directories(sve_backend_x264 SYSTEM PRIVATE ${X264_INCLUDE_DIRS})
    target_link_directories(sve_backend_x264 PUBLIC ${X264_LIBRARY_DIRS})
    target_link_libraries(sve_backend_x264 PUBLIC ${X264_LIBRARIES})
    add_library(sve::backend_x264_static ALIAS sve_backend_x264)
endif()

if(SVE_BUILD_BACKEND_X265)
    add_library(sve_backend_x265 src/backend/x265/x265_backend.cpp)
    sve_add_backend_component(sve_backend_x265)
    target_include_directories(sve_backend_x265 SYSTEM PRIVATE ${X265_INCLUDE_DIRS})
    target_link_directories(sve_backend_x265 PUBLIC ${X265_LIBRARY_DIRS})
    target_link_libraries(sve_backend_x265 PUBLIC ${X265_LIBRARIES})
    add_library(sve::backend_x265_static ALIAS sve_backend_x265)
endif()

# ---------------------------------------------------------------------------
# sve_ffmpeg_interop -- §8.5, and the only FFmpeg §5.5's graph allows
#
# Turns an AVFrame into a Frame and nothing else. libavutil alone: it does not encode, so
# libavcodec has no business here, and §26.4 says the interop component may declare
# libavutil but not libavcodec.
# ---------------------------------------------------------------------------
if(SVE_BUILD_FFMPEG_INTEROP)
    add_library(sve_ffmpeg_interop
        src/core/av_frame_adapter.cpp
        src/backend/common/av_pixel_format.cpp
    )
    target_include_directories(sve_ffmpeg_interop
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_include_directories(sve_ffmpeg_interop SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sve_ffmpeg_interop PUBLIC ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(sve_ffmpeg_interop PUBLIC sve ${FFMPEG_LIBRARIES})
    target_compile_options(sve_ffmpeg_interop PRIVATE ${SVE_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# sve_backend_ffmpeg -- the reference encoders, and not in §5.5's graph
#
# libx264, libx265, oneVPL, VA-API and NVENC through libavcodec. §5.5 does not list these:
# what it wants is the direct components above. They are kept because a comparison needs
# both sides -- backend-compare.sh is what says whether the direct path holds the zone as
# well -- and off by default so that the default build is the graph the specification draws.
# ---------------------------------------------------------------------------
if(SVE_BUILD_BACKEND_FFMPEG)
    add_library(sve_backend_ffmpeg
        src/backend/common/backend_common.cpp
        src/backend/software/software_backend.cpp
        src/backend/onevpl/onevpl_backend.cpp
        src/backend/ffmpeg/vaapi_backend.cpp
        src/backend/nvenc/nvenc_backend.cpp
    )
    sve_add_backend_component(sve_backend_ffmpeg)
    target_include_directories(sve_backend_ffmpeg SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sve_backend_ffmpeg PUBLIC ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(sve_backend_ffmpeg PUBLIC ${FFMPEG_LIBRARIES})
endif()

# ---------------------------------------------------------------------------
# sve_examples -- §52's example applications, not the library
#
# transcode_file and Session: a demuxer, a decoder and a muxer around the encoder. The
# specification calls them examples and question 12 asks the customer whether they belong
# outside the core at all; either way they are the reason libavformat appears anywhere, and
# keeping them in their own target is what stops that spreading.
# ---------------------------------------------------------------------------
if(SVE_BUILD_EXAMPLES)
    add_library(sve_examples
        src/core/transcode_file.cpp
        src/core/session.cpp
        src/core/frame_input.cpp
        src/core/file_mux_sink.cpp
    )
    target_include_directories(sve_examples
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_include_directories(sve_examples SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sve_examples PUBLIC ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(sve_examples PUBLIC sve ${FFMPEG_LIBRARIES})
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
# transitive dependencies of sve, which §26.4 forbids in as many words, and it showed up
# immediately: a test linking the core alone could no longer link, because the core now
# named libx264's registration function.
#
# So it lives on the deployment's side of the line. The core declares the function and
# calls it lazily; this target defines it. A test that never touches backend selection
# never pulls the object that needs it, which is how static archives work and why the
# arithmetic tests still link against the core by itself.
# ---------------------------------------------------------------------------
# An OBJECT library, not a static one, and that is the whole trick. The core has an
# undefined RegisterDeploymentBackends and the components depend on the core, so CMake
# topologically sorts the core last and an archive holding the definition would sit in
# front of the reference that needs it. Object files go onto the link line unconditionally
# and satisfy any archive whatever the order, so the cycle never has to be declared -- and
# declaring it would have made the components transitive dependencies of the core, which
# §26.4 forbids.
add_library(sve_deployment OBJECT "${SVE_GENERATED_REGISTRY}")
target_include_directories(sve_deployment PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(sve_deployment PRIVATE ${SVE_WARNINGS})

# What a consumer links to get "everything this build has". Kept as one name so that
# add_subdirectory() users -- ta_vms among them -- do not have to learn the component list.
add_library(sve_all INTERFACE)
target_link_libraries(sve_all INTERFACE sve $<TARGET_OBJECTS:sve_deployment>)
foreach(component sve_backend_x264 sve_backend_x265 sve_backend_ffmpeg
                  sve_ffmpeg_interop sve_examples)
    if(TARGET ${component})
        target_link_libraries(sve_all INTERFACE ${component})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# tools -- §23
#
# Linked against sve_all rather than sve alone: the report is about which backends this
# build has, so it needs them present. It compiles against include/ and nothing else, which
# is the other half of what §23 asks -- a tool that had to reach into src/ would be
# describing a library nobody else can use.
# ---------------------------------------------------------------------------
if(SVE_BUILD_TOOLS)
    add_executable(sve-capabilities tools/sve_capabilities.cpp)
    target_link_libraries(sve-capabilities PRIVATE sve_all)
    target_compile_options(sve-capabilities PRIVATE ${SVE_WARNINGS})
    install(TARGETS sve-capabilities RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Install and export -- §26.3
#
# find_package(sve CONFIG) yields sve::sve, and each component is its own explicitly
# linkable target. The pkg-config file is the part §26.4 states as a rule and §44 checks:
#
#     Основной sve.pc не объявляет FFmpeg, x264, x265 или vendor SDK зависимостями.
#     Component metadata объявляет только собственные зависимости соответствующего backend.
#
# The core's Requires.private is empty, and that is now a fact about the build rather than
# a promise: the core links nothing, so there is nothing for it to declare.
# ---------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

set(SVE_INSTALL_TARGETS sve sve_all)
set(SVE_INSTALLED_COMPONENTS "")
foreach(component sve_backend_x264 sve_backend_x265 sve_backend_ffmpeg
                  sve_ffmpeg_interop sve_examples)
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
    # libavutil and not libavcodec: the adapter converts a frame, it does not encode, and
    # §26.4 allows the interop component exactly this one.
    sve_write_pc(sve-ffmpeg-interop sve_ffmpeg_interop
        "Adaptive ROI video encoding -- AVFrame interop" "libavutil")
endif()

# ---------------------------------------------------------------------------
# Tests -- §52 tests/unit
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

    add_executable(v3_policy_test tests/unit/v3_policy_test.cpp)
    target_link_libraries(v3_policy_test PRIVATE sve)
    target_compile_options(v3_policy_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME v3_policy COMMAND v3_policy_test)

    add_executable(geometry_test tests/unit/geometry_test.cpp)
    target_link_libraries(geometry_test PRIVATE sve)
    target_compile_options(geometry_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME geometry COMMAND geometry_test)

    # cbor_test compiles the encoder directly rather than linking the library: it checks a
    # format against RFC 8949's own vectors, and has no business needing FFmpeg to do it.
    add_executable(cbor_test tests/unit/cbor_test.cpp src/metadata/cbor.cpp)
    target_compile_options(cbor_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME cbor COMMAND cbor_test)

    add_executable(sei_payload_test tests/unit/sei_payload_test.cpp)
    target_link_libraries(sei_payload_test PRIVATE sve)
    target_compile_options(sei_payload_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME sei_payload COMMAND sei_payload_test)

    # Compiles against the public headers only -- no src/ on the include path. That is the
    # point of it: it stops compiling the moment an internal type leaks into the surface.
    # frame_test links sve because it covers AvFrameAdapter, which is the §8.5 interop, so
    # unlike the tests above there is a libavutil behind this one. WrapRawFrame is in the
    # same test and no longer needs it: §8.10 moved to sve/raw_frame.h and is proved
    # FFmpeg-free by public_headers_isolated below.
    add_executable(frame_test tests/unit/frame_test.cpp)
    target_link_libraries(frame_test PRIVATE sve_all)
    target_compile_options(frame_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME frame COMMAND frame_test)

    # encoder_test drives the public encoder end to end on the software backend: no GPU,
    # but a real libx264 behind it, so it links sve like frame_test does.
    add_executable(encoder_test tests/unit/encoder_test.cpp)
    target_link_libraries(encoder_test PRIVATE sve_all)
    target_compile_options(encoder_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME encoder COMMAND encoder_test)

    add_executable(push_encoder_test tests/unit/push_encoder_test.cpp)
    target_link_libraries(push_encoder_test PRIVATE sve_all)
    target_compile_options(push_encoder_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME push_encoder COMMAND push_encoder_test)

    # AT-009 from §48. Runs where a backend reports main10 and skips with the encoder's own
    # words where it does not, which is the capability evidence §50 asks a SKIP to carry.
    add_executable(ten_bit_test tests/unit/ten_bit_test.cpp)
    target_link_libraries(ten_bit_test PRIVATE sve_all)
    target_compile_options(ten_bit_test PRIVATE ${SVE_WARNINGS})
    add_test(NAME ten_bit COMMAND ten_bit_test)

    # AT-002/003/006/007 across every backend in the build (§48). Absent hardware is a
    # skip inside the test, so this runs everywhere and reports how much of it ran.
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
    # frame.h and push_encoder.h are the public headers that name an FFmpeg type (TZ v3.4
    # §8.5, §8.8 and §8.10), so this target needs libavutil on its path to compile them. It does not
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
