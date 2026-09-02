// NumPy and buffer-protocol frames -- TZ v4.2 §58.4.
//
// The section is strict and every line of it is earned:
//
//     Zero-copy разрешён только если lifetime Python owner гарантирован до завершения
//     асинхронной C++ операции. ... Скрытое использование dangling pointer запрещено.
//
// The encoder holds frames past the call that submitted them -- lookahead and B-frames are
// the whole reason §8.13 has a Flush -- so a plain pointer into a NumPy array is a dangling
// pointer waiting for the garbage collector. WrapRawFrame already asks for an owner and
// refuses without one (§8.10), so the binding's job is to produce that owner: a strong
// reference to the Python object, released when the last Frame holding it goes.
//
// Validation happens before any of that. §58.4 lists dtype, shape, strides, alignment,
// plane sizes and lifetime, and a wrong shape must be a ValueError from Python rather than
// a short read inside an encoder several frames later.

#include "bindings.h"

#include <pybind11/numpy.h>

#include <cstdint>
#include <memory>
#include <string>

namespace py = pybind11;

namespace sve_python {

namespace {

struct PlaneShape {
    int32_t rows;       ///< lines of samples in this plane
    int32_t width;      ///< samples per line
    int32_t bytes_per_sample;
};

/// How the planes of one format are laid out for a frame of this size.
///
/// The same short list the rest of the library works from (raw_frame.cpp). Not derived from
/// a descriptor, because what is being checked is the caller's array against what the
/// format means here.
std::vector<PlaneShape> planes_of(sve::PixelFormat format, int32_t width, int32_t height)
{
    const int32_t half_w = (width + 1) / 2;
    const int32_t half_h = (height + 1) / 2;
    switch (format) {
    case sve::PixelFormat::YUV420P:
        return {{height, width, 1}, {half_h, half_w, 1}, {half_h, half_w, 1}};
    case sve::PixelFormat::YUV422P:
        return {{height, width, 1}, {height, half_w, 1}, {height, half_w, 1}};
    case sve::PixelFormat::YUV444P:
        return {{height, width, 1}, {height, width, 1}, {height, width, 1}};
    case sve::PixelFormat::NV12:
        // Two planes: luma, then interleaved chroma at half height and full width.
        return {{height, width, 1}, {half_h, width, 1}};
    case sve::PixelFormat::P010:
        // The same shape as NV12 with two bytes a sample -- §17's ten-bit input.
        return {{height, width, 2}, {half_h, width, 2}};
    case sve::PixelFormat::GRAY8:
        return {{height, width, 1}};
    case sve::PixelFormat::Unknown:
    case sve::PixelFormat::HardwareSurface:
        break;
    }
    return {};
}

[[noreturn]] void bad_value(const std::string& what)
{
    throw py::value_error(what);
}

} // namespace

sve::Frame frame_from_buffer(const py::object& array, sve::PixelFormat format,
    int32_t width, int32_t height)
{
    const std::vector<PlaneShape> shapes = planes_of(format, width, height);
    if (shapes.empty()) {
        bad_value("pixel format " + std::string(sve::pixel_format_name(format))
            + " cannot be wrapped from a buffer");
    }
    if (width <= 0 || height <= 0) {
        bad_value("width and height must be positive");
    }

    // A sequence of arrays, one per plane, or a single array for a single-plane format.
    std::vector<py::object> items;
    if (py::isinstance<py::sequence>(array) && !py::isinstance<py::buffer>(array)) {
        for (py::handle h : py::cast<py::sequence>(array)) {
            items.push_back(py::reinterpret_borrow<py::object>(h));
        }
    } else {
        items.push_back(array);
    }
    if (items.size() != shapes.size()) {
        bad_value("pixel format " + std::string(sve::pixel_format_name(format)) + " needs "
            + std::to_string(shapes.size()) + " plane(s), got "
            + std::to_string(items.size()));
    }

    sve::RawFrameView view;
    view.format.pixel_format = format;
    view.format.width = width;
    view.format.height = height;
    view.plane_count = shapes.size();

    for (std::size_t i = 0; i < shapes.size(); ++i) {
        py::buffer buffer;
        try {
            buffer = py::cast<py::buffer>(items[i]);
        } catch (const py::cast_error&) {
            throw py::type_error("plane " + std::to_string(i)
                + " does not support the buffer protocol");
        }
        const py::buffer_info info = buffer.request(false);
        const PlaneShape& want = shapes[i];

        if (info.itemsize != want.bytes_per_sample) {
            bad_value("plane " + std::to_string(i) + " has "
                + std::to_string(info.itemsize) + "-byte samples, expected "
                + std::to_string(want.bytes_per_sample));
        }
        if (info.ndim != 2) {
            bad_value("plane " + std::to_string(i) + " must be two-dimensional, got "
                + std::to_string(info.ndim) + " dimension(s)");
        }
        if (info.shape[0] != want.rows || info.shape[1] != want.width) {
            bad_value("plane " + std::to_string(i) + " is "
                + std::to_string(info.shape[0]) + "x" + std::to_string(info.shape[1])
                + ", expected " + std::to_string(want.rows) + "x"
                + std::to_string(want.width));
        }
        // Rows may be padded -- that is what a stride is for -- but samples within a row
        // may not: the encoder reads a row as contiguous memory, and a column-strided view
        // is not that. §58.4 allows non-contiguous arrays "только при валидных strides",
        // and this is the boundary of valid.
        if (info.strides[1] != want.bytes_per_sample) {
            bad_value("plane " + std::to_string(i)
                + " is strided within a row; only row padding is supported");
        }
        if (info.strides[0] < static_cast<py::ssize_t>(want.width) * want.bytes_per_sample) {
            bad_value("plane " + std::to_string(i) + " has a row stride shorter than a row");
        }

        sve::RawFramePlane& plane = view.planes[i];
        plane.data = static_cast<const uint8_t*>(info.ptr);
        plane.stride = static_cast<int32_t>(info.strides[0]);
        plane.size = static_cast<std::size_t>(info.strides[0]) * static_cast<std::size_t>(want.rows);
    }

    // The owner: strong references to the plane objects whose memory was just mapped,
    // released on the Python side when the last Frame lets go. The deleter takes the GIL
    // because dropping a reference is a Python operation and the Frame may well die on a
    // C++ thread.
    //
    // The planes and not the container the caller passed them in. Holding the list keeps
    // the planes alive only for as long as the list still refers to them, and a caller who
    // reuses one list per frame -- `pl[0] = next_luma`, which is the obvious way to write
    // that loop -- would free a plane the encoder is still reading. §58.4 forbids exactly
    // that: "скрытое использование dangling pointer запрещено". Holding what was mapped
    // makes the guarantee independent of what the caller does with their own container.
    py::tuple mapped(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        mapped[i] = items[i];
    }
    auto held = std::make_shared<py::object>(std::move(mapped));
    view.owner = std::shared_ptr<const void>(held, held.get());

    sve::Frame frame;
    if (const sve::Status s = sve::WrapRawFrame(view, &frame); !s.ok()) {
        raise_status(s);
    }
    return frame;
}

void register_frames(py::module_& m)
{
    m.def("_frame_from_arrays",
        [](const py::object& array, sve::PixelFormat format, int32_t width, int32_t height) {
            return frame_from_buffer(array, format, width, height);
        },
        py::arg("planes"), py::arg("pixel_format"), py::arg("width"), py::arg("height"),
        "Wrap plane arrays as a Frame without copying, holding a reference to the arrays.");

    py::class_<sve::Frame>(m, "Frame",
        "Pixels the encoder can read. Holds whatever owns them alive (§8.10, §58.4).")
        .def("valid", [](const sve::Frame& f) { return static_cast<bool>(f); });
}

} // namespace sve_python
