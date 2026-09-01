// The extension module -- TZ v4.2 §58.8.
//
// Registration order is not cosmetic: exceptions come first because everything else can
// raise through them, and configuration before the encoder because its signatures name
// those types.

#include "bindings.h"

namespace py = pybind11;

PYBIND11_MODULE(_core, m)
{
    m.doc() = "smart-video-encoder: adaptive ROI video encoding (TZ v4.2 §58).";

    // §58.7: the Python and C++ versions come from one source. SVE_VERSION is set by the
    // build from the same vcpkg.json the C++ project version is read from, so the two
    // cannot drift.
    m.attr("__version__") = SVE_VERSION;

    sve_python::register_exceptions(m);
    sve_python::register_config(m);
    sve_python::register_metadata(m);
    sve_python::register_frames(m);
    sve_python::register_encoder(m);
}
