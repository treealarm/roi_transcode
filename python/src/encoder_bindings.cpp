// The encoder itself -- TZ v4.2 §58.3, §58.5, §58.6.
//
// Three requirements shape this file.
//
// §58.5: the GIL is released around anything that can block. submit_frame and flush reach
// into a real encoder, and holding the GIL through a hardware encode would stop every other
// Python thread for the duration -- which is exactly what §58.5 forbids. It is reacquired
// before any Python object is built.
//
// §58.6: Again and EndOfStream are not exceptions. They are the ordinary answers of a
// drain loop, and turning them into control flow by exception would make `receive_available`
// a try/except in every caller. Again is an empty list; EndOfStream is a flag.
//
// §58.6 again: close() is idempotent, the context manager calls it, and afterwards every
// operation raises a predictable Python exception rather than touching a destroyed encoder.

#include "bindings.h"

#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;

namespace sve_python {

namespace {

/// Owns the C++ encoder and survives its destruction, so a method called after close()
/// finds a null pointer and says so rather than dereferencing one.
class Encoder {
public:
    explicit Encoder(const sve::EncoderConfig& config)
        : m_encoder(sve::CreateVideoEncoder(config))
    {
    }

    sve::SmartVideoEncoder& live()
    {
        if (!m_encoder) {
            throw py::value_error("encoder is closed");
        }
        return *m_encoder;
    }

    void close()
    {
        if (!m_encoder) {
            return;  // §58.6: idempotent.
        }
        // Stop under a released GIL like everything else that reaches the backend, then
        // drop the object. Order matters: Stop is the graceful end, the destructor is not.
        {
            py::gil_scoped_release unlocked;
            (void)m_encoder->Stop();
        }
        m_encoder.reset();
    }

    bool closed() const { return !m_encoder; }

    bool ended = false;  ///< EndOfStream has been seen; §58.6 wants it observable.

private:
    std::unique_ptr<sve::SmartVideoEncoder> m_encoder;
};

} // namespace

void register_encoder(py::module_& m)
{
    py::class_<Encoder>(m, "SmartVideoEncoder",
        "The encoder (§8.1). Use it as a context manager, or call close() yourself.")
        .def(py::init([](const sve::EncoderConfig& config) {
            // CreateVideoEncoder throws EncoderError; §58.6 forbids a C++ exception
            // crossing the CPython boundary, so it is translated here.
            try {
                return std::make_unique<Encoder>(config);
            } catch (const sve::EncoderError& e) {
                raise_status(e.status());
            }
        }), py::arg("config"))

        .def("submit_frame",
            [](Encoder& self, sve::Frame frame, OwnedFrameMetadata& metadata) {
                sve::SmartVideoEncoder& enc = self.live();
                sve::FrameMetadata& meta = metadata.view();
                sve::Status status = sve::Status::Ok();
                {
                    // §58.5: nothing Python happens in here, so nothing needs the GIL.
                    py::gil_scoped_release unlocked;
                    status = enc.SubmitFrame(sve::TimedFrame{std::move(frame), meta});
                }
                if (status.code() == sve::StatusCode::Again) {
                    // Backpressure (§24), and an answer rather than a failure: the caller
                    // drains and submits again. Reported as False so the ordinary loop is
                    // `while not enc.submit_frame(...)` and not a try/except.
                    return false;
                }
                if (!status.ok()) {
                    raise_status(status);
                }
                return true;
            },
            py::arg("frame"), py::arg("metadata"),
            "Submit one frame. False means the queue is full (§24's backpressure): drain "
            "and try again. Errors raise.")

        .def("receive_available", [](Encoder& self) {
            sve::SmartVideoEncoder& enc = self.live();
            std::vector<sve::EncodedPacket> out;
            bool ended = false;
            {
                py::gil_scoped_release unlocked;
                for (;;) {
                    sve::EncodedPacket packet;
                    const sve::Status s = enc.ReceivePacket(&packet);
                    if (s.code() == sve::StatusCode::Again) {
                        break;
                    }
                    if (s.code() == sve::StatusCode::EndOfStream) {
                        ended = true;
                        break;
                    }
                    if (!s.ok()) {
                        // Raising needs the GIL, so the status travels out of the block.
                        out.clear();
                        ended = false;
                        py::gil_scoped_acquire locked;
                        raise_status(s);
                    }
                    out.push_back(std::move(packet));
                }
            }
            self.ended = self.ended || ended;
            return out;
        }, "Every packet ready right now, oldest first. An empty list means 'not yet' -- "
           "§58.6's documented spelling of Again, not an error.")

        .def("flush", [](Encoder& self) {
            sve::SmartVideoEncoder& enc = self.live();
            sve::Status status = sve::Status::Ok();
            {
                py::gil_scoped_release unlocked;
                status = enc.Flush();
            }
            if (!status.ok()) {
                raise_status(status);
            }
        }, "Start draining (§8.13). Keep calling receive_available until at_end is true.")

        .def_property_readonly("at_end", [](Encoder& self) { return self.ended; },
            "True once the encoder has reported end of stream.")

        .def("get_capabilities", [](Encoder& self) {
            return self.live().GetCapabilities();
        }, "What the backend that opened can do (§7.2).")

        .def("get_statistics", [](Encoder& self) {
            return self.live().GetStatistics();
        }, "Counters, including the degradations §22 wants visible.")

        .def("close", &Encoder::close, "Idempotent (§58.6).")
        .def_property_readonly("closed", [](Encoder& self) { return self.closed(); })
        .def("__enter__", [](py::object self) { return self; })
        .def("__exit__", [](Encoder& self, const py::object&, const py::object&,
                            const py::object&) {
            self.close();
            return false;  // never swallow the exception that unwound us
        });

    // §58.3 asks for capability discovery without opening an encoder. The registry of
    // §26.4 is what makes it possible: it lists what this build linked, and each backend
    // answers for itself.
    m.def("available_backends", [] {
        std::vector<sve::Backend> out;
        for (const sve::Backend kind : sve::default_backend_registry().kinds()) {
            out.push_back(kind);
        }
        return out;
    }, "The backends this build has, whether or not their device is present.");

    m.def("get_capabilities", [](sve::Backend backend, sve::Codec codec) {
        const sve::IEncoderBackend* found = sve::default_backend_registry().Find(backend);
        if (found == nullptr) {
            throw py::value_error(std::string("backend ") + sve::backend_name(backend)
                + " is not in this build");
        }
        return found->GetCapabilities(codec);
    }, py::arg("backend"), py::arg("codec"),
       "What one backend can do for one codec, without opening an encoder (§23, §58.3).");

    m.def("backend_name", &sve::backend_name, py::arg("backend"));
    m.def("status_code_name", &sve::status_code_name, py::arg("code"));
}

} // namespace sve_python
