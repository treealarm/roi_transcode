#pragma once

// A frame the encoder can hold on to, and what it is to be told about it.
//
// This header names no FFmpeg type, which is the whole point of it existing separately.
// The frame is an owning handle to an IPictureStore -- an interface an embedder can
// implement over their own pixels without linking FFmpeg at all.
//
// AVFrame interop lives in `ta_roienc/picture.h`, which is the opt-in header and turns one into a
// Picture; see AvPictureBridge there.

#include "ta_roienc/picture_store.h"
#include "ta_roienc/picture_info.h"

#include <memory>
#include <utility>

namespace ta_roienc {

/// A frame this object keeps alive, however its pixels are stored.
///
/// Copyable, and shared rather than unique, because of the encoder's own behaviour: with
/// lookahead or B-frames it holds a frame past the call that submitted it. Unique ownership
/// would force the caller to choose between giving the frame away and keeping a way to look
/// at it.
///
/// The buffer behind it can be CPU planes, a surface from a backend's own pool, or
/// anything else an embedder implements IPictureStore over. That is what makes this type
/// FFmpeg-free without giving up the zero-copy path.
class Picture {
public:
    Picture() = default;

    explicit Picture(std::shared_ptr<IPictureStore> buffer) : m_buffer(std::move(buffer)) {}

    /// The pixels. Only valid on a non-empty Picture -- check with operator bool, or let
    /// SubmitPicture do it, which refuses an empty one with InvalidArgument.
    const IPictureStore& buffer() const noexcept { return *m_buffer; }
    IPictureStore& writable_data() noexcept { return *m_buffer; }

    /// The handle itself, for a caller that wants to keep a reference of their own.
    const std::shared_ptr<IPictureStore>& shared() const noexcept { return m_buffer; }

    explicit operator bool() const noexcept { return m_buffer != nullptr; }

private:
    std::shared_ptr<IPictureStore> m_buffer;
};

/// A frame and what the encoder is to be told about it.
///
/// The frame inside holds its own reference, so an encoder that buffers it for lookahead
/// or B-frames is not depending on the caller keeping theirs.
struct StampedPicture {
    Picture frame;
    PictureInfo metadata;
};

} // namespace ta_roienc
