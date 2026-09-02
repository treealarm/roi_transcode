// The extension module -- TZ v4.2 §58.8.
//
// Registration order is not cosmetic: exceptions come first because everything else can
// raise through them, and configuration before the encoder because its signatures name
// those types.

#include "bindings.h"

#include "sve_build_manifest.h"

#include <string>

namespace py = pybind11;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "smart-video-encoder: adaptive ROI video encoding (TZ v4.2 §58).";

    // §58.7: the Python and C++ versions come from one source -- vcpkg.json, read by the
    // top-level build and written into the generated manifest header, so the two cannot
    // drift.
    m.attr("__version__") = sve::build::kVersion;

    // §58.7's build manifest. A wheel is opaque: which libx264 went into it, built by which
    // compiler, from which commit, under which licence terms -- none of that survives into
    // a binary, and all of it is the first thing a bug report needs. Every value comes from
    // the build, so the manifest cannot describe a build that never happened.
    //
    // The licence field is not decoration either. Linking libx264 and libx265 directly puts
    // this artefact under the GPL, which the FFmpeg-mediated path used to hide behind
    // ffmpeg[gpl]; §55 wants a license review before release and this is where a packager
    // finds out there is one to do.
    //
    // The values come from the generated header rather than from this file's own compile
    // definitions: §38's baseline profiles pin the same encoder versions, and one source is
    // what stops a field being added here and missed there.
    py::dict manifest;
    manifest["version"] = sve::build::kVersion;
    manifest["commit"] = sve::build::kCommit;
    manifest["compiler"] = sve::build::kCompiler;
    manifest["build_type"] = sve::build::kBuildType;
    manifest["cxx_standard"] = sve::build::kCxxStandard;
    manifest["cxx_abi"] = std::to_string(_GLIBCXX_USE_CXX11_ABI);
    manifest["libx264"] = sve::build::kX264Version;
    manifest["libx265"] = sve::build::kX265Version;
    manifest["ffmpeg_libavutil"] = sve::build::kFfmpegLibavutilVersion;
    manifest["features"] = sve::build::kFeatures;
    manifest["license"] = sve::build::kLicense;
    m.attr("build_manifest") = manifest;

    sve_python::register_exceptions(m);
    sve_python::register_config(m);
    sve_python::register_metadata(m);
    sve_python::register_frames(m);
    sve_python::register_encoder(m);
}
