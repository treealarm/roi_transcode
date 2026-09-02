// Objects, regions and packets -- TZ v4.2 §58.3.
//
// One thing here is not a plain translation. Regions::boxes is a std::span, so it does not
// own what it points at; a Python list converted to a temporary vector would leave the
// encoder reading freed memory the moment the call returned. FrameMetadata is therefore
// wrapped in a holder that owns its boxes, and the span is rebuilt to point into that. The
// C++ side keeps the span, which is right for a C++ caller who has the vector in hand, and
// the binding is where the difference in ownership between the two languages is paid for.

#include "bindings.h"

#include <pybind11/stl.h>

#include <vector>

namespace py = pybind11;

namespace sve_python {

void register_metadata(py::module_& m)
{
    py::class_<sve::MediaTimestamp>(m, "MediaTimestamp", "§8.7: a timestamp with units.")
        .def(py::init<>())
        .def(py::init([](int64_t value, sve::Rational tb) {
            return sve::MediaTimestamp{value, tb};
        }), py::arg("value"), py::arg("time_base") = sve::Rational{1, 90000})
        .def_readwrite("value", &sve::MediaTimestamp::value)
        .def_readwrite("time_base", &sve::MediaTimestamp::time_base)
        .def("valid", &sve::MediaTimestamp::valid);

    // §58.3 lists `Polygon` among the mandatory bindings. There is no Polygon type in the
    // C++ library: an outline is a std::vector<Point> on the box, and inventing a wrapper
    // class here would give Python a type the library does not have and that nothing on the
    // C++ side could be handed. So the point is bound and the outline is a list of them,
    // which is the same thing under the name the library actually uses.
    py::class_<sve::Point>(m, "Point", "One vertex of an outline, normalized 0..1 (§9).")
        .def(py::init<>())
        .def_readwrite("x", &sve::Point::x)
        .def_readwrite("y", &sve::Point::y);

    // §11's last two factors read off a track. Supplied rather than computed on this path:
    // a caller pushing frames has whatever its own tracker knows, and leaving `present`
    // false is how it says it has none.
    py::class_<sve::TrackEvidence>(m, "TrackEvidence",
        "What a track says about an object beyond where it is (§11).")
        .def(py::init<>())
        .def_readwrite("present", &sve::TrackEvidence::present)
        .def_readwrite("age_sec", &sve::TrackEvidence::age_sec)
        .def_readwrite("speed", &sve::TrackEvidence::speed)
        .def_readwrite("sample_count", &sve::TrackEvidence::sample_count);

    py::class_<sve::Box>(m, "BoundingBox",
        "One detected object, in normalized frame coordinates (§9).")
        .def(py::init<>())
        .def_readwrite("t_sec", &sve::Box::t_sec)
        .def_readwrite("kind", &sve::Box::kind)
        .def_readwrite("x", &sve::Box::x)
        .def_readwrite("y", &sve::Box::y)
        .def_readwrite("width", &sve::Box::width)
        .def_readwrite("height", &sve::Box::height)
        .def_readwrite("track_id", &sve::Box::track_id)
        .def_readwrite("confidence", &sve::Box::confidence)
        .def_readwrite("label", &sve::Box::label)
        // Both absent until 2026-09-02, and both are decisions the encoder makes: with three
        // or more points the polygon is protected instead of the axis-aligned box, and the
        // track evidence feeds two of §11's five factors. A Python caller could reach
        // neither, so the two paths were not the same encoder.
        .def_readwrite("polygon", &sve::Box::polygon)
        .def_readwrite("track", &sve::Box::track);

    // FrameMetadata with owned boxes. The C++ struct holds a span into the caller's
    // storage; Python has no such storage to point at, so this owns it and hands the
    // encoder a span into itself. Callers see one type and never learn the difference.
    py::class_<OwnedFrameMetadata>(m, "FrameMetadata",
        "What accompanies one frame: its timestamp and the objects found in it (§9).")
        .def(py::init<>())
        .def_property("timestamp",
            [](const OwnedFrameMetadata& f) { return f.meta.timestamp; },
            [](OwnedFrameMetadata& f, sve::MediaTimestamp t) { f.meta.timestamp = t; })
        .def_property("force_key_frame",
            [](const OwnedFrameMetadata& f) { return f.meta.force_key_frame; },
            [](OwnedFrameMetadata& f, bool v) { f.meta.force_key_frame = v; })
        .def_property("objects",
            [](const OwnedFrameMetadata& f) { return f.boxes; },
            [](OwnedFrameMetadata& f, std::vector<sve::Box> boxes) {
                f.boxes = std::move(boxes);
                f.meta.regions.boxes = f.boxes;
            });

    py::class_<sve::EncodedPacket>(m, "EncodedPacket",
        "One encoded packet (§8.6). `data` is a copy: §58.6 forbids a view outliving its "
        "owner, and the packet's buffer is shared immutable on the C++ side.")
        .def_property_readonly("data", [](const sve::EncodedPacket& p) {
            const std::span<const uint8_t> bytes = p.payload.bytes();
            return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        })
        .def_readonly("pts", &sve::EncodedPacket::pts)
        .def_readonly("dts", &sve::EncodedPacket::dts)
        .def_readonly("duration", &sve::EncodedPacket::duration)
        .def_readonly("key_frame", &sve::EncodedPacket::key_frame)
        .def_readonly("codec", &sve::EncodedPacket::codec)
        .def_readonly("time_base", &sve::EncodedPacket::time_base)
        .def("__len__", [](const sve::EncodedPacket& p) { return p.payload.bytes().size(); });
}

} // namespace sve_python
