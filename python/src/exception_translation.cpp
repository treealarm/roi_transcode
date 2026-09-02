// C++ Status to Python exceptions -- TZ v4.2 §58.6.
//
// The hierarchy carries the StatusCode rather than flattening everything into RuntimeError,
// because §58.6 asks for exactly that: a caller must be able to branch on what went wrong
// without parsing a message. InvalidArgument means "fix the configuration",
// DeviceUnavailable means "try another machine", and those are different decisions.
//
// Again and EndOfStream are deliberately not exceptions. §58.6 requires them to have
// documented Python semantics rather than being masked as an unknown error, and in Python
// the honest spelling of "nothing right now" is an empty list, while the honest spelling of
// "the stream ended" is a flag on the encoder. Raising for either would make the ordinary
// drain loop a try/except, which is not what those codes mean.

#include "bindings.h"

#include <pybind11/pybind11.h>

#include <string>

namespace py = pybind11;

namespace sve_python {

namespace {

py::object g_base;
py::object g_invalid_argument;
py::object g_unsupported;
py::object g_device_unavailable;
py::object g_backend_error;

} // namespace

void register_exceptions(py::module_& m)
{
    // A base every caller can catch, and subclasses for the decisions that differ. The
    // base derives from Exception rather than RuntimeError so that `except RuntimeError`
    // in surrounding code does not swallow an encoder error by accident.
    g_base = py::exception<void>(m, "SmartVideoEncoderError", PyExc_Exception);
    m.attr("SmartVideoEncoderError") = g_base;
    (void)0;

    const auto derive = [&](const char* name) {
        py::object cls = py::reinterpret_steal<py::object>(PyErr_NewException(
            (std::string("smart_video_encoder._core.") + name).c_str(), g_base.ptr(), nullptr));
        m.attr(name) = cls;
        return cls;
    };
    g_invalid_argument = derive("InvalidArgumentError");
    g_unsupported = derive("UnsupportedError");
    g_device_unavailable = derive("DeviceUnavailableError");
    g_backend_error = derive("BackendError");

    // §58.6 asks the mapping to be documented; this is the mapping itself, so a caller can
    // read it rather than infer it from a table in a README that nothing checks. It is the
    // same function raise_status consults, which is what makes the answer true.
    m.def("exception_for_status_code", &exception_type_for, py::arg("code"),
        "Which exception a StatusCode becomes, or None for the three codes that are not "
        "errors: Ok, Again and EndOfStream (§58.6).");
}

py::object exception_type_for(sve::StatusCode code)
{
    switch (code) {
    // Not errors, and deliberately not exceptions -- see the note at the top of this file.
    case sve::StatusCode::Ok:
    case sve::StatusCode::Again:
    case sve::StatusCode::EndOfStream:
        return py::none();
    case sve::StatusCode::InvalidArgument:   return g_invalid_argument;
    case sve::StatusCode::Unsupported:       return g_unsupported;
    case sve::StatusCode::DeviceUnavailable: return g_device_unavailable;
    // Everything that is not one of the three decisions above arrives as BackendError with
    // `status_code` saying which it was. That includes OutOfMemory, Timeout and Error: they
    // are not decisions a caller branches on differently, and a class per code would be a
    // hierarchy nobody catches. The code itself is not lost either way.
    case sve::StatusCode::BackendError:
    case sve::StatusCode::OutOfMemory:
    case sve::StatusCode::Timeout:
    case sve::StatusCode::Error:
        return g_backend_error;
    }
    // A value that is no enumerator at all. Nothing in the library produces one; if
    // something ever does, it arrives as the base rather than as a wrong subclass.
    return g_base;
}

[[noreturn]] void raise_status(const sve::Status& status)
{
    // The message is kept whole. §58.6 asks for the original text alongside the code, and
    // the C++ messages are written to be read -- rewriting them here would lose the part
    // that says which backend refused and why.
    py::object type = exception_type_for(status.code());
    if (type.is_none()) {
        // Ok, Again or EndOfStream reached a raising path, which is a defect in the
        // bindings rather than in the caller's code: those three are answers, and every
        // call site handles them before it gets here. Raising the base with the code in it
        // says so out loud instead of manufacturing a plausible-looking BackendError.
        type = g_base;
    }
    py::object error = type(status.message());
    error.attr("status_code") = static_cast<int>(status.code());
    error.attr("status_name") = sve::status_code_name(status.code());
    PyErr_SetObject(type.ptr(), error.ptr());
    throw py::error_already_set();
}

} // namespace sve_python
