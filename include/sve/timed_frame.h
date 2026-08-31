#pragma once

// A frame the encoder can hold on to, and what it is to be told about it -- TZ v4.1 §8.5.
//
// This header names no FFmpeg type, which is the whole point of it existing separately.
// Until 4.1 `Frame` was a wrapper around AVFrame, so the one type a caller needs in order
// to submit anything dragged libavutil in with it, and `sve.h` could not include it. Now
// the frame is an owning handle to an IFrameBuffer -- an interface an embedder can
// implement over their own pixels without linking FFmpeg at all -- and this header is in
// the umbrella where it belongs.
//
// AVFrame has not gone anywhere. It moved to `sve/frame.h`, which is the opt-in interop
// header and turns one into a Frame; see AvFrameAdapter there.

#include "sve/frame_buffer.h"
#include "sve/frame_metadata.h"

#include <memory>
#include <utility>

namespace sve {

/// A frame this object keeps alive, however its pixels are stored.
///
/// Copyable, and shared rather than unique, because §8.5 asks for shared ownership and the
/// reason is the encoder's own behaviour: with lookahead or B-frames it holds a frame past
/// the call that submitted it. Unique ownership would force the caller to choose between
/// giving the frame away and keeping a way to look at it.
///
/// The buffer behind it can be CPU planes, a surface from a backend's own pool, or
/// anything else an embedder implements IFrameBuffer over. That is what makes this type
/// FFmpeg-free without giving up the zero-copy path.
class Frame {
public:
    Frame() = default;

    explicit Frame(std::shared_ptr<IFrameBuffer> buffer) : m_buffer(std::move(buffer)) {}

    /// The pixels. Only valid on a non-empty Frame -- check with operator bool, or let
    /// SubmitFrame do it, which refuses an empty one with InvalidArgument.
    const IFrameBuffer& buffer() const noexcept { return *m_buffer; }
    IFrameBuffer& mutable_buffer() noexcept { return *m_buffer; }

    /// The handle itself, for a caller that wants to keep a reference of their own.
    const std::shared_ptr<IFrameBuffer>& shared() const noexcept { return m_buffer; }

    explicit operator bool() const noexcept { return m_buffer != nullptr; }

private:
    std::shared_ptr<IFrameBuffer> m_buffer;
};

/// A frame and what the encoder is to be told about it -- TZ v4.1 §8.5.
///
/// The frame inside holds its own reference, so an encoder that buffers it for lookahead
/// or B-frames is not depending on the caller keeping theirs.
struct TimedFrame {
    Frame frame;
    FrameMetadata metadata;
};

} // namespace sve
