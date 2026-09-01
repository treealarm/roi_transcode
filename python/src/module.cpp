// The extension module -- TZ v4.2 §58.8.
//
// Registration order is not cosmetic: exceptions come first because everything else can
// raise through them, and configuration before the encoder because its signatures name
// those types.

#include "bindings.h"

#include <string>

namespace py = pybind11;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "smart-video-encoder: adaptive ROI video encoding (TZ v4.2 §58).";

    // §58.7: the Python and C++ versions come from one source. SVE_VERSION is set by the
    // build from the same vcpkg.json the C++ project version is read from, so the two
    // cannot drift.
    m.attr("__version__") = SVE_VERSION;

    // §58.7's build manifest. A wheel is opaque: which libx264 went into it, built by which
    // compiler, from which commit, under which licence terms -- none of that survives into
    // a binary, and all of it is the first thing a bug report needs. Every value comes from
    // the build, so the manifest cannot describe a build that never happened.
    //
    // The licence field is not decoration either. Linking libx264 and libx265 directly puts
    // this artefact under the GPL, which the FFmpeg-mediated path used to hide behind
    // ffmpeg[gpl]; §55 wants a license review before release and this is where a packager
    // finds out there is one to do.
    py::dict manifest;
    manifest["version"] = SVE_VERSION;
    manifest["commit"] = SVE_BUILD_COMMIT;
    manifest["compiler"] = SVE_BUILD_COMPILER;
    manifest["build_type"] = SVE_BUILD_TYPE;
    manifest["cxx_standard"] = SVE_BUILD_CXX_STANDARD;
    manifest["cxx_abi"] = std::to_string(_GLIBCXX_USE_CXX11_ABI);
    manifest["libx264"] = SVE_BUILD_X264_VERSION;
    manifest["libx265"] = SVE_BUILD_X265_VERSION;
    manifest["ffmpeg_libavutil"] = SVE_BUILD_FFMPEG_VERSION;
    manifest["features"] = SVE_BUILD_FEATURES;
    manifest["license"] = "GPL-2.0-or-later (libx264/libx265 are linked directly)";
    m.attr("build_manifest") = manifest;

    sve_python::register_exceptions(m);
    sve_python::register_config(m);
    sve_python::register_metadata(m);
    sve_python::register_frames(m);
    sve_python::register_encoder(m);
}
