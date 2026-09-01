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
}

[[noreturn]] void raise_status(const sve::Status& status)
{
    // The message is kept whole. §58.6 asks for the original text alongside the code, and
    // the C++ messages are written to be read -- rewriting them here would lose the part
    // that says which backend refused and why.
    py::object type = g_backend_error;
    switch (status.code()) {
    case sve::StatusCode::InvalidArgument: type = g_invalid_argument; break;
    case sve::StatusCode::Unsupported:     type = g_unsupported; break;
    case sve::StatusCode::DeviceUnavailable: type = g_device_unavailable; break;
    default: break;
    }
    py::object error = type(status.message());
    error.attr("status_code") = static_cast<int>(status.code());
    error.attr("status_name") = sve::status_code_name(status.code());
    PyErr_SetObject(type.ptr(), error.ptr());
    throw py::error_already_set();
}

} // namespace sve_python
