#pragma once

// Shared declarations for the binding translation units -- TZ v4.2 §58.8.
//
// The layout §58.8 recommends splits the bindings by subject rather than keeping one file,
// and this header is what lets them be separate: each unit registers its own types into the
// module, and module.cpp calls them in order.

#include "sve/sve.h"

#include <pybind11/pybind11.h>

#include <vector>

namespace sve_python {

/// FrameMetadata plus the storage its boxes live in.
///
/// sve::Regions::boxes is a std::span -- right for a C++ caller holding the vector, and
/// impossible to hand a Python list directly: the temporary would be gone before the
/// encoder read it. The binding owns the boxes and points the span at its own storage.
struct OwnedFrameMetadata {
    std::vector<sve::Box> boxes;
    sve::FrameMetadata meta;

    /// Re-points the span after any move or copy, since the vector's address changes.
    sve::FrameMetadata& view()
    {
        meta.regions.boxes = boxes;
        return meta;
    }
};

/// §58.6's exception hierarchy. Registered first: everything else may raise through it.
void register_exceptions(pybind11::module_& m);

/// Raises the Python exception that corresponds to `status`, keeping the code and the
/// original message. Does nothing when the status is Ok.
///
/// Again and EndOfStream are never raised through here -- they are answers, not failures,
/// and §58.6 requires them to have their own Python semantics rather than being masked.
[[noreturn]] void raise_status(const sve::Status& status);

void register_config(pybind11::module_& m);
void register_metadata(pybind11::module_& m);
void register_frames(pybind11::module_& m);
void register_encoder(pybind11::module_& m);

/// A NumPy array (or any buffer-protocol object) as something the encoder can read.
///
/// §58.4 is strict about this and for good reason: the encoder may hold a frame well past
/// the call that submitted it, so the binding either keeps the Python owner alive or copies.
/// Returns a frame that holds a strong reference to the object it was built from.
sve::Frame frame_from_buffer(const pybind11::object& array, sve::PixelFormat format,
    int32_t width, int32_t height);

} // namespace sve_python
