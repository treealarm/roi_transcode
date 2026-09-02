# The Python extension -- TZ v4.2 §58.8.
#
# Built only when SVE_BUILD_PYTHON is on, and the core never learns it exists: §58.8
# requires that the C++ core not depend on Python, NumPy or pybind11, which is why this
# lives in its own directory and links the public library from outside rather than being
# folded into it.
#
# It links sve_all -- the encoder plus whichever backends this build has -- because a
# binding with no backend can be imported and can encode nothing.

# A Python extension is a shared object, so everything it links has to be position
# independent. vcpkg builds FFmpeg as a static library without -fPIC on this machine, which
# means the libavcodec-backed backends and the §52 examples cannot go into one -- the link
# fails with "relocation R_X86_64_PC32 ... recompile with -fPIC". The same wall the
# dependency scan of §44 met, and the reason its profile is §5.5's graph.
#
# That is not a compromise here. §58.9 asks for the Python tests on X264Backend and
# X265Backend, which is exactly the graph that does build: core, the two direct codec
# components and the FFmpeg interop.
if(SVE_BUILD_BACKEND_FFMPEG OR SVE_BUILD_EXAMPLES)
    message(FATAL_ERROR
        "SVE_BUILD_PYTHON needs a build without the libavcodec-backed components: they "
        "link a static FFmpeg built without -fPIC and cannot go into a shared object. "
        "Configure a separate build directory with -DSVE_BUILD_BACKEND_FFMPEG=OFF "
        "-DSVE_BUILD_EXAMPLES=OFF, which is §5.5's dependency graph and what §58.9 tests.")
endif()

find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)

# pybind11 is header-only, so where it comes from is a packaging question rather than a
# build one. The interpreter that will import the module is asked first -- a pip-installed
# pybind11 ships its own CMake config and matches that interpreter by construction -- and
# an explicit pybind11_DIR or a vcpkg tree still wins if one is set.
if(NOT pybind11_DIR)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
        OUTPUT_VARIABLE SVE_PYBIND11_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE SVE_PYBIND11_FOUND
        ERROR_QUIET)
    if(SVE_PYBIND11_FOUND EQUAL 0)
        set(pybind11_DIR "${SVE_PYBIND11_DIR}")
    endif()
endif()
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(sve_python_core MODULE
    src/module.cpp
    src/exception_translation.cpp
    src/config_bindings.cpp
    src/metadata_bindings.cpp
    src/numpy_frame.cpp
    src/encoder_bindings.cpp
)
set_target_properties(sve_python_core PROPERTIES
    OUTPUT_NAME _core
    # Beside the package, so `python -m pytest` from python/ imports the built extension
    # without an install step. The wheel build (§58.7) places it the same way.
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/smart_video_encoder")
target_include_directories(sve_python_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sve_python_core PRIVATE sve_all)
target_compile_options(sve_python_core PRIVATE ${SVE_WARNINGS})

# The version the package reports comes from the same place the C++ one does (§58.7).
target_compile_definitions(sve_python_core PRIVATE SVE_VERSION="${PROJECT_VERSION}")

# The build manifest §58.7 asks for: what this particular artefact was made from.
#
# It exists because a wheel is opaque. "Which libx264 is in here, built how, under what
# licence, from which commit" is exactly what a bug report needs and exactly what nobody
# can recover from a binary. Every field is read from the build rather than typed in, so it
# cannot describe a build that did not happen.
#
# The values are gathered by cmake/build_manifest.cmake at the top level, because §38's
# baseline profiles pin the same encoder versions and two copies of these facts would drift.
target_include_directories(sve_python_core PRIVATE "${SVE_BUILD_MANIFEST_DIR}")
